#include "srpc/debug.hpp"

#include "connection.hpp"
#include "debug_protocol.hpp"
#include "protocol.hpp"
#include "srpc/client.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace srpc {
namespace {

// The notification socket must never time out.
//
// Connection::receive_more() calls close() *before* throwing TimeoutError -
// correct for a request/response exchange, where a timed-out reply leaves the
// stream in an unknown state, but fatal here: an idle notification channel is
// the normal state, and a short timeout silently killed the socket about a
// second after attaching. Breakpoints still armed, so it looked like events
// simply never fired.
//
// srpc clamps a zero timeout up to 1 ms, so "no timeout" has to be spelled as a
// very large one. detach() unblocks the read by closing the socket, not by
// waiting for this to expire.
constexpr std::chrono::milliseconds kNotifyReceiveTimeout{ std::chrono::hours(24) };

} // namespace

struct DebugSession::Impl {
    Impl(std::string host_value, ClientOptions options_value)
        : host(std::move(host_value))
        , options(options_value)
        , command(host, options) {}

    std::string host;
    ClientOptions options;

    // Commands go through a normal Client so the exchange, framing and error
    // handling are the ones already in use everywhere else.
    Client command;
    mutable std::recursive_mutex command_mutex;

    // Notifications need their own socket: the channel is a one-way stream the
    // console pushes to, so a command reply cannot be read from it.
    detail::Connection notify;
    std::thread worker;
    std::atomic<bool> running{ false };
    // Set before the socket is closed, so the worker can tell a deliberate
    // teardown from the channel genuinely dropping and stay quiet about it.
    std::atomic<bool> stopping{ false };
    std::atomic<bool> attached{ false };
    std::atomic<bool> owner{ false };

    DebugEventHandler on_event;
    DebugLogHandler on_log;

    void log(std::string_view message) const {
        if (on_log) on_log(message);
    }

    Response send(std::string_view command_text) {
        std::scoped_lock lock(command_mutex);
        return command.send_command(command_text);
    }

    std::vector<std::string> send_multiline(std::string_view command_text) {
        std::scoped_lock lock(command_mutex);
        return command.send_multiline_command(command_text);
    }

    // Best effort: used on teardown paths where throwing would mask the real
    // problem and leave the console worse off.
    void send_quiet(std::string_view command_text) noexcept {
        try {
            static_cast<void>(send(command_text));
        } catch (...) {
        }
    }

    void notification_loop();
    void start_notifications();
    void stop_notifications() noexcept;
};

void DebugSession::Impl::start_notifications() {
    notify.connect(host, options.port, options.connect_timeout, kNotifyReceiveTimeout);

    // The greeting every XBDM connection opens with.
    const std::string greeting = notify.read_line();
    const Response parsed = detail::parse_response_line(greeting);
    if (!parsed.ok()) {
        throw ProtocolError("Notification channel refused the connection: " + parsed.message);
    }

    // Turn this connection into a notification channel.
    //
    // Verified against a live console: `notify` is answered with
    // "205- now a notification channel", after which the console streams events
    // on this socket. xbdm.dll's own NOTIFYAT/reconnectport forms ask the
    // console to dial *back* to a port we listen on, which a firewall would
    // block; this direction needs no inbound rule.
    const std::string request = "notify\r\n";
    notify.send_all(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(request.data()), request.size()));

    const Response ack = detail::parse_response_line(notify.read_line());
    if (!ack.ok()) {
        notify.close();
        throw ProtocolError("Console refused the notification channel: " + ack.message);
    }

    stopping.store(false);
    running.store(true);
    worker = std::thread([this] { notification_loop(); });
}

void DebugSession::Impl::stop_notifications() noexcept {
    stopping.store(true);
    running.store(false);
    // Closing the socket is what unblocks the worker's read.
    try {
        notify.close();
    } catch (...) {
    }
    if (worker.joinable()) worker.join();
}

