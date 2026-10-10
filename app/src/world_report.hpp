#pragma once

#include <vx/world/memory_budget.hpp>
#include <vx/world/region.hpp>

#include <array>
#include <cstddef>

namespace vx::app {

// What the overlay shows of streaming (RFC 0006), from the world thread when
// the main thread asks.
struct WorldReport {
    std::array<std::size_t, 8> chunks{}; // by ChunkState
    // Entries in the backlogs, stale ones included, and plans parked.
    std::size_t waiting = 0;
    // The chunk data held at the last memory check, and its budget.
    std::size_t chunk_bytes = 0;
    std::size_t budget = 0;
    // The region's limits as the budget leaves them, the player's, and why
    // the first are below the second.
    RegionLimits limits{};
    RegionLimits chosen{};
    LimitedBy limited_by = LimitedBy::nothing;
};

} // namespace vx::app
