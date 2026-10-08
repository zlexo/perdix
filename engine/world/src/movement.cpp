#include <vx/world/movement.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>

#include <algorithm>

namespace vx {
Box body_box(Vec3d position, Pose pose) noexcept {
    constexpr double half_width = 0.3;
    const double height = pose == Pose::crouching ? 1.5 : 1.8;
    return {.min = {.x = position.x - half_width, .y = position.y, .z = position.z - half_width},
            .max = {.x = position.x + half_width,
                    .y = position.y + height,
                    .z = position.z + half_width}};
}

double ground_acceleration(double slipperiness, bool sprinting,
                           const MovementNumbers& numbers) noexcept {
    // The game's 0.216 over the slipperiness cubed: 0.216 is 0.6 cubed.
    constexpr double formula_slipperiness = 0.6;
    const double ratio = formula_slipperiness / slipperiness;
    const double slow_start = std::min(1.0, ratio * ratio * ratio);
    return numbers.walk_acceleration * (sprinting ? numbers.sprint_factor : 1.0) * slow_start;
}

double ground_drag(double slipperiness, const MovementNumbers& numbers) noexcept {
    return slipperiness * numbers.air_drag;
}

} // namespace vx
