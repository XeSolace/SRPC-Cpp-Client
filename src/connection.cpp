#include "connection.hpp"

#include "srpc/error.hpp"

#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <memory>
#include <string>

namespace srpc::detail {
namespace {

constexpr std::size_t receive_buffer_size = 8192;
constexpr std::size_t maximum_line_size = 64 * 1024;

std::string socket_error(std::string_view action, int code) {
    return std::string(action) + " (Winsock error " + std::to_string(code) + ")";
}

timeval make_timeval(std::chrono::milliseconds timeout) {
    const auto seconds = timeout.count() / 1000;
    const auto microseconds = (timeout.count() % 1000) * 1000;
    timeval value{};
    value.tv_sec = static_cast<long>(std::min<std::int64_t>(seconds, LONG_MAX));
    value.tv_usec = static_cast<long>(microseconds);
    return value;
}

DWORD timeout_value(std::chrono::milliseconds timeout) {
    const auto count = std::clamp<std::int64_t>(
        timeout.count(), 1, std::numeric_limits<DWORD>::max());
    return static_cast<DWORD>(count);
}

} // namespace

Connection::Connection() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw ConnectionError(socket_error("WSAStartup failed", result));
    }
    winsock_started_ = true;
}

Connection::~Connection() {
    close();
    if (winsock_started_) {
        WSACleanup();
    }
}

