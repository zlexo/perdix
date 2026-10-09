// RFC 0007's draw overhead benchmark (its pull request plan item 2): what
// 10,000 indexed indirect commands cost to record, submit and draw, with
// quads and empty, against one instanced draw of 64-quad clusters covering
// the same quads. Open question 1 keeps RFC 0007's merged runs unless
// recording 10,000 of them takes over 2 ms of CPU on KosmicKrisp. MoltenVK
// encodes its Metal commands when they are submitted, not while they are
// recorded, so its CPU time is the submit call's.
//
// The loader picks the driver: to compare the two on a Mac, run it once with
// each manifest the Vulkan SDK installs,
//   VK_DRIVER_FILES=<sdk>/macOS/share/vulkan/icd.d/libkosmickrisp_icd.json
//   VK_DRIVER_FILES=<sdk>/macOS/share/vulkan/icd.d/MoltenVK_icd.json
// Run a release build for numbers. --quick draws each variant once, so CI
// checks it still runs, on lavapipe, and times nothing.

#include <vx/core/result.hpp>
#include <vx/core/thread.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/context.hpp>
#include <vx/render/device.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/image.hpp>
#include <vx/render/pipeline.hpp>
#include <vx/shaders/draw_overhead.hpp>

#include <volk.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t draws = 10'000;
constexpr std::uint32_t quads_per_run = 16;
constexpr std::uint32_t cluster_quads = 64;
constexpr std::uint32_t total_quads = draws * quads_per_run;    // 160,000
constexpr std::uint32_t clusters = total_quads / cluster_quads; // 2,500
constexpr VkExtent2D extent{.width = 1280, .height = 720};

// The shader's quad and push constants, as draw_overhead.slang lays them out.
struct BenchQuad {
    float x = 0.0F;
    float y = 0.0F;
    float size = 0.0F;
    std::uint32_t unused = 0;
};

struct BenchPush {
    VkDeviceAddress quads = 0;
    VkDeviceAddress origins = 0;
    std::uint32_t cluster_quads = 0;
    std::array<std::uint32_t, 3> padding{};
};
static_assert(sizeof(BenchPush) == 32);

enum class Variant : std::uint8_t { runs, empty_runs, clusters };

constexpr std::string_view variant_name(Variant variant) {
    switch (variant) {
    case Variant::runs:
        return "10,000 indirect commands, 16 quads each";
    case Variant::empty_runs:
        return "10,000 indirect commands, empty";
    case Variant::clusters:
        return "1 instanced draw, 2,500 clusters of 64 quads";
    }
    return "";
}

// One variant's times over the frames: recording the commands, the draw
// call alone within them, the submit call, then waiting for the queue, and
// the GPU's.
struct Times {
    std::vector<double> record_us;
    std::vector<double> draw_call_us;
    std::vector<double> submit_us;
    std::vector<double> wait_us;
    std::vector<double> gpu_us;
};

double median(std::vector<double> values) {
    if (values.empty()) {
        return 0.0;
    }
    std::ranges::sort(values);
    return values[values.size() / 2];
}

double microseconds(Clock::duration duration) {
    return std::chrono::duration<double, std::micro>(duration).count();
}

template <class T> void fill(const vx::render::Buffer& buffer, std::span<const T> values) {
    std::memcpy(buffer.mapping().data(), values.data(), values.size_bytes());
}

vx::Result<vx::render::Buffer> make_buffer(const vx::render::Device& device, VkDeviceSize size,
                                           VkBufferUsageFlags usage, std::string_view name) {
    // Host-visible and mapped wherever the device puts it: the benchmark
    // writes everything once, from the CPU.
    return vx::render::Buffer::create(device, size, usage, vx::render::BufferMemory::per_frame,
                                      name);
}

void to_color_attachment(VkCommandBuffer commands, VkImage image) {
    const VkImageMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .baseMipLevel = 0,
                             .levelCount = 1,
                             .baseArrayLayer = 0,
                             .layerCount = 1},
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

