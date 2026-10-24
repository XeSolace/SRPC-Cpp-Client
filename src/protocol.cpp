#include "protocol.hpp"

#include "srpc/error.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <type_traits>
#include <utility>

namespace srpc::detail {
namespace {

template <typename... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

bool is_ascii_space(char value) {
    return std::isspace(static_cast<unsigned char>(value)) != 0;
}

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

template <typename T>
T parse_unsigned(std::string_view text, int base, std::string_view description) {
    auto cleaned = trim_ascii(text);
    if (base == 16 && cleaned.size() >= 2 && cleaned[0] == '0' &&
        (cleaned[1] == 'x' || cleaned[1] == 'X')) {
        cleaned.erase(0, 2);
    }
    if (cleaned.empty()) {
        throw ProtocolError(std::string("Missing ") + std::string(description));
    }

    T value{};
    const auto* begin = cleaned.data();
    const auto* end = begin + cleaned.size();
    const auto result = std::from_chars(begin, end, value, base);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw ProtocolError(
            std::string("Invalid ") + std::string(description) + ": " + cleaned);
    }
    return value;
}

std::string format_float(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10)
           << std::defaultfloat << value;
    return stream.str();
}

std::int64_t signed_bits(std::uint64_t value) {
    return std::bit_cast<std::int64_t>(value);
}

