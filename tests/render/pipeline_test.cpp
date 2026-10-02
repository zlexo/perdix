// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/context.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/shaders/triangle.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>

TEST_CASE("the triangle's pipeline builds for the offscreen format and the swapchain's") {
    vx::Result<vx::render::Context> created =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Context& context = created.value();

    for (const VkFormat format : std::array{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB}) {
        INFO("color format ", static_cast<int>(format));
        // One push constant: the vertices' buffer address.
        vx::Result<vx::render::GraphicsPipeline> made = vx::render::GraphicsPipeline::create(
            context.device(), {.spirv = vx::shaders::triangle,
                               .vertex_entry = "vertex_main",
                               .fragment_entry = "fragment_main",
                               .color_format = format,
                               .push_constant_size = sizeof(VkDeviceAddress),
                               .push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT});
        INFO((made.has_value() ? std::string() : made.error().message));
        REQUIRE(made.has_value());
        CHECK(made.value().handle() != VK_NULL_HANDLE);
        CHECK(made.value().layout() != VK_NULL_HANDLE);
        CHECK(made.value().color_format() == format);
    }
    // The validation layers check each stage against the module and the
    // layout: its entry point, its push constants, the capabilities it uses.
    CHECK(context.instance().validation_counts().errors == 0);
    CHECK(context.instance().validation_counts().warnings == 0);
}
