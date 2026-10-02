#include <vx/render/renderer.hpp>

#include "vk_result.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace vx::render {
namespace {

constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

void transition(VkCommandBuffer commands, VkImage image, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
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

// One frame's commands: the image becomes a color attachment, is cleared, and
// becomes presentable.
void record_clear(VkCommandBuffer commands, VkImage image, VkImageView view, VkExtent2D extent,
                  Color clear) {
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(commands, &begin);

    // The submission waits for the acquired image at this stage, so starting
    // the barrier there chains onto that wait. The old contents do not matter.
    transition(commands, image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {clear.r, clear.g, clear.b, clear.a}}},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands, &rendering);
    vkCmdEndRendering(commands);

    // Presentation waits on the semaphore the submission signals after all of
    // its commands, this layout transition included.
    transition(commands, image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    vkEndCommandBuffer(commands);
}

Result<VkSemaphore> make_semaphore(VkDevice device, const void* type) {
    const VkSemaphoreCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = type,
        .flags = 0,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
        result != VK_SUCCESS) {
        return Error{"vkCreateSemaphore failed: " + vk_result_name(result)};
    }
    return semaphore;
}

VkSemaphoreSubmitInfo semaphore_submit(VkSemaphore semaphore, std::uint64_t value,
                                       VkPipelineStageFlags2 stage) {
    return {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = semaphore,
        .value = value,
        .stageMask = stage,
        .deviceIndex = 0,
    };
}

} // namespace

Result<Renderer> Renderer::create(const RendererDesc& desc) {
    Result<Context> context =
        Context::create({.surface = desc.surface, .validation = desc.validation});
    if (!context.has_value()) {
        return context.error();
    }
    Result<Swapchain> swapchain =
        Swapchain::create(context.value().device(), context.value().surface(), desc.framebuffer,
                          desc.present_mode, VK_NULL_HANDLE);
    if (!swapchain.has_value()) {
        return swapchain.error();
    }

    Renderer renderer(std::move(context).value(), std::move(swapchain).value(), desc.framebuffer,
                      desc.present_mode);
    if (const Result<void> made = renderer.create_frame_objects(); !made.has_value()) {
        return made.error();
    }
    return renderer;
}

Renderer::Renderer(Context context, Swapchain swapchain, Extent framebuffer,
                   PresentMode present_mode)
    : context_(std::move(context)), swapchain_(std::move(swapchain)), framebuffer_(framebuffer),
      swapchain_framebuffer_(framebuffer), present_mode_(present_mode) {}

Renderer::Renderer(Renderer&& other) noexcept
    : context_(std::move(other.context_)), swapchain_(std::move(other.swapchain_)),
      framebuffer_(other.framebuffer_), swapchain_framebuffer_(other.swapchain_framebuffer_),
      present_mode_(other.present_mode_), swapchain_stale_(other.swapchain_stale_),
      slots_(std::exchange(other.slots_, {})),
      image_rendered_(std::exchange(other.image_rendered_, {})),
      frames_done_(std::exchange(other.frames_done_, VK_NULL_HANDLE)), frame_(other.frame_) {}

Renderer::~Renderer() {
    destroy();
}

Result<void> Renderer::create_frame_objects() {
    VkDevice device = context_.device().handle();

    for (FrameSlot& slot : slots_) {
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_.device().queue_family(),
        };
        if (const VkResult result =
                vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool);
            result != VK_SUCCESS) {
            return Error{"vkCreateCommandPool failed: " + vk_result_name(result)};
        }
        const VkCommandBufferAllocateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = slot.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (const VkResult result =
                vkAllocateCommandBuffers(device, &buffer_info, &slot.command_buffer);
            result != VK_SUCCESS) {
            return Error{"vkAllocateCommandBuffers failed: " + vk_result_name(result)};
        }
        Result<VkSemaphore> acquired = make_semaphore(device, nullptr);
        if (!acquired.has_value()) {
            return acquired.error();
        }
        slot.image_acquired = acquired.value();
    }

    const VkSemaphoreTypeCreateInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    Result<VkSemaphore> done = make_semaphore(device, &timeline);
    if (!done.has_value()) {
        return done.error();
    }
    frames_done_ = done.value();

    return add_rendered_semaphores();
}