// What every frame draws with.
struct Scene {
    const vx::render::GraphicsPipeline* pipeline;
    const vx::render::Image* target;
    const vx::render::Buffer* indices;
    const vx::render::Buffer* commands;
    const vx::render::Buffer* empty_commands;
    BenchPush push;
};

// Records one frame of `variant` into `commands`, timing the draw call alone
// into `draw_call`.
void record(const Scene& scene, Variant variant, VkCommandBuffer commands,
            vx::render::GpuTimer& timer, Clock::duration& draw_call) {
    timer.begin(commands, 0);
    to_color_attachment(commands, scene.target->image());
    const VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = scene.target->view(),
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
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
        .pDepthAttachment = nullptr,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands, &rendering);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, scene.pipeline->handle());
    const VkViewport viewport = {.x = 0.0F,
                                 .y = 0.0F,
                                 .width = static_cast<float>(extent.width),
                                 .height = static_cast<float>(extent.height),
                                 .minDepth = 0.0F,
                                 .maxDepth = 1.0F};
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindIndexBuffer(commands, scene.indices->handle(), 0, VK_INDEX_TYPE_UINT32);
    BenchPush push = scene.push;
    push.cluster_quads = variant == Variant::clusters ? cluster_quads : 0;
    vkCmdPushConstants(commands, scene.pipeline->layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(push), &push);
    const Clock::time_point before = Clock::now();
    switch (variant) {
    case Variant::runs:
        vkCmdDrawIndexedIndirect(commands, scene.commands->handle(), 0, draws,
                                 sizeof(VkDrawIndexedIndirectCommand));
        break;
    case Variant::empty_runs:
        vkCmdDrawIndexedIndirect(commands, scene.empty_commands->handle(), 0, draws,
                                 sizeof(VkDrawIndexedIndirectCommand));
        break;
    case Variant::clusters:
        vkCmdDrawIndexed(commands, cluster_quads * 6, clusters, 0, 0, 0);
        break;
    }
    draw_call = Clock::now() - before;
    vkCmdEndRendering(commands);
    timer.end(commands, 0);
}

