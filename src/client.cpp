#include "srpc/client.hpp"

#include "connection.hpp"
#include "protocol.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <system_error>
#include <thread>
#include <utility>

namespace srpc {
namespace {

constexpr std::uint32_t jrpc_resolve_function = 9;
constexpr std::uint32_t jrpc_cpu_key = 10;
constexpr std::uint32_t jrpc_shutdown = 11;
constexpr std::uint32_t jrpc_xnotify = 12;
constexpr std::uint32_t jrpc_kernel_version = 13;
constexpr std::uint32_t jrpc_set_leds = 14;
constexpr std::uint32_t jrpc_constant_memory_set = 18;
constexpr std::uint32_t jrpc_srpc_tunnel = 100;
constexpr std::size_t maximum_multiline_size = 16 * 1024 * 1024;
constexpr std::size_t maximum_transfer_chunk_size = 16 * 1024 * 1024;

namespace fs = std::filesystem;

bool is_array_return(ReturnType type) {
    return type == ReturnType::int32_array || type == ReturnType::float_array ||
        type == ReturnType::byte_array;
}

void validate_options(const std::string& host, const ClientOptions& options) {
    if (host.empty()) {
        throw ProtocolError("Console host cannot be empty");
    }
    if (host.find_first_of("\r\n\0", 0, 3) != std::string::npos) {
        throw ProtocolError("Console host cannot contain CR, LF, or NUL characters");
    }
    if (options.port == 0) {
        throw ProtocolError("Console port cannot be zero");
    }
    if (options.connect_timeout.count() <= 0 || options.io_timeout.count() <= 0 ||
        options.rpc_timeout.count() <= 0 || options.poll_interval.count() <= 0) {
        throw ProtocolError("All client timeouts and polling intervals must be positive");
    }
}

std::string make_memory_command(
    std::string_view name,
    std::uint32_t address,
    std::size_t size) {
    std::ostringstream command;
    command << name << " addr=0x" << std::uppercase << std::hex << address
            << std::dec << " length=" << size;
    return command.str();
}

void validate_memory_range(std::uint32_t address, std::size_t size) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("A single memory operation cannot exceed 4 GiB");
    }
    if (size != 0 && size - 1 > std::numeric_limits<std::uint32_t>::max() - address) {
        throw ProtocolError("Memory operation crosses the 32-bit address boundary");
    }
}

void validate_text(std::string_view text, std::string_view description) {
    if (text.find_first_of("\r\n\0", 0, 3) != std::string_view::npos) {
        throw ProtocolError(
            std::string(description) + " cannot contain CR, LF, or NUL characters");
    }
}

void validate_file_transfer_options(const FileTransferOptions& options) {
    if (options.chunk_size == 0) {
        throw ProtocolError("File transfer chunk size must be greater than zero");
    }
    if (options.chunk_size > maximum_transfer_chunk_size) {
        throw ProtocolError("File transfer chunk size cannot exceed 16 MiB");
    }
    switch (options.existing_file) {
    case ExistingFilePolicy::fail:
    case ExistingFilePolicy::overwrite:
    case ExistingFilePolicy::skip:
        break;
    default:
        throw ProtocolError("Invalid existing-file policy");
    }
}

TransferControl report_file_progress(
    const FileTransferOptions& options,
    TransferPhase phase,
    const fs::path& local_path,
    std::string_view remote_path,
    std::uint64_t transferred,
    std::uint64_t total) {
    if (!options.progress) {
        return TransferControl::continue_;
    }

    TransferProgress progress;
    progress.phase = phase;
    progress.local_path = local_path;
    progress.remote_path = std::string(remote_path);
    progress.file_bytes_transferred = transferred;
    progress.file_bytes_total = total;
    progress.overall_bytes_transferred = transferred;
    progress.overall_bytes_total = total;
    progress.file_index = 1;
    progress.file_count = 1;
    return options.progress(progress);
}

std::uint32_t parse_named_hex_value(
    std::string_view text,
    std::string_view name) {
    const auto marker = std::string(name) + "=";
    const auto marker_position = text.find(marker);
    if (marker_position == std::string_view::npos) {
        throw ProtocolError(
            "Plugin response is missing the " + std::string(name) + " field");
    }
    auto first = marker_position + marker.size();
    if (first + 2 <= text.size() && text[first] == '0' &&
        (text[first + 1] == 'x' || text[first + 1] == 'X')) {
        first += 2;
    }
    auto last = first;
    while (last < text.size() && std::isxdigit(
            static_cast<unsigned char>(text[last])) != 0) {
        ++last;
    }
    if (last == first) {
        throw ProtocolError(
            "Plugin response contains an invalid " + std::string(name) + " field");
    }
    return detail::parse_hex_u32(text.substr(first, last - first));
}

std::optional<std::uint32_t> parse_optional_screenshot_field(
    std::string_view metadata,
    std::string_view name) {
    std::optional<std::uint32_t> result;
    for (std::size_t offset = 0; offset < metadata.size();) {
        while (offset < metadata.size() && std::isspace(
                static_cast<unsigned char>(metadata[offset])) != 0) {
            ++offset;
        }
        const auto token_begin = offset;
        while (offset < metadata.size() && std::isspace(
                static_cast<unsigned char>(metadata[offset])) == 0) {
            ++offset;
        }
        if (token_begin == offset) {
            continue;
        }

        const auto token = metadata.substr(token_begin, offset - token_begin);
        const auto separator = token.find('=');
        if (separator == std::string_view::npos ||
            token.substr(0, separator) != name) {
            continue;
        }
        if (result.has_value()) {
            throw ProtocolError(
                "Screenshot metadata contains duplicate " +
                std::string(name) + " fields");
        }
        try {
            auto value = token.substr(separator + 1);
            // Microsoft's screenshot metadata places a comma immediately
            // after offsety on some XBDM builds.
            if (value.ends_with(',')) {
                value.remove_suffix(1);
            }
            result = detail::parse_hex_u32(value);
        } catch (const ProtocolError&) {
            throw ProtocolError(
                "Screenshot metadata contains an invalid " +
                std::string(name) + " field");
        }
    }
    return result;
}

std::uint32_t parse_screenshot_field(
    std::string_view metadata,
    std::string_view name) {
    const auto value = parse_optional_screenshot_field(metadata, name);
    if (!value.has_value()) {
        throw ProtocolError(
            "Screenshot metadata is missing the " + std::string(name) +
            " field");
    }
    return *value;
}

bool path_exists(const fs::path& path) {
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        throw fs::filesystem_error("Could not inspect local path", path, error);
    }
    return exists;
}

