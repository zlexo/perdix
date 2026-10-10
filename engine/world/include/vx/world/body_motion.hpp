#pragma once

#include <vx/core/vec3d.hpp>

namespace vx {

// The body's state a tick leaves (RFC 0012): what the camera needs to pose
// it between ticks. Yaws are the engine's (RFC 0010: 0 looks down -z, and
// positive turns left, seen from above), in radians; the pitch is positive
// looking up.
struct BodyMotion {
    double limb_speed = 0.0;    // the walk swing's speed: 0 standing, 1 at most
    double limb_position = 0.0; // the walk swing's phase: every tick's speed added up
    float body_yaw = 0.0F;      // where the body faces, from -pi to pi
    float head_yaw = 0.0F;      // the look's, set each tick
    float head_pitch = 0.0F;    // the look's, set each tick
};

// A new body: facing its look, the swing at rest, as a player appears.
[[nodiscard]] BodyMotion body_facing(float look_yaw, float look_pitch) noexcept;

// One tick's animation, after its move, as Minecraft 26.2 animates a player
// (numbers from its jar): `moved` is how far the body went over the tick,
// `look_yaw` and `look_pitch` where it looked.
//
// The walk swing follows the move the body made, not the keys: its speed
// eases 0.4 of the way toward four times the horizontal distance moved, at
// most 1, and the phase adds the speed, so walking into a wall swings
// nothing and flying swings the limbs. The head takes the look. The body
// keeps facing where it faced.
void animate(BodyMotion& motion, Vec3d moved, float look_yaw, float look_pitch) noexcept;

// A body between two ticks, as the camera poses it.
struct BodyAt {
    double limb_position = 0.0;
    double limb_amount = 0.0; // how far the limbs swing: 0 to 1
    float body_yaw = 0.0F;
    float head_yaw = 0.0F;
    float head_pitch = 0.0F;
};

// The body `between` (0 to 1) of the way from the tick that left `previous`
// to the one that left `current`, as the game draws it: the swing's phase
// runs up to the current one at the current speed, the amount is the speed
// blended between the ticks, at most 1, the yaws turn the shorter way round
// and the pitch is blended.
[[nodiscard]] BodyAt body_at(const BodyMotion& previous, const BodyMotion& current,
                             double between) noexcept;

} // namespace vx
