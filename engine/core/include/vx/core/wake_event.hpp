#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace vx {

// A latched flag one thread sleeps on (RFC 0005): a signal sent while nobody
// waits is kept, so one sent just before the owner sleeps is not lost, and
// any number of signals before a wait end it once.
class WakeEvent {
public:
    // Any thread. The event must outlive the call: it notifies after letting
    // go of its lock, so the owner's wait can return before signal() does.
    void signal() noexcept;
    // The owner: returns once signaled, or at `deadline`, and clears the
    // flag. True when it was signaled.
    bool wait_until(std::chrono::steady_clock::time_point deadline);
    // Signals sent so far: for plots and tests.
    [[nodiscard]] std::uint64_t signals() const noexcept;

private:
    mutable std::mutex lock_;
    std::condition_variable changed_;
    bool signaled_ = false;
    std::uint64_t signals_ = 0;
};

} // namespace vx