bool is_reparse_point(const fs::path& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        throw fs::filesystem_error(
            "Could not inspect local path attributes",
            path,
            std::error_code(static_cast<int>(error), std::system_category()));
    }
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

fs::path make_temporary_download_path(const fs::path& target) {
    static std::atomic<std::uint64_t> counter{0};
    for (std::size_t attempt = 0; attempt < 10000; ++attempt) {
        auto candidate = target;
        candidate += L".srpc-part-";
        candidate += std::to_wstring(GetCurrentProcessId());
        candidate += L"-";
        candidate += std::to_wstring(counter.fetch_add(1, std::memory_order_relaxed));
        if (!path_exists(candidate)) {
            return candidate;
        }
    }
    throw ProtocolError("Could not choose a unique temporary download path");
}

void remove_partial_file(const fs::path& path) noexcept {
    std::error_code ignored;
    static_cast<void>(fs::remove(path, ignored));
}

void commit_download(
    const fs::path& temporary_path,
    const fs::path& target_path,
    ExistingFilePolicy policy) {
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (policy == ExistingFilePolicy::overwrite) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (MoveFileExW(temporary_path.c_str(), target_path.c_str(), flags) == FALSE) {
        const auto error = GetLastError();
        throw fs::filesystem_error(
            "Could not commit downloaded file",
            temporary_path,
            target_path,
            std::error_code(static_cast<int>(error), std::system_category()));
    }
}

} // namespace

struct Client::Impl {
    explicit Impl(std::string client_host, ClientOptions client_options)
        : host(std::move(client_host)),
          options(client_options),
          selected_protocol(options.protocol) {
        validate_options(host, options);
    }

    void ensure_connected_locked() const {
        if (!connection.connected()) {
            throw ConnectionError("Not connected to a console");
        }
    }

    void connect_locked() {
        if (binary_transfer_active) {
            throw ProtocolError(
                "Cannot reconnect from inside a file-transfer progress callback");
        }
        if (connection.connected()) {
            return;
        }

        selected_protocol = options.protocol;
        try {
            connection.connect(
                host, options.port, options.connect_timeout, options.io_timeout);
            const auto banner = detail::parse_response_line(connection.read_line());
            detail::throw_if_command_failed(banner);
        } catch (...) {
            connection.close();
            throw;
        }
    }

