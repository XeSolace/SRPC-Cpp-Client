#include "debug_protocol.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace srpc {

std::string_view to_string(BreakType type) {
    switch (type) {
    case BreakType::read:    return "read";
    case BreakType::write:   return "write";
    case BreakType::execute: return "execute";
    }
    return "read";
}

std::string_view to_string(DebugEventType type) {
    switch (type) {
    case DebugEventType::execution:    return "execution";
    case DebugEventType::breakpoint:   return "break";
    case DebugEventType::data_break:   return "databreak";
    case DebugEventType::single_step:  return "singlestep";
    case DebugEventType::exception:    return "exception";
    case DebugEventType::assertion:    return "assert";
    case DebugEventType::rip:          return "rip";
    case DebugEventType::debug_string: return "debugstr";
    case DebugEventType::unknown:      break;
    }
    return "unknown";
}

double PpcContext::float_arg(std::size_t index) const {
    if (index >= 8) return 0.0;
    double value = 0.0;
    const std::uint64_t bits = fpr[1 + index];
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool DebugEvent::halts_title() const {
    switch (type) {
    case DebugEventType::breakpoint:
    case DebugEventType::data_break:
    case DebugEventType::single_step:
    case DebugEventType::exception:
    case DebugEventType::assertion:
    case DebugEventType::rip:
        return true;
    // A debug string or an execution-state change is informational; the title
    // is not waiting on anything.
    case DebugEventType::debug_string:
    case DebugEventType::execution:
    case DebugEventType::unknown:
        break;
    }
    return false;
}

namespace detail {
namespace {

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lower = [](char c) {
            return static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));
        };
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

std::string to_hex(std::uint64_t value, int digits) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(static_cast<std::size_t>(digits), '0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xF];
        value >>= 4;
    }
    return out;
}

std::string hex32(std::uint32_t value) { return "0x" + to_hex(value, 8); }
std::string hex64(std::uint64_t value) { return "0q" + to_hex(value, 16); }

} // namespace

// --- key=value lines ------------------------------------------------------

std::optional<std::uint64_t> parse_xbdm_number(std::string_view text) {
    if (text.empty()) return std::nullopt;

    int base = 16;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'q' || text[1] == 'Q')) {
        // XBDM's marker for a 64-bit quantity. Easy to mistake for 0x.
        text.remove_prefix(2);
    } else if (std::all_of(text.begin(), text.end(), [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)) != 0;
               })) {
        // A bare run of decimal digits is decimal - SIZE=4 and similar.
        base = 10;
    }

    if (text.empty()) return std::nullopt;

    std::uint64_t value = 0;
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value, base);
    if (result.ec != std::errc{} || result.ptr != last) return std::nullopt;
    return value;
}

bool KeyValues::has(std::string_view key) const {
    return value(key).has_value();
}