std::string encode_jrpc_argument(const RpcArgument& argument) {
    return std::visit(
        Overloaded{
            [](bool value) {
                return std::string("1\\") + (value ? "1\\" : "0\\");
            },
            [](std::int32_t value) {
                return std::string("1\\") +
                    std::to_string(static_cast<std::uint32_t>(value)) + "\\";
            },
            [](std::uint32_t value) {
                return std::string("1\\") + std::to_string(value) + "\\";
            },
            [](std::int64_t value) {
                return std::string("8\\") + std::to_string(value) + "\\";
            },
            [](std::uint64_t value) {
                return std::string("8\\") + std::to_string(signed_bits(value)) + "\\";
            },
            [](float value) {
                return std::string("3\\") + format_float(value) + "\\";
            },
            [](const std::string& value) {
                const auto bytes = std::span(
                    reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
                return std::string("2/") + std::to_string(value.size()) + "\\" +
                    hex_encode(bytes) + "\\";
            },
            [](const ByteBuffer& value) {
                return std::string("7/") + std::to_string(value.size()) + "\\" +
                    hex_encode(value) + "\\";
            },
        },
        argument.storage());
}

std::string encode_native_argument(const RpcArgument& argument) {
    return std::visit(
        Overloaded{
            [](bool value) { return value ? std::string("1") : std::string("0"); },
            [](std::int32_t value) {
                return std::to_string(static_cast<std::uint32_t>(value));
            },
            [](std::uint32_t value) { return std::to_string(value); },
            [](std::int64_t) -> std::string {
                throw ProtocolError("Native SRPC only supports 32-bit integer arguments");
            },
            [](std::uint64_t) -> std::string {
                throw ProtocolError("Native SRPC only supports 32-bit integer arguments");
            },
            [](float value) {
                const auto bits = std::bit_cast<std::uint32_t>(value);
                std::ostringstream stream;
                stream << "flt:" << std::uppercase << std::hex << std::setw(8)
                       << std::setfill('0') << bits;
                return stream.str();
            },
            [](const std::string& value) {
                const auto bytes = std::span(
                    reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
                return std::string("str:") + hex_encode(bytes);
            },
            [](const ByteBuffer& value) {
                return std::string("str:") + hex_encode(value);
            },
        },
        argument.storage());
}

std::vector<std::string> split_array_response(std::string_view response) {
    std::vector<std::string> values;
    std::size_t start = 0;
    while (start < response.size()) {
        const auto separator = response.find_first_of(",;", start);
        const auto end = separator == std::string_view::npos ? response.size() : separator;
        auto value = trim_ascii(response.substr(start, end - start));
        if (!value.empty()) {
            values.push_back(std::move(value));
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return values;
}

} // namespace

std::string trim_ascii(std::string_view text) {
    while (!text.empty() && is_ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

Response parse_response_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.size() < 4 || !std::isdigit(static_cast<unsigned char>(line[0])) ||
        !std::isdigit(static_cast<unsigned char>(line[1])) ||
        !std::isdigit(static_cast<unsigned char>(line[2])) ||
        (line[3] != '-' && line[3] != ' ')) {
        throw ProtocolError("Malformed XBDM response line: " + std::string(line));
    }

    const int status =
        (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    return Response{status, trim_ascii(line.substr(4))};
}

void validate_command(std::string_view command) {
    if (command.empty()) {
        throw ProtocolError("Command cannot be empty");
    }
    if (command.size() > max_command_length) {
        throw ProtocolError(
            "Command is " + std::to_string(command.size()) +
            " characters; XBDM's safe limit is " +
            std::to_string(max_command_length));
    }
    if (command.find_first_of("\r\n\0", 0, 3) != std::string_view::npos) {
        throw ProtocolError("Commands cannot contain CR, LF, or NUL characters");
    }
}

std::string quote_xbdm_argument(
    std::string_view value,
    std::string_view description) {
    if (value.find_first_of("\"\r\n\0", 0, 4) != std::string_view::npos) {
        throw ProtocolError(
            std::string(description) +
            " cannot contain a quote, CR, LF, or NUL character");
    }
    return "\"" + std::string(value) + "\"";
}

void throw_if_command_failed(const Response& response) {
    if (!response.ok()) {
        throw CommandError(response.status_code, response.message);
    }
}

std::string normalize_srpc_command(std::string_view command) {
    auto cleaned = trim_ascii(command);
    if (cleaned.empty()) {
        return "s360";
    }

    const bool has_prefix = cleaned.size() >= 4 &&
        std::tolower(static_cast<unsigned char>(cleaned[0])) == 's' &&
        cleaned[1] == '3' && cleaned[2] == '6' && cleaned[3] == '0' &&
        (cleaned.size() == 4 || is_ascii_space(cleaned[4]));
    if (!has_prefix) {
        cleaned.insert(0, "s360 ");
    }
    validate_command(cleaned);
    return cleaned;
}

std::string hex_encode(std::span<const std::uint8_t> bytes, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        result[index * 2] = digits[bytes[index] >> 4];
        result[index * 2 + 1] = digits[bytes[index] & 0x0F];
    }
    return result;
}

ByteBuffer hex_decode(std::string_view text) {
    std::string compact;
    compact.reserve(text.size());
    for (const char value : text) {
        if (!is_ascii_space(value)) {
            compact.push_back(value);
        }
    }
    if ((compact.size() % 2) != 0) {
        throw ProtocolError("Hex response has an odd number of digits");
    }

    ByteBuffer result(compact.size() / 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        const int high = hex_nibble(compact[index * 2]);
        const int low = hex_nibble(compact[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw ProtocolError("Hex response contains a non-hexadecimal character");
        }
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return result;
}

std::string build_jrpc_command(
    std::uint32_t type,
    std::uint32_t address,
    std::span<const RpcArgument> arguments,
    CallOptions options,
    std::optional<ModuleTarget> target) {
    if (arguments.size() > 37) {
        throw ProtocolError("JRPC2 supports at most 37 arguments");
    }
    if (target.has_value()) {
        validate_command(target->module);
        if (target->module.find('"') != std::string_view::npos) {
            throw ProtocolError("JRPC2 module names cannot contain quotes");
        }
    }

    std::ostringstream command;
    command.imbue(std::locale::classic());
    command << "consolefeatures ver=2 type=" << type;
    if (options.system_thread) {
        command << " system";
    }
    if (target.has_value()) {
        command << " module=\"" << target->module << "\" ord=" << target->ordinal;
    }
    if (options.virtual_machine) {
        command << " VM";
    }
    command << " as=" << options.array_size;
    command << " params=\"A\\" << std::uppercase << std::hex << address
            << std::dec << "\\A\\" << arguments.size() << "\\";
    for (const auto& argument : arguments) {
        command << encode_jrpc_argument(argument);
    }
    command << '"';

    auto result = command.str();
    validate_command(result);
    return result;
}

std::string build_jrpc_poll_command(std::uint32_t buffer_address) {
    std::ostringstream command;
    command << "consolefeatures buf_addr=0x" << std::uppercase << std::hex
            << std::setw(8) << std::setfill('0') << buffer_address;
    return command.str();
}

std::optional<std::uint32_t> parse_jrpc_pending_address(std::string_view response) {
    constexpr std::string_view marker = "buf_addr=";
    const auto marker_index = response.find(marker);
    if (marker_index == std::string_view::npos) {
        return std::nullopt;
    }

    auto value = response.substr(marker_index + marker.size());
    const auto end = value.find_first_of(" \t\r\n");
    if (end != std::string_view::npos) {
        value = value.substr(0, end);
    }
    return parse_hex_u32(value);
}

void throw_if_jrpc_failed(std::string_view response) {
    const auto cleaned = trim_ascii(response);
    constexpr std::string_view marker = "error=";
    if (cleaned.starts_with(marker)) {
        throw RpcError(trim_ascii(std::string_view(cleaned).substr(marker.size())));
    }
}

RpcValue parse_jrpc_result(std::string_view response, ReturnType return_type) {
    throw_if_jrpc_failed(response);
    const auto cleaned = trim_ascii(response);

    switch (return_type) {
    case ReturnType::void_:
        return std::monostate{};
    case ReturnType::int32:
    case ReturnType::byte:
        return parse_hex_u32(cleaned);
    case ReturnType::uint64:
        return parse_hex_u64(cleaned);
    case ReturnType::float32:
        return parse_float(cleaned);
    case ReturnType::string:
        return cleaned;
    case ReturnType::int32_array: {
        std::vector<std::uint32_t> result;
        for (const auto& item : split_array_response(cleaned)) {
            result.push_back(parse_hex_u32(item));
        }
        return result;
    }
    case ReturnType::float_array: {
        std::vector<float> result;
        for (const auto& item : split_array_response(cleaned)) {
            result.push_back(parse_float(item));
        }
        return result;
    }
    case ReturnType::byte_array: {
        ByteBuffer result;
        for (const auto& item : split_array_response(cleaned)) {
            const auto value = parse_decimal_u32(item);
            if (value > std::numeric_limits<std::uint8_t>::max()) {
                throw ProtocolError("JRPC2 byte-array element is outside 0..255");
            }
            result.push_back(static_cast<std::uint8_t>(value));
        }
        return result;
    }
    }
    throw ProtocolError("Unknown JRPC2 return type");
}

std::string build_native_call(
    std::uint32_t address,
    std::span<const RpcArgument> arguments) {
    std::ostringstream command;
    command << "s360 " << std::uppercase << std::hex << std::setw(8)
            << std::setfill('0') << address;
    for (const auto& argument : arguments) {
        command << ' ' << encode_native_argument(argument);
    }
    auto result = command.str();
    validate_command(result);
    return result;
}

std::uint32_t parse_native_word(std::string_view response) {
    const auto cleaned = trim_ascii(response);
    if (cleaned == "idle") {
        return 0;
    }
    if (cleaned == "pending") {
        throw ProtocolError("Native SRPC call is still pending");
    }
    constexpr std::string_view error_marker = "err=";
    if (cleaned.starts_with(error_marker)) {
        const auto code = parse_hex_u32(
            std::string_view(cleaned).substr(error_marker.size()));
        std::ostringstream message;
        message << "Remote function returned error 0x" << std::uppercase << std::hex
                << std::setw(8) << std::setfill('0') << code;
        throw RpcError(message.str());
    }

    // SRPC formats the r3 word as hexadecimal. Parsing digit-only values as
    // decimal (as the Python reference did) corrupts values such as 00000010.
    return parse_hex_u32(cleaned);
}

std::uint32_t parse_hex_u32(std::string_view text) {
    return parse_unsigned<std::uint32_t>(text, 16, "32-bit hexadecimal value");
}

std::uint64_t parse_hex_u64(std::string_view text) {
    return parse_unsigned<std::uint64_t>(text, 16, "64-bit hexadecimal value");
}

std::uint32_t parse_decimal_u32(std::string_view text) {
    return parse_unsigned<std::uint32_t>(text, 10, "unsigned decimal value");
}

float parse_float(std::string_view text) {
    const auto cleaned = trim_ascii(text);
    if (cleaned.empty()) {
        throw ProtocolError("Missing floating-point value");
    }

    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(cleaned.c_str(), &end);
    if (errno == ERANGE || end != cleaned.data() + cleaned.size() ||
        !std::isfinite(value)) {
        throw ProtocolError("Invalid floating-point value: " + cleaned);
    }
    return value;
}

} // namespace srpc::detail