    Response send_command_raw_locked(std::string_view command) {
        if (binary_transfer_active) {
            throw ProtocolError(
                "A progress callback cannot start another command during a binary transfer");
        }
        ensure_connected_locked();
        detail::validate_command(command);

        std::string wire(command);
        wire += "\r\n";
        connection.send_all(std::span(
            reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
        return detail::parse_response_line(connection.read_line());
    }

    Response send_command_locked(std::string_view command) {
        auto response = send_command_raw_locked(command);
        detail::throw_if_command_failed(response);
        return response;
    }

    void send_command_no_reply_locked(std::string_view command) {
        if (binary_transfer_active) {
            throw ProtocolError(
                "A progress callback cannot start another command during a binary transfer");
        }
        ensure_connected_locked();
        detail::validate_command(command);

        std::string wire(command);
        wire += "\r\n";
        try {
            connection.send_all(std::span(
                reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()));
        } catch (...) {
            connection.close();
            selected_protocol = options.protocol;
            throw;
        }
        connection.close();
        selected_protocol = options.protocol;
    }

    void abandon_binary_transfer_locked() noexcept {
        binary_transfer_active = false;
        connection.close();
        selected_protocol = options.protocol;
    }

    std::vector<std::string> read_multiline_locked() {
        std::vector<std::string> lines;
        std::size_t total_size = 0;
        for (;;) {
            auto line = connection.read_line();
            if (line == ".") {
                return lines;
            }
            total_size += line.size();
            if (total_size > maximum_multiline_size) {
                connection.close();
                throw ProtocolError("XBDM multiline response exceeded 16 MiB");
            }
            lines.push_back(std::move(line));
        }
    }

    std::string read_multiline_text_locked() {
        auto lines = read_multiline_locked();
        std::size_t total_size = 0;
        for (const auto& line : lines) {
            total_size += line.size();
        }
        std::string text;
        text.reserve(total_size);
        for (const auto& line : lines) {
            text += line;
        }
        return text;
    }

    std::string jrpc_send_locked(std::string_view command) {
        const auto response = send_command_raw_locked(command);
        if (response.status_code == 202) {
            static_cast<void>(read_multiline_locked());
            throw RpcError(
                "consolefeatures returned a multiline response; the SRPC/JRPC2 "
                "plugin does not appear to be loaded");
        }
        detail::throw_if_command_failed(response);
        return response.message;
    }

    std::string await_jrpc_locked(std::string response) {
        auto pending = detail::parse_jrpc_pending_address(response);
        if (!pending.has_value()) {
            return response;
        }

        const auto deadline = std::chrono::steady_clock::now() + options.rpc_timeout;
        while (pending.has_value()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw TimeoutError(
                    "JRPC2 call timed out after " +
                    std::to_string(options.rpc_timeout.count()) + " ms");
            }
            const auto delay = std::min(
                options.poll_interval,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            std::this_thread::sleep_for(delay);
            response = jrpc_send_locked(detail::build_jrpc_poll_command(*pending));
            pending = detail::parse_jrpc_pending_address(response);
        }
        return response;
    }

    std::string jrpc_call_locked(
        std::uint32_t type,
        std::uint32_t address,
        std::span<const RpcArgument> arguments,
        CallOptions call_options = {},
        std::optional<detail::ModuleTarget> target = std::nullopt) {
        auto command = detail::build_jrpc_command(
            type, address, arguments, call_options, target);
        auto response = await_jrpc_locked(jrpc_send_locked(command));
        detail::throw_if_jrpc_failed(response);
        return response;
    }

    Protocol detect_protocol_locked() {
        ensure_connected_locked();
        if (selected_protocol != Protocol::automatic) {
            return selected_protocol;
        }

        const auto native_probe = send_command_raw_locked("s360");
        if (native_probe.status_code == 200) {
            selected_protocol = Protocol::native_srpc;
            return selected_protocol;
        }
        if (native_probe.status_code == 202) {
            static_cast<void>(read_multiline_locked());
        }

        try {
            const auto response = jrpc_call_locked(
                jrpc_kernel_version,
                0,
                std::span<const RpcArgument>{});
            static_cast<void>(detail::parse_decimal_u32(response));
        } catch (const Error& error) {
            throw RpcError(
                std::string("Console answered neither native SRPC nor JRPC2; ") +
                "is the SRPC plugin loaded? Details: " + error.what());
        }

        selected_protocol = Protocol::jrpc2;
        return selected_protocol;
    }

    std::string send_srpc_locked(std::string_view command) {
        const auto normalized = detail::normalize_srpc_command(command);
        if (detect_protocol_locked() == Protocol::native_srpc) {
            return send_command_locked(normalized).message;
        }

        const std::vector<RpcArgument> arguments{std::string(normalized)};
        return detail::trim_ascii(jrpc_call_locked(
            jrpc_srpc_tunnel,
            0,
            std::span<const RpcArgument>(arguments)));
    }

    RpcValue native_call_locked(
        std::uint32_t address,
        ReturnType return_type,
        std::span<const RpcArgument> arguments,
        CallOptions call_options) {
        if (return_type != ReturnType::void_ && return_type != ReturnType::int32 &&
            return_type != ReturnType::byte && return_type != ReturnType::float32) {
            throw ProtocolError(
                "Native SRPC only returns the 32-bit r3 word; strings, 64-bit "
                "values, and arrays require JRPC2");
        }
        if (!call_options.system_thread || call_options.virtual_machine ||
            call_options.array_size != 0) {
            throw ProtocolError(
                "Thread selection, VM calls, and array sizing require JRPC2");
        }

        auto response = send_srpc_locked(detail::build_native_call(address, arguments));
        const auto deadline = std::chrono::steady_clock::now() + options.rpc_timeout;
        while (detail::trim_ascii(response) == "pending") {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw TimeoutError(
                    "Native SRPC call timed out after " +
                    std::to_string(options.rpc_timeout.count()) + " ms");
            }
            const auto delay = std::min(
                options.poll_interval,
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
            std::this_thread::sleep_for(delay);
            response = send_srpc_locked("s360 poll");
        }

        const auto word = detail::parse_native_word(response);
        switch (return_type) {
        case ReturnType::void_:
            return std::monostate{};
        case ReturnType::int32:
            return word;
        case ReturnType::byte:
            return word & 0xFFu;
        case ReturnType::float32:
            return std::bit_cast<float>(word);
        default:
            break;
        }
        throw ProtocolError("Unsupported native SRPC return type");
    }

    RpcValue call_locked(
        std::uint32_t address,
        ReturnType return_type,
        std::span<const RpcArgument> arguments,
        CallOptions call_options,
        std::optional<detail::ModuleTarget> target = std::nullopt) {
        const auto active_protocol = detect_protocol_locked();
        if (active_protocol == Protocol::native_srpc) {
            if (target.has_value()) {
                address = resolve_function_locked(target->module, target->ordinal);
            }
            return native_call_locked(address, return_type, arguments, call_options);
        }

        if (is_array_return(return_type) && call_options.array_size == 0) {
            throw ProtocolError("JRPC2 array returns require CallOptions::array_size > 0");
        }
        const auto response = jrpc_call_locked(
            static_cast<std::uint32_t>(return_type),
            address,
            arguments,
            call_options,
            target);
        return detail::parse_jrpc_result(response, return_type);
    }

    std::uint32_t resolve_function_locked(
        std::string_view module,
        std::uint32_t ordinal) {
        if (module.empty()) {
            throw ProtocolError("Module name cannot be empty");
        }
        validate_text(module, "Module name");

        if (detect_protocol_locked() == Protocol::jrpc2) {
            const std::vector<RpcArgument> arguments{
                std::string(module), ordinal};
            const auto response = jrpc_call_locked(
                jrpc_resolve_function,
                0,
                std::span<const RpcArgument>(arguments));
            return detail::parse_hex_u32(response);
        }

        const auto bytes = std::span(
            reinterpret_cast<const std::uint8_t*>(module.data()), module.size());
        const auto response = send_srpc_locked(
            "s360 resolve str:" + detail::hex_encode(bytes) + " " +
            std::to_string(ordinal));
        return detail::parse_hex_u32(response);
    }

    ByteBuffer read_memory_text_locked(std::uint32_t address, std::size_t size) {
        const auto response = send_command_raw_locked(
            make_memory_command("getmem", address, size));
        detail::throw_if_command_failed(response);
        if (response.status_code != 202) {
            throw ProtocolError(
                "getmem returned status " + std::to_string(response.status_code) +
                " instead of 202");
        }
        auto bytes = detail::hex_decode(read_multiline_text_locked());
        if (bytes.size() != size) {
            throw ProtocolError(
                "getmem returned " + std::to_string(bytes.size()) +
                " bytes; expected " + std::to_string(size));
        }
        return bytes;
    }

    std::string host;
    ClientOptions options;
    Protocol selected_protocol;
    mutable std::recursive_mutex mutex;
    detail::Connection connection;
    bool binary_transfer_active = false;
};

Client::Client(std::string host, ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(host), options)) {}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

void Client::connect() {
    std::scoped_lock lock(impl_->mutex);
    impl_->connect_locked();
}

void Client::reconnect() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->binary_transfer_active) {
        throw ProtocolError(
            "Cannot reconnect from inside a file-transfer progress callback");
    }
    impl_->connection.close();
    impl_->selected_protocol = impl_->options.protocol;
    impl_->connect_locked();
}

void Client::close() noexcept {
    if (!impl_) {
        return;
    }
    try {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->binary_transfer_active) {
            return;
        }
        impl_->connection.close();
        impl_->selected_protocol = impl_->options.protocol;
    } catch (...) {
        // close() is intentionally safe to call from destructors.
    }
}

bool Client::connected() const noexcept {
    if (!impl_) {
        return false;
    }
    try {
        std::scoped_lock lock(impl_->mutex);
        return impl_->connection.connected();
    } catch (...) {
        return false;
    }
}

std::string_view Client::host() const noexcept {
    return impl_ ? std::string_view(impl_->host) : std::string_view{};
}

Protocol Client::protocol() {
    std::scoped_lock lock(impl_->mutex);
    return impl_->detect_protocol_locked();
}

bool Client::plugin_available() {
    std::scoped_lock lock(impl_->mutex);
    try {
        impl_->ensure_connected_locked();
        if (impl_->options.protocol == Protocol::native_srpc) {
            const auto response = impl_->send_command_raw_locked("s360");
            if (response.status_code == 202) {
                static_cast<void>(impl_->read_multiline_locked());
                return false;
            }
            return response.status_code == 200;
        }
        if (impl_->options.protocol == Protocol::jrpc2) {
            const auto response = impl_->jrpc_call_locked(
                jrpc_kernel_version,
                0,
                std::span<const RpcArgument>{});
            static_cast<void>(detail::parse_decimal_u32(response));
            return true;
        }
        static_cast<void>(impl_->detect_protocol_locked());
        return true;
    } catch (const Error&) {
        return false;
    }
}

