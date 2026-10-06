// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include "block_texture_probe.hpp"
#include "one_shot.hpp"

#include <vx/render/block_texture_array.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/render/context.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/render/texture_table.hpp>
#include <vx/render/upload.hpp>
#include <vx/shaders/triangle.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

using Pixel = std::array<std::uint8_t, 4>;

// Within one step in every channel, a margin for how drivers round while
// encoding to sRGB.
bool near(const Pixel& pixel, const Pixel& expected) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::abs(int{pixel.at(channel)} - int{expected.at(channel)}) > 1) {
            return false;
        }
    }
    return true;
}

// Two layers, laid out as upload_image takes them, made for telling
// filtering modes apart. Layer 0: levels 0 and 1 checkers of single white
// and black texels, the other levels white. Layer 1: level 0 black, level 1
// white, the others a dark red stored as sRGB 128, which reads back as 128
// only if the array is sampled as sRGB: read as UNORM, it would be light
// 0.5 and come out as 188.
std::vector<std::byte> probe_layers() {
    constexpr std::size_t layers = 2;
    std::vector<std::byte> bytes(layers * vx::render::block_texture_layer_bytes);
    for (std::size_t level = 0; level < 5; ++level) {
        const std::size_t side = std::size_t{16} >> level;
        for (std::size_t layer = 0; layer < layers; ++layer) {
            for (std::size_t texel = 0; texel < side * side; ++texel) {
                Pixel color{255, 255, 255, 255};
                if (layer == 0 && level <= 1 && (texel % side + texel / side) % 2 == 1) {
                    color = {0, 0, 0, 255};
                } else if (layer == 1 && level == 0) {
                    color = {0, 0, 0, 255};
                } else if (layer == 1 && level >= 2) {
                    color = {128, 0, 0, 255};
                }
                const std::size_t at =
                    4 * ((layers * vx::render::block_texture_level_start.at(level)) +
                         (layer * side * side) + texel);
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    bytes.at(at + channel) = std::byte{color.at(channel)};
                }
            }
        }
    }
    return bytes;
}

vx::Result<vx::render::Image> texture(const vx::render::Device& device) {
    return vx::render::Image::create(device, {.width = 16, .height = 16}, VK_FORMAT_R8G8B8A8_SRGB,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, "test texture");
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

TEST_CASE("the block texture array goes into binding 2, and a new array takes its place") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();
    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    REQUIRE(table.has_value());
    // The array counts against the stage's sampled images, beside the slots.
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    CHECK(table.value().capacity() + 1 <= properties.limits.maxPerStageDescriptorSampledImages);

    // Two arrays, of one layer and of three, in the layout shaders read.
    std::vector<vx::render::Image> arrays;
    vx::render::UploadContext uploads(device);
    vx::test::OneShot shot(device);
    for (const std::uint32_t layers : {1U, 3U}) {
        vx::Result<vx::render::Image> array =
            vx::render::create_block_texture_array(device, layers);
        REQUIRE(array.has_value());
        const std::vector<std::byte> bytes(layers * vx::render::block_texture_layer_bytes);
        REQUIRE(
            vx::render::upload_image(uploads, shot.commands(), array.value(), bytes).has_value());
        arrays.push_back(std::move(array).value());
    }
    shot.submit_and_wait();
    table.value().set_block_textures(arrays[0].view());
    table.value().set_block_textures(arrays[1].view());
    // The slots of binding 0 are untouched by it.
    vx::Result<vx::render::Image> image = texture(device);
    REQUIRE(image.has_value());
    const vx::Result<std::uint32_t> slot = table.value().add(image.value().view());
    REQUIRE(slot.has_value());
    CHECK(slot.value() == 0);
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("binding 3 samples the array linearly between texels and levels, and repeats") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();
    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    REQUIRE(table.has_value());
    vx::Result<vx::render::Image> array = vx::render::create_block_texture_array(device, 2);
    REQUIRE(array.has_value());
    {
        vx::render::UploadContext uploads(device);
        vx::test::OneShot shot(device);
        REQUIRE(vx::render::upload_image(uploads, shot.commands(), array.value(), probe_layers())
                    .has_value());
        shot.submit_and_wait();
    }
    table.value().set_block_textures(array.value().view());
    vx::Result<vx::test::BlockTextureProbe> probe =
        vx::test::BlockTextureProbe::create(device, table.value().layout());
    INFO((probe.has_value() ? std::string() : probe.error().message));
    REQUIRE(probe.has_value());
    const auto at = [&](float u, float v, float layer, float level) {
        return probe.value().sample(table.value().set(),
                                    {.u = u, .v = v, .layer = layer, .level = level});
    };
    constexpr Pixel white = {255, 255, 255, 255};
    constexpr Pixel black = {0, 0, 0, 255};
    // Half white and half black in linear light, which sRGB stores as 188.
    constexpr Pixel half = {188, 188, 188, 255};
    constexpr float texel = 1.0F / 16.0F;

    // At a texel's center, that texel; on the edge between two, both. At
    // level 0 that is the magnification filter.
    CHECK(near(at(0.5F * texel, 0.5F * texel, 0.0F, 0.0F), white));
    CHECK(near(at(1.5F * texel, 0.5F * texel, 0.0F, 0.0F), black));
    CHECK(near(at(1.0F * texel, 0.5F * texel, 0.0F, 0.0F), half));
    // At level 1 alone the minification filter, the one distant ground
    // uses: on the edge between two of level 1's texels, both again.
    CHECK(near(at(2.0F * texel, 1.0F * texel, 0.0F, 1.0F), half));
    // Past the right edge, the left one again, and past the bottom, the top:
    // clamping would read texel 15 of the row or the column, which is black.
    CHECK(near(at(1.0F + (0.5F * texel), 0.5F * texel, 0.0F, 0.0F), white));
    CHECK(near(at(0.5F * texel, 1.0F + (0.5F * texel), 0.0F, 0.0F), white));
    // Halfway between level 0, black, and level 1, white; then each layer's
    // level 2, layer 1's dark red read back as stored, so as sRGB.
    CHECK(near(at(0.5F, 0.5F, 1.0F, 0.5F), half));
    CHECK(near(at(0.5F, 0.5F, 1.0F, 2.0F), {128, 0, 0, 255}));
    CHECK(near(at(0.5F, 0.5F, 0.0F, 2.0F), white));
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("a pipeline takes the texture table as its set 0") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::TextureTable> table = vx::render::TextureTable::create(device);
    REQUIRE(table.has_value());
    const vx::Result<vx::render::GraphicsPipeline> pipeline =
        vx::render::GraphicsPipeline::create(device,
                                             {.spirv = vx::shaders::triangle,
                                              .color_format = VK_FORMAT_R8G8B8A8_SRGB,
                                              .set_layout = table.value().layout(),
                                              .push_constant_size = sizeof(VkDeviceAddress)},
                                             "triangle on the texture table");
    INFO((pipeline.has_value() ? std::string() : pipeline.error().message));
    REQUIRE(pipeline.has_value());
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}
