#pragma once

#include <vx/core/vec3d.hpp>

namespace vx {

// Where a head turns, as Minecraft 26.2's look control turns a mob's (RFC
// 0012; numbers from its jar). Given a point, the head's yaw turns toward it
// by at most 10 degrees a tick and its pitch goes straight to the point's,
// held within 40 degrees up or down; the point holds for the tick it is
// given and the next. With none, the pitch is level and the yaw turns back
// toward the body's by at most 10 degrees a tick. Yaws are the engine's (RFC
// 0010), in radians, from -pi to pi; the pitch is positive looking up.
class LookControl {
public:
    explicit LookControl(float yaw = 0.0F) noexcept;

    // Looks at `point` from this tick to the next.
    void look_at(Vec3d point) noexcept;
    // Turns the head for one tick, its eyes at `eye`.
    void tick(Vec3d eye, float body_yaw) noexcept;

    [[nodiscard]] float yaw() const noexcept { return yaw_; }
    [[nodiscard]] float pitch() const noexcept { return pitch_; }

private:
    Vec3d point_;
    float yaw_;
    float pitch_ = 0.0F;
    int hold_ = 0; // ticks the point still holds
};

} // namespace vx
