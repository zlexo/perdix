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

// Whether channel `strong` is over twice each other color channel.
bool leans_to(const Pixel& pixel, std::size_t strong) {
    for (std::size_t channel = 0; channel < 3; ++channel) {
        if (channel != strong && pixel[strong] <= 2 * pixel[channel]) {
            return false;
        }
    }
    return true;
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

    // The triangle spans rows 7 to 25 of the 32: the rows above and below it
    // keep the clear color.
    CHECK(pixels_off(pixels.value(), image_size.width, 0, 6, {255, 0, 128, 255}) == 0);
    CHECK(pixels_off(pixels.value(), image_size.width, 27, 32, {255, 0, 128, 255}) == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a headless renderer draws the triangle with each vertex's color at its corner") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    REQUIRE(renderer.render({.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}).has_value());
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    // The vertices sit at (32, 6.4), (51.2, 25.6) and (12.8, 25.6) in pixels.
    // Near each, its own color outweighs the others: the vertex shader pulled
    // the vertices from their buffer, in order.
    const std::uint32_t width = image_size.width;
    CHECK(leans_to(pixel_at(pixels.value(), width, 32, 9), 0));  // red, top
    CHECK(leans_to(pixel_at(pixels.value(), width, 47, 24), 1)); // green, right
    CHECK(leans_to(pixel_at(pixels.value(), width, 16, 24), 2)); // blue, left
    CHECK(near(pixel_at(pixels.value(), width, 0, 0), {0, 0, 0, 255}));
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("where the triangles overlap, the nearer shows, though it is drawn first") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();

    REQUIRE(renderer.render({.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F}).has_value());
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    // The gray triangle is drawn second, at depth 0.25; the colored one first,
    // at 0.75, nearer under reversed-Z. Linear gray 0.5 encodes to sRGB 188.
    const std::uint32_t width = image_size.width;
    const Pixel gray_only = pixel_at(pixels.value(), width, 14, 10);
    const Pixel both = pixel_at(pixels.value(), width, 32, 16);
    CHECK(near(gray_only, {188, 188, 188, 255}));
    CHECK_FALSE(near(both, {188, 188, 188, 255}));
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
    // The triangle shrank with the image; its corners keep the new color.
    for (const auto [x, y] :
         std::array{std::array<std::uint32_t, 2>{0, 0}, std::array<std::uint32_t, 2>{15, 0},
                    std::array<std::uint32_t, 2>{0, 7}, std::array<std::uint32_t, 2>{15, 7}}) {
        CHECK(near(pixel_at(pixels.value(), 16, x, y), {0, 255, 0, 255}));
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
