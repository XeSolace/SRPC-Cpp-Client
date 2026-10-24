#include "srpc/client.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace srpc {
namespace {

constexpr std::uint64_t address_space_size = std::uint64_t{1} << 32;

void validate_memory_range(
    std::uint32_t address,
    std::size_t size,
    std::string_view operation) {
    if (size > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError(
            std::string(operation) + " cannot exceed the 32-bit address space");
    }
    if (size != 0 &&
        size - 1 > std::numeric_limits<std::uint32_t>::max() - address) {
        throw ProtocolError(
            std::string(operation) + " crosses the 32-bit address boundary");
    }
}

std::size_t checked_utf16_byte_size(
    std::size_t character_count,
    bool include_terminator) {
    const auto terminator_count = include_terminator ? std::size_t{1} : std::size_t{0};
    if (character_count >
        std::numeric_limits<std::size_t>::max() - terminator_count) {
        throw ProtocolError("UTF-16 string length overflow");
    }
    const auto code_units = character_count + terminator_count;
    if (code_units > std::numeric_limits<std::size_t>::max() / sizeof(char16_t)) {
        throw ProtocolError("UTF-16 string byte length overflow");
    }
    return code_units * sizeof(char16_t);
}

bool is_hex_digit(char value) noexcept {
    return (value >= '0' && value <= '9') ||
        (value >= 'a' && value <= 'f') ||
        (value >= 'A' && value <= 'F');
}

std::optional<std::uint32_t> parse_walkmem_field(
    std::string_view line,
    std::string_view name) {
    const auto marker = std::string(name) + "=";
    const auto marker_position = line.find(marker);
    if (marker_position == std::string_view::npos) {
        return std::nullopt;
    }

    auto first = marker_position + marker.size();
    if (first + 2 <= line.size() && line[first] == '0' &&
        (line[first + 1] == 'x' || line[first + 1] == 'X')) {
        first += 2;
    }

    auto last = first;
    while (last < line.size() && is_hex_digit(line[last])) {
        ++last;
    }
    if (last == first) {
        throw ProtocolError(
            "walkmem returned an invalid " + std::string(name) + " field");
    }
    return detail::parse_hex_u32(line.substr(first, last - first));
}

void require_instruction_alignment(
    std::uint32_t address,
    std::string_view description) {
    if ((address & 0x3u) != 0) {
        throw ProtocolError(
            std::string(description) + " must be aligned to four bytes");
    }
}

template <std::size_t Size>
void store_big_endian_word(
    std::array<std::uint8_t, Size>& output,
    std::size_t offset,
    std::uint32_t word) {
    output[offset] = static_cast<std::uint8_t>(word >> 24);
    output[offset + 1] = static_cast<std::uint8_t>(word >> 16);
    output[offset + 2] = static_cast<std::uint8_t>(word >> 8);
    output[offset + 3] = static_cast<std::uint8_t>(word);
}

struct ReadableInterval {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

std::vector<ReadableInterval> normalize_readable_intervals(
    std::uint64_t request_begin,
    std::uint64_t request_end,
    std::span<const MemoryRegion> regions) {
    std::vector<ReadableInterval> intervals;
    intervals.reserve(regions.size());
    for (const auto& region : regions) {
        if (region.end() > address_space_size) {
            throw ProtocolError(
                "Sparse memory read received a region outside the 32-bit address space");
        }

        const auto begin = std::max(request_begin, static_cast<std::uint64_t>(region.base));
        const auto end = std::min(request_end, region.end());
        if (begin < end) {
            intervals.push_back({begin, end});
        }
    }

    std::sort(
        intervals.begin(),
        intervals.end(),
        [](const ReadableInterval& left, const ReadableInterval& right) {
            return std::tie(left.begin, left.end) < std::tie(right.begin, right.end);
        });

    std::vector<ReadableInterval> normalized;
    normalized.reserve(intervals.size());
    for (const auto& interval : intervals) {
        if (!normalized.empty() && interval.begin < normalized.back().end) {
            normalized.back().end = std::max(normalized.back().end, interval.end);
        } else {
            normalized.push_back(interval);
        }
    }
    return normalized;
}

void report_sparse_progress(
    const MemoryProgressCallback& progress,
    std::size_t processed,
    std::size_t total) {
    if (progress && !progress(processed, total)) {
        throw Error("Sparse memory read was cancelled by its progress callback");
    }
}

} // namespace

ByteBuffer Client::read_memory_chunked(
    std::uint32_t address,
    std::size_t size,
    std::size_t chunk_size) {
    if (chunk_size == 0) {
        throw ProtocolError("Memory read chunk size must be greater than zero");
    }
    validate_memory_range(address, size, "Chunked memory read");

    ByteBuffer result;
    result.reserve(size);
    for (std::size_t offset = 0; offset < size;) {
        const auto current_size = std::min(chunk_size, size - offset);
        auto chunk = read_memory(
            address + static_cast<std::uint32_t>(offset), current_size);
        if (chunk.size() != current_size) {
            throw ProtocolError("Chunked memory read returned an unexpected byte count");
        }
        result.insert(result.end(), chunk.begin(), chunk.end());
        offset += current_size;
    }
    return result;
}

ByteBuffer Client::read_memory_sparse(
    std::uint32_t address,
    std::size_t size,
    std::span<const MemoryRegion> readable_regions,
    std::size_t chunk_size,
    MemoryProgressCallback progress) {
    if (chunk_size == 0) {
        throw ProtocolError("Sparse memory read chunk size must be greater than zero");
    }
    validate_memory_range(address, size, "Sparse memory read");

    const auto request_begin = static_cast<std::uint64_t>(address);
    const auto request_end = request_begin + size;
    const auto intervals = normalize_readable_intervals(
        request_begin, request_end, readable_regions);

    report_sparse_progress(progress, 0, size);
    ByteBuffer result(size, 0);
    std::uint64_t cursor = request_begin;

    const auto advance_gap = [&](std::uint64_t gap_end) {
        while (cursor < gap_end) {
            const auto remaining = static_cast<std::size_t>(gap_end - cursor);
            const auto current_size = std::min(chunk_size, remaining);
            cursor += current_size;
            report_sparse_progress(
                progress,
                static_cast<std::size_t>(cursor - request_begin),
                size);
        }
    };

    for (const auto& interval : intervals) {
        advance_gap(interval.begin);
        while (cursor < interval.end) {
            const auto remaining = static_cast<std::size_t>(interval.end - cursor);
            const auto current_size = std::min(chunk_size, remaining);
            auto chunk = read_memory(static_cast<std::uint32_t>(cursor), current_size);
            if (chunk.size() != current_size) {
                throw ProtocolError(
                    "Sparse memory read returned an unexpected byte count");
            }

            const auto output_offset = static_cast<std::size_t>(cursor - request_begin);
            std::copy(
                chunk.begin(),
                chunk.end(),
                result.begin() + static_cast<std::ptrdiff_t>(output_offset));
            cursor += current_size;
            report_sparse_progress(
                progress,
                static_cast<std::size_t>(cursor - request_begin),
                size);
        }
    }
    advance_gap(request_end);
    return result;
}

ByteBuffer Client::read_memory_sparse(
    std::uint32_t address,
    std::size_t size,
    std::size_t chunk_size,
    MemoryProgressCallback progress) {
    if (chunk_size == 0) {
        throw ProtocolError("Sparse memory read chunk size must be greater than zero");
    }
    validate_memory_range(address, size, "Sparse memory read");

    auto regions = memory_regions();
    return read_memory_sparse(
        address,
        size,
        std::span<const MemoryRegion>(regions),
        chunk_size,
        std::move(progress));
}

std::vector<MemoryRegion> Client::memory_regions() {
    const auto lines = send_multiline_command("walkmem");
    std::vector<MemoryRegion> regions;
    regions.reserve(lines.size());

    for (const auto& line : lines) {
        const auto base = parse_walkmem_field(line, "base");
        const auto size = parse_walkmem_field(line, "size");
        if (!base.has_value() || !size.has_value()) {
            continue;
        }

        MemoryRegion region;
        region.base = *base;
        region.size = *size;
        region.protection = parse_walkmem_field(line, "protect").value_or(0);
        if (region.end() > address_space_size) {
            throw ProtocolError("walkmem returned a region outside the 32-bit address space");
        }
        regions.push_back(region);
    }

    std::sort(
        regions.begin(),
        regions.end(),
        [](const MemoryRegion& left, const MemoryRegion& right) {
            return std::tie(left.base, left.size, left.protection) <
                std::tie(right.base, right.size, right.protection);
        });
    return regions;
}

bool Client::is_valid_address(std::uint32_t address) {
    try {
        static_cast<void>(read_memory(address, 1));
        return true;
    } catch (const CommandError& error) {
        if (error.status_code() == 404) {
            return false;
        }
        throw;
    }
}

void Client::fill_memory(
    std::uint32_t address,
    std::size_t size,
    std::uint8_t value) {
    validate_memory_range(address, size, "Memory fill");
    if (size == 0) {
        return;
    }

    std::array<std::uint8_t, detail::setmem_chunk_size> chunk{};
    chunk.fill(value);
    for (std::size_t offset = 0; offset < size;) {
        const auto current_size = std::min(chunk.size(), size - offset);
        write_memory(
            address + static_cast<std::uint32_t>(offset),
            std::span<const std::uint8_t>(chunk).first(current_size));
        offset += current_size;
    }
}

void Client::zero_memory(std::uint32_t address, std::size_t size) {
    fill_memory(address, size, 0);
}

std::string Client::read_cstring(
    std::uint32_t address,
    std::size_t maximum_length) {
    auto bytes = read_memory(address, maximum_length);
    const auto terminator = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    const auto length = static_cast<std::size_t>(terminator - bytes.begin());
    if (length == 0) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(bytes.data()),
        length);
}