void DebugSession::Impl::notification_loop() {
    // Blocks in read_line until an event arrives or the socket is closed by
    // stop_notifications(); the running flag is only a belt-and-braces check.
    while (running.load()) {
        std::string line;
        try {
            line = notify.read_line();
        } catch (const std::exception& error) {
            if (!stopping.load()) {
                log(std::string("Notification channel closed: ") + error.what());
            }
            break;
        }

        if (line.empty()) continue;

        const DebugEvent event = detail::parse_debug_event(detail::trim_ascii(line));
        if (event.type == DebugEventType::unknown) {
            log("Undecoded notification: " + event.raw);
        }

        // The handler runs here, on the worker. It is allowed to issue
        // commands: those take command_mutex and go out on the other socket, so
        // they cannot deadlock against this read.
        if (on_event) {
            try {
                on_event(event);
            } catch (const std::exception& error) {
                log(std::string("Event handler threw: ") + error.what());
            } catch (...) {
                log("Event handler threw");
            }
        }
    }
}

DebugSession::DebugSession(std::string host, ClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(host), options)) {}

DebugSession::~DebugSession() {
    detach();
}

void DebugSession::attach(DebugEventHandler on_event, DebugLogHandler on_log) {
    impl_->on_event = std::move(on_event);
    impl_->on_log = std::move(on_log);

    impl_->command.connect();

    // Registering as the debugger is what makes the console deliver events.
    // ALREADYEXISTS is the usual answer when something else already owns the
    // session - breakpoints still work, notifications go to the other owner.
    try {
        static_cast<void>(impl_->send("debugger connect"));
        impl_->owner.store(true);
    } catch (const CommandError& error) {
        impl_->owner.store(false);
        impl_->log(std::string("Not the debug session owner: ") + error.what());
    }

    try {
        impl_->start_notifications();
    } catch (const std::exception& error) {
        impl_->log(std::string("Notifications unavailable: ") + error.what());
    }

    impl_->attached.store(true);
}

void DebugSession::detach() noexcept {
    if (!impl_ || !impl_->attached.exchange(false)) {
        if (impl_) impl_->stop_notifications();
        return;
    }

    impl_->stop_notifications();

    // The safety property: never leave the console halted. Disarm everything
    // this session could have armed, release every thread waiting on a debug
    // event, and clear the global stop - in that order, and regardless of what
    // failed on the way in.
    impl_->send_quiet("break clearall");
    try {
        static_cast<void>(release_stopped_threads());
    } catch (...) {
    }
    impl_->send_quiet("go");

    if (impl_->owner.load()) {
        impl_->send_quiet("debugger disconnect");
        impl_->owner.store(false);
    }

    impl_->command.close();
}

bool DebugSession::attached() const noexcept {
    return impl_ && impl_->attached.load();
}

bool DebugSession::is_debugger_owner() const noexcept {
    return impl_ && impl_->owner.load();
}

// --- title control --------------------------------------------------------

void DebugSession::stop() { static_cast<void>(impl_->send("stop")); }
void DebugSession::go() { static_cast<void>(impl_->send("go")); }

void DebugSession::continue_thread(std::uint32_t thread_id, bool deliver_to_title) {
    static_cast<void>(impl_->send(detail::build_continue(thread_id, deliver_to_title)));
}

void DebugSession::halt_thread(std::uint32_t thread_id) {
    static_cast<void>(impl_->send(detail::build_thread_command("halt", thread_id)));
}

void DebugSession::resume_thread(std::uint32_t thread_id) {
    static_cast<void>(impl_->send(detail::build_thread_command("resume", thread_id)));
}

bool DebugSession::thread_stopped(std::uint32_t thread_id) {
    try {
        const Response response =
            impl_->send(detail::build_thread_command("isstopped", thread_id));
        // The console answers with a status line whose message says which it
        // is; "not stopped" is a successful reply, not an error.
        return response.message.find("not stopped") == std::string::npos;
    } catch (const CommandError&) {
        return false;
    }
}

int DebugSession::release_stopped_threads() {
    int released = 0;
    for (const std::uint32_t id : thread_ids()) {
        if (!thread_stopped(id)) continue;
        try {
            continue_thread(id, false);
            ++released;
        } catch (const Error&) {
            // A thread that has since exited is not a failure of the sweep.
        }
    }
    return released;
}

