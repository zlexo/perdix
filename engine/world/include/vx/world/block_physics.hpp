#pragma once

#include <cstdint>

namespace vx {

// Whether a block has a box to collide with (RFC 0010): a full cube, or
// nothing, as for a flower.
enum class Collision : std::uint8_t { none, full };

// What movement needs to know about a block state (RFC 0010), one per state,
// built by the block registry (RFC 0004): whether it is a full cube to
// collide with, and how slippery it is to stand on, above 0 and at most 1:
// 0.6 by default, 0.98 for ice.
struct BlockPhysics {
    Collision collision = Collision::none;
    double slipperiness = 0.6;

    friend constexpr bool operator==(const BlockPhysics&, const BlockPhysics&) = default;
};

} // namespace vx