std::optional<std::string_view> KeyValues::value(std::string_view key) const {
    for (const auto& [name, text] : pairs) {
        if (equals_ignore_case(name, key)) return std::string_view(text);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> KeyValues::number(std::string_view key) const {
    const auto text = value(key);
    if (!text) return std::nullopt;
    return parse_xbdm_number(*text);
}

std::uint32_t KeyValues::number_or(std::string_view key, std::uint32_t fallback) const {
    const auto parsed = number(key);
    return parsed ? static_cast<std::uint32_t>(*parsed) : fallback;
}

KeyValues parse_key_values(std::string_view line) {
    KeyValues out;

    std::size_t index = 0;
    const auto skip_spaces = [&] {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) ++index;
    };

    skip_spaces();

    // A leading bare word - the event name - only when it is not itself a key.
    if (index < line.size()) {
        const std::size_t start = index;
        std::size_t scan = index;
        while (scan < line.size() && line[scan] != ' ' && line[scan] != '=') ++scan;
        if (scan >= line.size() || line[scan] != '=') {
            out.leading = std::string(line.substr(start, scan - start));
            index = scan;
        }
    }

    while (index < line.size()) {
        skip_spaces();
        if (index >= line.size()) break;

        const std::size_t key_start = index;
        while (index < line.size() && line[index] != '=' && line[index] != ' ') ++index;
        if (index >= line.size() || line[index] != '=') {
            // A bare word among the pairs: keep it as a valueless key so
            // callers can still test for flags like CLEAR.
            if (index > key_start) {
                out.pairs.emplace_back(
                    std::string(line.substr(key_start, index - key_start)), std::string());
            }
            continue;
        }

        std::string key(line.substr(key_start, index - key_start));
        ++index;  // '='

        std::string value;
        if (index < line.size() && line[index] == '"') {
            ++index;
            const std::size_t value_start = index;
            while (index < line.size() && line[index] != '"') ++index;
            value = std::string(line.substr(value_start, index - value_start));
            if (index < line.size()) ++index;  // closing quote
        } else {
            const std::size_t value_start = index;
            while (index < line.size() && line[index] != ' ') ++index;
            value = std::string(line.substr(value_start, index - value_start));
        }

        out.pairs.emplace_back(std::move(key), std::move(value));
    }

    return out;
}

// --- command builders -----------------------------------------------------

std::string build_set_breakpoint(std::uint32_t address) {
    return "break addr=" + hex32(address);
}

std::string build_clear_breakpoint(std::uint32_t address) {
    return "break addr=" + hex32(address) + " clear";
}

std::string build_is_breakpoint(std::uint32_t address) {
    return "isbreak addr=" + hex32(address);
}

std::string build_data_breakpoint(
    std::uint32_t address, std::uint32_t size, BreakType type, bool clear) {
    // BREAK %s=0x%08x SIZE=%lu %s - the access type is the key itself.
    std::string command = "break ";
    command += to_string(type);
    command += "=" + hex32(address);
    command += " size=" + std::to_string(size);
    if (clear) command += " clear";
    return command;
}

std::string build_thread_command(std::string_view verb, std::uint32_t thread_id) {
    std::string command(verb);
    command += " thread=" + hex32(thread_id);
    return command;
}

std::string build_continue(std::uint32_t thread_id, bool deliver_to_title) {
    std::string command = build_thread_command("continue", thread_id);
    // Without EXCEPTION the debugger consumes the exception, which is the
    // Windows DBG_CONTINUE case. With it, the title's own handler runs - and
    // for a breakpoint that means crashing it.
    if (deliver_to_title) command += " exception";
    return command;
}

std::string build_get_context(std::uint32_t thread_id, ContextFlags flags) {
    std::string command = build_thread_command("getcontext", thread_id);
    if (flags.control) command += " control";
    if (flags.integer) command += " int";
    if (flags.floating) command += " fp";
    if (flags.vector) command += " vr";
    return command;
}

std::string build_set_context(
    std::uint32_t thread_id, const PpcContext& context, ContextFlags flags) {
    std::string command = build_thread_command("setcontext", thread_id);

    if (flags.control) {
        command += " Iar=" + hex32(context.iar);
        command += " Msr=" + hex32(context.msr);
        command += " Cr=" + hex32(context.cr);
        command += " Xer=" + hex32(context.xer);
        command += " Lr=" + hex32(context.lr);
        command += " Ctr=" + hex64(context.ctr);
    }

    if (flags.integer) {
        for (std::size_t i = 0; i < context.gpr.size(); ++i) {
            command += " Gpr" + std::to_string(i) + "=" + hex64(context.gpr[i]);
        }
    }

    if (flags.floating) {
        for (std::size_t i = 0; i < context.fpr.size(); ++i) {
            command += " Fpr" + std::to_string(i) + "=" + hex64(context.fpr[i]);
        }
        command += " Fpscr=" + hex64(context.fpscr);
    }

    return command;
}

// --- reply parsers --------------------------------------------------------

namespace {

// "Gpr12" -> 12. Returns false for anything else.
bool indexed_register(std::string_view key, std::string_view prefix, std::size_t& outIndex) {
    if (key.size() <= prefix.size()) return false;
    if (!equals_ignore_case(key.substr(0, prefix.size()), prefix)) return false;

    const std::string_view digits = key.substr(prefix.size());
    if (digits.empty()) return false;
    if (!std::all_of(digits.begin(), digits.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        })) {
        return false;
    }

    std::size_t value = 0;
    const auto result =
        std::from_chars(digits.data(), digits.data() + digits.size(), value, 10);
    if (result.ec != std::errc{}) return false;
    outIndex = value;
    return true;
}

} // namespace

PpcContext parse_context(const std::vector<std::string>& lines) {
    PpcContext context;

    for (const std::string& line : lines) {
        const KeyValues fields = parse_key_values(line);

        for (const auto& [key, text] : fields.pairs) {
            const auto parsed = parse_xbdm_number(text);
            if (!parsed) continue;
            const std::uint64_t value = *parsed;

            std::size_t index = 0;
            if (indexed_register(key, "Gpr", index)) {
                if (index < context.gpr.size()) context.gpr[index] = value;
                context.valid = true;
                continue;
            }
            if (indexed_register(key, "Fpr", index)) {
                if (index < context.fpr.size()) context.fpr[index] = value;
                context.valid = true;
                continue;
            }
            // Vr/Vscr are comma-separated quads; parse_xbdm_number rejects
            // them, so they fall out here rather than being half-decoded.

            const auto assign32 = [&](std::string_view name, std::uint32_t& target) {
                if (!equals_ignore_case(key, name)) return false;
                target = static_cast<std::uint32_t>(value);
                context.valid = true;
                return true;
            };

            if (assign32("Iar", context.iar)) continue;
            if (assign32("Msr", context.msr)) continue;
            if (assign32("Cr", context.cr)) continue;
            if (assign32("Xer", context.xer)) continue;
            if (assign32("Lr", context.lr)) continue;

            if (equals_ignore_case(key, "Ctr")) {
                context.ctr = value;
                context.valid = true;
            } else if (equals_ignore_case(key, "Fpscr")) {
                context.fpscr = value;
                context.valid = true;
            }
        }
    }

    return context;
}

