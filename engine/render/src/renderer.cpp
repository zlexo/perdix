#include <vx/render/renderer.hpp>

#include "transition.hpp"
#include "vk_result.hpp"

#include <vx/render/buffer.hpp>
#include <vx/render/camera_relative.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/texture_table.hpp>
#include <vx/render/upload.hpp>
#include <vx/shaders/cube.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

// RGBA byte order, so a captured frame reads without swizzling.
constexpr VkFormat offscreen_format = VK_FORMAT_R8G8B8A8_SRGB;

// Headless rendering's color target, a copy source for capture().
Result<Image> make_offscreen_target(const Device& device, VkExtent2D extent) {
    return Image::create(device, extent, offscreen_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);
}

// RFC 0001: depth is a 32-bit float, so reversed-Z keeps its precision far
// away. Vulkan promises a 24-bit or a 32-bit depth format, not both, so a
// device without this one is told why here.
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

Result<Image> make_depth_target(const Device& device, VkExtent2D extent) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), depth_format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
        return Error{"the device cannot draw depth into 32-bit floats, which RFC 0001's "
                     "reversed-Z needs"};
    }
    return Image::create(device, extent, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
}

enum class Target : std::uint8_t { swapchain, offscreen };

// What a frame draws into: a color image, a swapchain's or the offscreen
// target, and the depth attachment beside it.
struct FrameTarget {
    Target kind;
    VkImage image;
    VkImageView view;
    VkExtent2D extent;
    VkImage depth_image;
    VkImageView depth_view;
};

// Cube vertices as cube.slang reads them: a corner within the block and
// the face's linear color.
struct CubeVertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
    std::array<float, 2> uv;
    std::array<float, 2> padding;
};
static_assert(sizeof(CubeVertex) == 48, "cube.slang's vertex stride");

// A face: its corners counterclockwise seen from outside, so it is a front
// face from there, and its color, which tints the texture.
struct Face {
    std::array<std::array<float, 3>, 4> corners;
    std::array<float, 4> color;
};

