#pragma once

#include "srpc/debug.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Pure command building and reply parsing for the debug layer.
//
// Split out from the session so it can be tested without a console: everything
// here is a string in, a value out.
namespace srpc::detail {

// --- key=value lines ------------------------------------------------------

// XBDM replies and notifications are a leading word (sometimes) followed by
// space-separated key=value pairs. Values may be quoted.
//
// Parsing generically rather than by positional format is deliberate: the
// per-event field sets are not documented anywhere recoverable, only the key
// vocabulary is, so reading whichever keys are present is correct for every
// layout instead of one guessed one.
struct KeyValues {
    std::string leading;  // the first bare word, empty when the line starts with a key
    std::vector<std::pair<std::string, std::string>> pairs;

    [[nodiscard]] bool has(std::string_view key) const;
    [[nodiscard]] std::optional<std::string_view> value(std::string_view key) const;

    // Accepts 0x-prefixed hex, bare hex, and XBDM's 0q 64-bit form.
    [[nodiscard]] std::optional<std::uint64_t> number(std::string_view key) const;
    [[nodiscard]] std::uint32_t number_or(std::string_view key, std::uint32_t fallback) const;
};

[[nodiscard]] KeyValues parse_key_values(std::string_view line);

// XBDM writes 64-bit quantities as 0q%016I64x and 32-bit ones as 0x%x. Both
// forms, and a bare hex run, are accepted.
[[nodiscard]] std::optional<std::uint64_t> parse_xbdm_number(std::string_view text);

// --- command builders -----------------------------------------------------

[[nodiscard]] std::string build_set_breakpoint(std::uint32_t address);
[[nodiscard]] std::string build_clear_breakpoint(std::uint32_t address);
[[nodiscard]] std::string build_is_breakpoint(std::uint32_t address);

[[nodiscard]] std::string build_data_breakpoint(
    std::uint32_t address, std::uint32_t size, BreakType type, bool clear);

[[nodiscard]] std::string build_thread_command(
    std::string_view verb, std::uint32_t thread_id);

[[nodiscard]] std::string build_continue(std::uint32_t thread_id, bool deliver_to_title);

[[nodiscard]] std::string build_get_context(std::uint32_t thread_id, ContextFlags flags);

// One SETCONTEXT command carrying the registers named by `flags`.
[[nodiscard]] std::string build_set_context(
    std::uint32_t thread_id, const PpcContext& context, ContextFlags flags);

// --- reply parsers --------------------------------------------------------

// Accepts the whole GETCONTEXT body, one register per line or several per
// line; both layouts appear depending on which groups were asked for.
[[nodiscard]] PpcContext parse_context(const std::vector<std::string>& lines);

[[nodiscard]] std::vector<std::uint32_t> parse_thread_ids(
    const std::vector<std::string>& lines);

[[nodiscard]] ThreadInfo parse_thread_info(std::string_view line);

// Decodes one notification line into a typed event.
[[nodiscard]] DebugEvent parse_debug_event(std::string_view line);

} // namespace srpc::detail
