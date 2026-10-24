#include "srpc/screenshot.hpp"

#include "srpc/error.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace srpc {
namespace {

constexpr std::size_t bytes_per_pixel = 4;
constexpr std::uint32_t alternate_format_marker = 0x80000000u;

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

std::size_t image_size(
    std::uint32_t width,
    std::uint32_t height,
    std::size_t maximum_size,
    std::string_view description) {
    if (width == 0 || height == 0) {
        throw ProtocolError(std::string(description) + " dimensions must be nonzero");
    }
    const auto pixels = checked_multiply(
        static_cast<std::size_t>(width),
        static_cast<std::size_t>(height),
        description);
    const auto size = checked_multiply(pixels, bytes_per_pixel, description);
    if (maximum_size == 0) {
        throw ProtocolError("Maximum decoded screenshot size must be greater than zero");
    }
    if (size > maximum_size) {
        throw ProtocolError(
            std::string(description) + " requires " + std::to_string(size) +
            " bytes, over the configured limit of " +
            std::to_string(maximum_size));
    }
    return size;
}

std::uint64_t align_32(std::uint64_t value) {
    return (value + 31u) & ~std::uint64_t{31};
}

std::uint64_t xenos_tiled_offset(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t pitch_pixels) {
    const auto aligned_width = align_32(pitch_pixels);
    const auto macro_tiles_per_row = aligned_width >> 5;
    const auto outer_blocks =
        ((static_cast<std::uint64_t>(y >> 5) * macro_tiles_per_row) +
         static_cast<std::uint64_t>(x >> 5))
        << 6;
    const auto inner_blocks =
        ((static_cast<std::uint64_t>((y >> 1) & 7u) << 3) |
         static_cast<std::uint64_t>(x & 7u));
    const auto outer_inner_bytes = (outer_blocks | inner_blocks) << 2;
    const auto bank = static_cast<std::uint64_t>((y >> 4) & 1u);
    const auto pipe = static_cast<std::uint64_t>(
        ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1));

    return
        (outer_inner_bytes & 0xFu) |
        (static_cast<std::uint64_t>(y & 1u) << 4) |
        (((outer_inner_bytes >> 4) & 1u) << 5) |
        (pipe << 6) |
        (((outer_inner_bytes >> 5) & 7u) << 8) |
        (bank << 11) |
        ((outer_inner_bytes >> 8) << 12);
}

std::uint32_t spread_morton_bits(std::uint32_t value) {
    value &= 0x0000FFFFu;
    value = (value | (value << 8)) & 0x00FF00FFu;
    value = (value | (value << 4)) & 0x0F0F0F0Fu;
    value = (value | (value << 2)) & 0x33333333u;
    value = (value | (value << 1)) & 0x55555555u;
    return value;
}

std::uint64_t morton_tiled_offset(
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t aligned_width) {
    const auto macro_tile_pitch = static_cast<std::uint64_t>(aligned_width >> 5);
    const auto macro_tile_index =
        static_cast<std::uint64_t>(x >> 5) +
        static_cast<std::uint64_t>(y >> 5) * macro_tile_pitch;
    const auto macro_tile_offset = macro_tile_index * 32u * 32u * bytes_per_pixel;
    const auto morton =
        spread_morton_bits(x & 31u) |
        (spread_morton_bits(y & 31u) << 1);
    return macro_tile_offset +
        static_cast<std::uint64_t>(morton) * bytes_per_pixel;
}

std::uint32_t infer_morton_width(
    std::uint32_t pitch_pixels,
    std::uint32_t height,
    std::size_t framebuffer_size) {
    const auto minimum_width = align_32(pitch_pixels);
    const auto minimum_height = align_32(height);
    const auto pixels = framebuffer_size / bytes_per_pixel;

    for (std::uint64_t candidate = minimum_width, attempts = 0;
         attempts < 64;
         candidate += 32, ++attempts) {
        if (candidate > std::numeric_limits<std::uint32_t>::max()) {
            break;
        }
        if (candidate != 0 && pixels % candidate == 0) {
            const auto candidate_height = pixels / candidate;
            if (candidate_height >= minimum_height &&
                candidate_height % 32 == 0) {
                return static_cast<std::uint32_t>(candidate);
            }
        }
    }
    if (minimum_width > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("Morton screenshot width overflow");
    }
    return static_cast<std::uint32_t>(minimum_width);
}

