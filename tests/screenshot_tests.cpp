#include <srpc/error.hpp>
#include <srpc/screenshot.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Pixel = std::array<std::uint8_t, 4>;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void place_pixel(
    srpc::RawScreenshot& screenshot,
    std::size_t byte_offset,
    Pixel pixel) {
    require(
        byte_offset + pixel.size() <= screenshot.tiled_framebuffer.size(),
        "test pixel does not fit the tiled fixture");
    for (std::size_t index = 0; index < pixel.size(); ++index) {
        screenshot.tiled_framebuffer[byte_offset + index] = pixel[index];
    }
}

Pixel image_pixel(
    const srpc::ScreenshotImage& image,
    std::uint32_t x,
    std::uint32_t y) {
    require(x < image.width && y < image.height, "test pixel is outside the image");
    const auto offset =
        (static_cast<std::size_t>(y) * image.width + x) * 4;
    Pixel result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = image.bgra[offset + index];
    }
    return result;
}

srpc::RawScreenshot make_capture(
    std::uint32_t pitch,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t framebuffer_size) {
    srpc::RawScreenshot screenshot;
    screenshot.metadata.pitch_bytes = pitch;
    screenshot.metadata.width = width;
    screenshot.metadata.height = height;
    screenshot.metadata.framebuffer_size = framebuffer_size;
    screenshot.tiled_framebuffer.resize(framebuffer_size);
    return screenshot;
}

void xenos_golden_test() {
    auto screenshot = make_capture(0x80, 32, 32, 0x1000);
    struct GoldenPixel {
        std::uint32_t x;
        std::uint32_t y;
        std::size_t tiled_offset;
        Pixel pixel;
    };
    const std::vector<GoldenPixel> golden{
        {0, 0, 0x000, {1, 2, 3, 4}},
        {1, 0, 0x004, {5, 6, 7, 8}},
        {0, 1, 0x010, {9, 10, 11, 12}},
        {4, 0, 0x020, {13, 14, 15, 16}},
        {8, 0, 0x040, {17, 18, 19, 20}},
        {16, 0, 0x080, {21, 22, 23, 24}},
        {0, 2, 0x100, {25, 26, 27, 28}},
        {0, 8, 0x480, {29, 30, 31, 32}},
        {0, 16, 0x800, {33, 34, 35, 36}},
        {15, 31, 0xFFC, {37, 38, 39, 40}},
    };
    for (const auto& item : golden) {
        place_pixel(screenshot, item.tiled_offset, item.pixel);
    }

    srpc::ScreenshotOptions options;
    options.compose_display_surface = false;
    options.preserve_alpha = true;
    const auto image = srpc::decode_screenshot(screenshot, options);
    require(image.width == 32 && image.height == 32, "Xenos dimensions changed");
    require(image.bgra.size() == 0x1000, "Xenos output byte count is wrong");
    for (const auto& item : golden) {
        require(
            image_pixel(image, item.x, item.y) == item.pixel,
            "Xenos tiled-address golden vector failed");
    }
}

void xenos_macro_tile_test() {
    auto screenshot = make_capture(0x100, 64, 64, 0x4000);
    const Pixel top_right{1, 3, 5, 7};
    const Pixel bottom_left{2, 4, 6, 8};
    const Pixel bottom_right{9, 10, 11, 12};
    place_pixel(screenshot, 0x1000, top_right);
    place_pixel(screenshot, 0x2000, bottom_left);
    place_pixel(screenshot, 0x3000, bottom_right);

    srpc::ScreenshotOptions options;
    options.compose_display_surface = false;
    options.preserve_alpha = true;
    const auto image = srpc::decode_screenshot(screenshot, options);
    require(image_pixel(image, 32, 0) == top_right, "right macro tile is wrong");
    require(image_pixel(image, 0, 32) == bottom_left, "lower macro tile is wrong");
    require(
        image_pixel(image, 32, 32) == bottom_right,
        "diagonal macro tile is wrong");
}

