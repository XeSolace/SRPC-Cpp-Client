#include "debug_protocol.hpp"

#include "srpc/debug.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}

void expect_equal(const std::string& actual, const std::string& expected,
                  const char* description) {
    if (actual != expected) {
        std::cerr << "FAILED: " << description << "\n  expected: " << expected
                  << "\n  actual:   " << actual << '\n';
        ++failures;
    }
}

void number_tests() {
    using srpc::detail::parse_xbdm_number;

    expect(parse_xbdm_number("0x82A00000").value_or(0) == 0x82A00000ull,
           "0x prefix parses as hex");

    // The one that guessing gets wrong: XBDM writes 64-bit values with 0q.
    expect(parse_xbdm_number("0q00000000deadbeef").value_or(0) == 0xdeadbeefull,
           "0q prefix parses as a 64-bit hex quantity");
    expect(parse_xbdm_number("0qFFFFFFFFFFFFFFFF").value_or(0) == 0xFFFFFFFFFFFFFFFFull,
           "0q parses a full 64-bit value");

    expect(parse_xbdm_number("4").value_or(0) == 4, "a bare digit run is decimal");
    expect(parse_xbdm_number("deadbeef").value_or(0) == 0xdeadbeefull,
           "a bare hex run is hex");
    expect(!parse_xbdm_number("").has_value(), "empty text has no value");
    expect(!parse_xbdm_number("0x").has_value(), "a lone prefix has no value");
    expect(!parse_xbdm_number("0x12,0x34").has_value(),
           "a comma-separated quad is rejected rather than half-parsed");
}

void key_value_tests() {
    using srpc::detail::parse_key_values;

    const auto event = parse_key_values("break addr=0x82123456 thread=0x0000000f");
    expect_equal(event.leading, "break", "leading event word is captured");
    expect(event.number("addr").value_or(0) == 0x82123456ull, "addr is parsed");
    expect(event.number("thread").value_or(0) == 0x0f, "thread is parsed");
    expect(!event.has("size"), "absent keys report absent");

    const auto quoted = parse_key_values("debugstr thread=0x2 string=\"hello world\"");
    expect_equal(std::string(quoted.value("string").value_or("")), "hello world",
                 "quoted values keep their spaces");

    const auto flagged = parse_key_values("break addr=0x1000 clear");
    expect(flagged.has("clear"), "a bare trailing word is kept as a valueless key");

    const auto keyed = parse_key_values("thread=0x5 priority=0x1");
    expect(keyed.leading.empty(), "a line starting with a key has no leading word");
    expect(keyed.number("priority").value_or(99) == 1, "later keys parse");

    const auto mixed = parse_key_values("threadinfo thread=0X0000000A SUSPEND=0x1");
    expect(mixed.number("Thread").value_or(0) == 10, "key lookup is case-insensitive");
    expect(mixed.number("suspend").value_or(0) == 1, "upper-case keys are found");
}

void command_builder_tests() {
    using namespace srpc;
    using namespace srpc::detail;

    expect_equal(build_set_breakpoint(0x82123456), "break addr=0x82123456",
                 "code breakpoint command");
    expect_equal(build_clear_breakpoint(0x82123456), "break addr=0x82123456 clear",
                 "code breakpoint clear command");

    // BREAK %s=0x%08x SIZE=%lu %s - the access type is the key itself.
    expect_equal(build_data_breakpoint(0x82A00000, 4, BreakType::read, false),
                 "break read=0x82a00000 size=4", "read watchpoint command");
    expect_equal(build_data_breakpoint(0x82A00000, 8, BreakType::write, true),
                 "break write=0x82a00000 size=8 clear", "write watchpoint clear command");

    expect_equal(build_continue(0x0f, false), "continue thread=0x0000000f",
                 "continue consumes the exception by default");
    expect_equal(build_continue(0x0f, true), "continue thread=0x0000000f exception",
                 "continue can pass the exception to the title");

    ContextFlags flags;
    flags.control = true;
    flags.integer = true;
    flags.floating = false;
    flags.vector = false;
    expect_equal(build_get_context(0x2, flags), "getcontext thread=0x00000002 control int",
                 "getcontext appends only the requested groups");

    flags.floating = true;
    flags.vector = true;
    expect_equal(build_get_context(0x2, flags),
                 "getcontext thread=0x00000002 control int fp vr",
                 "getcontext appends every requested group");
}

