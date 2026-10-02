// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/camera.hpp>
#include <vx/render/renderer.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_int3_sized.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr vx::render::Extent image_size = {.width = 64, .height = 32};
constexpr vx::render::Color black = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F};

// Two blocks in front of a cube at the origin, level with its middle,
// looking at its front face, +z.
vx::render::Camera in_front() {
    vx::render::Camera camera;
    camera.position = {0.5, 0.5, 3.0};
    return camera;
}

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

TEST_CASE("the sky shows around a cube, in the color it was given") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    CHECK(renderer.swapchain() == nullptr);

    // Linear 1, 0 and 0.214 encode to sRGB 255, 0 and 127.5.
    const std::array cubes = {glm::i64vec3(0, 0, 0)};
    for (int frame = 0; frame < 3; ++frame) {
        const vx::Result<vx::render::Frame> drawn =
            renderer.render({.sky = {.r = 1.0F, .g = 0.0F, .b = 0.214F, .a = 1.0F},
                             .camera = in_front(),
                             .cubes = cubes});
        REQUIRE(drawn.has_value());
        CHECK(drawn.value() == vx::render::Frame::drawn);
    }
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    INFO((pixels.has_value() ? std::string() : pixels.error().message));
    REQUIRE(pixels.has_value());
    REQUIRE(pixels.value().size() == std::size_t{image_size.width} * image_size.height * 4);

    // The cube's front face, two blocks away, spans rows 11 to 21 of the 32:
    // the rows above and below it show the sky.
    CHECK(pixels_off(pixels.value(), image_size.width, 0, 9, {255, 0, 128, 255}) == 0);
    CHECK(pixels_off(pixels.value(), image_size.width, 24, 32, {255, 0, 128, 255}) == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
    CHECK(renderer.context().instance().validation_counts().warnings == 0);
}

TEST_CASE("a headless renderer follows a resize") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    const std::array cubes = {glm::i64vec3(0, 0, 0)};

    REQUIRE(renderer.render({.sky = black, .camera = in_front(), .cubes = cubes}).has_value());
    renderer.resize({.width = 16, .height = 8});
    REQUIRE(renderer
                .render({.sky = {.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F},
                         .camera = in_front(),
                         .cubes = cubes})
                .has_value());
    const vx::Result<std::vector<std::uint8_t>> pixels = renderer.capture();
    REQUIRE(pixels.has_value());

    REQUIRE(pixels.value().size() == std::size_t{16} * 8 * 4);
    // The cube shrank with the image; the image's corners show the new sky.
    for (const auto [x, y] :
         std::array{std::array<std::uint32_t, 2>{0, 0}, std::array<std::uint32_t, 2>{15, 0},
                    std::array<std::uint32_t, 2>{0, 7}, std::array<std::uint32_t, 2>{15, 7}}) {
        CHECK(near(pixel_at(pixels.value(), 16, x, y), {0, 255, 0, 255}));
    }
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
