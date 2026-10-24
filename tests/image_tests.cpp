#include <srpc/error.hpp>
#include <srpc/image.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, std::string_view message) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

std::uint32_t read_big_endian_u32(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4,
        "PNG parser reached a truncated 32-bit field");
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
        bytes[offset + 3];
}

std::uint32_t test_crc32(std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

std::uint32_t test_adler32(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint32_t modulus = 65521;
    std::uint32_t first = 1;
    std::uint32_t second = 0;
    for (const auto byte : bytes) {
        first = (first + byte) % modulus;
        second = (second + first) % modulus;
    }
    return (second << 16) | first;
}

struct ParsedPng {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    srpc::ByteBuffer filtered_pixels;
    std::size_t idat_chunks = 0;
    std::size_t stored_blocks = 0;
};

ParsedPng parse_png(std::span<const std::uint8_t> png) {
    constexpr std::array<std::uint8_t, 8> signature{
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    require(png.size() >= signature.size(), "PNG was shorter than its signature");
    require(
        std::equal(signature.begin(), signature.end(), png.begin()),
        "PNG signature was invalid");

    ParsedPng parsed;
    srpc::ByteBuffer zlib;
    bool saw_header = false;
    bool saw_end = false;
    std::size_t position = signature.size();
    while (position < png.size()) {
        require(png.size() - position >= 12, "PNG ended inside a chunk");
        const auto length = static_cast<std::size_t>(
            read_big_endian_u32(png, position));
        require(length <= png.size() - position - 12, "PNG chunk length overflowed the file");

        const auto type_offset = position + 4;
        const auto data_offset = position + 8;
        const auto crc_offset = data_offset + length;
        const auto type = std::string_view(
            reinterpret_cast<const char*>(png.data() + type_offset), 4);
        const auto data = png.subspan(data_offset, length);
        require(
            read_big_endian_u32(png, crc_offset) ==
                test_crc32(png.subspan(type_offset, 4 + length)),
            "PNG chunk CRC32 did not match its contents");

        if (type == "IHDR") {
            require(!saw_header && length == 13, "PNG IHDR was missing or malformed");
            parsed.width = read_big_endian_u32(data, 0);
            parsed.height = read_big_endian_u32(data, 4);
            require(
                data[8] == 8 && data[9] == 6 && data[10] == 0 &&
                    data[11] == 0 && data[12] == 0,
                "PNG IHDR did not describe non-interlaced 8-bit RGBA pixels");
            saw_header = true;
        } else if (type == "IDAT") {
            ++parsed.idat_chunks;
            zlib.insert(zlib.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            require(length == 0, "PNG IEND contained unexpected data");
            saw_end = true;
        }
        position += length + 12;
    }
    require(saw_header && saw_end && parsed.idat_chunks != 0,
        "PNG did not contain its required chunks");
    require(zlib.size() >= 6, "PNG zlib stream was truncated");
    require((zlib[0] & 0x0F) == 8, "PNG zlib stream did not use DEFLATE");
    require(
        ((static_cast<std::uint32_t>(zlib[0]) << 8) | zlib[1]) % 31 == 0,
        "PNG zlib header check bits were invalid");
    require((zlib[1] & 0x20) == 0, "PNG zlib stream unexpectedly used a dictionary");

    const auto deflate_end = zlib.size() - 4;
    position = 2;
    bool saw_final_block = false;
    while (position < deflate_end) {
        const auto block_header = zlib[position++];
        require(block_header == 0 || block_header == 1,
            "PNG did not use byte-aligned stored DEFLATE blocks");
        require(deflate_end - position >= 4, "PNG stored block header was truncated");
        const auto length = static_cast<std::uint16_t>(
            zlib[position] | (static_cast<std::uint16_t>(zlib[position + 1]) << 8));
        const auto inverse = static_cast<std::uint16_t>(
            zlib[position + 2] |
            (static_cast<std::uint16_t>(zlib[position + 3]) << 8));
        position += 4;
        require(static_cast<std::uint16_t>(length ^ inverse) == 0xFFFFu,
            "PNG stored block length complement was invalid");
        require(length <= deflate_end - position, "PNG stored block data was truncated");
        parsed.filtered_pixels.insert(
            parsed.filtered_pixels.end(),
            zlib.begin() + static_cast<std::ptrdiff_t>(position),
            zlib.begin() + static_cast<std::ptrdiff_t>(position + length));
        position += length;
        ++parsed.stored_blocks;

        if ((block_header & 1u) != 0) {
            saw_final_block = true;
            break;
        }
    }
    require(saw_final_block && position == deflate_end,
        "PNG DEFLATE stream did not end at its final block");
    require(
        read_big_endian_u32(zlib, deflate_end) == test_adler32(parsed.filtered_pixels),
        "PNG zlib Adler-32 did not match the filtered pixels");
    return parsed;
}

srpc::ByteBuffer read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "PNG test output file could not be opened");
    srpc::ByteBuffer bytes;
    char value = 0;
    while (input.get(value)) {
        bytes.push_back(static_cast<std::uint8_t>(
            static_cast<unsigned char>(value)));
    }
    require(input.eof(), "PNG test output file could not be read completely");
    return bytes;
}

struct TemporaryFile {
    std::filesystem::path path;

    ~TemporaryFile() {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(path, error));
    }
};

