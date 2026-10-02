#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <volk.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx::render {

// How many bits of each timestamp the device's queue writes: 36 to 64, or 0
// when it writes none.
[[nodiscard]] std::uint32_t timestamp_valid_bits(const Device& device);

// The time from timestamp `first` to `second`, of `valid_bits` bits and ticks
// `nanoseconds_per_tick` long (VkPhysicalDeviceLimits::timestampPeriod). The
// counter may have wrapped past zero in between.
[[nodiscard]] std::chrono::nanoseconds timestamp_interval(std::uint64_t first, std::uint64_t second,
                                                          std::uint32_t valid_bits,
                                                          double nanoseconds_per_tick);

// Times command buffers on the GPU, with two timestamp queries for each of a
// fixed number of slots: RFC 0001's GPU frame time, which the overlay shows
// with or without Tracy. A slot is read without waiting, once the commands
// that timed it are done: the renderer reads a frame slot's last frame when
// the slot comes round again.
//
// KosmicKrisp (SDK 1.4.363.0) times from the first render pass on: a fill
// before it is left out, and a fill alone takes no time, its two timestamps
// equal. A frame starts with its first pass, so it is timed whole.
class GpuTimer {
public:
    // A timer of `slots` slots; one that times nothing when the device's
    // queue writes no timestamps.
    [[nodiscard]] static Result<GpuTimer> create(const Device& device, std::uint32_t slots);

    GpuTimer() = default; // times nothing
    GpuTimer(GpuTimer&& other) noexcept;
    GpuTimer& operator=(GpuTimer&& other) noexcept;
    GpuTimer(const GpuTimer&) = delete;
    GpuTimer& operator=(const GpuTimer&) = delete;
    ~GpuTimer();

    // Starts timing `slot` in `commands`, outside any rendering: resets the
    // slot's queries, then writes the first timestamp once every earlier
    // command in the queue is done, as Tracy's GPU zones do, so the time
    // leaves out the tail of the frame before. The commands must be
    // submitted before the slot is read.
    void begin(VkCommandBuffer commands, std::uint32_t slot);
    // Ends it: the second timestamp, likewise once every earlier command is
    // done.
    void end(VkCommandBuffer commands, std::uint32_t slot) const;
    // The time from the first timestamp to the second, once the commands
    // that wrote them are done. Empty for a slot never begun, from a timer
    // that times nothing, and while the driver has not delivered them:
    // KosmicKrisp's can come in after the semaphore that says the commands
    // are done.
    [[nodiscard]] std::optional<std::chrono::nanoseconds> read(std::uint32_t slot) const;

    [[nodiscard]] bool times() const noexcept { return pool_ != VK_NULL_HANDLE; }

private:
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    std::uint32_t valid_bits_ = 0;
    double nanoseconds_per_tick_ = 0.0;
    std::vector<bool> begun_;
};

} // namespace vx::render
