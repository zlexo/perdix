// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include <vx/render/buffer.hpp>
#include <vx/render/context.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

bool host_visible(const vx::render::Device& device, const vx::render::Buffer& buffer) {
    VkMemoryPropertyFlags properties = 0;
    vmaGetAllocationMemoryProperties(device.allocator(), buffer.allocation(), &properties);
    return (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
}

} // namespace

TEST_CASE("an upload target is mapped exactly when its memory is host-visible") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Buffer> made = vx::render::Buffer::create(
        device, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        vx::render::BufferMemory::upload_target);
    REQUIRE(made.has_value());
    const vx::render::Buffer& buffer = made.value();

    CHECK(buffer.size() == 256);
    CHECK(buffer.address() != 0);
    if (host_visible(device, buffer)) {
        CHECK(buffer.mapping().size() == 256);
    } else {
        CHECK(buffer.mapping().empty());
    }
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("a device-local buffer is not mapped, and has an address only when asked") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Buffer> addressed = vx::render::Buffer::create(
        device, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        vx::render::BufferMemory::device_local);
    vx::Result<vx::render::Buffer> plain = vx::render::Buffer::create(
        device, 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vx::render::BufferMemory::device_local);
    REQUIRE(addressed.has_value());
    REQUIRE(plain.has_value());

    CHECK(addressed.value().mapping().empty());
    CHECK(addressed.value().address() != 0);
    CHECK(plain.value().mapping().empty());
    CHECK(plain.value().address() == 0);
    CHECK(created.value().instance().validation_counts().errors == 0);
}

TEST_CASE("a buffer without a size is an error, not a Vulkan call") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());

    const vx::Result<vx::render::Buffer> made =
        vx::render::Buffer::create(created.value().device(), 0, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   vx::render::BufferMemory::device_local);
    REQUIRE_FALSE(made.has_value());
    CHECK(made.error().message == "a buffer needs a size above zero");
    CHECK(created.value().instance().validation_counts().errors == 0);
}
