// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/renderer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr vx::render::Extent image_size = {.width = 64, .height = 32};

vx::Result<vx::render::Renderer> headless_renderer() {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = image_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo});
}

using Pixel = std::array<std::uint8_t, 4>;

Pixel pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t x,
               std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * width + x) * 4;
    return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

// Whether two pixels are within one step in every channel, a margin for how
// each driver rounds while encoding to sRGB.
bool near(const Pixel& pixel, const Pixel& expected) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(int{pixel[channel]} - int{expected[channel]}) > 1) {
            return false;
        }
    }
    return true;
}

// How many pixels in rows [first, end) are not near `expected`.
std::size_t pixels_off(const std::vector<std::uint8_t>& pixels, std::uint32_t width,
                       std::uint32_t first, std::uint32_t end, const Pixel& expected) {
    std::size_t off = 0;
    for (std::uint32_t y = first; y < end; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (!near(pixel_at(pixels, width, x, y), expected)) {
                ++off;
            }
        }
    }
    return off;
}

} // namespace

TEST_CASE("a headless renderer clears around the triangle to the color it was given") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    CHECK(renderer.swapchain() == nullptr);

    // Linear 1, 0 and 0.214 encode to sRGB 255, 0 and 127.5.
    for (int frame = 0; frame < 3; ++frame) {
        const vx::Result<vx::render::Frame> drawn =
            renderer.render({.r = 1.0F, .g = 0.0F, .b = 0.214F, .a = 1.0F});
        REQUIRE(drawn.has_value());
        CHECK(drawn.value() == vx::render::Frame::drawn);
    }
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    INFO((pixels.has_value() ? std::string() : pixels.error().message));
    REQUIRE(pixels.has_value());
    REQUIRE(pixels.value().size() == std::size_t{image_size.width} * image_size.height * 4);

    // The rows above and below the image's middle keep the clear color: the
    // middle is where the first pipeline will draw.
    CHECK(pixels_off(pixels.value(), image_size.width, 0, 6, {255, 0, 128, 255}) == 0);
    CHECK(pixels_off(pixels.value(), image_size.width, 27, 32, {255, 0, 128, 255}) == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a headless renderer follows a resize") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    REQUIRE(renderer.render({}).has_value());
    renderer.resize({.width = 16, .height = 8});
    REQUIRE(renderer.render({.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F}).has_value());
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    REQUIRE(pixels.value().size() == std::size_t{16} * 8 * 4);
    // The image's corners keep the new color.
    for (const auto [x, y] :
         std::array{std::array<std::uint32_t, 2>{0, 0}, std::array<std::uint32_t, 2>{15, 0},
                    std::array<std::uint32_t, 2>{0, 7}, std::array<std::uint32_t, 2>{15, 7}}) {
        CHECK(near(pixel_at(pixels.value(), 16, x, y), {0, 255, 0, 255}));
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
