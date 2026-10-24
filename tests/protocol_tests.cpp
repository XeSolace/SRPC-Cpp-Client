#include "protocol.hpp"

#include "srpc/error.hpp"
#include "srpc/types.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
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

template <typename Exception, typename Function>
void expect_throws(Function&& function, const char* description) {
    try {
        std::invoke(std::forward<Function>(function));
        std::cerr << "FAILED: " << description << " (no exception)\n";
        ++failures;
    } catch (const Exception&) {
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << description << " (wrong exception: "
                  << error.what() << ")\n";
        ++failures;
    }
}

void response_tests() {
    const auto response = srpc::detail::parse_response_line("201- connected\r\n");
    expect(response.status_code == 201, "response status is parsed");
    expect(response.message == "connected", "response message is trimmed");
    expect(response.ok(), "2xx response is successful");

    const auto error = srpc::detail::parse_response_line("407- unknown command");
    expect(!error.ok(), "4xx response is unsuccessful");
    expect_throws<srpc::CommandError>(
        [&] { srpc::detail::throw_if_command_failed(error); },
        "4xx response raises CommandError");
    expect_throws<srpc::ProtocolError>(
        [] { static_cast<void>(srpc::detail::parse_response_line("OK")); },
        "malformed response is rejected");
}

void hex_tests() {
    const srpc::ByteBuffer bytes{0x00, 0x12, 0xAB, 0xFF};
    expect(
        srpc::detail::hex_encode(bytes) == "0012ABFF",
        "hex encoder uses uppercase by default");
    expect(
        srpc::detail::hex_decode("00 12\r\nABff") == bytes,
        "hex decoder accepts whitespace and mixed case");
    expect_throws<srpc::ProtocolError>(
        [] { static_cast<void>(srpc::detail::hex_decode("123")); },
        "odd-length hex is rejected");
}

void jrpc_builder_tests() {
    const std::vector<srpc::RpcArgument> arguments{
        std::uint32_t{5}, std::uint32_t{7}};
    const auto command = srpc::detail::build_jrpc_command(
        static_cast<std::uint32_t>(srpc::ReturnType::int32),
        0x82345678,
        std::span<const srpc::RpcArgument>(arguments));
    expect(
        command ==
            "consolefeatures ver=2 type=1 system as=0 "
            "params=\"A\\82345678\\A\\2\\1\\5\\1\\7\\\"",
        "JRPC2 integer call matches the documented wire form");

    const std::vector<srpc::RpcArgument> string_argument{"Hi"};
    const auto string_command = srpc::detail::build_jrpc_command(
        2, 0x1000, std::span<const srpc::RpcArgument>(string_argument));
    expect(
        string_command.find("2/2\\4869\\") != std::string::npos,
        "string literals are encoded as strings rather than booleans");

    const std::vector<srpc::RpcArgument> unsigned_64{
        std::numeric_limits<std::uint64_t>::max()};
    const auto unsigned_command = srpc::detail::build_jrpc_command(
        8, 0x1000, std::span<const srpc::RpcArgument>(unsigned_64));
    expect(
        unsigned_command.find("8\\-1\\") != std::string::npos,
        "unsigned 64-bit arguments preserve their bit pattern");

    expect(
        srpc::detail::build_jrpc_poll_command(0x825F0008) ==
            "consolefeatures buf_addr=0x825F0008",
        "JRPC2 poll address includes the required 0x prefix");
    expect(
        srpc::detail::parse_jrpc_pending_address("buf_addr=825F0008") ==
            std::optional<std::uint32_t>{0x825F0008},
        "JRPC2 pending address is parsed as hexadecimal");
}

void result_tests() {
    expect(
        std::get<std::uint32_t>(srpc::detail::parse_jrpc_result(
            "89ABCDEF", srpc::ReturnType::int32)) == 0x89ABCDEF,
        "JRPC2 integer return is hexadecimal");
    expect(
        std::fabs(std::get<float>(srpc::detail::parse_jrpc_result(
            "1.25", srpc::ReturnType::float32)) - 1.25F) < 0.0001F,
        "JRPC2 float return is parsed");
    expect(
        std::get<std::vector<std::uint32_t>>(srpc::detail::parse_jrpc_result(
            "1A,2B,3C;", srpc::ReturnType::int32_array)) ==
            std::vector<std::uint32_t>{0x1A, 0x2B, 0x3C},
        "JRPC2 integer array is parsed");
    expect(
        std::get<srpc::ByteBuffer>(srpc::detail::parse_jrpc_result(
            "1,2,255;", srpc::ReturnType::byte_array)) ==
            srpc::ByteBuffer{1, 2, 255},
        "JRPC2 byte array uses decimal elements");
    expect_throws<srpc::RpcError>(
        [] {
            static_cast<void>(srpc::detail::parse_jrpc_result(
                "error=bad address", srpc::ReturnType::int32));
        },
        "JRPC2 error replies raise RpcError");
}

void native_tests() {
    const std::vector<srpc::RpcArgument> arguments{
        std::uint32_t{1}, 1.0F, "A"};
    expect(
        srpc::detail::build_native_call(
            0x82345678, std::span<const srpc::RpcArgument>(arguments)) ==
            "s360 82345678 1 flt:3F800000 str:41",
        "native SRPC call matches the wire format");
    expect(
        srpc::detail::parse_native_word("00000010") == 0x10,
        "digit-only native results are treated as hexadecimal");
    expect_throws<srpc::RpcError>(
        [] { static_cast<void>(srpc::detail::parse_native_word("err=C0000005")); },
        "native call errors raise RpcError");

    const std::vector<srpc::RpcArgument> unsupported{std::uint64_t{1}};
    expect_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::detail::build_native_call(
                0x1000, std::span<const srpc::RpcArgument>(unsupported)));
        },
        "native SRPC rejects 64-bit arguments");
}

void validation_tests() {
    expect_throws<srpc::ProtocolError>(
        [] { srpc::detail::validate_command("getpid\r\nbye"); },
        "command injection is rejected");
    expect_throws<srpc::ProtocolError>(
        [] {
            const std::string oversized(srpc::detail::max_command_length + 1, 'A');
            srpc::detail::validate_command(oversized);
        },
        "oversized commands are rejected");
    expect(
        srpc::detail::normalize_srpc_command(" titleid ") == "s360 titleid",
        "SRPC prefix is added when omitted");
    expect(
        srpc::detail::quote_xbdm_argument("Hdd:\\Content", "Path") ==
            "\"Hdd:\\Content\"",
        "XBDM quoted arguments preserve normal console paths");
    expect_throws<srpc::ProtocolError>(
        [] {
            static_cast<void>(srpc::detail::quote_xbdm_argument(
                "Hdd:\\bad\"name", "Path"));
        },
        "XBDM quoted arguments reject quote injection");
    expect(
        static_cast<std::uint32_t>(srpc::NotificationType::console_message) == 34 &&
            static_cast<std::uint32_t>(
                srpc::NotificationType::smartglass_available) == 77,
        "notification enum preserves Xbox notification IDs");
}

} // namespace

int main() {
    response_tests();
    hex_tests();
    jrpc_builder_tests();
    result_tests();
    native_tests();
    validation_tests();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "All SRPC protocol tests passed\n";
    return 0;
}
