// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include "image_readback.hpp"
#include "one_shot.hpp"

#include <vx/render/buffer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/image.hpp>
#include <vx/render/upload.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

// Records a copy of all of `source` into `readback`, and the barrier that
// makes it visible to the host once the queue is idle.
void record_readback(VkCommandBuffer commands, const vx::render::Buffer& source,
                     const vx::render::Buffer& readback) {
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .pNext = nullptr,
        .srcOffset = 0,
        .dstOffset = 0,
        .size = source.size(),
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcBuffer = source.handle(),
        .dstBuffer = readback.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(commands, &copy);
    const VkMemoryBarrier2 to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
}

std::vector<std::byte> pattern(std::size_t size, std::byte first) {
    std::vector<std::byte> bytes(size);
    std::byte value = first;
    for (std::byte& byte : bytes) {
        byte = value;
        value = static_cast<std::byte>(std::to_integer<unsigned>(value) + 1U);
    }
    return bytes;
}

} // namespace

TEST_CASE("a buffer without a mapping is filled through staging copies") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Buffer> dst = vx::render::Buffer::create(
        device, 96, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        vx::render::BufferMemory::device_local, "upload destination");
    vx::Result<vx::render::Buffer> readback =
        vx::render::Buffer::create(device, 96, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   vx::render::BufferMemory::readback, "readback buffer");
    REQUIRE(dst.has_value());
    REQUIRE(readback.has_value());
    REQUIRE(dst.value().mapping().empty());

    // Three pieces at three offsets cover the buffer.
    const std::vector<std::byte> bytes = pattern(96, std::byte{0x10});
    const std::span<const std::byte> all(bytes);
    vx::render::UploadContext uploads(device);
    vx::test::OneShot shot(device);
    for (const auto [offset, size] :
         std::array{std::array<std::size_t, 2>{64, 32}, std::array<std::size_t, 2>{0, 40},
                    std::array<std::size_t, 2>{40, 24}}) {
        const vx::Result<void> uploaded = vx::render::upload(uploads, shot.commands(), dst.value(),
                                                             offset, all.subspan(offset, size));
        REQUIRE(uploaded.has_value());
    }
    uploads.finish(shot.commands());
    record_readback(shot.commands(), dst.value(), readback.value());
    shot.submit_and_wait();
    vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0, VK_WHOLE_SIZE);

    CHECK(uploads.staged_bytes() == 96);
    CHECK(std::ranges::equal(readback.value().mapping(), bytes));
    uploads.reset();
    CHECK(uploads.staged_bytes() == 0);
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("an upload into a mapped buffer goes through the mapping, without staging") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Buffer> dst =
        vx::render::Buffer::create(device, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   vx::render::BufferMemory::upload_target, "mapped destination");
    REQUIRE(dst.has_value());
    // Upload targets are mapped wherever the device has device-local memory
    // the CPU can write, as on unified memory; elsewhere the test above covers
    // the staged path they take.
    if (dst.value().mapping().empty()) {
        MESSAGE("this device's upload targets are not host-visible");
        return;
    }

    const std::vector<std::byte> bytes = pattern(16, std::byte{0xA0});
    vx::render::UploadContext uploads(device);
    REQUIRE(vx::render::upload(uploads, VK_NULL_HANDLE, dst.value(), 16, bytes).has_value());

    CHECK(uploads.staged_bytes() == 0);
    CHECK(std::ranges::equal(dst.value().mapping().subspan(16, 16), bytes));
    CHECK(created.value().instance().validation_counts().errors == 0);
}

TEST_CASE("an upload past the end of its buffer is an error and writes nothing") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Buffer> dst =
        vx::render::Buffer::create(device, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   vx::render::BufferMemory::device_local, "small destination");
    REQUIRE(dst.has_value());
    const std::vector<std::byte> bytes = pattern(8, std::byte{0});
    vx::render::UploadContext uploads(device);

    const vx::Result<void> overlapping =
        vx::render::upload(uploads, VK_NULL_HANDLE, dst.value(), 12, bytes);
    const vx::Result<void> beyond =
        vx::render::upload(uploads, VK_NULL_HANDLE, dst.value(), 20, std::span(bytes).first(0));
    REQUIRE_FALSE(overlapping.has_value());
    REQUIRE_FALSE(beyond.has_value());
    CHECK(overlapping.error().message ==
          "an upload of 8 bytes at offset 12 runs past the end of a buffer of 16");
    CHECK(uploads.staged_bytes() == 0);
    CHECK(created.value().instance().validation_counts().errors == 0);
}