Response Client::send_command(std::string_view command) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->send_command_locked(command);
}

Response Client::send_command_raw(std::string_view command) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->send_command_raw_locked(command);
}

std::vector<std::string> Client::send_multiline_command(std::string_view command) {
    std::scoped_lock lock(impl_->mutex);
    const auto response = impl_->send_command_raw_locked(command);
    detail::throw_if_command_failed(response);
    if (response.status_code != 202) {
        return {};
    }
    return impl_->read_multiline_locked();
}

std::string Client::send_srpc(std::string_view command) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->send_srpc_locked(command);
}

namespace {

// Reads the body of a successful `getmemex`.
//
// The body is not a flat run of bytes, which is what this used to assume. XBDM
// frames it as a sequence of blocks:
//
//     uint16 little-endian count
//         bit 15     set on the final block
//         bits 0-14  how many bytes of data follow
//     <count & 0x7FFF bytes>
//
// Verified against a retail console: a 0x1000 request answers with headers
// 0x400, 0x400, 0x400, 0x8400, and a 0x30 request answers with a single
// 0x8030. Reading `size` bytes flat therefore consumed the first header as if
// it were data - every result came back shifted two bytes and truncated - and
// left the remaining headers in the socket buffer, where they desynchronised
// every command sent afterwards.
//
// A block whose count is zero marks memory that could not be read, and XBDM
// sets the end flag on it, so the response is a prefix of the request stopping
// exactly at the first unreadable page. That makes a short body a normal
// answer rather than a protocol error - the connection is still in a good
// state - so it is reported as a failed read at a named address rather than
// papered over. Callers that expect holes should go through
// read_memory_sparse, which asks walkmem first.
ByteBuffer read_getmemex_body(
    detail::Connection& connection,
    std::uint32_t address,
    std::size_t size) {
    ByteBuffer data;
    data.reserve(size);

    while (data.size() < size) {
        const auto header = connection.read_exact(2);
        const auto count = static_cast<std::size_t>(
            static_cast<unsigned>(header[0]) |
            (static_cast<unsigned>(header[1]) << 8));
        const bool last = (count & 0x8000u) != 0;
        const auto length = count & 0x7FFFu;

        if (length > size - data.size()) {
            throw ProtocolError(
                "getmemex announced a " + std::to_string(length) +
                " byte block that overruns the " + std::to_string(size) +
                " bytes requested");
        }
        if (length != 0) {
            const auto block = connection.read_exact(length);
            data.insert(data.end(), block.begin(), block.end());
        }
        if (last) {
            break;
        }
        if (length == 0) {
            // No end flag and no progress: stop rather than spin on a console
            // that frames its response in some way this does not model.
            break;
        }
    }

    if (data.size() != size) {
        std::ostringstream message;
        message << "getmemex returned " << data.size() << " of " << size
                << " bytes from 0x" << std::uppercase << std::hex << address
                << "; memory is unmapped at 0x"
                << (address + static_cast<std::uint32_t>(data.size()));
        throw ProtocolError(message.str());
    }
    return data;
}

} // namespace

ByteBuffer Client::read_memory(std::uint32_t address, std::size_t size) {
    validate_memory_range(address, size);
    if (size == 0) {
        return {};
    }

    std::scoped_lock lock(impl_->mutex);
    const auto response = impl_->send_command_raw_locked(
        make_memory_command("getmemex", address, size));
    if (response.status_code == 203) {
        return read_getmemex_body(impl_->connection, address, size);
    }
    if (response.status_code == 202) {
        const auto text = impl_->read_multiline_text_locked();
        try {
            auto bytes = detail::hex_decode(text);
            if (bytes.size() == size) {
                return bytes;
            }
        } catch (const ProtocolError&) {
            // Some XBDM builds answer unsupported getmemex with text. The body
            // is drained above, so it is safe to retry with getmem.
        }
        return impl_->read_memory_text_locked(address, size);
    }
    if (response.status_code == 407) {
        return impl_->read_memory_text_locked(address, size);
    }
    detail::throw_if_command_failed(response);
    throw ProtocolError(
        "getmemex returned unexpected status " +
        std::to_string(response.status_code));
}

void Client::write_memory(
    std::uint32_t address,
    std::span<const std::uint8_t> data) {
    validate_memory_range(address, data.size());
    if (data.empty()) {
        return;
    }

    std::scoped_lock lock(impl_->mutex);
    for (std::size_t offset = 0; offset < data.size(); offset += detail::setmem_chunk_size) {
        const auto chunk_size = std::min(
            detail::setmem_chunk_size, data.size() - offset);
        const auto chunk = data.subspan(offset, chunk_size);

        std::ostringstream command;
        command << "setmem addr=0x" << std::uppercase << std::hex
                << (address + static_cast<std::uint32_t>(offset))
                << " data=" << detail::hex_encode(chunk, false);
        const auto response = impl_->send_command_locked(command.str());
        if (response.status_code != 200) {
            throw ProtocolError(
                "setmem returned status " + std::to_string(response.status_code) +
                " instead of 200");
        }
    }
}

ByteBuffer Client::download_file(
    std::string_view remote_path,
    std::size_t maximum_size) {
    if (remote_path.empty()) {
        throw ProtocolError("Remote file path cannot be empty");
    }

    std::scoped_lock lock(impl_->mutex);
    const auto response = impl_->send_command_raw_locked(
        "getfile name=" +
        detail::quote_xbdm_argument(remote_path, "Remote file path"));
    detail::throw_if_command_failed(response);
    if (response.status_code == 202) {
        static_cast<void>(impl_->read_multiline_locked());
    }
    if (response.status_code != 203) {
        if (response.status_code != 200 && response.status_code != 201 &&
            response.status_code != 202) {
            // In particular, 204 means the monitor is waiting for a binary
            // upload. There is no safe way to reuse that exchange here.
            impl_->connection.close();
            impl_->selected_protocol = impl_->options.protocol;
        }
        throw ProtocolError(
            "getfile returned status " + std::to_string(response.status_code) +
            " instead of 203");
    }

    const auto size_bytes = impl_->connection.read_exact(4);
    const std::uint32_t file_size =
        static_cast<std::uint32_t>(size_bytes[0]) |
        (static_cast<std::uint32_t>(size_bytes[1]) << 8) |
        (static_cast<std::uint32_t>(size_bytes[2]) << 16) |
        (static_cast<std::uint32_t>(size_bytes[3]) << 24);
    if (file_size > maximum_size) {
        impl_->connection.close();
        impl_->selected_protocol = impl_->options.protocol;
        throw ProtocolError(
            "Remote file is " + std::to_string(file_size) +
            " bytes, over the configured download limit of " +
            std::to_string(maximum_size));
    }
    return impl_->connection.read_exact(file_size);
}