constexpr std::array<Face, 6> cube_faces = {{
    {.corners = {{{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}}, .color = {1, 0, 0, 1}}, // +x red
    {.corners = {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}, .color = {0, 1, 1, 1}}, // -x cyan
    {.corners = {{{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}}, .color = {0, 1, 0, 1}}, // +y green
    {.corners = {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
     .color = {1, 0, 1, 1}}, // -y magenta
    {.corners = {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}, .color = {0, 0, 1, 1}}, // +z blue
    {.corners = {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}, .color = {1, 1, 0, 1}}, // -z yellow
}};

// Two triangles a face, both counterclockwise: 36 vertices. Each face shows
// the whole texture, its first corner at the texture's.
constexpr std::array<CubeVertex, 36> make_cube_vertices() {
    constexpr std::array<std::array<float, 2>, 4> corner_uv = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    std::array<CubeVertex, 36> vertices{};
    std::size_t next = 0;
    for (const Face& face : cube_faces) {
        for (const std::size_t corner : std::array<std::size_t, 6>{0, 1, 2, 0, 2, 3}) {
            const auto& [x, y, z] = face.corners.at(corner);
            vertices.at(next++) = {.position = {x, y, z, 1.0F},
                                   .color = face.color,
                                   .uv = corner_uv.at(corner),
                                   .padding = {0, 0}};
        }
    }
    return vertices;
}

constexpr std::array<CubeVertex, 36> cube_vertices = make_cube_vertices();

// RFC 0001's cube texture, until RFC 0004's block textures: 16 by 16
// texels, a checker of two grays, as sRGB bytes.
constexpr std::uint32_t checker_size = 16;
constexpr std::uint8_t checker_light = 230;
constexpr std::uint8_t checker_dark = 150;

std::array<std::byte, std::size_t{checker_size} * checker_size * 4> make_checker() {
    std::array<std::byte, std::size_t{checker_size} * checker_size * 4> texels{};
    for (std::uint32_t y = 0; y < checker_size; ++y) {
        for (std::uint32_t x = 0; x < checker_size; ++x) {
            const std::uint8_t gray = (x + y) % 2 == 0 ? checker_light : checker_dark;
            const std::size_t at = ((std::size_t{y} * checker_size) + x) * 4;
            texels.at(at) = std::byte{gray};
            texels.at(at + 1) = std::byte{gray};
            texels.at(at + 2) = std::byte{gray};
            texels.at(at + 3) = std::byte{255};
        }
    }
    return texels;
}

// The vertex shader reads the push constants' positions and addresses, the
// fragment shader its texture.
constexpr VkShaderStageFlags push_stages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// RFC 0001's push constants, as cube.slang reads them.
struct DrawPush {
    glm::mat4 view_projection; // offset 0: rotation and projection only
    VkDeviceAddress vertices;  // offset 64
    VkDeviceAddress origins;   // offset 72: one float4 per instance
    glm::vec3 camera_fraction; // offset 80: the camera's place within its block
    std::uint32_t texture;     // offset 92: the texture table's slot
};
static_assert(sizeof(DrawPush) == 96 && offsetof(DrawPush, texture) == 92);

// One frame's push constants for a target of `extent`.
DrawPush make_draw_push(const Camera& camera, const BlockAndFraction& split, VkExtent2D extent,
                        VkDeviceAddress vertices, VkDeviceAddress origins, std::uint32_t texture) {
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    return {.view_projection = view_projection(camera, aspect),
            .vertices = vertices,
            .origins = origins,
            .camera_fraction = split.fraction,
            .texture = texture};
}

Result<GraphicsPipeline> make_cube_pipeline(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures) {
    return GraphicsPipeline::create(device, {.spirv = shaders::cube,
                                             .vertex_entry = "vertex_main",
                                             .fragment_entry = "fragment_main",
                                             .color_format = color_format,
                                             .depth_format = depth_format,
                                             .cull_mode = VK_CULL_MODE_BACK_BIT,
                                             .set_layout = textures,
                                             .push_constant_size = sizeof(DrawPush),
                                             .push_constant_stages = push_stages});
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

// What a frame's passes draw, besides the target they draw into.
struct FrameDraw {
    Color clear;
    const GraphicsPipeline* pipeline;
    VkDescriptorSet textures; // set 0, the texture table
    DrawPush push;
    std::uint32_t cubes;
    ImDrawData* overlay; // null: no overlay
};

// Records the frame's one pass, inside the GPU zone Main pass: it clears the
// target, draws the cubes, then the overlay over them when there is one. The
// zone's timestamps fall outside the pass: KosmicKrisp drops timestamps
// written within rendering.
//
// The overlay draws here, not in a pass of its own: a second pass loads and
// stores the whole target again, which took 0.57 ms of GPU time a frame on
// MoltenVK and 0.92 ms on KosmicKrisp at 2560 by 1440, where drawing it here
// takes 0.10 ms and 0.16 ms.
void record_main_pass(VkCommandBuffer commands, [[maybe_unused]] tracy::VkCtx* gpu_profiler,
                      const FrameTarget& target, const FrameDraw& draw) {
    TracyVkNamedZone(gpu_profiler, pass_zone, commands, "Main pass", gpu_profiler != nullptr);

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = target.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {draw.clear.r, draw.clear.g, draw.clear.b,
                                             draw.clear.a}}},
    };
    // Reversed-Z: depth clears to 0, the far end. Nothing reads it after the
    // frame, so it is not stored.
    const VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = target.depth_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {.depth = 0.0F, .stencil = 0}},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = target.extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = &depth_attachment,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands, &rendering);

    // The vertex shader pulls the vertices through their buffer's address.
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->handle());
    const VkViewport viewport = {
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(target.extent.width),
        .height = static_cast<float>(target.extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = target.extent};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout(), 0,
                            1, &draw.textures, 0, nullptr);
    vkCmdPushConstants(commands, draw.pipeline->layout(), push_stages, 0, sizeof(draw.push),
                       &draw.push);
    vkCmdDraw(commands, static_cast<std::uint32_t>(cube_vertices.size()), draw.cubes, 0, 0);
    if (draw.overlay != nullptr) {
        // Last in the pass, so it draws over everything else. Its pipeline
        // neither tests nor writes depth.
        ImGui_ImplVulkan_RenderDrawData(draw.overlay, commands);
    }

    vkCmdEndRendering(commands);
}