std::vector<std::uint32_t> parse_thread_ids(const std::vector<std::string>& lines) {
    std::vector<std::uint32_t> ids;
    for (const std::string& line : lines) {
        const std::string trimmed = trim_ascii(line);
        if (trimmed.empty()) continue;

        // THREADS answers with one bare id per line, and a real console prints
        // them as *signed decimal*: thread 0xFB000040 comes across the wire as
        // "-83886016". Parsing that as hex silently yields nothing, which shows
        // up as a console with zero threads.
        if (trimmed.front() == '-') {
            std::int64_t signed_value = 0;
            const char* first = trimmed.data();
            const char* last = trimmed.data() + trimmed.size();
            const auto result = std::from_chars(first, last, signed_value, 10);
            if (result.ec == std::errc{} && result.ptr == last) {
                ids.push_back(static_cast<std::uint32_t>(
                    static_cast<std::uint64_t>(signed_value) & 0xFFFFFFFFull));
            }
            continue;
        }

        const auto parsed = parse_xbdm_number(trimmed);
        if (parsed) ids.push_back(static_cast<std::uint32_t>(*parsed));
    }
    return ids;
}

ThreadInfo parse_thread_info(std::string_view line) {
    const KeyValues fields = parse_key_values(line);

    ThreadInfo info;
    info.id = fields.number_or("thread", 0);
    info.suspend_count = fields.number_or("suspend", 0);
    info.priority = fields.number_or("priority", 0);
    info.tls_base = fields.number_or("tlsbase", 0);
    info.stack_base = fields.number_or("base", 0);
    info.stack_limit = fields.number_or("limit", 0);
    info.start_address = fields.number_or("start", 0);
    return info;
}

DebugEvent parse_debug_event(std::string_view line) {
    DebugEvent event;
    event.raw = std::string(line);

    const KeyValues fields = parse_key_values(line);
    const std::string_view leading = fields.leading;

    if (equals_ignore_case(leading, "break")) {
        event.type = DebugEventType::breakpoint;
    } else if (equals_ignore_case(leading, "databreak")) {
        event.type = DebugEventType::data_break;
    } else if (equals_ignore_case(leading, "singlestep")) {
        event.type = DebugEventType::single_step;
    } else if (equals_ignore_case(leading, "exception")) {
        event.type = DebugEventType::exception;
    } else if (equals_ignore_case(leading, "assert")
               || equals_ignore_case(leading, "ntassert")) {
        event.type = DebugEventType::assertion;
    } else if (equals_ignore_case(leading, "rip")) {
        event.type = DebugEventType::rip;
    } else if (equals_ignore_case(leading, "debugstr")) {
        event.type = DebugEventType::debug_string;
    } else if (equals_ignore_case(leading, "execution")) {
        event.type = DebugEventType::execution;
    }

    event.thread_id = fields.number_or("thread", 0);
    event.address = fields.number_or("addr", 0);
    event.exception_code = fields.number_or("code", 0);
    event.first_chance = fields.has("first");

    if (const auto text = fields.value("string")) event.text = std::string(*text);

    // A data break names its access type as a key carrying the address, the
    // same shape the BREAK command uses.
    for (const BreakType type : { BreakType::read, BreakType::write, BreakType::execute }) {
        const auto address = fields.number(to_string(type));
        if (!address) continue;
        event.access = type;
        event.data_address = static_cast<std::uint32_t>(*address);
        if (event.type == DebugEventType::unknown) event.type = DebugEventType::data_break;
        break;
    }

    if (event.data_address == 0) {
        event.data_address = fields.number_or("data", 0);
    }

    // An execution-state line carries its state as a bare word after the
    // event name rather than a key.
    if (event.type == DebugEventType::execution && event.text.empty()) {
        for (const auto& [key, value] : fields.pairs) {
            if (value.empty()) {
                event.text = key;
                break;
            }
        }
    }

    return event;
}

} // namespace detail
} // namespace srpc