void Client::upload_file(
    std::string_view remote_path,
    std::span<const std::uint8_t> data,
    bool overwrite) {
    if (remote_path.empty()) {
        throw ProtocolError("Remote file path cannot be empty");
    }
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("XBDM cannot upload a file larger than 4 GiB");
    }

    const auto quoted_path =
        detail::quote_xbdm_argument(remote_path, "Remote file path");
    std::ostringstream command;
    command << "sendfile name=" << quoted_path << " length=0x"
            << std::uppercase << std::hex << data.size();

    std::scoped_lock lock(impl_->mutex);
    auto response = impl_->send_command_raw_locked(command.str());
    if (response.status_code == 410 && overwrite) {
        static_cast<void>(impl_->send_command_locked(
            "delete name=" + quoted_path));
        response = impl_->send_command_raw_locked(command.str());
    }

    detail::throw_if_command_failed(response);
    if (response.status_code != 204) {
        throw ProtocolError(
            "sendfile returned status " + std::to_string(response.status_code) +
            " instead of 204");
    }

    impl_->connection.send_all(data);
    const auto final_response = detail::parse_response_line(
        impl_->connection.read_line());
    detail::throw_if_command_failed(final_response);
    if (final_response.status_code != 200) {
        throw ProtocolError(
            "sendfile completion returned status " +
            std::to_string(final_response.status_code) + " instead of 200");
    }
}

RawScreenshot Client::capture_raw_screenshot(
    std::size_t maximum_framebuffer_size) {
    if (maximum_framebuffer_size == 0) {
        throw ProtocolError(
            "Maximum screenshot framebuffer size must be greater than zero");
    }

    std::scoped_lock lock(impl_->mutex);
    const auto response = impl_->send_command_raw_locked("screenshot");
    detail::throw_if_command_failed(response);
    if (response.status_code == 202) {
        // Stock/alternate monitors may describe an unsupported command with a
        // multiline body. Drain it before reporting the unexpected status so
        // the next command still starts on a response boundary.
        static_cast<void>(impl_->read_multiline_locked());
    }
    if (response.status_code != 203) {
        if (response.status_code != 200 && response.status_code != 201 &&
            response.status_code != 202) {
            // In particular, 204 means the monitor is waiting for a binary
            // upload. There is no safe way to reuse that exchange here.
            impl_->connection.close();
            impl_->selected_protocol = impl_->options.protocol;
        }
        throw ProtocolError(
            "screenshot returned status " +
            std::to_string(response.status_code) + " instead of 203");
    }

    // From this point onward the server has started a binary exchange. Any
    // rejected metadata or short body makes the remaining stream ambiguous.
    impl_->binary_transfer_active = true;
    try {
        const auto line = impl_->connection.read_line();
        RawScreenshot screenshot;
        auto& metadata = screenshot.metadata;
        metadata.pitch_bytes = parse_screenshot_field(line, "pitch");
        metadata.width = parse_screenshot_field(line, "width");
        metadata.height = parse_screenshot_field(line, "height");
        metadata.framebuffer_size =
            parse_screenshot_field(line, "framebuffersize");
        metadata.format = parse_optional_screenshot_field(line, "format");
        metadata.display_width =
            parse_optional_screenshot_field(line, "sw").value_or(metadata.width);
        metadata.display_height =
            parse_optional_screenshot_field(line, "sh").value_or(metadata.height);
        metadata.offset_x =
            parse_optional_screenshot_field(line, "offsetx").value_or(0);
        metadata.offset_y =
            parse_optional_screenshot_field(line, "offsety").value_or(0);
        metadata.color_space =
            parse_optional_screenshot_field(line, "colorspace");

        if (metadata.pitch_bytes == 0 || metadata.pitch_bytes % 4 != 0) {
            throw ProtocolError(
                "Screenshot pitch must be a nonzero multiple of four");
        }
        if (metadata.width == 0 || metadata.height == 0) {
            throw ProtocolError("Screenshot width and height must be nonzero");
        }
        if (metadata.pitch_bytes / 4 < metadata.width) {
            throw ProtocolError(
                "Screenshot pitch is smaller than the visible width");
        }
        if (metadata.framebuffer_size == 0 ||
            metadata.framebuffer_size % 4 != 0) {
            throw ProtocolError(
                "Screenshot framebuffer size must be a nonzero multiple of four");
        }
        if (metadata.framebuffer_size > maximum_framebuffer_size) {
            throw ProtocolError(
                "Screenshot framebuffer is " +
                std::to_string(metadata.framebuffer_size) +
                " bytes, over the configured limit of " +
                std::to_string(maximum_framebuffer_size));
        }
        if (metadata.display_width == 0 || metadata.display_height == 0) {
            throw ProtocolError("Screenshot display dimensions must be nonzero");
        }
        if (metadata.offset_x != 0 || metadata.offset_y != 0) {
            const auto right =
                static_cast<std::uint64_t>(metadata.offset_x) + metadata.width;
            const auto bottom =
                static_cast<std::uint64_t>(metadata.offset_y) + metadata.height;
            if (right > metadata.display_width ||
                bottom > metadata.display_height) {
                throw ProtocolError(
                    "Screenshot visible rectangle does not fit its display surface");
            }
        }

        screenshot.tiled_framebuffer =
            impl_->connection.read_exact(metadata.framebuffer_size);
        impl_->binary_transfer_active = false;
        return screenshot;
    } catch (...) {
        if (impl_->binary_transfer_active) {
            impl_->abandon_binary_transfer_locked();
        }
        throw;
    }
}

ScreenshotImage Client::capture_screenshot(ScreenshotOptions options) {
    if (options.maximum_decoded_size == 0) {
        throw ProtocolError(
            "Maximum decoded screenshot size must be greater than zero");
    }
    switch (options.untile_mode) {
    case ScreenshotUntileMode::xenos:
    case ScreenshotUntileMode::morton:
        break;
    default:
        throw ProtocolError("Unknown screenshot untile mode");
    }
    switch (options.packed_10bit_endian) {
    case Endian::little:
    case Endian::big:
        break;
    default:
        throw ProtocolError("Unknown packed 10-bit screenshot byte order");
    }

    auto raw = capture_raw_screenshot(options.maximum_framebuffer_size);
    return decode_screenshot(raw, options);
}