ScreenshotImage untile_visible(
    const RawScreenshot& screenshot,
    ScreenshotOptions options) {
    const auto& metadata = screenshot.metadata;
    const auto format = metadata.format.has_value()
        ? std::optional<std::uint32_t>(
              *metadata.format & ~alternate_format_marker)
        : std::nullopt;
    if (format.has_value() &&
        *format != static_cast<std::uint32_t>(ScreenshotFormat::a8r8g8b8) &&
        *format != static_cast<std::uint32_t>(ScreenshotFormat::a2r10g10b10)) {
        throw ProtocolError(
            "Unsupported XBDM screenshot format " +
            std::to_string(*metadata.format));
    }
    if (metadata.pitch_bytes == 0 ||
        metadata.pitch_bytes % bytes_per_pixel != 0) {
        throw ProtocolError("Screenshot pitch must be a nonzero multiple of four");
    }
    const auto pitch_pixels =
        metadata.pitch_bytes / static_cast<std::uint32_t>(bytes_per_pixel);
    if (metadata.width == 0 || metadata.height == 0) {
        throw ProtocolError("Screenshot width and height must be nonzero");
    }
    if (pitch_pixels < metadata.width) {
        throw ProtocolError("Screenshot pitch is smaller than the visible width");
    }
    if (metadata.framebuffer_size != screenshot.tiled_framebuffer.size()) {
        throw ProtocolError(
            "Screenshot framebuffer size does not match the captured byte count");
    }
    if (metadata.framebuffer_size == 0 ||
        metadata.framebuffer_size % bytes_per_pixel != 0) {
        throw ProtocolError(
            "Screenshot framebuffer size must be a nonzero multiple of four");
    }
    if (options.maximum_framebuffer_size == 0) {
        throw ProtocolError("Maximum screenshot framebuffer size must be greater than zero");
    }
    if (screenshot.tiled_framebuffer.size() > options.maximum_framebuffer_size) {
        throw ProtocolError(
            "Screenshot framebuffer exceeds the configured decode limit");
    }

    ScreenshotImage result;
    result.source = metadata;
    result.width = metadata.width;
    result.height = metadata.height;
    result.bgra.resize(image_size(
        result.width,
        result.height,
        options.maximum_decoded_size,
        "Decoded screenshot"));

    std::uint32_t morton_width = 0;
    switch (options.untile_mode) {
    case ScreenshotUntileMode::xenos:
        break;
    case ScreenshotUntileMode::morton:
        morton_width = infer_morton_width(
            pitch_pixels, metadata.height, screenshot.tiled_framebuffer.size());
        break;
    default:
        throw ProtocolError("Unknown screenshot untile mode");
    }

    for (std::uint32_t y = 0; y < metadata.height; ++y) {
        for (std::uint32_t x = 0; x < metadata.width; ++x) {
            const auto source = options.untile_mode == ScreenshotUntileMode::xenos
                ? xenos_tiled_offset(x, y, pitch_pixels)
                : morton_tiled_offset(x, y, morton_width);
            if (source > screenshot.tiled_framebuffer.size() ||
                screenshot.tiled_framebuffer.size() -
                    static_cast<std::size_t>(source) <
                    bytes_per_pixel) {
                throw ProtocolError(
                    "Screenshot framebuffer is too small for its tiled dimensions");
            }
            const auto destination =
                (static_cast<std::size_t>(y) * metadata.width + x) *
                bytes_per_pixel;
            const auto* input =
                screenshot.tiled_framebuffer.data() +
                static_cast<std::size_t>(source);
            auto* output = result.bgra.data() + destination;
            if (!format.has_value()) {
                // The Python client and older third-party XBDM clients treat a
                // header without `format` as already byte-ordered BGRA.
                std::memcpy(output, input, bytes_per_pixel);
                if (!options.preserve_alpha) {
                    output[3] = 0xFF;
                }
            } else if (
                *format ==
                static_cast<std::uint32_t>(ScreenshotFormat::a8r8g8b8)) {
                // Real XBDM frontbuffer captures use PC-ready B,G,R,A/X bytes.
                std::memcpy(output, input, bytes_per_pixel);
                if (!options.preserve_alpha) {
                    output[3] = 0xFF;
                }
            } else {
                std::uint32_t packed = 0;
                switch (options.packed_10bit_endian) {
                case Endian::little:
                    packed =
                        static_cast<std::uint32_t>(input[0]) |
                        (static_cast<std::uint32_t>(input[1]) << 8) |
                        (static_cast<std::uint32_t>(input[2]) << 16) |
                        (static_cast<std::uint32_t>(input[3]) << 24);
                    break;
                case Endian::big:
                    packed =
                        (static_cast<std::uint32_t>(input[0]) << 24) |
                        (static_cast<std::uint32_t>(input[1]) << 16) |
                        (static_cast<std::uint32_t>(input[2]) << 8) |
                        static_cast<std::uint32_t>(input[3]);
                    break;
                default:
                    throw ProtocolError(
                        "Unknown packed 10-bit screenshot byte order");
                }
                const auto blue = packed & 0x3FFu;
                const auto green = (packed >> 10) & 0x3FFu;
                const auto red = (packed >> 20) & 0x3FFu;
                const auto alpha = (packed >> 30) & 3u;
                output[0] = static_cast<std::uint8_t>((blue * 255u + 511u) / 1023u);
                output[1] = static_cast<std::uint8_t>((green * 255u + 511u) / 1023u);
                output[2] = static_cast<std::uint8_t>((red * 255u + 511u) / 1023u);
                output[3] = options.preserve_alpha
                    ? static_cast<std::uint8_t>((alpha * 255u + 1u) / 3u)
                    : std::uint8_t{0xFF};
            }
        }
    }
    return result;
}

