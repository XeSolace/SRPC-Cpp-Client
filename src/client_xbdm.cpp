#include "srpc/client.hpp"

#include "protocol.hpp"

#include <chrono>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace srpc {
namespace {

bool is_ascii_space(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

char ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

bool equals_ascii_case_insensitive(
    std::string_view left,
    std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

bool starts_with_ascii_case_insensitive(
    std::string_view value,
    std::string_view prefix) {
    return value.size() >= prefix.size() &&
        equals_ascii_case_insensitive(value.substr(0, prefix.size()), prefix);
}

std::optional<std::string> find_field(
    std::string_view line,
    std::string_view key) {
    bool inside_quotes = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        if (line[index] == '"') {
            inside_quotes = !inside_quotes;
            continue;
        }
        if (inside_quotes ||
            (index != 0 && !is_ascii_space(line[index - 1])) ||
            index + key.size() > line.size() ||
            !equals_ascii_case_insensitive(line.substr(index, key.size()), key)) {
            continue;
        }

        std::size_t cursor = index + key.size();
        while (cursor < line.size() && is_ascii_space(line[cursor])) {
            ++cursor;
        }
        if (cursor == line.size() || line[cursor] != '=') {
            continue;
        }
        ++cursor;
        while (cursor < line.size() && is_ascii_space(line[cursor])) {
            ++cursor;
        }

        if (cursor < line.size() && line[cursor] == '"') {
            const auto end = line.find('"', cursor + 1);
            if (end == std::string_view::npos) {
                throw ProtocolError(
                    "Unterminated quoted " + std::string(key) +
                    " field in XBDM response: " + std::string(line));
            }
            return std::string(line.substr(cursor + 1, end - cursor - 1));
        }

        const auto end = line.find_first_of(" \t\r\n", cursor);
        return std::string(line.substr(
            cursor,
            end == std::string_view::npos ? line.size() - cursor : end - cursor));
    }
    return std::nullopt;
}

bool has_flag_token(std::string_view line, std::string_view flag) {
    bool inside_quotes = false;
    std::size_t token_start = std::string_view::npos;
    for (std::size_t index = 0; index <= line.size(); ++index) {
        const bool at_end = index == line.size();
        const char value = at_end ? ' ' : line[index];

        if (!at_end && value == '"') {
            inside_quotes = !inside_quotes;
        }
        if (!inside_quotes && (at_end || is_ascii_space(value))) {
            if (token_start != std::string_view::npos &&
                equals_ascii_case_insensitive(
                    line.substr(token_start, index - token_start), flag)) {
                return true;
            }
            token_start = std::string_view::npos;
        } else if (token_start == std::string_view::npos) {
            token_start = index;
        }
    }
    return false;
}

std::optional<std::uint64_t> combine_hex_dwords(
    std::string_view line,
    std::string_view high_key,
    std::string_view low_key,
    std::string_view description) {
    const auto high = find_field(line, high_key);
    const auto low = find_field(line, low_key);
    if (high.has_value() != low.has_value()) {
        throw ProtocolError(
            "Incomplete " + std::string(description) +
            " fields in XBDM response: " + std::string(line));
    }
    if (!high) {
        return std::nullopt;
    }

    return (static_cast<std::uint64_t>(detail::parse_hex_u32(*high)) << 32U) |
        detail::parse_hex_u32(*low);
}

std::string require_nonempty(
    std::string value,
    std::string_view description) {
    value = detail::trim_ascii(value);
    if (value.empty()) {
        throw ProtocolError("XBDM did not return " + std::string(description));
    }
    return value;
}

std::string make_load_module_argument(std::string_view remote_path) {
    if (remote_path.empty()) {
        throw ProtocolError("Remote module path cannot be empty");
    }
    if (remote_path.size() >= 260) {
        throw ProtocolError("Remote module path exceeds the console's 259-byte limit");
    }

    bool encode = starts_with_ascii_case_insensitive(remote_path, "str:");
    for (const char value : remote_path) {
        if (is_ascii_space(value)) {
            encode = true;
            break;
        }
    }
    if (!encode) {
        return std::string(remote_path);
    }

    const auto bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(remote_path.data()),
        remote_path.size());
    const auto encoded = std::string("str:") + detail::hex_encode(bytes);
    constexpr std::string_view prefix = "s360 loadmod ";
    if (prefix.size() + encoded.size() > detail::max_command_length) {
        throw ProtocolError(
            "Encoded remote module path exceeds XBDM's safe command length");
    }
    return encoded;
}

} // namespace