void Client::save_screenshot(
    const fs::path& output_path,
    ScreenshotOptions options) {
    if (output_path.empty()) {
        throw ProtocolError("Screenshot output path cannot be empty");
    }
    const auto image = capture_screenshot(options);
    write_png(output_path, image.view());
}

TransferResult Client::download_file_to(
    std::string_view remote_path,
    const fs::path& local_path,
    FileTransferOptions options) {
    if (remote_path.empty()) {
        throw ProtocolError("Remote file path cannot be empty");
    }
    if (local_path.empty()) {
        throw ProtocolError("Local file path cannot be empty");
    }
    validate_file_transfer_options(options);

    const auto quoted_path =
        detail::quote_xbdm_argument(remote_path, "Remote file path");
    if (path_exists(local_path)) {
        if (fs::is_directory(local_path)) {
            throw fs::filesystem_error(
                "Download destination is a directory",
                local_path,
                std::make_error_code(std::errc::is_a_directory));
        }
        if (is_reparse_point(local_path)) {
            throw ProtocolError(
                "Refusing to replace a local reparse point during download");
        }
        if (options.existing_file == ExistingFilePolicy::skip) {
            TransferResult result;
            result.files_skipped = 1;
            result.cancelled = report_file_progress(
                options,
                TransferPhase::file_skipped,
                local_path,
                remote_path,
                0,
                0) == TransferControl::cancel;
            return result;
        }
        if (options.existing_file == ExistingFilePolicy::fail) {
            throw fs::filesystem_error(
                "Download destination already exists",
                local_path,
                std::make_error_code(std::errc::file_exists));
        }
    }

    const auto parent = local_path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        static_cast<void>(fs::create_directories(parent, error));
        if (error) {
            throw fs::filesystem_error(
                "Could not create the download destination directory",
                parent,
                error);
        }
    }

    const auto temporary_path = make_temporary_download_path(local_path);
    std::ofstream output(temporary_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw fs::filesystem_error(
            "Could not open the temporary download file",
            temporary_path,
            std::make_error_code(std::errc::io_error));
    }

    bool cancelled = false;
    std::uint64_t file_size = 0;
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto response = impl_->send_command_raw_locked(
            "getfile name=" + quoted_path);
        detail::throw_if_command_failed(response);
        if (response.status_code != 203) {
            throw ProtocolError(
                "getfile returned status " + std::to_string(response.status_code) +
                " instead of 203");
        }

        const auto size_bytes = impl_->connection.read_exact(4);
        const std::uint32_t announced_size =
            static_cast<std::uint32_t>(size_bytes[0]) |
            (static_cast<std::uint32_t>(size_bytes[1]) << 8) |
            (static_cast<std::uint32_t>(size_bytes[2]) << 16) |
            (static_cast<std::uint32_t>(size_bytes[3]) << 24);
        file_size = announced_size;
        if (file_size > options.maximum_file_size) {
            impl_->abandon_binary_transfer_locked();
            throw ProtocolError(
                "Remote file is " + std::to_string(file_size) +
                " bytes, over the configured download limit of " +
                std::to_string(options.maximum_file_size));
        }

        impl_->binary_transfer_active = true;
        try {
            if (report_file_progress(
                    options,
                    TransferPhase::transferring,
                    local_path,
                    remote_path,
                    0,
                    file_size) == TransferControl::cancel) {
                cancelled = true;
                impl_->abandon_binary_transfer_locked();
            }

            std::uint64_t transferred = 0;
            while (!cancelled && transferred < file_size) {
                const auto current_size = static_cast<std::size_t>(std::min<std::uint64_t>(
                    options.chunk_size, file_size - transferred));
                const auto chunk = impl_->connection.read_exact(current_size);
                output.write(
                    reinterpret_cast<const char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
                if (!output) {
                    throw fs::filesystem_error(
                        "Could not write the downloaded file",
                        temporary_path,
                        std::make_error_code(std::errc::io_error));
                }
                transferred += chunk.size();
                if (report_file_progress(
                        options,
                        TransferPhase::transferring,
                        local_path,
                        remote_path,
                        transferred,
                        file_size) == TransferControl::cancel) {
                    cancelled = true;
                    impl_->abandon_binary_transfer_locked();
                }
            }
            if (!cancelled) {
                impl_->binary_transfer_active = false;
            }
        } catch (...) {
            if (impl_->binary_transfer_active) {
                impl_->abandon_binary_transfer_locked();
            }
            throw;
        }
    } catch (...) {
        output.close();
        if (options.remove_partial_file) {
            remove_partial_file(temporary_path);
        }
        throw;
    }

    output.close();
    if (!output) {
        if (options.remove_partial_file) {
            remove_partial_file(temporary_path);
        }
        throw fs::filesystem_error(
            "Could not finish writing the downloaded file",
            temporary_path,
            std::make_error_code(std::errc::io_error));
    }
    if (cancelled) {
        if (options.remove_partial_file) {
            remove_partial_file(temporary_path);
        }
        TransferResult result;
        result.cancelled = true;
        return result;
    }

    try {
        commit_download(temporary_path, local_path, options.existing_file);
    } catch (...) {
        if (options.remove_partial_file) {
            remove_partial_file(temporary_path);
        }
        throw;
    }

    TransferResult result;
    result.files_completed = 1;
    result.bytes_transferred = file_size;
    result.cancelled = report_file_progress(
        options,
        TransferPhase::file_complete,
        local_path,
        remote_path,
        file_size,
        file_size) == TransferControl::cancel;
    return result;
}