void morton_golden_test() {
    auto screenshot = make_capture(0x80, 32, 32, 0x1000);
    struct GoldenPixel {
        std::uint32_t x;
        std::uint32_t y;
        std::size_t tiled_offset;
        Pixel pixel;
    };
    const std::vector<GoldenPixel> golden{
        {0, 0, 0x000, {1, 1, 1, 1}},
        {1, 0, 0x004, {2, 2, 2, 2}},
        {0, 1, 0x008, {3, 3, 3, 3}},
        {1, 1, 0x00C, {4, 4, 4, 4}},
        {2, 0, 0x010, {5, 5, 5, 5}},
        {0, 2, 0x020, {6, 6, 6, 6}},
        {31, 31, 0xFFC, {7, 7, 7, 7}},
    };
    for (const auto& item : golden) {
        place_pixel(screenshot, item.tiled_offset, item.pixel);
    }

    srpc::ScreenshotOptions options;
    options.untile_mode = srpc::ScreenshotUntileMode::morton;
    options.compose_display_surface = false;
    options.preserve_alpha = true;
    const auto image = srpc::decode_screenshot(screenshot, options);
    for (const auto& item : golden) {
        require(
            image_pixel(image, item.x, item.y) == item.pixel,
            "Morton tiled-address golden vector failed");
    }
}

void crop_compose_and_scale_test() {
    auto screenshot = make_capture(0x80, 2, 1, 0x1000);
    const Pixel left{1, 2, 3, 4};
    const Pixel right{5, 6, 7, 8};
    place_pixel(screenshot, 0x000, left);
    place_pixel(screenshot, 0x004, right);

    srpc::ScreenshotOptions visible_options;
    visible_options.compose_display_surface = false;
    visible_options.preserve_alpha = true;
    const auto visible = srpc::decode_screenshot(screenshot, visible_options);
    require(
        visible.width == 2 && visible.height == 1 && visible.bgra.size() == 8,
        "pitch padding was not cropped from the visible image");
    require(image_pixel(visible, 0, 0) == left, "cropped left pixel is wrong");
    require(image_pixel(visible, 1, 0) == right, "cropped right pixel is wrong");

    screenshot.metadata.display_width = 4;
    screenshot.metadata.display_height = 3;
    screenshot.metadata.offset_x = 1;
    screenshot.metadata.offset_y = 1;
    srpc::ScreenshotOptions composed_options;
    composed_options.compose_display_surface = true;
    composed_options.preserve_alpha = true;
    const auto composed = srpc::decode_screenshot(screenshot, composed_options);
    require(
        composed.width == 4 && composed.height == 3,
        "display-surface composition dimensions are wrong");
    require(image_pixel(composed, 1, 1) == left, "composed left pixel is wrong");
    require(image_pixel(composed, 2, 1) == right, "composed right pixel is wrong");
    require(
        image_pixel(composed, 0, 0) == Pixel{0, 0, 0, 0xFF},
        "composed canvas is not opaque black");

    screenshot.metadata.offset_x = 0;
    screenshot.metadata.offset_y = 0;
    screenshot.metadata.display_height = 2;
    const auto scaled = srpc::decode_screenshot(screenshot, composed_options);
    require(scaled.width == 4 && scaled.height == 2, "scaled dimensions are wrong");
    require(image_pixel(scaled, 0, 0) == left, "nearest left pixel is wrong");
    require(image_pixel(scaled, 1, 1) == left, "nearest left repeat is wrong");
    require(image_pixel(scaled, 2, 0) == right, "nearest right pixel is wrong");
    require(image_pixel(scaled, 3, 1) == right, "nearest right repeat is wrong");
}