std::string Client::console_id() {
    const auto response = send_command("getconsoleid");
    auto value = find_field(response.message, "consoleid")
        .value_or(detail::trim_ascii(response.message));
    value = detail::trim_ascii(value);
    if (starts_with_ascii_case_insensitive(value, "0x")) {
        value.erase(0, 2);
    }
    if (value.empty()) {
        throw ProtocolError("XBDM returned an empty console ID");
    }
    for (const char character : value) {
        if (std::isxdigit(static_cast<unsigned char>(character)) == 0) {
            throw ProtocolError("Invalid console ID returned by XBDM: " + value);
        }
    }
    return value;
}

std::string Client::console_type() {
    const auto response = send_command("consoletype");
    if (const auto value = find_field(response.message, "consoletype")) {
        return require_nonempty(*value, "a console type");
    }
    if (const auto value = find_field(response.message, "type")) {
        return require_nonempty(*value, "a console type");
    }
    return require_nonempty(response.message, "a console type");
}

bool Client::is_devkit() {
    const auto value = detail::trim_ascii(console_type());
    return equals_ascii_case_insensitive(value, "devkit") ||
        equals_ascii_case_insensitive(value, "testkit");
}

std::string Client::title_path() {
    const auto lines = send_multiline_command("xbeinfo name=");
    for (const auto& line : lines) {
        if (const auto name = find_field(line, "name")) {
            return require_nonempty(*name, "a current title path");
        }
    }
    throw ProtocolError("XBDM xbeinfo response did not contain a name field");
}

std::string Client::dm_version() {
    return require_nonempty(send_command("dmversion").message, "a debug monitor version");
}

std::vector<std::string> Client::drives() {
    std::vector<std::string> result;
    for (const auto& line : send_multiline_command("drivelist")) {
        if (const auto name = find_field(line, "drivename"); name && !name->empty()) {
            result.push_back(*name);
        }
    }
    return result;
}

std::vector<std::string> Client::modules() {
    return send_multiline_command("modules");
}

std::vector<ModuleInfo> Client::module_list() {
    std::vector<ModuleInfo> result;

    for (const std::string& line : send_multiline_command("modules")) {
        const std::string trimmed = detail::trim_ascii(line);
        if (trimmed.empty()) {
            continue;
        }

        ModuleInfo info;
        if (const auto name = find_field(trimmed, "name")) {
            info.name = *name;
        }

        const auto number = [&trimmed](std::string_view key) -> std::uint32_t {
            const auto text = find_field(trimmed, key);
            if (!text) {
                return 0;
            }
            try {
                return detail::parse_hex_u32(*text);
            } catch (const Error&) {
                return 0;
            }
        };

        info.base = number("base");
        info.size = number("size");
        info.checksum = number("check");
        info.timestamp = number("timestamp");

        // `dll` is a bare flag word among the key=value pairs, not a field, so
        // it has to be matched as a whole space-delimited token - a substring
        // test would also hit a module whose name contains "dll".
        std::size_t cursor = 0;
        while (cursor < trimmed.size()) {
            const std::size_t start = trimmed.find_first_not_of(" 	", cursor);
            if (start == std::string::npos) {
                break;
            }
            std::size_t stop = trimmed.find_first_of(" 	", start);
            if (stop == std::string::npos) {
                stop = trimmed.size();
            }
            if (trimmed.compare(start, stop - start, "dll") == 0) {
                info.is_dll = true;
                break;
            }
            cursor = stop;
        }

        if (info.base == 0 || info.size == 0) {
            continue;
        }
        result.push_back(std::move(info));
    }

    return result;
}

std::optional<ModuleInfo> Client::title_module() {
    // The title is the one module that is neither a dll nor the kernel. The
    // kernel is not flagged dll either, but it loads below 0x82000000.
    constexpr std::uint32_t title_space_base = 0x82000000;

    for (const ModuleInfo& info : module_list()) {
        if (info.is_dll || info.base < title_space_base) {
            continue;
        }
        return info;
    }
    return std::nullopt;
}

std::uint32_t Client::module_handle(std::string_view module_name) {
    if (module_name.empty()) {
        throw ProtocolError("Module name cannot be empty");
    }
    const auto lookup = resolve_function("xam.xex", 1102);
    const auto handle = call_uint32(
        lookup,
        std::vector<RpcArgument>{std::string(module_name)});
    if (handle == 0) {
        throw RpcError("Module not found: " + std::string(module_name));
    }
    return handle;
}

std::uint32_t Client::process_id() {
    const auto response = send_command("getpid");
    const auto value = find_field(response.message, "pid")
        .value_or(detail::trim_ascii(response.message));
    return detail::parse_hex_u32(value);
}

SignInState Client::sign_in_state(std::uint32_t user_index) {
    if (user_index > 3) {
        throw ProtocolError("Xbox user index must be between 0 and 3");
    }
    const auto address = resolve_function("xam.xex", 528);
    const auto value = call_uint32(
        address,
        std::vector<RpcArgument>{user_index});
    if (value > static_cast<std::uint32_t>(SignInState::guest_account_xbox_live)) {
        throw ProtocolError(
            "Console returned an invalid sign-in state: " + std::to_string(value));
    }
    return static_cast<SignInState>(value);
}

