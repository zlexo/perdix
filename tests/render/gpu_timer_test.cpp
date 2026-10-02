// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// Linux CI runs these on Mesa's lavapipe; on macOS, source the Vulkan SDK's
// setup-env.sh first.

#include "one_shot.hpp"

#include <vx/render/context.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/image.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>

namespace {

vx::Result<vx::render::Context> create_context() {
    return vx::render::Context::create({.surface = std::nullopt, .validation = true});
}

// Moves `image` from undefined to a color attachment, discarding what it held.
void to_color_attachment(VkCommandBuffer commands, VkImage image) {
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
        .srcAccessMask = VK_ACCESS_2_NONE,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
}

// A render pass that only clears `image`, a color attachment.
void clear_pass(VkCommandBuffer commands, const vx::render::Image& image) {
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = image.view(),
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = image.extent()},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands, &rendering);
    vkCmdEndRendering(commands);
}

} // namespace

TEST_CASE("a GPU timer reads nothing from a slot it has not timed") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());

    const vx::Result<vx::render::GpuTimer> timer =
        vx::render::GpuTimer::create(created.value().device(), 2);
    REQUIRE(timer.has_value());
    // Every driver the engine runs on writes timestamps: lavapipe, KosmicKrisp
    // and MoltenVK. One that does not gets a timer that times nothing.
    REQUIRE(timer.value().times());
    CHECK_FALSE(timer.value().read(0).has_value());
    CHECK_FALSE(timer.value().read(1).has_value());
    CHECK(created.value().instance().validation_counts().errors == 0);
}

TEST_CASE("a GPU timer reads the time a pass took, within the time it was waited for") {
    vx::Result<vx::render::Context> created = create_context();
    INFO((created.has_value() ? std::string() : created.error().message));
    REQUIRE(created.has_value());
    const vx::render::Device& device = created.value().device();

    vx::Result<vx::render::GpuTimer> timer = vx::render::GpuTimer::create(device, 2);
    REQUIRE(timer.has_value());
    REQUIRE(timer.value().times());

    // A pass with work enough to time: clearing 4096 by 4096 texels. A pass,
    // not a transfer, which KosmicKrisp does not time (see GpuTimer).
    const vx::Result<vx::render::Image> image =
        vx::render::Image::create(device, {.width = 4096, .height = 4096}, VK_FORMAT_R8G8B8A8_UNORM,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    REQUIRE(image.has_value());

    vx::test::OneShot shot(device);
    timer.value().begin(shot.commands(), 1);
    to_color_attachment(shot.commands(), image.value().image());
    clear_pass(shot.commands(), image.value());
    timer.value().end(shot.commands(), 1);
    const auto submitted = std::chrono::steady_clock::now();
    shot.submit_and_wait();
    const auto waited = std::chrono::steady_clock::now() - submitted;

    const std::optional<std::chrono::nanoseconds> took = timer.value().read(1);
    REQUIRE(took.has_value());
    MESSAGE("clearing 4096 by 4096: ", took->count(), " ns on the GPU, ", waited.count(),
            " ns from submission to idle");
    // Some time, and no more than the wait around it: a tick length applied
    // twice, say, would run past it.
    CHECK(took->count() > 0);
    CHECK(*took <= waited);
    CHECK_FALSE(timer.value().read(0).has_value()); // never begun
    CHECK(created.value().instance().validation_counts().errors == 0);
    CHECK(created.value().instance().validation_counts().warnings == 0);
}
