// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include "image_readback.hpp"
#include "one_shot.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/render/block_texture_array.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/upload.hpp>
#include <vx/world/block_registry.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

vx::TextureRef layer(std::string name, std::uint8_t quarter_turns) {
    return {.name = std::move(name),
            .quarter_turns = quarter_turns,
            .used_opaque = false,
            .used_cutout = true,
            .first_use = "assets/test/blocks/x.toml:3:7"};
}

} // namespace

TEST_CASE("the block texture array gives back every layer at every level, byte for byte") {
    // RFC 0004's test plan: upload with validation on, read back, and match
    // the CPU's bytes. Layers as the registry lists them: the checker, then
    // textures by name and turns, cutout leaves among them.
    const std::vector<vx::TextureRef> textures = {{},
                                                  layer("test:leaves", 0),
                                                  layer("test:leaves", 2),
                                                  layer("test:pattern", 0),
                                                  layer("test:pattern", 1),
                                                  layer("test:pattern", 3),
                                                  layer("test:solid", 0)};
    std::vector<vx::Diagnostic> diagnostics;
    const std::vector<std::byte> bytes =
        vx::render::build_block_textures(textures, VX_BLOCK_TEXTURES_DIR, diagnostics);
    REQUIRE(bytes.size() == textures.size() * vx::render::block_texture_layer_bytes);

    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();
    vx::Result<vx::render::Image> array =
        vx::render::create_block_texture_array(device, static_cast<std::uint32_t>(textures.size()));
    INFO((array.has_value() ? std::string() : array.error().message));
    REQUIRE(array.has_value());
    CHECK(array.value().format() == VK_FORMAT_R8G8B8A8_SRGB);
    CHECK(array.value().extent().width == 16);
    CHECK(array.value().extent().height == 16);
    CHECK(array.value().levels() == 5);
    CHECK(array.value().layers() == textures.size());

    vx::Result<vx::render::Buffer> readback =
        vx::render::Buffer::create(device, bytes.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   vx::render::BufferMemory::readback, "readback buffer");
    REQUIRE(readback.has_value());
    vx::render::UploadContext uploads(device);
    vx::test::OneShot shot(device);
    const vx::Result<void> uploaded =
        vx::render::upload_image(uploads, shot.commands(), array.value(), bytes);
    INFO((uploaded.has_value() ? std::string() : uploaded.error().message));
    REQUIRE(uploaded.has_value());
    vx::test::record_image_readback(shot.commands(), array.value(), readback.value());
    shot.submit_and_wait();
    vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0, VK_WHOLE_SIZE);

    CHECK(std::ranges::equal(readback.value().mapping(), bytes));
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("more layers than the device holds in one image are refused, naming both numbers") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    const std::uint32_t most = properties.limits.maxImageArrayLayers;

    const vx::Result<vx::render::Image> too_many =
        vx::render::create_block_texture_array(device, most + 1);
    REQUIRE_FALSE(too_many.has_value());
    CHECK(too_many.error().message == "the block textures need " + std::to_string(most + 1) +
                                          " layers, and this device holds " + std::to_string(most) +
                                          " at most in one image");
    const vx::Result<vx::render::Image> none = vx::render::create_block_texture_array(device, 0);
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().message == "a block texture array has one layer at least, the checker");
    // Exactly the most is not refused for its count.
    CHECK(vx::render::create_block_texture_array(device, most).has_value());
    CHECK(created.value().instance().validation_counts().errors == 0);
}
