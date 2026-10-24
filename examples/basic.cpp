#include <srpc/client.hpp>
#include <srpc/discovery.hpp>

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

std::string_view protocol_name(srpc::Protocol protocol) {
    switch (protocol) {
    case srpc::Protocol::native_srpc:
        return "native SRPC";
    case srpc::Protocol::jrpc2:
        return "JRPC2";
    case srpc::Protocol::automatic:
        return "automatic";
    }
    return "unknown";
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: srpc_basic_example [console-ip-or-hostname]\n";
        return 2;
    }

    try {
        const auto discovered = argc == 2
            ? srpc::probe_console(argv[1])
            : srpc::discover_console();
        if (!discovered.has_value()) {
            std::cerr << "No reachable XBDM console was found\n";
            return 2;
        }

        srpc::Client console(discovered->host);
        console.connect();

        std::cout << "Console: " << console.console_name() << '\n';
        std::cout << "Transport: " << protocol_name(console.protocol()) << '\n';
        std::cout << "Kernel: " << console.kernel_version() << '\n';
        std::cout << "Motherboard: " << console.motherboard_type() << '\n';
        std::cout << "Title ID: 0x" << std::uppercase << std::hex
                  << std::setw(8) << std::setfill('0') << console.title_id() << '\n';
    } catch (const srpc::Error& error) {
        std::cerr << "SRPC error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
