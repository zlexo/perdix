// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/context.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/render/texture_table.hpp>
#include <vx/shaders/triangle.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

vx::Result<vx::render::Image> texture(const vx::render::Device& device) {
    return vx::render::Image::create(device, {.width = 16, .height = 16}, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
}

} // namespace

TEST_CASE("a texture table hands out its slots in order") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    vx::Result<vx::render::Image> image = texture(device);
    REQUIRE(table.has_value());
    REQUIRE(image.has_value());

    // At most 1024, and at least the 16 every device offers.
    CHECK(table.value().capacity() <= 1024);
    CHECK(table.value().capacity() >= 16);
    const vx::Result<std::uint32_t> first = table.value().add(image.value().view());
    const vx::Result<std::uint32_t> second = table.value().add(image.value().view());
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value() == 0);
    CHECK(second.value() == 1);
    CHECK(table.value().size() == 2);
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("a full texture table says so, rather than writing past its end") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    vx::Result<vx::render::Image> image = texture(device);
    REQUIRE(table.has_value());
    REQUIRE(image.has_value());
    const std::uint32_t capacity = table.value().capacity();
    for (std::uint32_t slot = 0; slot < capacity; ++slot) {
        REQUIRE(table.value().add(image.value().view()).has_value());
    }

    const vx::Result<std::uint32_t> beyond = table.value().add(image.value().view());
    REQUIRE_FALSE(beyond.has_value());
    CHECK(beyond.error().message ==
          "the texture table is full, at " + std::to_string(capacity) + " textures");
    CHECK(created.value().instance().validation_counts().errors == 0);
}

TEST_CASE("a pipeline takes the texture table as its set 0") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    REQUIRE(table.has_value());
    const vx::Result<vx::render::GraphicsPipeline> pipeline = vx::render::GraphicsPipeline::create(
        device, {.spirv = vx::shaders::triangle,
                 .color_format = VK_FORMAT_R8G8B8A8_SRGB,
                 .set_layout = table.value().layout(),
                 .push_constant_size = sizeof(VkDeviceAddress)});
    INFO((pipeline.has_value() ? std::string() : pipeline.error().message));
    REQUIRE(pipeline.has_value());
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}
