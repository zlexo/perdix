#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

namespace vx::test {

// A block's box, its lowest corner at (x, y, z).
constexpr Box block_box(double x, double y, double z) {
    return {.min = {.x = x, .y = y, .z = z}, .max = {.x = x + 1.0, .y = y + 1.0, .z = z + 1.0}};
}

// The standing player's box, 0.6 wide and 1.8 tall, its bottom center at
// `feet` (RFC 0010).
constexpr Box player_box(Vec3d feet) {
    return {.min = {.x = feet.x - 0.3, .y = feet.y, .z = feet.z - 0.3},
            .max = {.x = feet.x + 0.3, .y = feet.y + 1.8, .z = feet.z + 0.3}};
}

} // namespace vx::test