void rgba_and_file_tests() {
    const std::array<std::uint8_t, 24> pixels{
        1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,
        10, 11, 12, 13, 14, 15, 16, 17, 0xDD, 0xDD, 0xDD, 0xDD,
    };
    const srpc::ImageView image{
        std::span<const std::uint8_t>(pixels),
        2,
        2,
        12,
        srpc::PixelFormat::rgba8,
    };

    const auto png = srpc::encode_png(image);
    require(png == srpc::encode_png(image), "PNG encoding was not deterministic");
    const auto parsed = parse_png(png);
    require(parsed.width == 2 && parsed.height == 2, "PNG dimensions changed");
    require(
        parsed.filtered_pixels == srpc::ByteBuffer{
            0, 1, 2, 3, 4, 5, 6, 7, 8,
            0, 10, 11, 12, 13, 14, 15, 16, 17},
        "RGBA rows or stride padding were encoded incorrectly");

    TemporaryFile output{
        std::filesystem::temp_directory_path() / "srpc_image_tests_output.png"};
    std::error_code remove_error;
    static_cast<void>(std::filesystem::remove(output.path, remove_error));
    srpc::write_png(output.path, image);
    require(read_file(output.path) == png, "write_png changed the encoded bytes");
}

void bgra_test() {
    const std::array<std::uint8_t, 8> pixels{3, 2, 1, 4, 7, 6, 5, 8};
    const auto parsed = parse_png(srpc::encode_png({
        std::span<const std::uint8_t>(pixels),
        2,
        1,
        8,
        srpc::PixelFormat::bgra8,
    }));
    require(
        parsed.filtered_pixels ==
            srpc::ByteBuffer{0, 1, 2, 3, 4, 5, 6, 7, 8},
        "BGRA pixels were not converted to RGBA order");
}

void large_stored_stream_test() {
    constexpr std::uint32_t width = 270000;
    srpc::ByteBuffer pixels(static_cast<std::size_t>(width) * 4, 0x5A);
    const auto parsed = parse_png(srpc::encode_png({
        pixels,
        width,
        1,
        pixels.size(),
        srpc::PixelFormat::rgba8,
    }));
    require(parsed.stored_blocks > 1, "large PNG did not split stored DEFLATE blocks");
    require(parsed.idat_chunks > 1, "large PNG did not split oversized IDAT data");
    require(parsed.filtered_pixels.size() == pixels.size() + 1,
        "large PNG filtered data size changed");
    require(parsed.filtered_pixels.front() == 0,
        "large PNG omitted its scanline filter byte");
    require(
        std::all_of(
            parsed.filtered_pixels.begin() + 1,
            parsed.filtered_pixels.end(),
            [](std::uint8_t value) { return value == 0x5A; }),
        "large PNG stored block data changed");
}

void validation_tests() {
    const std::array<std::uint8_t, 8> pixels{};
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({pixels, 0, 1, 4}));
        },
        "PNG accepted a zero width");
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({pixels, 0x80000000u, 1, 4}));
        },
        "PNG accepted a dimension outside its 31-bit limit");
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({pixels, 2, 1, 7}));
        },
        "PNG accepted a short stride");
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({
                std::span<const std::uint8_t>(pixels).first(4), 2, 1, 8}));
        },
        "PNG accepted a short pixel buffer");
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({
                pixels, 1, 1, 4, static_cast<srpc::PixelFormat>(0xFF)}));
        },
        "PNG accepted an unknown pixel format");
    require_throws<srpc::ProtocolError>(
        [&] {
            static_cast<void>(srpc::encode_png({
                pixels,
                1,
                2,
                std::numeric_limits<std::size_t>::max(),
                srpc::PixelFormat::rgba8}));
        },
        "PNG accepted overflowing stride arithmetic");
    require_throws<srpc::ProtocolError>(
        [&] {
            srpc::write_png({}, {pixels, 1, 1, 4});
        },
        "PNG accepted an empty output path");
}

} // namespace

int main() {
    try {
        rgba_and_file_tests();
        bgra_test();
        large_stored_stream_test();
        validation_tests();
        std::cout << "All SRPC image tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
