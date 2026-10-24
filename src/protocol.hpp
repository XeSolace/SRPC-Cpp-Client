#pragma once

#include "srpc/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace srpc::detail {

inline constexpr std::size_t max_command_length = 480;
inline constexpr std::size_t setmem_chunk_size = 128;

struct ModuleTarget {
    std::string_view module;
    std::uint32_t ordinal;
};

[[nodiscard]] std::string trim_ascii(std::string_view text);
[[nodiscard]] Response parse_response_line(std::string_view line);
void validate_command(std::string_view command);
[[nodiscard]] std::string quote_xbdm_argument(
    std::string_view value,
    std::string_view description);
void throw_if_command_failed(const Response& response);

[[nodiscard]] std::string normalize_srpc_command(std::string_view command);
[[nodiscard]] std::string hex_encode(
    std::span<const std::uint8_t> bytes,
    bool uppercase = true);
[[nodiscard]] ByteBuffer hex_decode(std::string_view text);

[[nodiscard]] std::string build_jrpc_command(
    std::uint32_t type,
    std::uint32_t address,
    std::span<const RpcArgument> arguments,
    CallOptions options = {},
    std::optional<ModuleTarget> target = std::nullopt);

[[nodiscard]] std::string build_jrpc_poll_command(std::uint32_t buffer_address);
[[nodiscard]] std::optional<std::uint32_t> parse_jrpc_pending_address(
    std::string_view response);
void throw_if_jrpc_failed(std::string_view response);
[[nodiscard]] RpcValue parse_jrpc_result(
    std::string_view response,
    ReturnType return_type);

[[nodiscard]] std::string build_native_call(
    std::uint32_t address,
    std::span<const RpcArgument> arguments);
[[nodiscard]] std::uint32_t parse_native_word(std::string_view response);

[[nodiscard]] std::uint32_t parse_hex_u32(std::string_view text);
[[nodiscard]] std::uint64_t parse_hex_u64(std::string_view text);
[[nodiscard]] std::uint32_t parse_decimal_u32(std::string_view text);
[[nodiscard]] float parse_float(std::string_view text);

} // namespace srpc::detail