TEST_CASE("an image takes its pixels through a staging copy and gives back the same") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Image> image =
        vx::render::Image::create(device, {.width = 8, .height = 4}, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, "uploaded image");
    vx::Result<vx::render::Buffer> readback =
        vx::render::Buffer::create(device, 8 * 4 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   vx::render::BufferMemory::readback, "readback buffer");
    REQUIRE(image.has_value());
    REQUIRE(readback.has_value());

    const std::vector<std::byte> pixels = pattern(8 * 4 * 4, std::byte{0x20});
    vx::render::UploadContext uploads(device);
    vx::test::OneShot shot(device);
    const vx::Result<void> uploaded =
        vx::render::upload_image(uploads, shot.commands(), image.value(), pixels);
    INFO((uploaded.has_value() ? std::string() : uploaded.error().message));
    REQUIRE(uploaded.has_value());
    vx::test::record_image_readback(shot.commands(), image.value(), readback.value());
    shot.submit_and_wait();
    vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0, VK_WHOLE_SIZE);

    CHECK(uploads.staged_bytes() == pixels.size());
    CHECK(std::ranges::equal(readback.value().mapping(), pixels));
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("an image of several levels and layers takes them in one copy, level by level") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    // 8 by 4, 4 by 2, 2 by 1 and 1 by 1, the last level's height held at 1,
    // in three layers: 43 texels a layer. Then the same turned on its side,
    // 4 by 8, so the width is the one held at 1.
    for (const VkExtent2D extent :
         {VkExtent2D{.width = 8, .height = 4}, VkExtent2D{.width = 4, .height = 8}}) {
        INFO(extent.width, " by ", extent.height);
        vx::Result<vx::render::Image> image = vx::render::Image::create_array(
            device, extent, VK_FORMAT_R8G8B8A8_UNORM, 4, 3,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            "uploaded array");
        constexpr std::size_t bytes = std::size_t{43} * 3 * 4;
        vx::Result<vx::render::Buffer> readback =
            vx::render::Buffer::create(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       vx::render::BufferMemory::readback, "readback buffer");
        REQUIRE(image.has_value());
        REQUIRE(readback.has_value());
        CHECK(image.value().levels() == 4);
        CHECK(image.value().layers() == 3);

        const std::vector<std::byte> pixels = pattern(bytes, std::byte{0x30});
        vx::render::UploadContext uploads(device);
        vx::test::OneShot shot(device);
        const vx::Result<void> uploaded =
            vx::render::upload_image(uploads, shot.commands(), image.value(), pixels);
        INFO((uploaded.has_value() ? std::string() : uploaded.error().message));
        REQUIRE(uploaded.has_value());
        vx::test::record_image_readback(shot.commands(), image.value(), readback.value());
        shot.submit_and_wait();
        vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0,
                                VK_WHOLE_SIZE);

        CHECK(uploads.staged_bytes() == bytes);
        CHECK(std::ranges::equal(readback.value().mapping(), pixels));
    }
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("pixels of the wrong size for an image are an error, and nothing is staged") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::Image> image =
        vx::render::Image::create(device, {.width = 8, .height = 4}, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  VK_IMAGE_ASPECT_COLOR_BIT, "uneven image");
    REQUIRE(image.has_value());
    const std::vector<std::byte> pixels = pattern(127, std::byte{0});
    vx::render::UploadContext uploads(device);

    const vx::Result<void> uploaded =
        vx::render::upload_image(uploads, VK_NULL_HANDLE, image.value(), pixels);
    REQUIRE_FALSE(uploaded.has_value());
    CHECK(uploaded.error().message == "an image of 8 by 4 takes 128 bytes of pixels, not 127");
    CHECK(uploads.staged_bytes() == 0);

    vx::Result<vx::render::Image> array = vx::render::Image::create_array(
        device, {.width = 8, .height = 4}, VK_FORMAT_R8G8B8A8_UNORM, 4, 3,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "uneven array");
    REQUIRE(array.has_value());
    // Every level of one layer's worth: 43 texels, not 129.
    const std::vector<std::byte> one_layer = pattern(std::size_t{43} * 4, std::byte{0});
    const vx::Result<void> short_array =
        vx::render::upload_image(uploads, VK_NULL_HANDLE, array.value(), one_layer);
    REQUIRE_FALSE(short_array.has_value());
    CHECK(short_array.error().message ==
          "an image of 8 by 4, 4 levels and 3 layers, takes 516 bytes of pixels, not 172");
    CHECK(uploads.staged_bytes() == 0);
    // Too many bytes are as wrong as too few: a fourth layer's would be
    // dropped without a word.
    const std::vector<std::byte> four_layers = pattern(std::size_t{43} * 4 * 4, std::byte{0});
    const vx::Result<void> long_array =
        vx::render::upload_image(uploads, VK_NULL_HANDLE, array.value(), four_layers);
    REQUIRE_FALSE(long_array.has_value());
    CHECK(long_array.error().message ==
          "an image of 8 by 4, 4 levels and 3 layers, takes 516 bytes of pixels, not 688");
    CHECK(uploads.staged_bytes() == 0);

    // One layer of five levels, as a block texture array holding only the
    // checker: its shape is named, the layer in the singular.
    vx::Result<vx::render::Image> one_layer_array = vx::render::Image::create_array(
        device, {.width = 16, .height = 16}, VK_FORMAT_R8G8B8A8_UNORM, 5, 1,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "one-layer array");
    REQUIRE(one_layer_array.has_value());
    const vx::Result<void> no_pixels =
        vx::render::upload_image(uploads, VK_NULL_HANDLE, one_layer_array.value(), {});
    REQUIRE_FALSE(no_pixels.has_value());
    CHECK(no_pixels.error().message ==
          "an image of 16 by 16, 5 levels and 1 layer, takes 1364 bytes of pixels, not 0");
    CHECK(uploads.staged_bytes() == 0);
    CHECK(created.value().instance().validation_counts().errors == 0);
}