void Connection::connect(
    std::string_view host,
    std::uint16_t port,
    std::chrono::milliseconds connect_timeout,
    std::chrono::milliseconds io_timeout) {
    close();

    const std::string host_text(host);
    const std::string port_text = std::to_string(port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* raw_addresses = nullptr;
    const int address_result = getaddrinfo(
        host_text.c_str(), port_text.c_str(), &hints, &raw_addresses);
    if (address_result != 0) {
        throw ConnectionError(
            "Could not resolve host '" + host_text + "' (getaddrinfo error " +
            std::to_string(address_result) + ")");
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(
        raw_addresses, freeaddrinfo);

    int last_error = WSAECONNREFUSED;
    for (auto* address = addresses.get(); address != nullptr; address = address->ai_next) {
        SOCKET candidate = socket(
            address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            last_error = WSAGetLastError();
            continue;
        }

        u_long nonblocking = 1;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            last_error = WSAGetLastError();
            closesocket(candidate);
            continue;
        }

        bool did_connect = ::connect(
            candidate,
            address->ai_addr,
            static_cast<int>(address->ai_addrlen)) == 0;

        if (!did_connect) {
            const int connect_error = WSAGetLastError();
            if (connect_error == WSAEWOULDBLOCK || connect_error == WSAEINPROGRESS ||
                connect_error == WSAEALREADY) {
                fd_set write_set;
                fd_set error_set;
                FD_ZERO(&write_set);
                FD_ZERO(&error_set);
                FD_SET(candidate, &write_set);
                FD_SET(candidate, &error_set);
                auto timeout = make_timeval(connect_timeout);
                const int selected = select(
                    0, nullptr, &write_set, &error_set, &timeout);
                if (selected > 0) {
                    int socket_status = 0;
                    int status_size = sizeof(socket_status);
                    if (getsockopt(
                            candidate,
                            SOL_SOCKET,
                            SO_ERROR,
                            reinterpret_cast<char*>(&socket_status),
                            &status_size) == 0 &&
                        socket_status == 0) {
                        did_connect = true;
                    } else {
                        last_error = socket_status != 0 ? socket_status : WSAGetLastError();
                    }
                } else if (selected == 0) {
                    last_error = WSAETIMEDOUT;
                } else {
                    last_error = WSAGetLastError();
                }
            } else {
                last_error = connect_error;
            }
        }

        if (!did_connect) {
            closesocket(candidate);
            continue;
        }

        nonblocking = 0;
        if (ioctlsocket(candidate, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            last_error = WSAGetLastError();
            closesocket(candidate);
            continue;
        }

        const DWORD receive_timeout = timeout_value(io_timeout);
        const DWORD send_timeout = timeout_value(io_timeout);
        if (setsockopt(
                candidate,
                SOL_SOCKET,
                SO_RCVTIMEO,
                reinterpret_cast<const char*>(&receive_timeout),
                sizeof(receive_timeout)) == SOCKET_ERROR ||
            setsockopt(
                candidate,
                SOL_SOCKET,
                SO_SNDTIMEO,
                reinterpret_cast<const char*>(&send_timeout),
                sizeof(send_timeout)) == SOCKET_ERROR) {
            last_error = WSAGetLastError();
            closesocket(candidate);
            continue;
        }

        socket_ = candidate;
        buffer_.clear();
        buffer_offset_ = 0;
        return;
    }

    if (last_error == WSAETIMEDOUT) {
        throw TimeoutError("Connection to " + host_text + " timed out");
    }
    throw ConnectionError(socket_error("Could not connect to " + host_text, last_error));
}

void Connection::close() noexcept {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    buffer_.clear();
    buffer_offset_ = 0;
}

bool Connection::connected() const noexcept {
    return socket_ != INVALID_SOCKET;
}

void Connection::send_all(std::span<const std::uint8_t> data) {
    if (!connected()) {
        throw ConnectionError("Not connected to a console");
    }

    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = std::min<std::size_t>(
            data.size() - sent, static_cast<std::size_t>(INT_MAX));
        const int result = send(
            socket_,
            reinterpret_cast<const char*>(data.data() + sent),
            static_cast<int>(remaining),
            0);
        if (result == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error == WSAETIMEDOUT) {
                close();
                throw TimeoutError("Timed out while sending data to the console");
            }
            close();
            throw ConnectionError(socket_error("Failed to send data", error));
        }
        if (result == 0) {
            close();
            throw ConnectionError("Console closed the connection while data was being sent");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::string Connection::read_line() {
    if (!connected()) {
        throw ConnectionError("Not connected to a console");
    }

    for (;;) {
        const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_offset_);
        const auto newline = std::find(begin, buffer_.end(), static_cast<std::uint8_t>('\n'));
        if (newline != buffer_.end()) {
            auto end = newline;
            if (end != begin && *(end - 1) == static_cast<std::uint8_t>('\r')) {
                --end;
            }
            std::string line(
                reinterpret_cast<const char*>(&*begin),
                static_cast<std::size_t>(end - begin));
            buffer_offset_ = static_cast<std::size_t>(newline - buffer_.begin()) + 1;
            compact_buffer();
            return line;
        }

        if (buffer_.size() - buffer_offset_ > maximum_line_size) {
            throw ProtocolError("XBDM response line exceeded 64 KiB");
        }
        receive_more();
    }
}

ByteBuffer Connection::read_exact(std::size_t size) {
    if (!connected()) {
        throw ConnectionError("Not connected to a console");
    }

    ByteBuffer result(size);
    std::size_t copied = 0;
    while (copied < size) {
        const auto available = buffer_.size() - buffer_offset_;
        if (available == 0) {
            receive_more();
            continue;
        }
        const auto amount = std::min(available, size - copied);
        std::copy_n(
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_offset_),
            static_cast<std::ptrdiff_t>(amount),
            result.begin() + static_cast<std::ptrdiff_t>(copied));
        buffer_offset_ += amount;
        copied += amount;
        compact_buffer();
    }
    return result;
}

void Connection::receive_more() {
    std::array<std::uint8_t, receive_buffer_size> chunk{};
    const int result = recv(
        socket_, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
    if (result == 0) {
        close();
        throw ConnectionError("Console closed the connection");
    }
    if (result == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        if (error == WSAETIMEDOUT) {
            close();
            throw TimeoutError("Timed out while waiting for the console");
        }
        close();
        throw ConnectionError(socket_error("Failed to receive data", error));
    }

    buffer_.insert(buffer_.end(), chunk.begin(), chunk.begin() + result);
}

void Connection::compact_buffer() {
    if (buffer_offset_ == buffer_.size()) {
        buffer_.clear();
        buffer_offset_ = 0;
        return;
    }
    if (buffer_offset_ >= receive_buffer_size && buffer_offset_ >= buffer_.size() / 2) {
        buffer_.erase(
            buffer_.begin(),
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_offset_));
        buffer_offset_ = 0;
    }
}

} // namespace srpc::detail
