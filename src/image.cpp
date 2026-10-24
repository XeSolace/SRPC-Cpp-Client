#include "srpc/image.hpp"

#include "srpc/error.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace srpc {
namespace {

constexpr std::uint32_t maximum_png_dimension = 0x7FFFFFFFu;
constexpr std::size_t stored_block_size = 65535;
constexpr std::size_t maximum_idat_chunk_size = 1024 * 1024;

struct ImageLayout {
    std::size_t row_bytes = 0;
    std::size_t raw_size = 0;
};

std::size_t checked_add(
    std::size_t left,
    std::size_t right,
    std::string_view description) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw ProtocolError(std::string(description) + " size overflow");
    }
    return left + right;
}

std::size_t checked_multiply(
    std::size_t left,
    std::size_t right,
    std::string_view description) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        throw ProtocolError(std::string(description) + " size overflow");
    }
    return left * right;
}

ImageLayout validate_image(const ImageView& image) {
    if (image.width == 0 || image.height == 0) {
        throw ProtocolError("PNG width and height must be greater than zero");
    }
    if (image.width > maximum_png_dimension ||
        image.height > maximum_png_dimension) {
        throw ProtocolError("PNG width and height must fit the PNG 31-bit limit");
    }
    if (image.format != PixelFormat::rgba8 && image.format != PixelFormat::bgra8) {
        throw ProtocolError("Unsupported PNG pixel format");
    }

    const auto row_bytes = checked_multiply(
        static_cast<std::size_t>(image.width), 4, "PNG row");
    if (image.stride < row_bytes) {
        throw ProtocolError("PNG input stride is smaller than one pixel row");
    }

    const auto last_row_offset = checked_multiply(
        static_cast<std::size_t>(image.height - 1), image.stride, "PNG input");
    const auto required_input = checked_add(last_row_offset, row_bytes, "PNG input");
    if (image.pixels.size() < required_input) {
        throw ProtocolError("PNG input buffer is smaller than its dimensions and stride");
    }

    const auto filtered_row_bytes = checked_add(row_bytes, 1, "PNG filtered row");
    const auto raw_size = checked_multiply(
        filtered_row_bytes,
        static_cast<std::size_t>(image.height),
        "PNG filtered image");
    return {row_bytes, raw_size};
}

void append_big_endian_u32(ByteBuffer& output, std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24));
    output.push_back(static_cast<std::uint8_t>(value >> 16));
    output.push_back(static_cast<std::uint8_t>(value >> 8));
    output.push_back(static_cast<std::uint8_t>(value));
}

void store_big_endian_u32(
    std::span<std::uint8_t, 4> output,
    std::uint32_t value) {
    output[0] = static_cast<std::uint8_t>(value >> 24);
    output[1] = static_cast<std::uint8_t>(value >> 16);
    output[2] = static_cast<std::uint8_t>(value >> 8);
    output[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t update_crc32(
    std::uint32_t crc,
    std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

std::uint32_t crc32(
    std::string_view type,
    std::span<const std::uint8_t> data) noexcept {
    auto crc = std::numeric_limits<std::uint32_t>::max();
    const auto type_bytes = std::span(
        reinterpret_cast<const std::uint8_t*>(type.data()), type.size());
    crc = update_crc32(crc, type_bytes);
    crc = update_crc32(crc, data);
    return ~crc;
}

std::uint32_t adler32(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint32_t modulus = 65521;
    constexpr std::size_t maximum_unreduced_bytes = 5552;
    std::uint32_t first = 1;
    std::uint32_t second = 0;

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = std::min(
            maximum_unreduced_bytes, bytes.size() - offset);
        for (std::size_t index = 0; index < count; ++index) {
            first += bytes[offset + index];
            second += first;
        }
        first %= modulus;
        second %= modulus;
        offset += count;
    }
    return (second << 16) | first;
}

void append_chunk(
    ByteBuffer& output,
    std::string_view type,
    std::span<const std::uint8_t> data) {
    if (type.size() != 4) {
        throw ProtocolError("PNG chunk type must contain four bytes");
    }
    if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("PNG chunk exceeds its 32-bit length field");
    }
    static_cast<void>(checked_add(output.size(), data.size(), "PNG output"));
    static_cast<void>(checked_add(
        checked_add(output.size(), data.size(), "PNG output"),
        12,
        "PNG output"));

    append_big_endian_u32(output, static_cast<std::uint32_t>(data.size()));
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), data.begin(), data.end());
    append_big_endian_u32(output, crc32(type, data));
}

