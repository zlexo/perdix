#pragma once

#include <vx/world/region.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace vx {

// What a memory check hears (RFC 0006).
struct MemoryUse {
    // The chunk data held: blocks and the bookkeeping that keeps them.
    std::size_t chunk_bytes = 0;
    // RFC 0007's mesh arena: a mesh found no room and the arena cannot grow,
    // and the share of the most it may grow to that it holds, 0 to 1.
    bool arena_full = false;
    double arena_share = 0.0;
};

// Why the limits are below the player's.
enum class LimitedBy : std::uint8_t {
    nothing,
    chunk_memory, // chunk data over its budget
    mesh_memory,  // the mesh arena full, and unable to grow
};

// RFC 0006's memory budget. Each check, a few times a second, sees the chunk
// data held and the mesh arena's state:
//
// - Over budget, or with the arena full, the limits drop one step: first
//   the hysteresis, so the chunks kept only by it leave, the farthest ring
//   first; then the view distance, a chunk at a time, down to the floor: 4,
//   or the near cylinder's radius when that is wider, since the cylinder
//   keeps what a nearer distance would let go.
// - Once both stay under 90% of their budgets for 5 seconds, the limits
//   rise one step, and again after each further 5 seconds: the view
//   distance first, back to the player's, then the hysteresis.
// - A raise the budget goes back over before the next raise is due failed:
//   the next one waits twice as long, up to 16 times, so a ring larger than
//   the 10% between calm and the budget is not loaded and evicted every 5
//   seconds. A raise that holds until the next is due sets the wait back.
//
// The chunks inside the region the limits leave are never evicted, and
// neither are edited ones, which the streamer keeps outside every region.
class MemoryBudget {
public:
    static constexpr std::size_t default_bytes = std::size_t{512} << 20U;
    static constexpr std::chrono::seconds calm_time{5};
    static constexpr std::int32_t least_distance = 4;
    static constexpr std::uint32_t most_back_off = 16;

    // `floor` is the nearest view distance the budget lowers to; at least 4.
    MemoryBudget(std::size_t bytes, RegionLimits chosen,
                 std::int32_t floor = least_distance) noexcept
        : bytes_(bytes), floor_(std::max(floor, least_distance)), chosen_(chosen), limits_(chosen) {
    }

    // The player's limits, from the Render Distance option: the budget's
    // never pass them. Followed at once when the budget holds nothing back;
    // otherwise the limits come back to them a step at a time.
    void choose(RegionLimits chosen) noexcept;
    // A check: true when the limits changed.
    bool check(const MemoryUse& use, std::chrono::steady_clock::time_point now) noexcept;

    [[nodiscard]] RegionLimits limits() const noexcept { return limits_; }
    [[nodiscard]] RegionLimits chosen() const noexcept { return chosen_; }
    [[nodiscard]] LimitedBy limited_by() const noexcept { return limited_by_; }
    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
    // How long both budgets must stay under 90% for the next raise.
    [[nodiscard]] std::chrono::seconds calm_needed() const noexcept {
        return calm_time * back_off_;
    }

private:
    std::size_t bytes_;
    std::int32_t floor_;
    RegionLimits chosen_;
    RegionLimits limits_;
    LimitedBy limited_by_ = LimitedBy::nothing;
    // Since when both budgets have stayed under 90%, while the limits are
    // below the player's.
    std::optional<std::chrono::steady_clock::time_point> calm_since_;
    // The last raise, until the next is due or the budget goes back over.
    std::optional<std::chrono::steady_clock::time_point> raised_at_;
    std::uint32_t back_off_ = 1;
};

} // namespace vx
