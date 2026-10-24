#include "srpc/discovery.hpp"

#include "connection.hpp"
#include "protocol.hpp"
#include "srpc/error.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace srpc {
namespace {

void validate_options(const DiscoveryOptions& options) {
    if (options.port == 0) {
        throw ProtocolError("Discovery port cannot be zero");
    }
    if (options.connect_timeout.count() <= 0 || options.io_timeout.count() <= 0) {
        throw ProtocolError("Discovery timeouts must be positive");
    }
    if (options.max_concurrency == 0) {
        throw ProtocolError("Discovery concurrency must be at least one");
    }
    if (options.max_candidates_per_adapter == 0) {
        throw ProtocolError("Discovery candidate limit must be at least one");
    }
}

std::string ipv4_text(std::uint32_t host_order_address) {
    in_addr address{};
    address.S_un.S_addr = htonl(host_order_address);
    char buffer[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) == nullptr) {
        throw ConnectionError("Could not format an IPv4 discovery address");
    }
    return buffer;
}

bool is_usable_local_address(std::uint32_t address) {
    const auto first_octet = static_cast<std::uint8_t>(address >> 24);
    return address != 0 && address != std::numeric_limits<std::uint32_t>::max() &&
        first_octet != 127 && first_octet < 224;
}

void add_subnet_candidates(
    std::set<std::uint32_t>& candidates,
    std::uint32_t local_address,
    std::uint8_t prefix_length,
    std::size_t maximum) {
    if (prefix_length > 30 || maximum == 0) {
        return;
    }

    const std::uint64_t address_count = std::uint64_t{1} << (32 - prefix_length);
    std::uint32_t network = 0;
    std::uint64_t usable_count = address_count - 2;

    if (usable_count > maximum) {
        // Scanning a full /16 (or larger) is slow and noisy. XBDM consoles are
        // normally on the same /24 as the PC, so constrain oversized ranges.
        network = local_address & 0xFFFFFF00u;
        usable_count = 254;
    } else {
        const auto mask = prefix_length == 0
            ? 0u
            : std::numeric_limits<std::uint32_t>::max() << (32 - prefix_length);
        network = local_address & mask;
    }

    const auto count = std::min<std::uint64_t>(usable_count, maximum);
    for (std::uint64_t offset = 1; offset <= count; ++offset) {
        const auto candidate = network + static_cast<std::uint32_t>(offset);
        if (candidate != local_address) {
            candidates.insert(candidate);
        }
    }
}

std::vector<std::string> local_subnet_candidates(const DiscoveryOptions& options) {
    ULONG buffer_size = 16 * 1024;
    std::vector<std::uint8_t> buffer(buffer_size);
    IP_ADAPTER_ADDRESSES* adapters = nullptr;

    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME,
            nullptr,
            adapters,
            &buffer_size);
    }
    if (result != NO_ERROR) {
        throw ConnectionError(
            "Could not enumerate local network adapters (Windows error " +
            std::to_string(result) + ")");
    }

    std::set<std::uint32_t> addresses;
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (auto* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* socket_address = reinterpret_cast<const sockaddr_in*>(
                unicast->Address.lpSockaddr);
            const auto local_address = ntohl(socket_address->sin_addr.S_un.S_addr);
            if (!is_usable_local_address(local_address)) {
                continue;
            }
            add_subnet_candidates(
                addresses,
                local_address,
                unicast->OnLinkPrefixLength,
                options.max_candidates_per_adapter);
        }
    }

    std::vector<std::string> candidates;
    candidates.reserve(addresses.size());
    for (const auto address : addresses) {
        candidates.push_back(ipv4_text(address));
    }
    return candidates;
}

std::optional<DiscoveredConsole> probe_console_unchecked(
    std::string_view host,
    const DiscoveryOptions& options) {
    if (host.empty()) {
        return std::nullopt;
    }

    try {
        detail::Connection connection;
        connection.connect(
            host,
            options.port,
            options.connect_timeout,
            options.io_timeout);

        const auto banner = detail::parse_response_line(connection.read_line());
        auto banner_message = banner.message;
        std::transform(
            banner_message.begin(),
            banner_message.end(),
            banner_message.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (banner.status_code != 201 &&
            banner_message.find("connected") == std::string::npos) {
            return std::nullopt;
        }

        std::string name;
        try {
            constexpr std::string_view command = "dbgname\r\n";
            connection.send_all(std::span(
                reinterpret_cast<const std::uint8_t*>(command.data()),
                command.size()));
            const auto response = detail::parse_response_line(connection.read_line());
            if (response.ok()) {
                name = response.message;
            }
        } catch (const Error&) {
            // The valid XBDM banner is enough to report the console. Its name
            // is optional because some builds do not answer dbgname reliably.
        }

        return DiscoveredConsole{std::string(host), std::move(name), options.port};
    } catch (const Error&) {
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<DiscoveredConsole> scan(bool stop_after_first, DiscoveryOptions options) {
    validate_options(options);
    const auto candidates = local_subnet_candidates(options);
    if (candidates.empty()) {
        return {};
    }

    const auto worker_count = std::min(options.max_concurrency, candidates.size());
    std::atomic_size_t next_index{0};
    std::atomic_bool stop{false};
    std::mutex result_mutex;
    std::vector<DiscoveredConsole> results;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= candidates.size()) {
                    break;
                }
                auto console = probe_console_unchecked(candidates[index], options);
                if (!console.has_value()) {
                    continue;
                }

                {
                    std::scoped_lock lock(result_mutex);
                    results.push_back(std::move(*console));
                }
                if (stop_after_first) {
                    stop.store(true, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::sort(
        results.begin(),
        results.end(),
        [](const DiscoveredConsole& left, const DiscoveredConsole& right) {
            return left.host < right.host;
        });
    return results;
}

} // namespace

std::optional<DiscoveredConsole> probe_console(
    std::string_view host,
    DiscoveryOptions options) {
    validate_options(options);
    return probe_console_unchecked(host, options);
}

std::vector<DiscoveredConsole> discover_consoles(DiscoveryOptions options) {
    return scan(false, options);
}

std::optional<DiscoveredConsole> discover_console(DiscoveryOptions options) {
    auto consoles = scan(true, options);
    if (consoles.empty()) {
        return std::nullopt;
    }
    return std::move(consoles.front());
}

std::optional<DiscoveredConsole> discover_console(
    std::string_view fallback_host,
    DiscoveryOptions options) {
    if (auto console = discover_console(options); console.has_value()) {
        return console;
    }
    return probe_console(fallback_host, options);
}

} // namespace srpc
