#include <vx/render/texture_table.hpp>

#include <doctest/doctest.h>

#include <cstdint>

namespace {

// Limits where nothing binds but the three that bound the texture table.
VkPhysicalDeviceLimits limits(std::uint32_t stage_images, std::uint32_t set_images,
                              std::uint32_t stage_resources) {
    VkPhysicalDeviceLimits made{};
    made.maxPerStageDescriptorSampledImages = stage_images;
    made.maxDescriptorSetSampledImages = set_images;
    made.maxPerStageResources = stage_resources;
    return made;
}

} // namespace

TEST_CASE("the texture table holds 1024 textures where the device allows it") {
    CHECK(vx::render::TextureTable::capacity_for(limits(1'000'000, 1'000'000, 1'000'000)) == 1'024);
}

TEST_CASE("each limit that bounds the texture table leaves room for what else counts against it") {
    // The block texture array is a sampled image of the fragment stage and
    // of the set; the stage's resources count it and the color attachment.
    CHECK(vx::render::TextureTable::capacity_for(limits(100, 1'000'000, 1'000'000)) == 99);
    CHECK(vx::render::TextureTable::capacity_for(limits(1'000'000, 100, 1'000'000)) == 99);
    CHECK(vx::render::TextureTable::capacity_for(limits(1'000'000, 1'000'000, 200)) == 198);
    // MoltenVK 1.4.2 on the reference machine, from vulkaninfo.
    CHECK(vx::render::TextureTable::capacity_for(limits(256, 1'280, 287)) == 255);
    // The least Vulkan allows a device: 16, 96 and 128.
    CHECK(vx::render::TextureTable::capacity_for(limits(16, 96, 128)) == 15);
}
