// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/renderer.hpp>

#include <doctest/doctest.h>

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

// How many pixels differ from `expected` by more than one step in a channel,
// a margin for how each driver rounds while encoding to sRGB.
std::size_t pixels_off(const std::vector<std::uint8_t>& pixels,
                       const std::vector<std::uint8_t>& expected) {
    std::size_t off = 0;
    for (std::size_t pixel = 0; pixel + 4 <= pixels.size(); pixel += 4) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (std::abs(int{pixels[pixel + channel]} - int{expected[channel]}) > 1) {
                ++off;
                break;
            }
        }
    }
    return off;
}

} // namespace

TEST_CASE("a headless renderer clears its image to the color it was given") {
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

    CHECK(pixels_off(pixels.value(), {255, 0, 128, 255}) == 0);
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

    CHECK(pixels.value().size() == std::size_t{16} * 8 * 4);
    CHECK(pixels_off(pixels.value(), {0, 255, 0, 255}) == 0);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