void pixel_format_test() {
    auto screenshot = make_capture(0x80, 1, 1, 0x1000);
    place_pixel(screenshot, 0, {0x40, 0x30, 0x20, 0x10});
    screenshot.metadata.format = static_cast<std::uint32_t>(
        srpc::ScreenshotFormat::a8r8g8b8);
    const auto bgra = srpc::decode_screenshot(screenshot);
    require(
        image_pixel(bgra, 0, 0) == Pixel{0x40, 0x30, 0x20, 0xFF},
        "A8R8G8B8 screenshot bytes were not preserved as BGRA");

    screenshot.metadata.format =
        static_cast<std::uint32_t>(srpc::ScreenshotFormat::a8r8g8b8) |
        0x80000000u;
    const auto marked_bgra = srpc::decode_screenshot(screenshot);
    require(
        image_pixel(marked_bgra, 0, 0) == Pixel{0x40, 0x30, 0x20, 0xFF},
        "alternate XBDM format marker was not normalized");

    place_pixel(screenshot, 0, {0x00, 0x00, 0xF8, 0xFF});
    screenshot.metadata.format = static_cast<std::uint32_t>(
        srpc::ScreenshotFormat::a2r10g10b10);
    const auto ten_bit = srpc::decode_screenshot(screenshot);
    require(
        image_pixel(ten_bit, 0, 0) == Pixel{0, 128, 255, 255},
        "A2R10G10B10 screenshot was not converted to BGRA8");

    place_pixel(screenshot, 0, {0xFF, 0xF8, 0x00, 0x00});
    srpc::ScreenshotOptions big_endian;
    big_endian.packed_10bit_endian = srpc::Endian::big;
    const auto ten_bit_big = srpc::decode_screenshot(screenshot, big_endian);
    require(
        image_pixel(ten_bit_big, 0, 0) == Pixel{0, 128, 255, 255},
        "big-endian A2R10G10B10 override was not honored");
}

void validation_test() {
    auto screenshot = make_capture(0x80, 32, 32, 0x1000);
    screenshot.tiled_framebuffer.pop_back();
    bool mismatch_rejected = false;
    try {
        static_cast<void>(srpc::decode_screenshot(screenshot));
    } catch (const srpc::ProtocolError&) {
        mismatch_rejected = true;
    }
    require(mismatch_rejected, "framebuffer byte-count mismatch was accepted");

    screenshot = make_capture(0x80, 32, 32, 4);
    bool truncated_rejected = false;
    try {
        static_cast<void>(srpc::decode_screenshot(screenshot));
    } catch (const srpc::ProtocolError&) {
        truncated_rejected = true;
    }
    require(truncated_rejected, "truncated tiled framebuffer was accepted");

    screenshot = make_capture(0x80, 32, 32, 0x1000);
    srpc::ScreenshotOptions limited;
    limited.maximum_decoded_size = 0xFFF;
    bool limit_rejected = false;
    try {
        static_cast<void>(srpc::decode_screenshot(screenshot, limited));
    } catch (const srpc::ProtocolError&) {
        limit_rejected = true;
    }
    require(limit_rejected, "decoded screenshot limit was not enforced");

    screenshot.metadata.format = 0x12345678;
    bool format_rejected = false;
    try {
        static_cast<void>(srpc::decode_screenshot(screenshot));
    } catch (const srpc::ProtocolError&) {
        format_rejected = true;
    }
    require(format_rejected, "unknown screenshot format was accepted");
    screenshot.metadata.format.reset();

    screenshot.metadata.pitch_bytes = 127;
    bool pitch_rejected = false;
    try {
        static_cast<void>(srpc::decode_screenshot(screenshot));
    } catch (const srpc::ProtocolError&) {
        pitch_rejected = true;
    }
    require(pitch_rejected, "non-pixel-aligned screenshot pitch was accepted");
}

} // namespace

int main() {
    try {
        xenos_golden_test();
        xenos_macro_tile_test();
        morton_golden_test();
        crop_compose_and_scale_test();
        pixel_format_test();
        validation_test();
        std::cout << "All SRPC screenshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