ScreenshotImage resize_nearest(
    const ScreenshotImage& source,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t maximum_size) {
    ScreenshotImage result;
    result.source = source.source;
    result.width = width;
    result.height = height;
    result.bgra.resize(image_size(
        width, height, maximum_size, "Composed screenshot"));

    for (std::uint32_t y = 0; y < height; ++y) {
        const auto source_y = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * source.height) / height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto source_x = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * source.width) / width);
            const auto source_offset =
                (static_cast<std::size_t>(source_y) * source.width + source_x) *
                bytes_per_pixel;
            const auto destination =
                (static_cast<std::size_t>(y) * width + x) * bytes_per_pixel;
            std::memcpy(
                result.bgra.data() + destination,
                source.bgra.data() + source_offset,
                bytes_per_pixel);
        }
    }
    return result;
}

ScreenshotImage compose_with_offset(
    const ScreenshotImage& source,
    const ScreenshotMetadata& metadata,
    std::uint32_t display_width,
    std::uint32_t display_height,
    std::size_t maximum_size) {
    const auto right = checked_add(
        static_cast<std::size_t>(metadata.offset_x),
        static_cast<std::size_t>(source.width),
        "Screenshot horizontal offset");
    const auto bottom = checked_add(
        static_cast<std::size_t>(metadata.offset_y),
        static_cast<std::size_t>(source.height),
        "Screenshot vertical offset");
    if (right > display_width || bottom > display_height) {
        throw ProtocolError(
            "Screenshot visible rectangle does not fit its display surface");
    }

    ScreenshotImage result;
    result.source = source.source;
    result.width = display_width;
    result.height = display_height;
    result.bgra.resize(image_size(
        display_width,
        display_height,
        maximum_size,
        "Composed screenshot"));
    for (std::size_t index = 3; index < result.bgra.size(); index += 4) {
        result.bgra[index] = 0xFF;
    }

    const auto source_row_size =
        static_cast<std::size_t>(source.width) * bytes_per_pixel;
    for (std::uint32_t y = 0; y < source.height; ++y) {
        const auto source_offset =
            static_cast<std::size_t>(y) * source_row_size;
        const auto destination_offset =
            ((static_cast<std::size_t>(y) + metadata.offset_y) * display_width +
             metadata.offset_x) * bytes_per_pixel;
        std::memcpy(
            result.bgra.data() + destination_offset,
            source.bgra.data() + source_offset,
            source_row_size);
    }
    return result;
}

} // namespace

ScreenshotImage decode_screenshot(
    const RawScreenshot& screenshot,
    ScreenshotOptions options) {
    switch (options.packed_10bit_endian) {
    case Endian::little:
    case Endian::big:
        break;
    default:
        throw ProtocolError("Unknown packed 10-bit screenshot byte order");
    }
    auto visible = untile_visible(screenshot, options);
    if (!options.compose_display_surface) {
        return visible;
    }

    const auto display_width = screenshot.metadata.display_width == 0
        ? screenshot.metadata.width
        : screenshot.metadata.display_width;
    const auto display_height = screenshot.metadata.display_height == 0
        ? screenshot.metadata.height
        : screenshot.metadata.display_height;
    if (display_width == 0 || display_height == 0) {
        throw ProtocolError("Screenshot display dimensions must be nonzero");
    }

    if (screenshot.metadata.offset_x != 0 ||
        screenshot.metadata.offset_y != 0) {
        return compose_with_offset(
            visible,
            screenshot.metadata,
            display_width,
            display_height,
            options.maximum_decoded_size);
    }
    if (display_width != visible.width || display_height != visible.height) {
        return resize_nearest(
            visible,
            display_width,
            display_height,
            options.maximum_decoded_size);
    }
    return visible;
}

} // namespace srpc
