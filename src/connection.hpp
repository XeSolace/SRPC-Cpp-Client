#pragma once

#include "srpc/types.hpp"

#include <winsock2.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace srpc::detail {

class Connection final {
public:
    Connection();
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void connect(
        std::string_view host,
        std::uint16_t port,
        std::chrono::milliseconds connect_timeout,
        std::chrono::milliseconds io_timeout);
    void close() noexcept;

    [[nodiscard]] bool connected() const noexcept;

    void send_all(std::span<const std::uint8_t> data);
    [[nodiscard]] std::string read_line();
    [[nodiscard]] ByteBuffer read_exact(std::size_t size);

private:
    void receive_more();
    void compact_buffer();

    bool winsock_started_ = false;
    SOCKET socket_ = INVALID_SOCKET;
    ByteBuffer buffer_;
    std::size_t buffer_offset_ = 0;
};

} // namespace srpc::detail
