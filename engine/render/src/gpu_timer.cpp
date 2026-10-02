#include <vx/render/gpu_timer.hpp>

#include "debug_name.hpp"
#include "vk_result.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <utility>

namespace vx::render {

std::uint32_t timestamp_valid_bits(const Device& device) {
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physical_device(), &count, families.data());
    return families.at(device.queue_family()).timestampValidBits;
}

std::chrono::nanoseconds timestamp_interval(std::uint64_t first, std::uint64_t second,
                                            std::uint32_t valid_bits, double nanoseconds_per_tick) {
    assert(valid_bits > 0 && valid_bits <= 64);
    const std::uint64_t mask =
        valid_bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << valid_bits) - 1;
    const std::uint64_t ticks = (second - first) & mask;
    return std::chrono::nanoseconds(
        std::llround(static_cast<double>(ticks) * nanoseconds_per_tick));
}

Result<GpuTimer> GpuTimer::create(const Device& device, std::uint32_t slots) {
    GpuTimer timer;
    const std::uint32_t bits = timestamp_valid_bits(device);
    if (bits == 0) {
        return timer;
    }

    const VkQueryPoolCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = 2 * slots,
        .pipelineStatistics = 0,
    };
    if (const VkResult result = vkCreateQueryPool(device.handle(), &info, nullptr, &timer.pool_);
        result != VK_SUCCESS) {
        return Error{"vkCreateQueryPool failed: " + vk_result_name(result)};
    }
    timer.device_ = device.handle();
    set_debug_name(timer.device_, timer.pool_, "GPU frame timer");

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.physical_device(), &properties);
    timer.valid_bits_ = bits;
    timer.nanoseconds_per_tick_ = static_cast<double>(properties.limits.timestampPeriod);
    timer.begun_.assign(slots, false);
    return timer;
}

GpuTimer::GpuTimer(GpuTimer&& other) noexcept
    : device_(std::exchange(other.device_, VK_NULL_HANDLE)),
      pool_(std::exchange(other.pool_, VK_NULL_HANDLE)), valid_bits_(other.valid_bits_),
      nanoseconds_per_tick_(other.nanoseconds_per_tick_), begun_(std::move(other.begun_)) {}

GpuTimer& GpuTimer::operator=(GpuTimer&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ = std::exchange(other.device_, VK_NULL_HANDLE);
        pool_ = std::exchange(other.pool_, VK_NULL_HANDLE);
        valid_bits_ = other.valid_bits_;
        nanoseconds_per_tick_ = other.nanoseconds_per_tick_;
        begun_ = std::move(other.begun_);
    }
    return *this;
}

GpuTimer::~GpuTimer() {
    destroy();
}

void GpuTimer::begin(VkCommandBuffer commands, std::uint32_t slot) {
    if (pool_ == VK_NULL_HANDLE) {
        return;
    }
    assert(slot < begun_.size());
    vkCmdResetQueryPool(commands, pool_, 2 * slot, 2);
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, 2 * slot);
    begun_[slot] = true;
}

void GpuTimer::end(VkCommandBuffer commands, std::uint32_t slot) const {
    if (pool_ == VK_NULL_HANDLE) {
        return;
    }
    assert(slot < begun_.size());
    vkCmdWriteTimestamp2(commands, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, (2 * slot) + 1);
}

std::optional<std::chrono::nanoseconds> GpuTimer::read(std::uint32_t slot) const {
    if (pool_ == VK_NULL_HANDLE || !begun_.at(slot)) {
        return std::nullopt;
    }
    // Without VK_QUERY_RESULT_WAIT_BIT: VK_NOT_READY when the commands are
    // not done, and VK_SUCCESS only when both timestamps are in.
    std::array<std::uint64_t, 2> ticks{};
    if (vkGetQueryPoolResults(device_, pool_, 2 * slot, 2, sizeof(ticks), ticks.data(),
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return std::nullopt;
    }
    return timestamp_interval(ticks[0], ticks[1], valid_bits_, nanoseconds_per_tick_);
}

void GpuTimer::destroy() noexcept {
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
}

} // namespace vx::render
