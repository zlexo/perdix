#include <vx/world/memory_budget.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>

namespace vx {

void MemoryBudget::choose(RegionLimits chosen) noexcept {
    const bool held_back = limits_ != chosen_;
    chosen_ = chosen;
    // The player's new limits start the raises afresh.
    back_off_ = 1;
    raised_at_.reset();
    if (!held_back) {
        limits_ = chosen;
    } else {
        limits_.view_distance = std::min(limits_.view_distance, chosen.view_distance);
        limits_.hysteresis = std::min(limits_.hysteresis, chosen.hysteresis);
    }
    if (limits_ == chosen_) {
        limited_by_ = LimitedBy::nothing;
        calm_since_.reset();
    }
}

bool MemoryBudget::check(const MemoryUse& use, std::chrono::steady_clock::time_point now) noexcept {
    const bool chunks_over = use.chunk_bytes > bytes_;
    if (chunks_over || use.arena_full) {
        calm_since_.reset();
        // Over again before the next raise was due: that raise failed.
        if (raised_at_ && now - *raised_at_ < calm_needed()) {
            back_off_ = std::min(back_off_ * 2, most_back_off);
        }
        raised_at_.reset();
        limited_by_ = chunks_over ? LimitedBy::chunk_memory : LimitedBy::mesh_memory;
        if (limits_.hysteresis > 0) {
            --limits_.hysteresis;
        } else if (limits_.view_distance > floor_) {
            --limits_.view_distance;
        } else {
            return false; // as low as it goes
        }
        return true;
    }
    // Under 90% of both budgets.
    const bool calm = use.chunk_bytes <= bytes_ / 10 * 9 && use.arena_share <= 0.9;
    if (!calm || limits_ == chosen_) {
        calm_since_.reset();
        return false;
    }
    if (!calm_since_) {
        calm_since_ = now;
        return false;
    }
    if (now - *calm_since_ < calm_needed()) {
        return false;
    }
    // The raise before this one held until this one was due.
    if (raised_at_) {
        back_off_ = 1;
    }
    // The next step waits again.
    calm_since_ = now;
    raised_at_ = now;
    if (limits_.view_distance < chosen_.view_distance) {
        ++limits_.view_distance;
    } else {
        ++limits_.hysteresis;
    }
    if (limits_ == chosen_) {
        limited_by_ = LimitedBy::nothing;
    }
    return true;
}

} // namespace vx
