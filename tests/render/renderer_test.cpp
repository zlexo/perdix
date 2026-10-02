// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/renderer.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>

namespace {

constexpr vx::render::Extent image_size = {.width = 64, .height = 32};

vx::Result<vx::render::Renderer> headless_renderer() {
    return vx::render::Renderer::create({.surface = std::nullopt,
                                         .framebuffer = image_size,
                                         .validation = true,
                                         .present_mode = vx::render::PresentMode::fifo});
}

} // namespace

TEST_CASE("a headless renderer draws frames without a window") {
    vx::Result<vx::render::Renderer> created = headless_renderer();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    vx::render::Renderer& renderer = created.value();
    CHECK(renderer.swapchain() == nullptr);

    for (int frame = 0; frame < 3; ++frame) {
        const vx::Result<vx::render::Frame> drawn =
            renderer.render({.r = 1.0F, .g = 0.0F, .b = 0.214F, .a = 1.0F});
        REQUIRE(drawn.has_value());
        CHECK(drawn.value() == vx::render::Frame::drawn);
    }
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
    const vx::Result<vx::render::Frame> drawn =
        renderer.render({.r = 0.0F, .g = 1.0F, .b = 0.0F, .a = 1.0F});
    REQUIRE(drawn.has_value());
    CHECK(drawn.value() == vx::render::Frame::drawn);
    CHECK(renderer.context().instance().validation_counts().errors == 0);
}