// The presentation engine may hold an image's semaphore after the frame that
// signaled it is done, so none is destroyed before the renderer is; a new
// swapchain with more images only adds some.
Result<void> Renderer::add_rendered_semaphores() {
    VkDevice device = context_.device().handle();
    while (image_rendered_.size() < swapchain_.images().size()) {
        Result<VkSemaphore> semaphore = make_semaphore(device, nullptr);
        if (!semaphore.has_value()) {
            return semaphore.error();
        }
        image_rendered_.push_back(semaphore.value());
    }
    return {};
}

Result<void> Renderer::recreate_swapchain() {
    // The last frame and its presentation may still use the old swapchain.
    vkDeviceWaitIdle(context_.device().handle());
    Result<Swapchain> replacement = Swapchain::create(
        context_.device(), context_.surface(), framebuffer_, present_mode_, swapchain_.handle());
    if (!replacement.has_value()) {
        return replacement.error();
    }
    swapchain_ = std::move(replacement).value(); // destroys the old one
    swapchain_framebuffer_ = framebuffer_;
    swapchain_stale_ = false;
    return add_rendered_semaphores();
}

Result<Frame> Renderer::render(Color clear) {
    if (framebuffer_.width == 0 || framebuffer_.height == 0) {
        return Frame::skipped;
    }
    if (swapchain_stale_ || framebuffer_.width != swapchain_framebuffer_.width ||
        framebuffer_.height != swapchain_framebuffer_.height) {
        if (const Result<void> recreated = recreate_swapchain(); !recreated.has_value()) {
            return recreated.error();
        }
    }

    VkDevice device = context_.device().handle();
    const std::uint64_t frame = frame_ + 1;
    const FrameSlot& slot = slots_[frame % frames_in_flight];
    // The slot is free once the last frame that used it is done. The timeline
    // starts at 0, so the first frames do not wait.
    const std::uint64_t slot_free = frame > frames_in_flight ? frame - frames_in_flight : 0;
    const VkSemaphoreWaitInfo slot_wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &frames_done_,
        .pValues = &slot_free,
    };
    if (const VkResult result = vkWaitSemaphores(device, &slot_wait, forever);
        result != VK_SUCCESS) {
        return Error{"vkWaitSemaphores failed: " + vk_result_name(result)};
    }

    std::uint32_t index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(device, swapchain_.handle(), forever,
                                                    slot.image_acquired, VK_NULL_HANDLE, &index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale_ = true;
        return Frame::skipped;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return Error{"vkAcquireNextImageKHR failed: " + vk_result_name(acquired)};
    }
    swapchain_stale_ = acquired == VK_SUBOPTIMAL_KHR; // draw this frame, replace after it

    vkResetCommandPool(device, slot.command_pool, 0);
    record_clear(slot.command_buffer, swapchain_.images()[index], swapchain_.views()[index],
                 swapchain_.choice().extent, clear);

    const VkSemaphoreSubmitInfo wait =
        semaphore_submit(slot.image_acquired, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const std::array<VkSemaphoreSubmitInfo, 2> signals = {
        semaphore_submit(image_rendered_[index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
        semaphore_submit(frames_done_, frame, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
    };
    const VkCommandBufferSubmitInfo commands = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = slot.command_buffer,
        .deviceMask = 0,
    };
    const VkSubmitInfo2 submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &commands,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size()),
        .pSignalSemaphoreInfos = signals.data(),
    };
    VkQueue queue = context_.device().queue();
    if (const VkResult result = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
        result != VK_SUCCESS) {
        return Error{"vkQueueSubmit2 failed: " + vk_result_name(result)};
    }
    frame_ = frame;

    VkSwapchainKHR swapchain = swapchain_.handle();
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_rendered_[index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
        .pResults = nullptr,
    };
    const VkResult presented = vkQueuePresentKHR(queue, &present);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    } else if (presented != VK_SUCCESS) {
        return Error{"vkQueuePresentKHR failed: " + vk_result_name(presented)};
    }
    return Frame::drawn;
}

void Renderer::destroy() noexcept {
    VkDevice device = context_.device().handle();
    if (device == VK_NULL_HANDLE) {
        return; // moved from
    }
    vkDeviceWaitIdle(device);
    for (VkSemaphore semaphore : image_rendered_) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    image_rendered_.clear();
    vkDestroySemaphore(device, frames_done_, nullptr);
    for (const FrameSlot& slot : slots_) {
        vkDestroySemaphore(device, slot.image_acquired, nullptr);
        vkDestroyCommandPool(device, slot.command_pool, nullptr); // frees its command buffer
    }
}

} // namespace vx::render