TransferResult Client::upload_file_from(
    const fs::path& local_path,
    std::string_view remote_path,
    FileTransferOptions options) {
    if (local_path.empty()) {
        throw ProtocolError("Local file path cannot be empty");
    }
    if (remote_path.empty()) {
        throw ProtocolError("Remote file path cannot be empty");
    }
    validate_file_transfer_options(options);

    std::error_code filesystem_error;
    const auto status = fs::status(local_path, filesystem_error);
    if (filesystem_error) {
        throw fs::filesystem_error(
            "Could not inspect the upload source", local_path, filesystem_error);
    }
    if (!fs::is_regular_file(status) || is_reparse_point(local_path)) {
        throw ProtocolError("Upload source must be a regular, non-reparse-point file");
    }

    const auto file_size = fs::file_size(local_path, filesystem_error);
    if (filesystem_error) {
        throw fs::filesystem_error(
            "Could not determine upload source size", local_path, filesystem_error);
    }
    if (file_size > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("XBDM cannot upload a file larger than 4 GiB");
    }
    if (file_size > options.maximum_file_size) {
        throw ProtocolError(
            "Local file is " + std::to_string(file_size) +
            " bytes, over the configured upload limit of " +
            std::to_string(options.maximum_file_size));
    }

    TransferResult result;
    if (report_file_progress(
            options,
            TransferPhase::transferring,
            local_path,
            remote_path,
            0,
            file_size) == TransferControl::cancel) {
        result.cancelled = true;
        return result;
    }

    std::ifstream input(local_path, std::ios::binary);
    if (!input) {
        throw fs::filesystem_error(
            "Could not open the upload source",
            local_path,
            std::make_error_code(std::errc::io_error));
    }

    const auto quoted_path =
        detail::quote_xbdm_argument(remote_path, "Remote file path");
    std::ostringstream command;
    command << "sendfile name=" << quoted_path << " length=0x"
            << std::uppercase << std::hex << file_size;

    bool skipped = false;
    bool cancelled = false;
    {
        std::scoped_lock lock(impl_->mutex);
        auto response = impl_->send_command_raw_locked(command.str());
        if (response.status_code == 410) {
            if (options.existing_file == ExistingFilePolicy::skip) {
                skipped = true;
            } else if (options.existing_file == ExistingFilePolicy::overwrite) {
                static_cast<void>(impl_->send_command_locked(
                    "delete name=" + quoted_path));
                response = impl_->send_command_raw_locked(command.str());
            }
        }

        if (!skipped) {
            detail::throw_if_command_failed(response);
            if (response.status_code != 204) {
                throw ProtocolError(
                    "sendfile returned status " +
                    std::to_string(response.status_code) + " instead of 204");
            }

            impl_->binary_transfer_active = true;
            try {
                ByteBuffer chunk(options.chunk_size);
                std::uint64_t transferred = 0;
                while (transferred < file_size) {
                    const auto current_size = static_cast<std::size_t>(
                        std::min<std::uint64_t>(chunk.size(), file_size - transferred));
                    input.read(
                        reinterpret_cast<char*>(chunk.data()),
                        static_cast<std::streamsize>(current_size));
                    if (input.gcount() != static_cast<std::streamsize>(current_size)) {
                        throw fs::filesystem_error(
                            "Upload source changed or could not be read completely",
                            local_path,
                            std::make_error_code(std::errc::io_error));
                    }
                    impl_->connection.send_all(
                        std::span<const std::uint8_t>(chunk).first(current_size));
                    transferred += current_size;
                    if (report_file_progress(
                            options,
                            TransferPhase::transferring,
                            local_path,
                            remote_path,
                            transferred,
                            file_size) == TransferControl::cancel) {
                        cancelled = true;
                        impl_->abandon_binary_transfer_locked();
                        break;
                    }
                }

                if (!cancelled) {
                    const auto final_response = detail::parse_response_line(
                        impl_->connection.read_line());
                    impl_->binary_transfer_active = false;
                    detail::throw_if_command_failed(final_response);
                    if (final_response.status_code != 200) {
                        throw ProtocolError(
                            "sendfile completion returned status " +
                            std::to_string(final_response.status_code) +
                            " instead of 200");
                    }
                }
            } catch (...) {
                if (impl_->binary_transfer_active) {
                    impl_->abandon_binary_transfer_locked();
                }
                throw;
            }
        }
    }

    if (skipped) {
        result.files_skipped = 1;
        result.cancelled = report_file_progress(
            options,
            TransferPhase::file_skipped,
            local_path,
            remote_path,
            0,
            file_size) == TransferControl::cancel;
        return result;
    }
    if (cancelled) {
        result.cancelled = true;
        return result;
    }

    result.files_completed = 1;
    result.bytes_transferred = file_size;
    result.cancelled = report_file_progress(
        options,
        TransferPhase::file_complete,
        local_path,
        remote_path,
        file_size,
        file_size) == TransferControl::cancel;
    return result;
}

std::uint32_t Client::allocate_executable(std::uint32_t size) {
    constexpr std::uint32_t pool_size = 0x10000;
    if (size == 0 || size > pool_size) {
        throw ProtocolError("Executable allocation size must be between 1 and 0x10000 bytes");
    }

    std::string response;
    try {
        response = detail::trim_ascii(
            send_srpc("s360 alloc " + std::to_string(size)));
    } catch (const CommandError& error) {
        if (error.status_code() == 400) {
            throw RpcError("Executable hook pool is full");
        }
        throw;
    }
    auto normalized = response;
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (normalized == "pool_full") {
        throw RpcError("Executable hook pool is full");
    }

    const auto address = detail::parse_hex_u32(response);
    if (address == 0 || (address & 0x3u) != 0) {
        throw ProtocolError("Plugin returned an invalid executable allocation address");
    }
    return address;
}

ExecutablePoolInfo Client::executable_pool_info() {
    constexpr std::uint32_t pool_size = 0x10000;
    const auto response = send_srpc("s360 poolinfo");
    ExecutablePoolInfo result;
    result.used = parse_named_hex_value(response, "used");
    result.free = parse_named_hex_value(response, "free");
    if ((result.used & 0x3u) != 0 || result.used > pool_size ||
        result.free > pool_size || result.used + result.free != pool_size) {
        throw ProtocolError("Plugin returned inconsistent executable pool usage");
    }
    return result;
}