// Records a frame's work into `commands`, inside the GPU zone Frame: the
// target becomes a color attachment, the main pass clears it and draws into
// it, and a swapchain image then becomes presentable. An offscreen image
// stays a color attachment, ready for the next frame or for capture.
void record_passes(VkCommandBuffer commands, [[maybe_unused]] tracy::VkCtx* gpu_profiler,
                   const FrameTarget& target, const FrameDraw& draw) {
    TracyVkNamedZone(gpu_profiler, frame_zone, commands, "Frame", gpu_profiler != nullptr);

    // For a swapchain image, the submission waits for the acquired image at
    // this stage, so starting the barrier there chains onto that wait. The
    // offscreen image has no such wait: the barrier waits for the previous
    // frame's writes to it instead. The old contents do not matter either way.
    transition(commands, target.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               target.kind == Target::swapchain ? VK_ACCESS_2_NONE
                                                : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Depth is cleared every frame, so its old contents never matter either;
    // with two frames in flight, the barrier waits for the previous frame's
    // depth writes.
    constexpr VkPipelineStageFlags2 depth_stages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    transition(commands, target.depth_image, depth_stages,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT);

    record_main_pass(commands, gpu_profiler, target, draw);

    if (target.kind == Target::swapchain) {
        // Presentation waits on the semaphore the submission signals after all
        // of its commands, this layout transition included.
        transition(commands, target.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                   VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
}

// Records a frame: hands Tracy the GPU zones of earlier frames whose
// timestamps have landed, then records the frame's passes, timed in the frame
// slot's queries. The timer starts after Tracy's collect, so it times what
// Tracy's Frame zone does and none of Tracy's own work. Collecting and the
// timer's start reset queries for reuse, which must happen outside any
// rendering.
void record_frame(VkCommandBuffer commands, GpuTimer& timer, std::uint32_t slot,
                  tracy::VkCtx* gpu_profiler, const FrameTarget& target, const FrameDraw& draw) {
    ZoneScopedN("Record");
    begin_commands(commands);
    if (gpu_profiler != nullptr) {
        TracyVkCollect(gpu_profiler, commands);
    }
    timer.begin(commands, slot);
    record_passes(commands, gpu_profiler, target, draw);
    timer.end(commands, slot);
    vkEndCommandBuffer(commands);
}

// The overlay's pipeline, for the main pass: one color attachment of
// `format`, which must outlive the call it is passed to, and the pass's
// depth attachment, which the pipeline neither tests nor writes.
ImGui_ImplVulkan_PipelineInfo overlay_pipeline_info(const VkFormat* format) {
    ImGui_ImplVulkan_PipelineInfo info{};
    info.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = format,
        .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    return info;
}

// Tracy's timing of the device's queue, for RFC 0001's GPU zones: null
// without VX_PROFILE, when the queue writes no timestamps, or when its
// command pool cannot be made. Creating it begins one command buffer several
// times, submitting and waiting each time, so it gets one of its own, from a
// pool that allows that, and the frames' pools need not.
tracy::VkCtx* make_gpu_profiler(const Device& device) {
#ifndef TRACY_ENABLE
    static_cast<void>(device);
    return nullptr;
#else
    if (timestamp_valid_bits(device) == 0) {
        return nullptr;
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.queue_family(),
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return nullptr;
    }
    const VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commands = VK_NULL_HANDLE;
    tracy::VkCtx* profiler = nullptr;
    if (vkAllocateCommandBuffers(device.handle(), &allocate, &commands) == VK_SUCCESS) {
        profiler =
            TracyVkContext(device.physical_device(), device.handle(), device.queue(), commands);
        constexpr std::string_view name = "Graphics queue";
        TracyVkContextName(profiler, name.data(), static_cast<std::uint16_t>(name.size()));
    }
    vkDestroyCommandPool(device.handle(), pool, nullptr); // frees its command buffer
    return profiler;
#endif
}

// Adds the time from its making to its end to a frame's wait: for the calls
// that wait for the GPU or the display.
class WaitTimer {
public:
    explicit WaitTimer(std::chrono::nanoseconds& waited) noexcept
        : waited_(&waited), start_(std::chrono::steady_clock::now()) {}
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;
    WaitTimer(WaitTimer&&) = delete;
    WaitTimer& operator=(WaitTimer&&) = delete;
    ~WaitTimer() { *waited_ += std::chrono::steady_clock::now() - start_; }

private:
    std::chrono::nanoseconds* waited_;
    std::chrono::steady_clock::time_point start_;
};

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
    ZoneScopedN("Submit");
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

// Submits a frame's commands. It counts as a wait: MoltenVK waits there for
// the drawable the frame presents.
Result<void> submit_frame(VkQueue queue, VkCommandBuffer commands,
                          std::span<const VkSemaphoreSubmitInfo> waits,
                          std::span<const VkSemaphoreSubmitInfo> signals,
                          std::chrono::nanoseconds& waited) {
    const WaitTimer timer(waited);
    return submit(queue, commands, waits, signals);
}

// Blocks until the timeline reaches `value`: the GPU is done with the frames
// that last used a slot.
Result<void> wait_for_slot(VkDevice device, VkSemaphore frames_done, std::uint64_t value,
                           std::chrono::nanoseconds& waited) {
    ZoneScopedN("Wait for frame slot");
    const WaitTimer timer(waited);
    const VkSemaphoreWaitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &frames_done,
        .pValues = &value,
    };
    if (const VkResult result = vkWaitSemaphores(device, &wait, forever); result != VK_SUCCESS) {
        return Error{"vkWaitSemaphores failed: " + vk_result_name(result)};
    }
    return {};
}

// The swapchain's next image, its index in `index`; `acquired` signals once
// the presentation engine has let go of it.
VkResult acquire_image(VkDevice device, VkSwapchainKHR swapchain, VkSemaphore acquired,
                       std::uint32_t& index, std::chrono::nanoseconds& waited) {
    ZoneScopedN("Acquire");
    const WaitTimer timer(waited);
    return vkAcquireNextImageKHR(device, swapchain, forever, acquired, VK_NULL_HANDLE, &index);
}

// Queues image `index` for presentation once `rendered` signals.
VkResult present_image(VkQueue queue, VkSwapchainKHR swapchain, std::uint32_t index,
                       VkSemaphore rendered, std::chrono::nanoseconds& waited) {
    ZoneScopedN("Present");
    const WaitTimer timer(waited);
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
        .pResults = nullptr,
    };
    return vkQueuePresentKHR(queue, &present);
}

VkExtent2D vk_extent(Extent extent) {
    return {.width = extent.width, .height = extent.height};
}

} // namespace