void context_parse_tests() {
    using srpc::detail::parse_context;

    // Shaped exactly like xbdm.dll's own templates: 32-bit control registers
    // as 0x, 64-bit ones as 0q.
    const std::vector<std::string> lines{
        " Iar=0x82123456 Msr=0x0000b030 Cr=0x22004024 Xer=0x20000000 Lr=0x82123400",
        " Ctr=0q0000000000000001",
        " Gpr0=0q0000000000000000 Gpr1=0q000000007000ff90 Gpr3=0q0000000082a00000",
        " Gpr31=0q00000000deadbeef",
        " Fpr1=0q3ff0000000000000 Fpscr=0q0000000082004000",
    };

    const auto context = parse_context(lines);
    expect(context.valid, "a context with registers is valid");
    expect(context.iar == 0x82123456u, "iar is parsed");
    expect(context.msr == 0x0000b030u, "msr is parsed");
    expect(context.lr == 0x82123400u, "lr is parsed");
    expect(context.ctr == 1u, "ctr is parsed from its 0q form");
    expect(context.gpr[1] == 0x7000ff90ull, "gpr1 is parsed");
    expect(context.gpr[3] == 0x82a00000ull, "gpr3 is parsed");
    expect(context.gpr[31] == 0xdeadbeefull, "gpr31 is parsed");
    expect(context.fpscr == 0x82004000ull, "fpscr is parsed");

    // r3 is the first integer argument on PowerPC.
    expect(context.arg(0) == 0x82a00000ull, "arg(0) reads r3");

    // 0x3ff0000000000000 is 1.0 as a double; f1 is the first float argument.
    expect(context.float_arg(0) == 1.0, "float_arg(0) decodes f1");

    const auto empty = parse_context({});
    expect(!empty.valid, "an empty context is not valid");

    // Vector registers are comma-separated quads this client does not decode;
    // they must not corrupt anything else on the line.
    const auto with_vectors = parse_context(
        { " Vr0=0x00000001,0x00000002,0x00000003,0x00000004 Iar=0x82000000" });
    expect(with_vectors.iar == 0x82000000u, "an undecoded Vr does not eat the next key");
}

void thread_parse_tests() {
    using srpc::detail::parse_thread_ids;
    using srpc::detail::parse_thread_info;

    const auto ids = parse_thread_ids({ "0x00000004", "0x0000000f", "", "0x2a" });
    expect(ids.size() == 3, "blank lines are skipped");
    expect(ids[0] == 4 && ids[1] == 15 && ids[2] == 42, "thread ids are parsed");

    // What a real console actually sends: THREADS prints ids as signed decimal,
    // so 0xFB000040 arrives as "-83886016". Parsing those as hex yields nothing
    // and the console looks like it has no threads at all.
    const auto live = parse_thread_ids({ "-83886016", "-83886020", "-83886024" });
    expect(live.size() == 3, "signed decimal thread ids are parsed");
    expect(live[0] == 0xFB000040u, "-83886016 is thread 0xFB000040");
    expect(live[1] == 0xFB00003Cu, "-83886020 is thread 0xFB00003C");

    const auto info = parse_thread_info(
        "thread=0x0000000f suspend=0x0 priority=0x1 tlsbase=0x40000000 "
        "base=0x70000000 limit=0x6ffe0000 start=0x82010000");
    expect(info.id == 15, "thread id is parsed");
    expect(info.priority == 1, "priority is parsed");
    expect(info.tls_base == 0x40000000u, "tls base is parsed");
    expect(info.start_address == 0x82010000u, "start address is parsed");
}

void event_parse_tests() {
    using namespace srpc;
    using srpc::detail::parse_debug_event;

    const auto code_break = parse_debug_event("break addr=0x82123456 thread=0x0000000f");
    expect(code_break.type == DebugEventType::breakpoint, "break decodes as a breakpoint");
    expect(code_break.address == 0x82123456u, "break address is decoded");
    expect(code_break.thread_id == 15u, "break thread is decoded");
    expect(code_break.halts_title(), "a breakpoint halts the title");

    // The exact line a real console sent when a code breakpoint fired: lower
    // case hex, and a bare trailing "stop" word after the pairs.
    const auto live = parse_debug_event("break addr=0x8267b2c8 thread=0xf9000000 stop");
    expect(live.type == srpc::DebugEventType::breakpoint, "the live break line decodes");
    expect(live.address == 0x8267b2c8u, "lower-case hex address decodes");
    expect(live.thread_id == 0xf9000000u, "a high thread id decodes without sign trouble");
    expect(live.halts_title(), "the live break line halts the title");

    const auto data_break =
        parse_debug_event("databreak read=0x82a00000 addr=0x82123456 thread=0x4");
    expect(data_break.type == DebugEventType::data_break, "databreak decodes");
    expect(data_break.access.has_value() && *data_break.access == BreakType::read,
           "the access type is recovered from the key name");
    expect(data_break.data_address == 0x82a00000u, "the watched address is decoded");
    expect(data_break.address == 0x82123456u,
           "the faulting instruction is kept separate from the watched address");
    expect(data_break.halts_title(), "a data break halts the title");

    const auto text = parse_debug_event("debugstr thread=0x3 string=\"assert failed\"");
    expect(text.type == DebugEventType::debug_string, "debugstr decodes");
    expect_equal(text.text, "assert failed", "debug string payload is decoded");
    expect(!text.halts_title(), "a debug string does not halt the title");

    const auto state = parse_debug_event("execution started");
    expect(state.type == DebugEventType::execution, "execution decodes");
    expect_equal(state.text, "started", "the execution state word is captured");
    expect(!state.halts_title(), "an execution notice does not halt the title");

    const auto unknown = parse_debug_event("somethingelse foo=1");
    expect(unknown.type == DebugEventType::unknown, "an unknown event stays unknown");
    expect_equal(unknown.raw, "somethingelse foo=1", "the raw line is always kept");
}

} // namespace

int main() {
    number_tests();
    key_value_tests();
    command_builder_tests();
    context_parse_tests();
    thread_parse_tests();
    event_parse_tests();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All SRPC debug protocol tests passed\n";
    return 0;
}