void Client::write_cstring(
    std::uint32_t address,
    std::string_view value,
    bool null_terminate) {
    const auto terminator_size = null_terminate ? std::size_t{1} : std::size_t{0};
    if (value.size() > std::numeric_limits<std::size_t>::max() - terminator_size) {
        throw ProtocolError("C string length overflow");
    }
    const auto total_size = value.size() + terminator_size;
    validate_memory_range(address, total_size, "C string write");

    ByteBuffer bytes;
    bytes.reserve(total_size);
    bytes.insert(bytes.end(), value.begin(), value.end());
    if (null_terminate) {
        bytes.push_back(0);
    }
    write_memory(address, bytes);
}

std::u16string Client::read_utf16_string(
    std::uint32_t address,
    std::size_t maximum_characters,
    Endian endian) {
    static_cast<void>(checked_utf16_byte_size(maximum_characters, false));
    auto code_units = read_array<char16_t>(address, maximum_characters, endian);
    const auto terminator = std::find(code_units.begin(), code_units.end(), u'\0');
    return std::u16string(code_units.begin(), terminator);
}

void Client::write_utf16_string(
    std::uint32_t address,
    std::u16string_view value,
    Endian endian,
    bool null_terminate) {
    const auto byte_size = checked_utf16_byte_size(value.size(), null_terminate);
    validate_memory_range(address, byte_size, "UTF-16 string write");

    if (!null_terminate) {
        write_array<char16_t>(
            address,
            std::span<const char16_t>(value.data(), value.size()),
            endian);
        return;
    }

    std::vector<char16_t> code_units;
    code_units.reserve(value.size() + 1);
    code_units.insert(code_units.end(), value.begin(), value.end());
    code_units.push_back(u'\0');
    write_array<char16_t>(address, code_units, endian);
}

