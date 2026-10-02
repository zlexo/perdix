#include <vx/render/renderer.hpp>

#include "vk_result.hpp"

#include <vx/render/buffer.hpp>
#include <vx/render/upload.hpp>
#include <vx/shaders/triangle.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

namespace vx::render {
namespace {

constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

// RGBA byte order, so a captured frame reads without swizzling.
constexpr VkFormat offscreen_format = VK_FORMAT_R8G8B8A8_SRGB;

enum class Target : std::uint8_t { swapchain, offscreen };

// The triangle's vertices as triangle.slang reads them: positions in clip
// space, where y points down the screen, and linear colors.
struct TriangleVertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};
static_assert(sizeof(TriangleVertex) == 32, "triangle.slang's vertex stride");

constexpr std::array<TriangleVertex, 3> triangle_vertices = {{
    {.position = {0.0F, -0.6F, 0.0F, 1.0F}, .color = {1.0F, 0.0F, 0.0F, 1.0F}}, // top, red
    {.position = {0.6F, 0.6F, 0.0F, 1.0F}, .color = {0.0F, 1.0F, 0.0F, 1.0F}},  // right, green
    {.position = {-0.6F, 0.6F, 0.0F, 1.0F}, .color = {0.0F, 0.0F, 1.0F, 1.0F}}, // left, blue
}};

// triangle.slang's push constants.
struct TrianglePush {
    VkDeviceAddress vertices;
};

Result<GraphicsPipeline> make_triangle_pipeline(const Device& device, VkFormat color_format) {
    return GraphicsPipeline::create(device, {.spirv = shaders::triangle,
                                             .vertex_entry = "vertex_main",
                                             .fragment_entry = "fragment_main",
                                             .color_format = color_format,
                                             .push_constant_size = sizeof(TrianglePush),
                                             .push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT});
}

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

void begin_commands(VkCommandBuffer commands) {
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(commands, &begin);
}

// One frame's commands: the image becomes a color attachment, is cleared, has
// the triangle drawn over it, and, for a swapchain, becomes presentable. An
// offscreen image stays a color attachment, ready for the next frame or for
// capture.
void record_frame(VkCommandBuffer commands, Target target, VkImage image, VkImageView view,
                  VkExtent2D extent, Color clear, const GraphicsPipeline& pipeline,
                  VkDeviceAddress vertices) {
    begin_commands(commands);

    // For a swapchain image, the submission waits for the acquired image at
    // this stage, so starting the barrier there chains onto that wait. The
    // offscreen image has no such wait: the barrier waits for the previous
    // frame's writes to it instead. The old contents do not matter either way.
    transition(
        commands, image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        target == Target::swapchain ? VK_ACCESS_2_NONE : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

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

    // The vertex shader pulls the vertices through their buffer's address.
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
    const VkViewport viewport = {
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(extent.width),
        .height = static_cast<float>(extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    const TrianglePush push = {.vertices = vertices};
    vkCmdPushConstants(commands, pipeline.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(commands, static_cast<std::uint32_t>(triangle_vertices.size()), 1, 0, 0);

    vkCmdEndRendering(commands);

    if (target == Target::swapchain) {
        // Presentation waits on the semaphore the submission signals after all
        // of its commands, this layout transition included.
        transition(commands, image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                   VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
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

Result<void> submit(VkQueue queue, VkCommandBuffer commands,
                    std::span<const VkSemaphoreSubmitInfo> waits,
                    std::span<const VkSemaphoreSubmitInfo> signals) {
    const VkCommandBufferSubmitInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = commands,
        .deviceMask = 0,
    };
    const VkSubmitInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size()),
        .pSignalSemaphoreInfos = signals.data(),
    };
    if (const VkResult result = vkQueueSubmit2(queue, 1, &info, VK_NULL_HANDLE);
        result != VK_SUCCESS) {
        return Error{"vkQueueSubmit2 failed: " + vk_result_name(result)};
    }
    return {};
}

VkExtent2D vk_extent(Extent extent) {
    return {.width = extent.width, .height = extent.height};
}

} // namespace

Result<Renderer> Renderer::create(const RendererDesc& desc) {
    Result<Context> context =
        Context::create({.surface = desc.surface, .validation = desc.validation});
    if (!context.has_value()) {
        return context.error();
    }

    std::optional<Swapchain> swapchain;
    std::optional<OffscreenImage> offscreen;
    if (desc.surface.has_value()) {
        Result<Swapchain> made =
            Swapchain::create(context.value().device(), context.value().surface(), desc.framebuffer,
                              desc.present_mode, VK_NULL_HANDLE);
        if (!made.has_value()) {
            return made.error();
        }
        swapchain = std::move(made).value();
    } else {
        Result<OffscreenImage> made = OffscreenImage::create(
            context.value().device(), vk_extent(desc.framebuffer), offscreen_format);
        if (!made.has_value()) {
            return made.error();
        }
        offscreen = std::move(made).value();
    }

    const VkFormat color_format =
        swapchain.has_value() ? swapchain->choice().format.format : offscreen_format;
    Result<GraphicsPipeline> pipeline =
        make_triangle_pipeline(context.value().device(), color_format);
    if (!pipeline.has_value()) {
        return pipeline.error();
    }
    Result<Buffer> vertices = Buffer::create(context.value().device(), sizeof(triangle_vertices),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             BufferMemory::upload_target);
    if (!vertices.has_value()) {
        return vertices.error();
    }

    Renderer renderer(std::move(context).value(), std::move(swapchain), std::move(offscreen),
                      std::move(pipeline).value(), std::move(vertices).value(), desc.framebuffer,
                      desc.present_mode);
    if (const Result<void> made = renderer.create_frame_objects(); !made.has_value()) {
        return made.error();
    }
    if (const Result<void> uploaded = renderer.upload_triangle(); !uploaded.has_value()) {
        return uploaded.error();
    }
    return renderer;
}

Renderer::Renderer(Context context, std::optional<Swapchain> swapchain,
                   std::optional<OffscreenImage> offscreen, GraphicsPipeline triangle_pipeline,
                   Buffer triangle_buffer, Extent framebuffer, PresentMode present_mode)
    : context_(std::move(context)), swapchain_(std::move(swapchain)),
      offscreen_(std::move(offscreen)), triangle_pipeline_(std::move(triangle_pipeline)),
      triangle_buffer_(std::move(triangle_buffer)), framebuffer_(framebuffer),
      target_framebuffer_(framebuffer), present_mode_(present_mode) {}

Renderer::Renderer(Renderer&& other) noexcept
    : context_(std::move(other.context_)), swapchain_(std::move(other.swapchain_)),
      offscreen_(std::move(other.offscreen_)),
      triangle_pipeline_(std::move(other.triangle_pipeline_)),
      triangle_buffer_(std::move(other.triangle_buffer_)), framebuffer_(other.framebuffer_),
      target_framebuffer_(other.target_framebuffer_), present_mode_(other.present_mode_),
      swapchain_stale_(other.swapchain_stale_), slots_(std::exchange(other.slots_, {})),
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
    const std::size_t images = swapchain_.has_value() ? swapchain_->images().size() : 0;
    while (image_rendered_.size() < images) {
        Result<VkSemaphore> semaphore = make_semaphore(device, nullptr);
        if (!semaphore.has_value()) {
            return semaphore.error();
        }
        image_rendered_.push_back(semaphore.value());
    }
    return {};
}

// On unified memory the vertices go through their buffer's mapping and
// nothing is recorded. Elsewhere the staged copy is submitted, and waited for
// before its staging buffer goes away with `uploads`.
Result<void> Renderer::upload_triangle() {
    const Device& device = context_.device();
    UploadContext uploads(device);
    VkCommandBuffer commands = slots_[0].command_buffer;
    begin_commands(commands);
    const Result<void> uploaded =
        upload(uploads, commands, triangle_buffer_, 0, std::as_bytes(std::span(triangle_vertices)));
    uploads.finish(commands);
    vkEndCommandBuffer(commands);
    if (!uploaded.has_value()) {
        return uploaded.error();
    }
    if (uploads.staged_bytes() > 0) {
        if (const Result<void> submitted = submit(device.queue(), commands, {}, {});
            !submitted.has_value()) {
            return submitted.error();
        }
        vkQueueWaitIdle(device.queue());
    }
    return {};
}

Result<void> Renderer::recreate_target() {
    // The last frames, and their presentation, may still use the old target.
    vkDeviceWaitIdle(context_.device().handle());
    if (swapchain_.has_value()) {
        Result<Swapchain> replacement =
            Swapchain::create(context_.device(), context_.surface(), framebuffer_, present_mode_,
                              swapchain_->handle());
        if (!replacement.has_value()) {
            return replacement.error();
        }
        swapchain_ = std::move(replacement).value(); // destroys the old one
        swapchain_stale_ = false;
        // A surface may offer a different format after a change, such as a
        // move to another display.
        if (const VkFormat format = swapchain_->choice().format.format;
            format != triangle_pipeline_.color_format()) {
            Result<GraphicsPipeline> rebuilt = make_triangle_pipeline(context_.device(), format);
            if (!rebuilt.has_value()) {
                return rebuilt.error();
            }
            triangle_pipeline_ = std::move(rebuilt).value();
        }
    } else {
        Result<OffscreenImage> replacement =
            OffscreenImage::create(context_.device(), vk_extent(framebuffer_), offscreen_format);
        if (!replacement.has_value()) {
            return replacement.error();
        }
        offscreen_ = std::move(replacement).value();
    }
    target_framebuffer_ = framebuffer_;
    return add_rendered_semaphores();
}

Result<Frame> Renderer::render(Color clear) {
    if (framebuffer_.width == 0 || framebuffer_.height == 0) {
        return Frame::skipped;
    }
    if (swapchain_stale_ || framebuffer_.width != target_framebuffer_.width ||
        framebuffer_.height != target_framebuffer_.height) {
        if (const Result<void> recreated = recreate_target(); !recreated.has_value()) {
            return recreated.error();
        }
    }

    VkDevice device = context_.device().handle();
    VkQueue queue = context_.device().queue();
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
    const VkSemaphoreSubmitInfo frame_done =
        semaphore_submit(frames_done_, frame, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    if (offscreen_.has_value()) {
        vkResetCommandPool(device, slot.command_pool, 0);
        record_frame(slot.command_buffer, Target::offscreen, offscreen_->image(),
                     offscreen_->view(), offscreen_->extent(), clear, triangle_pipeline_,
                     triangle_buffer_.address());
        if (const Result<void> submitted =
                submit(queue, slot.command_buffer, {}, std::span(&frame_done, 1));
            !submitted.has_value()) {
            return submitted.error();
        }
        frame_ = frame;
        return Frame::drawn;
    }

    assert(swapchain_.has_value());
    const Swapchain& swapchain = *swapchain_;
    std::uint32_t index = 0;
    const VkResult acquired = vkAcquireNextImageKHR(device, swapchain.handle(), forever,
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
    record_frame(slot.command_buffer, Target::swapchain, swapchain.images()[index],
                 swapchain.views()[index], swapchain.choice().extent, clear, triangle_pipeline_,
                 triangle_buffer_.address());

    const VkSemaphoreSubmitInfo wait =
        semaphore_submit(slot.image_acquired, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const std::array<VkSemaphoreSubmitInfo, 2> signals = {
        semaphore_submit(image_rendered_[index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
        frame_done,
    };
    if (const Result<void> submitted =
            submit(queue, slot.command_buffer, std::span(&wait, 1), signals);
        !submitted.has_value()) {
        return submitted.error();
    }
    frame_ = frame;

    VkSwapchainKHR handle = swapchain.handle();
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &image_rendered_[index],
        .swapchainCount = 1,
        .pSwapchains = &handle,
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

Result<std::vector<std::uint8_t>> Renderer::capture() {
    assert(offscreen_.has_value() && "capture reads back headless frames");
    if (frame_ == 0) {
        return Error{"there is no frame to capture yet"};
    }
    const OffscreenImage& image = *offscreen_;
    VkDevice device = context_.device().handle();
    VmaAllocator allocator = context_.device().allocator();
    VkQueue queue = context_.device().queue();
    vkDeviceWaitIdle(device); // every frame is done, and every slot is free

    const VkExtent2D extent = image.extent();
    Result<Buffer> readback =
        Buffer::create(context_.device(), VkDeviceSize{extent.width} * extent.height * 4,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::readback);
    if (!readback.has_value()) {
        return readback.error();
    }
    const Buffer& buffer = readback.value();

    VkCommandBuffer commands = slots_[0].command_buffer;
    vkResetCommandPool(device, slots_[0].command_pool, 0);
    begin_commands(commands);
    transition(commands, image.image(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .pNext = nullptr,
        .bufferOffset = 0,
        .bufferRowLength = 0, // tightly packed
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
    const VkCopyImageToBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcImage = image.image(),
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = buffer.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyImageToBuffer2(commands, &copy);
    // The host reads the buffer once the queue is idle; the copy must be visible to it.
    const VkBufferMemoryBarrier2 to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.handle(),
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &to_host,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
    vkEndCommandBuffer(commands);

    if (const Result<void> submitted = submit(queue, commands, {}, {}); !submitted.has_value()) {
        return submitted.error();
    }
    vkQueueWaitIdle(queue);
    vmaInvalidateAllocation(allocator, buffer.allocation(), 0, VK_WHOLE_SIZE);

    const std::span<const std::byte> bytes = buffer.mapping();
    std::vector<std::uint8_t> pixels(bytes.size());
    std::memcpy(pixels.data(), bytes.data(), bytes.size());
    return pixels;
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
