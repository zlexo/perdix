#include <vx/core/wake_event.hpp>

#include <chrono>
#include <cstdint>
#include <mutex>

namespace vx {

void WakeEvent::signal() noexcept {
    {
        const std::scoped_lock guard(lock_);
        signaled_ = true;
        ++signals_;
    }
    changed_.notify_one();
}

bool WakeEvent::wait_until(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock guard(lock_);
    const bool signaled = changed_.wait_until(guard, deadline, [this] { return signaled_; });
    signaled_ = false;
    return signaled;
}

std::uint64_t WakeEvent::signals() const noexcept {
    const std::scoped_lock guard(lock_);
    return signals_;
}

} // namespace vx