ByteBuffer make_filtered_pixels(const ImageView& image, const ImageLayout& layout) {
    ByteBuffer filtered;
    filtered.reserve(layout.raw_size);
    for (std::size_t row = 0; row < image.height; ++row) {
        filtered.push_back(0); // PNG filter type: None
        const auto row_offset = row * image.stride;
        const auto input = image.pixels.subspan(row_offset, layout.row_bytes);
        if (image.format == PixelFormat::rgba8) {
            filtered.insert(filtered.end(), input.begin(), input.end());
            continue;
        }

        for (std::size_t column = 0; column < layout.row_bytes; column += 4) {
            filtered.push_back(input[column + 2]);
            filtered.push_back(input[column + 1]);
            filtered.push_back(input[column]);
            filtered.push_back(input[column + 3]);
        }
    }
    return filtered;
}

ByteBuffer make_zlib_stream(std::span<const std::uint8_t> raw) {
    const auto block_count = checked_add(
        raw.size(), stored_block_size - 1, "PNG DEFLATE block count") /
        stored_block_size;
    const auto block_headers = checked_multiply(
        block_count, 5, "PNG DEFLATE headers");
    auto reserve_size = checked_add(raw.size(), block_headers, "PNG zlib stream");
    reserve_size = checked_add(reserve_size, 6, "PNG zlib stream");

    ByteBuffer zlib;
    zlib.reserve(reserve_size);
    zlib.push_back(0x78); // DEFLATE with a 32 KiB window
    zlib.push_back(0x01); // no preset dictionary, fastest compression level

    std::size_t offset = 0;
    while (offset < raw.size()) {
        const auto count = std::min(stored_block_size, raw.size() - offset);
        const bool final = offset + count == raw.size();
        zlib.push_back(final ? 0x01 : 0x00); // BFINAL followed by BTYPE=00

        const auto length = static_cast<std::uint16_t>(count);
        const auto inverse_length = static_cast<std::uint16_t>(~length);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8));
        zlib.push_back(static_cast<std::uint8_t>(inverse_length));
        zlib.push_back(static_cast<std::uint8_t>(inverse_length >> 8));
        zlib.insert(
            zlib.end(),
            raw.begin() + static_cast<std::ptrdiff_t>(offset),
            raw.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    }
    append_big_endian_u32(zlib, adler32(raw));
    return zlib;
}

} // namespace

ByteBuffer encode_png(const ImageView& image) {
    const auto layout = validate_image(image);
    const auto filtered = make_filtered_pixels(image, layout);
    const auto zlib = make_zlib_stream(filtered);

    const auto idat_count = checked_add(
        zlib.size(), maximum_idat_chunk_size - 1, "PNG IDAT count") /
        maximum_idat_chunk_size;
    auto reserve_size = checked_add(std::size_t{8}, std::size_t{25}, "PNG output");
    reserve_size = checked_add(reserve_size, zlib.size(), "PNG output");
    reserve_size = checked_add(
        reserve_size,
        checked_multiply(idat_count, 12, "PNG IDAT chunks"),
        "PNG output");
    reserve_size = checked_add(reserve_size, 12, "PNG output");

    ByteBuffer png;
    png.reserve(reserve_size);
    constexpr std::array<std::uint8_t, 8> signature{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), signature.begin(), signature.end());

    std::array<std::uint8_t, 13> header{};
    store_big_endian_u32(std::span<std::uint8_t, 4>(header.data(), 4), image.width);
    store_big_endian_u32(std::span<std::uint8_t, 4>(header.data() + 4, 4), image.height);
    header[8] = 8; // bit depth
    header[9] = 6; // RGBA color type
    header[10] = 0; // compression method
    header[11] = 0; // filter method
    header[12] = 0; // no interlacing
    append_chunk(png, "IHDR", header);

    for (std::size_t offset = 0; offset < zlib.size();) {
        const auto count = std::min(maximum_idat_chunk_size, zlib.size() - offset);
        append_chunk(
            png,
            "IDAT",
            std::span<const std::uint8_t>(zlib).subspan(offset, count));
        offset += count;
    }
    append_chunk(png, "IEND", {});
    return png;
}

void write_png(const std::filesystem::path& path, const ImageView& image) {
    if (path.empty()) {
        throw ProtocolError("PNG output path cannot be empty");
    }
    const auto png = encode_png(image);
    if (png.size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        throw ProtocolError("PNG output is too large for a file stream");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw Error("Could not open the requested PNG output path");
    }
    output.write(
        reinterpret_cast<const char*>(png.data()),
        static_cast<std::streamsize>(png.size()));
    output.close();
    if (!output) {
        throw Error("Could not write the complete PNG output file");
    }
}

} // namespace srpc