std::uint32_t Client::resolve_pointer(
    std::uint32_t address,
    std::span<const std::int32_t> offsets) {
    auto cursor = address;
    for (const auto offset : offsets) {
        cursor = read<std::uint32_t>(cursor) +
            static_cast<std::uint32_t>(offset);
    }
    return cursor;
}

void Client::write_branch(
    std::uint32_t address,
    std::uint32_t destination,
    bool linked) {
    require_instruction_alignment(address, "Branch address");
    require_instruction_alignment(destination, "Branch destination");

    const auto offset = static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(address);
    constexpr std::int64_t minimum_offset = -0x02000000LL;
    constexpr std::int64_t maximum_offset = 0x01FFFFFCLL;
    if (offset < minimum_offset || offset > maximum_offset) {
        throw ProtocolError("Relative branch destination is outside the signed 26-bit range");
    }

    const auto instruction = 0x48000000u |
        (static_cast<std::uint32_t>(offset) & 0x03FFFFFCu) |
        (linked ? 1u : 0u);
    std::array<std::uint8_t, 4> bytes{};
    store_big_endian_word(bytes, 0, instruction);
    write_memory(address, bytes);
}

void Client::write_jump(
    std::uint32_t address,
    std::uint32_t destination,
    std::uint8_t scratch_register,
    bool linked) {
    require_instruction_alignment(address, "Jump address");
    require_instruction_alignment(destination, "Jump destination");
    if (scratch_register > 31) {
        throw ProtocolError("Jump scratch register must be in the range 0..31");
    }
    validate_memory_range(address, 16, "Far jump write");

    const auto reg = static_cast<std::uint32_t>(scratch_register);
    const auto high = (destination >> 16) & 0xFFFFu;
    const auto low = destination & 0xFFFFu;
    const std::array<std::uint32_t, 4> instructions{
        0x3C000000u | (reg << 21) | high,
        0x60000000u | (reg << 21) | (reg << 16) | low,
        (31u << 26) | (reg << 21) | (9u << 16) | (467u << 1),
        0x4E800420u | (linked ? 1u : 0u),
    };

    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        store_big_endian_word(bytes, index * 4, instructions[index]);
    }
    write_memory(address, bytes);
}

} // namespace srpc
