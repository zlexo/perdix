// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/context.hpp>
#include <vx/shaders/triangle.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>

TEST_CASE("the triangle's shaders make a shader module the device accepts") {
    vx::Result<vx::render::Context> created =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Context& context = created.value();

    const VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = vx::shaders::triangle.size() * sizeof(std::uint32_t),
        .pCode = vx::shaders::triangle.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    CHECK(vkCreateShaderModule(context.device().handle(), &info, nullptr, &module) == VK_SUCCESS);
    vkDestroyShaderModule(context.device().handle(), module, nullptr);

    // The validation layers check the module against the device as it is
    // created: its SPIR-V, and that the device enabled each capability it uses.
    CHECK(context.instance().validation_counts().errors == 0);
    CHECK(context.instance().validation_counts().warnings == 0);
}