int run(bool quick) {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = false});
    if (!context.has_value()) {
        std::cerr << "error: " << context.error().message << '\n';
        return 1;
    }
    const vx::render::Device& device = context.value().device();
    std::cout << "draw overhead on " << context.value().device_info().name << ", "
              << context.value().device_info().driver_name << " "
              << context.value().device_info().driver_info << '\n';

    // The quads in a grid across the target, a run's 16 side by side, each
    // run placed by its origin.
    std::vector<BenchQuad> quads(total_quads);
    for (std::uint32_t at = 0; at < total_quads; ++at) {
        quads[at] = {.x = static_cast<float>(at % quads_per_run) * 0.004F,
                     .y = 0.0F,
                     .size = 0.003F,
                     .unused = 0};
    }
    std::vector<float> origins(std::size_t{draws} * 4);
    for (std::uint32_t run_index = 0; run_index < draws; ++run_index) {
        origins[std::size_t{run_index} * 4] =
            -0.95F + (static_cast<float>(run_index % 25) * 0.075F);
        origins[(std::size_t{run_index} * 4) + 1] =
            -0.95F + (static_cast<float>(run_index / 25) * 0.0047F);
    }
    // Clusters cover the same quads: give each quad its run's origin.
    std::vector<BenchQuad> cluster_quads_data = quads;
    for (std::uint32_t at = 0; at < total_quads; ++at) {
        const std::size_t run_index = at / quads_per_run;
        cluster_quads_data[at].x += origins[run_index * 4];
        cluster_quads_data[at].y += origins[(run_index * 4) + 1];
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(std::size_t{cluster_quads} * 6);
    for (std::uint32_t quad = 0; quad < cluster_quads; ++quad) {
        for (const std::uint32_t corner : {0U, 1U, 2U, 2U, 3U, 0U}) {
            indices.push_back((quad * 4) + corner);
        }
    }
    std::vector<VkDrawIndexedIndirectCommand> commands(draws);
    std::vector<VkDrawIndexedIndirectCommand> empty(draws);
    for (std::uint32_t run_index = 0; run_index < draws; ++run_index) {
        commands[run_index] = {.indexCount = quads_per_run * 6,
                               .instanceCount = 1,
                               .firstIndex = 0,
                               .vertexOffset =
                                   static_cast<std::int32_t>(4 * quads_per_run * run_index),
                               .firstInstance = run_index};
        empty[run_index] = commands[run_index];
        empty[run_index].indexCount = 0;
    }

    constexpr VkBufferUsageFlags pulled =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vx::Result<vx::render::Buffer> quad_buffer =
        make_buffer(device, sizeof(BenchQuad) * total_quads, pulled, "bench quads");
    vx::Result<vx::render::Buffer> cluster_buffer =
        make_buffer(device, sizeof(BenchQuad) * total_quads, pulled, "bench cluster quads");
    vx::Result<vx::render::Buffer> origin_buffer =
        make_buffer(device, sizeof(float) * origins.size(), pulled, "bench origins");
    vx::Result<vx::render::Buffer> index_buffer =
        make_buffer(device, sizeof(std::uint32_t) * indices.size(),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "bench indices");
    vx::Result<vx::render::Buffer> command_buffer =
        make_buffer(device, sizeof(VkDrawIndexedIndirectCommand) * draws,
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "bench commands");
    vx::Result<vx::render::Buffer> empty_buffer =
        make_buffer(device, sizeof(VkDrawIndexedIndirectCommand) * draws,
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "bench empty commands");
    for (const vx::Result<vx::render::Buffer>* buffer :
         {&quad_buffer, &cluster_buffer, &origin_buffer, &index_buffer, &command_buffer,
          &empty_buffer}) {
        if (!buffer->has_value()) {
            std::cerr << "error: " << buffer->error().message << '\n';
            return 1;
        }
        if (buffer->value().mapping().empty()) {
            std::cerr << "error: the device offers no host-visible memory for the benchmark\n";
            return 1;
        }
    }
    fill<BenchQuad>(quad_buffer.value(), quads);
    fill<BenchQuad>(cluster_buffer.value(), cluster_quads_data);
    fill<float>(origin_buffer.value(), origins);
    fill<std::uint32_t>(index_buffer.value(), indices);
    fill<VkDrawIndexedIndirectCommand>(command_buffer.value(), commands);
    fill<VkDrawIndexedIndirectCommand>(empty_buffer.value(), empty);

    const vx::Result<vx::render::Image> target = vx::render::Image::create(
        device, extent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, "bench target");
    const vx::Result<vx::render::GraphicsPipeline> pipeline =
        vx::render::GraphicsPipeline::create(device,
                                             {.spirv = vx::shaders::draw_overhead,
                                              .vertex_entry = "vertex_main",
                                              .fragment_entry = "fragment_main",
                                              .color_format = VK_FORMAT_R8G8B8A8_UNORM,
                                              .depth_format = VK_FORMAT_UNDEFINED,
                                              .cull_mode = VK_CULL_MODE_NONE,
                                              .polygon_mode = VK_POLYGON_MODE_FILL,
                                              .set_layout = VK_NULL_HANDLE,
                                              .push_constant_size = sizeof(BenchPush),
                                              .push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT},
                                             "draw overhead");
    vx::Result<vx::render::GpuTimer> timer = vx::render::GpuTimer::create(device, 1);
    if (!target.has_value() || !pipeline.has_value() || !timer.has_value()) {
        std::cerr << "error: the target, pipeline or timer could not be made\n";
        return 1;
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.queue_family(),
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool);
    const VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commands_handle = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device.handle(), &allocate, &commands_handle);

    Scene scene{.pipeline = &pipeline.value(),
                .target = &target.value(),
                .indices = &index_buffer.value(),
                .commands = &command_buffer.value(),
                .empty_commands = &empty_buffer.value(),
                .push = {.quads = quad_buffer.value().address(),
                         .origins = origin_buffer.value().address(),
                         .cluster_quads = 0,
                         .padding = {}}};
    const int frames = quick ? 1 : 60;
    for (const Variant variant : {Variant::runs, Variant::empty_runs, Variant::clusters}) {
        scene.push.quads = variant == Variant::clusters ? cluster_buffer.value().address()
                                                        : quad_buffer.value().address();
        Times times;
        // A few frames first, so every pipeline and driver cache is warm.
        const int warm = quick ? 0 : 5;
        for (int frame = 0; frame < warm + frames; ++frame) {
            vkResetCommandBuffer(commands_handle, 0);
            const VkCommandBufferBeginInfo begin = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo = nullptr,
            };
            const Clock::time_point record_start = Clock::now();
            vkBeginCommandBuffer(commands_handle, &begin);
            Clock::duration draw_call{};
            record(scene, variant, commands_handle, timer.value(), draw_call);
            vkEndCommandBuffer(commands_handle);
            const Clock::duration recorded = Clock::now() - record_start;
            const VkCommandBufferSubmitInfo buffer_info = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext = nullptr,
                .commandBuffer = commands_handle,
                .deviceMask = 0,
            };
            const VkSubmitInfo2 submit = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                .pNext = nullptr,
                .flags = 0,
                .waitSemaphoreInfoCount = 0,
                .pWaitSemaphoreInfos = nullptr,
                .commandBufferInfoCount = 1,
                .pCommandBufferInfos = &buffer_info,
                .signalSemaphoreInfoCount = 0,
                .pSignalSemaphoreInfos = nullptr,
            };
            const Clock::time_point submit_start = Clock::now();
            vkQueueSubmit2(device.queue(), 1, &submit, VK_NULL_HANDLE);
            const Clock::time_point submitted = Clock::now();
            vkQueueWaitIdle(device.queue());
            const Clock::time_point idle = Clock::now();
            if (frame < warm) {
                continue;
            }
            times.record_us.push_back(microseconds(recorded));
            times.draw_call_us.push_back(microseconds(draw_call));
            times.submit_us.push_back(microseconds(submitted - submit_start));
            times.wait_us.push_back(microseconds(idle - submitted));
            // KosmicKrisp can deliver timestamps after the queue is idle.
            std::optional<std::chrono::nanoseconds> gpu;
            for (int attempt = 0; attempt < 100 && !gpu; ++attempt) {
                gpu = timer.value().read(0);
                if (!gpu) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
            if (gpu) {
                times.gpu_us.push_back(std::chrono::duration<double, std::micro>(*gpu).count());
            }
        }
        std::cout << "  " << variant_name(variant) << ": record " << median(times.record_us)
                  << " us (the draw call " << median(times.draw_call_us) << " us), submit "
                  << median(times.submit_us) << " us, then idle after " << median(times.wait_us)
                  << " us, GPU " << median(times.gpu_us) << " us, medians of " << frames << '\n';
    }
    vkDestroyCommandPool(device.handle(), pool, nullptr);
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::span<char* const> args(argv, static_cast<std::size_t>(argc));
    bool quick = false;
    for (const std::string_view arg : args.subspan(1)) {
        if (arg != "--quick") {
            std::cerr << "unknown option \"" << arg << "\": try --quick\n";
            return 2;
        }
        quick = true;
    }
    // On a performance core, where the main thread runs (RFC 0005): an
    // efficiency core times the CPU's side slower.
    vx::set_thread_qos(vx::ThreadQos::user_interactive);
    return run(quick);
}