void Client::reset_executable_pool(ExecutablePoolReset) {
    auto response = detail::trim_ascii(send_srpc("s360 poolreset"));
    std::transform(
        response.begin(),
        response.end(),
        response.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (response != "ok") {
        throw ProtocolError("Plugin returned an unexpected pool-reset response");
    }
}

RpcValue Client::call(
    std::uint32_t address,
    ReturnType return_type,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->call_locked(
        address,
        return_type,
        std::span<const RpcArgument>(arguments),
        options);
}

RpcValue Client::call(
    std::string_view module,
    std::uint32_t ordinal,
    ReturnType return_type,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->call_locked(
        0,
        return_type,
        std::span<const RpcArgument>(arguments),
        options,
        detail::ModuleTarget{module, ordinal});
}

std::uint32_t Client::call_uint32(
    std::uint32_t address,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    return std::get<std::uint32_t>(call(
        address, ReturnType::int32, arguments, options));
}

std::uint64_t Client::call_uint64(
    std::uint32_t address,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    return std::get<std::uint64_t>(call(
        address, ReturnType::uint64, arguments, options));
}

float Client::call_float(
    std::uint32_t address,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    return std::get<float>(call(
        address, ReturnType::float32, arguments, options));
}

std::string Client::call_string(
    std::uint32_t address,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    return std::get<std::string>(call(
        address, ReturnType::string, arguments, options));
}

void Client::call_void(
    std::uint32_t address,
    const std::vector<RpcArgument>& arguments,
    CallOptions options) {
    static_cast<void>(call(address, ReturnType::void_, arguments, options));
}

std::uint32_t Client::resolve_function(
    std::string_view module,
    std::uint32_t ordinal) {
    std::scoped_lock lock(impl_->mutex);
    return impl_->resolve_function_locked(module, ordinal);
}

std::string Client::console_name() {
    return send_command("dbgname").message;
}

std::string Client::cpu_key() {
    std::scoped_lock lock(impl_->mutex);
    std::string value;
    if (impl_->detect_protocol_locked() == Protocol::jrpc2) {
        value = detail::trim_ascii(impl_->jrpc_call_locked(
            jrpc_cpu_key,
            0,
            std::span<const RpcArgument>{}));
    } else {
        value = detail::trim_ascii(impl_->send_srpc_locked("s360 cpukey"));
    }

    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    if (value.size() != 32 || !std::all_of(
            value.begin(), value.end(), [](unsigned char character) {
                return std::isxdigit(character) != 0;
            })) {
        throw ProtocolError("Console returned an invalid CPU key");
    }
    return value;
}

std::string Client::gamertag() {
    std::scoped_lock lock(impl_->mutex);
    return detail::trim_ascii(impl_->send_srpc_locked("s360 gamertag"));
}

std::uint32_t Client::title_id() {
    std::scoped_lock lock(impl_->mutex);
    return detail::parse_hex_u32(impl_->send_srpc_locked("s360 titleid"));
}

std::uint16_t Client::kernel_version() {
    std::scoped_lock lock(impl_->mutex);
    const auto value = detail::parse_decimal_u32(
        impl_->send_srpc_locked("s360 kernel"));
    if (value > std::numeric_limits<std::uint16_t>::max()) {
        throw ProtocolError("Kernel version is outside the 16-bit range");
    }
    return static_cast<std::uint16_t>(value);
}

std::string Client::motherboard_type() {
    std::scoped_lock lock(impl_->mutex);
    return impl_->send_srpc_locked("s360 motherboard");
}

float Client::temperature(TemperatureSensor sensor) {
    std::scoped_lock lock(impl_->mutex);
    const auto value = detail::parse_float(impl_->send_srpc_locked(
        "s360 temperature " +
        std::to_string(static_cast<std::uint8_t>(sensor))));
    if (value == -2.0F) {
        throw RpcError("Invalid temperature sensor");
    }
    if (value < 0.0F) {
        throw RpcError("Console did not report a temperature");
    }
    return value;
}

void Client::reboot() {
    std::scoped_lock lock(impl_->mutex);
    impl_->send_command_no_reply_locked("magicboot cold");
}

void Client::shutdown() {
    std::scoped_lock lock(impl_->mutex);
    if (impl_->detect_protocol_locked() == Protocol::native_srpc) {
        impl_->send_command_no_reply_locked("s360 shutdown");
        return;
    }

    const auto command = detail::build_jrpc_command(
        jrpc_shutdown,
        0,
        std::span<const RpcArgument>{});
    impl_->send_command_no_reply_locked(command);
}

void Client::launch_xex(
    std::string_view title_path_value,
    std::string_view working_directory) {
    if (title_path_value.empty()) {
        throw ProtocolError("Title path cannot be empty");
    }
    if (working_directory.empty()) {
        throw ProtocolError("Working directory cannot be empty");
    }

    const auto command =
        "magicboot title=" +
        detail::quote_xbdm_argument(title_path_value, "Title path") +
        " directory=" +
        detail::quote_xbdm_argument(working_directory, "Working directory");
    std::scoped_lock lock(impl_->mutex);
    impl_->send_command_no_reply_locked(command);
}

void Client::constant_memory_set(
    std::uint32_t address,
    std::uint32_t value,
    std::optional<std::uint32_t> if_value,
    std::optional<std::uint32_t> title_id_value) {
    std::scoped_lock lock(impl_->mutex);
    static_cast<void>(impl_->detect_protocol_locked());
    const std::vector<RpcArgument> arguments{
        value,
        static_cast<std::uint32_t>(if_value.has_value()),
        if_value.value_or(0),
        static_cast<std::uint32_t>(title_id_value.has_value()),
        title_id_value.value_or(0),
    };
    static_cast<void>(impl_->jrpc_call_locked(
        jrpc_constant_memory_set,
        address,
        std::span<const RpcArgument>(arguments)));
}

void Client::notify(std::string_view text, std::uint32_t type) {
    validate_text(text, "Notification text");
    std::scoped_lock lock(impl_->mutex);
    static_cast<void>(impl_->detect_protocol_locked());
    const std::vector<RpcArgument> arguments{std::string(text), type};
    static_cast<void>(impl_->jrpc_call_locked(
        jrpc_xnotify,
        0,
        std::span<const RpcArgument>(arguments)));
}

void Client::notify(std::string_view text, NotificationType type) {
    notify(text, static_cast<std::uint32_t>(type));
}

void Client::set_leds(LedColor q1, LedColor q2, LedColor q3, LedColor q4) {
    const auto first = static_cast<std::uint32_t>(q1);
    const auto second = static_cast<std::uint32_t>(q2);
    const auto third = static_cast<std::uint32_t>(q3);
    const auto fourth = static_cast<std::uint32_t>(q4);

    std::scoped_lock lock(impl_->mutex);
    if (impl_->detect_protocol_locked() == Protocol::jrpc2) {
        const std::vector<RpcArgument> arguments{first, second, third, fourth};
        static_cast<void>(impl_->jrpc_call_locked(
            jrpc_set_leds,
            0,
            std::span<const RpcArgument>(arguments)));
        return;
    }

    static_cast<void>(impl_->send_srpc_locked(
        "s360 led " + std::to_string(first) + " " + std::to_string(second) +
        " " + std::to_string(third) + " " + std::to_string(fourth)));
}

} // namespace srpc