// --- breakpoints ----------------------------------------------------------

void DebugSession::set_breakpoint(std::uint32_t address) {
    static_cast<void>(impl_->send(detail::build_set_breakpoint(address)));
}

void DebugSession::clear_breakpoint(std::uint32_t address) {
    static_cast<void>(impl_->send(detail::build_clear_breakpoint(address)));
}

bool DebugSession::is_breakpoint(std::uint32_t address) {
    try {
        const Response response = impl_->send(detail::build_is_breakpoint(address));
        return response.message.find("not") == std::string::npos;
    } catch (const CommandError&) {
        return false;
    }
}

void DebugSession::set_data_breakpoint(
    std::uint32_t address, std::uint32_t size, BreakType type) {
    static_cast<void>(
        impl_->send(detail::build_data_breakpoint(address, size, type, false)));
}

void DebugSession::clear_data_breakpoint(
    std::uint32_t address, std::uint32_t size, BreakType type) {
    static_cast<void>(
        impl_->send(detail::build_data_breakpoint(address, size, type, true)));
}

void DebugSession::clear_all_breakpoints() {
    static_cast<void>(impl_->send("break clearall"));
}

// --- threads and registers ------------------------------------------------

std::vector<std::uint32_t> DebugSession::thread_ids() {
    return detail::parse_thread_ids(impl_->send_multiline("threads"));
}

ThreadInfo DebugSession::thread_info(std::uint32_t thread_id) {
    const auto lines = impl_->send_multiline(
        detail::build_thread_command("threadinfo", thread_id));

    ThreadInfo info;
    if (!lines.empty()) info = detail::parse_thread_info(lines.front());
    info.id = thread_id;
    info.stopped = thread_stopped(thread_id);
    return info;
}

PpcContext DebugSession::get_context(std::uint32_t thread_id, ContextFlags flags) {
    return detail::parse_context(
        impl_->send_multiline(detail::build_get_context(thread_id, flags)));
}

void DebugSession::set_context(
    std::uint32_t thread_id, const PpcContext& context, ContextFlags flags) {
    const std::string command = detail::build_set_context(thread_id, context, flags);

    // A full integer context is well over XBDM's command-buffer limit, so this
    // has to go out in pieces rather than as one line.
    if (command.size() <= detail::max_command_length) {
        static_cast<void>(impl_->send(command));
        return;
    }

    if (flags.control) {
        ContextFlags only;
        only.control = true;
        only.integer = false;
        only.floating = false;
        only.vector = false;
        static_cast<void>(impl_->send(detail::build_set_context(thread_id, context, only)));
    }

    // Registers go out in small batches, each its own command.
    const auto send_batch = [&](std::string_view prefix, std::size_t first, std::size_t count,
                                const std::array<std::uint64_t, 32>& values) {
        std::string batch = detail::build_thread_command("setcontext", thread_id);
        for (std::size_t i = first; i < first + count && i < values.size(); ++i) {
            batch += " ";
            batch += prefix;
            batch += std::to_string(i);
            batch += "=0q";
            static constexpr char digits[] = "0123456789abcdef";
            std::string hex(16, '0');
            std::uint64_t value = values[i];
            for (int d = 15; d >= 0; --d) {
                hex[static_cast<std::size_t>(d)] = digits[value & 0xF];
                value >>= 4;
            }
            batch += hex;
        }
        static_cast<void>(impl_->send(batch));
    };

    constexpr std::size_t kBatch = 8;
    if (flags.integer) {
        for (std::size_t i = 0; i < context.gpr.size(); i += kBatch) {
            send_batch("Gpr", i, kBatch, context.gpr);
        }
    }
    if (flags.floating) {
        for (std::size_t i = 0; i < context.fpr.size(); i += kBatch) {
            send_batch("Fpr", i, kBatch, context.fpr);
        }
    }
}

Response DebugSession::send_command(std::string_view command) {
    return impl_->send(command);
}

} // namespace srpc