Result<Renderer> Renderer::create(const RendererDesc& desc) {
    ZoneScopedN("Create renderer");
    Result<Context> context =
        Context::create({.surface = desc.surface, .validation = desc.validation});
    if (!context.has_value()) {
        return context.error();
    }

    std::optional<Swapchain> swapchain;
    std::optional<Image> offscreen;
    if (desc.surface.has_value()) {
        Result<Swapchain> made =
            Swapchain::create(context.value().device(), context.value().surface(), desc.framebuffer,
                              desc.present_mode, VK_NULL_HANDLE);
        if (!made.has_value()) {
            return made.error();
        }
        swapchain = std::move(made).value();
    } else {
        Result<Image> made =
            make_offscreen_target(context.value().device(), vk_extent(desc.framebuffer));
        if (!made.has_value()) {
            return made.error();
        }
        offscreen = std::move(made).value();
    }

    const VkFormat color_format =
        swapchain.has_value() ? swapchain->choice().format.format : offscreen_format;
    Result<Image> depth = make_depth_target(context.value().device(),
                                            swapchain.has_value() ? swapchain->choice().extent
                                                                  : vk_extent(desc.framebuffer));
    if (!depth.has_value()) {
        return depth.error();
    }
    Result<TextureTable> textures = TextureTable::create(context.value().device());
    if (!textures.has_value()) {
        return textures.error();
    }
    Result<GraphicsPipeline> pipeline =
        make_cube_pipeline(context.value().device(), color_format, textures.value().layout());
    if (!pipeline.has_value()) {
        return pipeline.error();
    }
    Result<Image> checker = Image::create(
        context.value().device(), {.width = checker_size, .height = checker_size},
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (!checker.has_value()) {
        return checker.error();
    }
    Result<Buffer> vertices = Buffer::create(context.value().device(), sizeof(cube_vertices),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             BufferMemory::upload_target);
    if (!vertices.has_value()) {
        return vertices.error();
    }

    Renderer renderer(std::move(context).value(), std::move(swapchain), std::move(offscreen),
                      std::move(depth).value(), std::move(textures).value(),
                      {.pipeline = std::move(pipeline).value(),
                       .vertices = std::move(vertices).value(),
                       .checker = std::move(checker).value()},
                      desc.framebuffer, desc.present_mode);
    if (const Result<void> made = renderer.create_frame_objects(); !made.has_value()) {
        return made.error();
    }
    if (const Result<void> uploaded = renderer.upload_cube(); !uploaded.has_value()) {
        return uploaded.error();
    }
    if (desc.overlay != nullptr) {
        if (const Result<void> started = renderer.start_overlay(desc.overlay);
            !started.has_value()) {
            return started.error();
        }
    }
    renderer.gpu_profiler_ = make_gpu_profiler(renderer.context_.device());
    return renderer;
}

Renderer::Renderer(Context context, std::optional<Swapchain> swapchain,
                   std::optional<Image> offscreen, Image depth, TextureTable textures, Cube cube,
                   Extent framebuffer, PresentMode present_mode)
    : context_(std::move(context)), swapchain_(std::move(swapchain)),
      offscreen_(std::move(offscreen)), depth_(std::move(depth)), textures_(std::move(textures)),
      cube_(std::move(cube)), framebuffer_(framebuffer), target_framebuffer_(framebuffer),
      present_mode_(present_mode) {}

Renderer::Renderer(Renderer&& other) noexcept
    : context_(std::move(other.context_)), swapchain_(std::move(other.swapchain_)),
      offscreen_(std::move(other.offscreen_)), depth_(std::move(other.depth_)),
      textures_(std::move(other.textures_)), cube_(std::move(other.cube_)),
      framebuffer_(other.framebuffer_), target_framebuffer_(other.target_framebuffer_),
      present_mode_(other.present_mode_), swapchain_stale_(other.swapchain_stale_),
      slots_(std::exchange(other.slots_, {})), origins_(std::move(other.origins_)),
      image_rendered_(std::exchange(other.image_rendered_, {})),
      frames_done_(std::exchange(other.frames_done_, VK_NULL_HANDLE)), frame_(other.frame_),
      gpu_timer_(std::move(other.gpu_timer_)), times_(other.times_),
      gpu_profiler_(std::exchange(other.gpu_profiler_, nullptr)),
      overlay_(std::exchange(other.overlay_, nullptr)) {}

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

    for (std::size_t slot = 0; slot < frames_in_flight; ++slot) {
        Result<Buffer> origins = Buffer::create(context_.device(), max_cubes * sizeof(glm::vec4),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                                BufferMemory::per_frame);
        if (!origins.has_value()) {
            return origins.error();
        }
        origins_.push_back(std::move(origins).value());
    }

    Result<GpuTimer> timer = GpuTimer::create(context_.device(), frames_in_flight);
    if (!timer.has_value()) {
        return timer.error();
    }
    gpu_timer_ = std::move(timer).value();
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
Result<void> Renderer::upload_cube() {
    ZoneScopedN("Upload cube");
    const Device& device = context_.device();
    UploadContext uploads(device);
    VkCommandBuffer commands = slots_[0].command_buffer;
    begin_commands(commands);
    Result<void> uploaded =
        upload(uploads, commands, cube_.vertices, 0, std::as_bytes(std::span(cube_vertices)));
    if (uploaded.has_value()) {
        uploaded = upload_image(uploads, commands, cube_.checker, make_checker());
    }
    uploads.finish(commands);
    vkEndCommandBuffer(commands);
    if (!uploaded.has_value()) {
        return uploaded.error();
    }
    // The checker always stages, so there is always a copy to wait for before
    // its staging buffer goes away with `uploads`.
    if (const Result<void> submitted = submit(device.queue(), commands, {}, {});
        !submitted.has_value()) {
        return submitted.error();
    }
    vkQueueWaitIdle(device.queue());

    const Result<std::uint32_t> slot = textures_.add(cube_.checker.view());
    if (!slot.has_value()) {
        return slot.error();
    }
    cube_.checker_slot = slot.value();
    return {};
}

// Starts ImGui's Vulkan backend on the app's context: the overlay's pipeline
// for the target's format, and the font texture, which it uploads itself,
// through a command buffer of its own, when the overlay first draws.
Result<void> Renderer::start_overlay(ImGuiContext* overlay) {
    ZoneScopedN("Start overlay");
    ImGui::SetCurrentContext(overlay);
    const VkFormat format = cube_.pipeline.color_format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = context_.instance().handle();
    info.PhysicalDevice = context_.device().physical_device();
    info.Device = context_.device().handle();
    info.QueueFamily = context_.device().queue_family();
    info.Queue = context_.device().queue();
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    // ImGui reuses its vertex buffers after this many frames: the frames in
    // flight, each done before its slot comes round again.
    info.MinImageCount = frames_in_flight;
    info.ImageCount = frames_in_flight;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain = overlay_pipeline_info(&format);
    if (!ImGui_ImplVulkan_Init(&info)) {
        return Error{"ImGui's Vulkan backend did not start"};
    }
    overlay_ = overlay;
    return {};
}

Result<void> Renderer::recreate_target() {
    ZoneScopedN("Recreate target");
    // The last frames, and their presentation, may still use the old target.
    vkDeviceWaitIdle(context_.device().handle());
    VkExtent2D extent = vk_extent(framebuffer_);
    if (swapchain_.has_value()) {
        Result<Swapchain> replacement =
            Swapchain::create(context_.device(), context_.surface(), framebuffer_, present_mode_,
                              swapchain_->handle());
        if (!replacement.has_value()) {
            return replacement.error();
        }
        swapchain_ = std::move(replacement).value(); // destroys the old one
        swapchain_stale_ = false;
        extent = swapchain_->choice().extent; // the surface may have its own
        // A surface may offer a different format after a change, such as a
        // move to another display.
        if (const VkFormat format = swapchain_->choice().format.format;
            format != cube_.pipeline.color_format()) {
            Result<GraphicsPipeline> rebuilt =
                make_cube_pipeline(context_.device(), format, textures_.layout());
            if (!rebuilt.has_value()) {
                return rebuilt.error();
            }
            cube_.pipeline = std::move(rebuilt).value();
            if (overlay_ != nullptr) {
                ImGui::SetCurrentContext(overlay_);
                const ImGui_ImplVulkan_PipelineInfo overlay_pipeline =
                    overlay_pipeline_info(&format);
                ImGui_ImplVulkan_CreateMainPipeline(&overlay_pipeline);
            }
        }
    } else {
        Result<Image> replacement =
            make_offscreen_target(context_.device(), vk_extent(framebuffer_));
        if (!replacement.has_value()) {
            return replacement.error();
        }
        offscreen_ = std::move(replacement).value();
    }
    Result<Image> depth = make_depth_target(context_.device(), extent);
    if (!depth.has_value()) {
        return depth.error();
    }
    depth_ = std::move(depth).value();
    target_framebuffer_ = framebuffer_;
    return add_rendered_semaphores();
}

void Renderer::set_present_mode(PresentMode mode) noexcept {
    if (mode != present_mode_) {
        present_mode_ = mode;
        swapchain_stale_ = swapchain_.has_value(); // replaced on the next frame
    }
}

Result<Frame> Renderer::render(const FrameInputs& inputs) {
    ZoneScopedN("Render");
    if (inputs.cubes.size() > max_cubes) {
        return Error{"a frame draws at most " + std::to_string(max_cubes) + " cubes, not " +
                     std::to_string(inputs.cubes.size())};
    }
    if (inputs.overlay != nullptr && overlay_ == nullptr) {
        return Error{"a frame's overlay needs a renderer made with an overlay context"};
    }
    if (framebuffer_.width == 0 || framebuffer_.height == 0) {
        return Frame::skipped;
    }
    if (overlay_ != nullptr) {
        ImGui::SetCurrentContext(overlay_); // ImGui's backend keeps its state there
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
    const auto slot_index = static_cast<std::uint32_t>(frame % frames_in_flight);
    const FrameSlot& slot = slots_[slot_index];
    std::chrono::nanoseconds waited{0};
    // The slot is free once the last frame that used it is done. The timeline
    // starts at 0, so the first frames do not wait.
    const std::uint64_t slot_free = frame > frames_in_flight ? frame - frames_in_flight : 0;
    if (const Result<void> ready = wait_for_slot(device, frames_done_, slot_free, waited);
        !ready.has_value()) {
        return ready.error();
    }
    // That frame's timestamps are in, then, and the slot can be timed again.
    times_.gpu = gpu_timer_.read(slot_index);
    const VkSemaphoreSubmitInfo frame_done =
        semaphore_submit(frames_done_, frame, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    // The slot's origins are free with the slot. Each is a cube's block as
    // seen from the camera's, subtracted in 64-bit integers.
    const BlockAndFraction split = split_position(inputs.camera.position);
    const Buffer& origins = origins_[frame % frames_in_flight];
    for (std::size_t cube = 0; cube < inputs.cubes.size(); ++cube) {
        const glm::vec4 origin(relative_origin(inputs.cubes[cube], split.block), 0.0F);
        std::memcpy(origins.mapping().subspan(cube * sizeof(glm::vec4)).data(), &origin,
                    sizeof(origin));
    }
    if (const VkResult result = vmaFlushAllocation(context_.device().allocator(),
                                                   origins.allocation(), 0, VK_WHOLE_SIZE);
        result != VK_SUCCESS) {
        return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
    }
    const auto cubes = static_cast<std::uint32_t>(inputs.cubes.size());

    if (offscreen_.has_value()) {
        vkResetCommandPool(device, slot.command_pool, 0);
        record_frame(slot.command_buffer, gpu_timer_, slot_index, gpu_profiler_,
                     {.kind = Target::offscreen,
                      .image = offscreen_->image(),
                      .view = offscreen_->view(),
                      .extent = offscreen_->extent(),
                      .depth_image = depth_.image(),
                      .depth_view = depth_.view()},
                     {.clear = inputs.sky,
                      .pipeline = &cube_.pipeline,
                      .textures = textures_.set(),
                      .push = make_draw_push(inputs.camera, split, offscreen_->extent(),
                                             cube_.vertices.address(), origins.address(),
                                             cube_.checker_slot),
                      .cubes = cubes,
                      .overlay = inputs.overlay});
        if (const Result<void> submitted =
                submit_frame(queue, slot.command_buffer, {}, std::span(&frame_done, 1), waited);
            !submitted.has_value()) {
            return submitted.error();
        }
        frame_ = frame;
        times_.waited = waited;
        return Frame::drawn;
    }

    assert(swapchain_.has_value());
    const Swapchain& swapchain = *swapchain_;
    std::uint32_t index = 0;
    const VkResult acquired =
        acquire_image(device, swapchain.handle(), slot.image_acquired, index, waited);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale_ = true;
        return Frame::skipped;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return Error{"vkAcquireNextImageKHR failed: " + vk_result_name(acquired)};
    }
    swapchain_stale_ = acquired == VK_SUBOPTIMAL_KHR; // draw this frame, replace after it

    vkResetCommandPool(device, slot.command_pool, 0);
    record_frame(
        slot.command_buffer, gpu_timer_, slot_index, gpu_profiler_,
        {.kind = Target::swapchain,
         .image = swapchain.images()[index],
         .view = swapchain.views()[index],
         .extent = swapchain.choice().extent,
         .depth_image = depth_.image(),
         .depth_view = depth_.view()},
        {.clear = inputs.sky,
         .pipeline = &cube_.pipeline,
         .textures = textures_.set(),
         .push = make_draw_push(inputs.camera, split, swapchain.choice().extent,
                                cube_.vertices.address(), origins.address(), cube_.checker_slot),
         .cubes = cubes,
         .overlay = inputs.overlay});

    const VkSemaphoreSubmitInfo wait =
        semaphore_submit(slot.image_acquired, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const std::array<VkSemaphoreSubmitInfo, 2> signals = {
        semaphore_submit(image_rendered_[index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
        frame_done,
    };
    if (const Result<void> submitted =
            submit_frame(queue, slot.command_buffer, std::span(&wait, 1), signals, waited);
        !submitted.has_value()) {
        return submitted.error();
    }
    frame_ = frame;

    const VkResult presented =
        present_image(queue, swapchain.handle(), index, image_rendered_[index], waited);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    } else if (presented != VK_SUCCESS) {
        return Error{"vkQueuePresentKHR failed: " + vk_result_name(presented)};
    }
    times_.waited = waited;
    return Frame::drawn;
}

Result<std::vector<std::uint8_t>> Renderer::capture() {
    assert(offscreen_.has_value() && "capture reads back headless frames");
    if (frame_ == 0) {
        return Error{"there is no frame to capture yet"};
    }
    const Image& image = *offscreen_;
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
    if (gpu_profiler_ != nullptr) {
        TracyVkDestroy(gpu_profiler_);
        gpu_profiler_ = nullptr;
    }
    if (overlay_ != nullptr) {
        ImGui::SetCurrentContext(overlay_);
        ImGui_ImplVulkan_Shutdown();
        overlay_ = nullptr;
    }
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