std::vector<DirectoryEntry> Client::directory_contents(
    std::string_view remote_path) {
    const auto quoted = detail::quote_xbdm_argument(remote_path, "Remote path");
    const auto lines = send_multiline_command("dirlist name=" + quoted);

    std::vector<DirectoryEntry> entries;
    entries.reserve(lines.size());
    for (const auto& line : lines) {
        if (line.empty()) {
            continue;
        }
        const auto name = find_field(line, "name");
        if (!name || name->empty()) {
            throw ProtocolError(
                "Directory entry did not contain a valid name: " + line);
        }

        DirectoryEntry entry;
        entry.name = *name;
        if (const auto size = combine_hex_dwords(
                line, "sizehi", "sizelo", "directory entry size")) {
            entry.size = *size;
        }
        entry.created_filetime = combine_hex_dwords(
            line, "createhi", "createlo", "directory entry creation time");
        entry.changed_filetime = combine_hex_dwords(
            line, "changehi", "changelo", "directory entry change time");
        entry.is_directory = has_flag_token(line, "directory");
        entries.push_back(std::move(entry));
    }
    return entries;
}

bool Client::is_directory(std::string_view remote_path) {
    try {
        static_cast<void>(directory_contents(remote_path));
        return true;
    } catch (const CommandError& error) {
        if (error.status_code() == 402) {
            return false;
        }
        throw;
    }
}

void Client::create_directory(std::string_view remote_path) {
    const auto quoted = detail::quote_xbdm_argument(remote_path, "Remote path");
    static_cast<void>(send_command("mkdir name=" + quoted));
}

void Client::delete_file(std::string_view remote_path) {
    const auto quoted = detail::quote_xbdm_argument(remote_path, "Remote path");
    static_cast<void>(send_command("delete name=" + quoted));
}

void Client::delete_directory(std::string_view remote_path) {
    const auto quoted = detail::quote_xbdm_argument(remote_path, "Remote path");
    static_cast<void>(send_command("delete name=" + quoted + " dir"));
}

void Client::rename_path(
    std::string_view old_path,
    std::string_view new_path) {
    const auto quoted_old = detail::quote_xbdm_argument(old_path, "Old remote path");
    const auto quoted_new = detail::quote_xbdm_argument(new_path, "New remote path");
    static_cast<void>(send_command(
        "rename name=" + quoted_old + " newname=" + quoted_new));
}

void Client::debug_go() {
    static_cast<void>(send_command("go"));
}

void Client::debug_stop() {
    static_cast<void>(send_command("stop"));
}

std::string Client::load_module(std::string_view remote_path) {
    return detail::trim_ascii(
        send_srpc("s360 loadmod " + make_load_module_argument(remote_path)));
}

void Client::unload_module(std::string_view module_name) {
    const auto handle = module_handle(module_name);
    if (handle > std::numeric_limits<std::uint32_t>::max() - 0x40U) {
        throw ProtocolError("Module handle cannot be adjusted safely");
    }

    write<std::uint16_t>(handle + 0x40U, 1U, Endian::big);
    const auto unload = resolve_function("xboxkrnl.exe", 417);
    static_cast<void>(call_uint32(
        unload,
        std::vector<RpcArgument>{handle}));
}

void Client::set_system_time(std::chrono::system_clock::time_point value) {
    constexpr std::int64_t unix_to_filetime_seconds = 11'644'473'600LL;
    constexpr std::uint64_t ticks_per_second = 10'000'000ULL;
    constexpr std::int64_t maximum_unix_seconds =
        static_cast<std::int64_t>(
            std::numeric_limits<std::uint64_t>::max() / ticks_per_second) -
        unix_to_filetime_seconds;

    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(value.time_since_epoch())
            .count();
    if (seconds < -unix_to_filetime_seconds || seconds > maximum_unix_seconds) {
        throw ProtocolError("System time is outside the Windows FILETIME range");
    }

    const auto filetime =
        static_cast<std::uint64_t>(
            static_cast<std::int64_t>(seconds) + unix_to_filetime_seconds) *
        ticks_per_second;
    const auto high = static_cast<std::uint32_t>(filetime >> 32U);
    const auto low = static_cast<std::uint32_t>(filetime);

    std::ostringstream command;
    command.imbue(std::locale::classic());
    command << "s360 settime " << std::uppercase << std::hex
            << std::setfill('0') << std::setw(8) << high << ' '
            << std::setw(8) << low;
    static_cast<void>(send_srpc(command.str()));
}

void Client::synchronize_time() {
    set_system_time(std::chrono::system_clock::now());
}

} // namespace srpc
