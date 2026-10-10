#include <vx/world/body_motion.hpp>

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vx {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double full_turn = 2.0 * pi;

// Minecraft 26.2's numbers for a player's body, from its jar. The walk
// swing's speed eases this far toward its target each tick, a target of
// this much per block moved across, at most 1.
constexpr double swing_easing = 0.4;
constexpr double swing_per_block = 4.0;

// `angle` as a turn from above -pi up to pi. The game wraps its angles to
// [-180, 180) degrees and turns the other way, so a half turn is +pi here:
// left, as the game turns it.
double wrapped(double angle) noexcept {
    const double turn = std::fmod(angle, full_turn);
    if (turn <= -pi) {
        return turn + full_turn;
    }
    if (turn > pi) {
        return turn - full_turn;
    }
    return turn;
}

} // namespace

BodyMotion body_facing(float look_yaw, float look_pitch) noexcept {
    return {.body_yaw = static_cast<float>(wrapped(static_cast<double>(look_yaw))),
            .head_yaw = look_yaw,
            .head_pitch = look_pitch};
}

void animate(BodyMotion& motion, Vec3d moved, float look_yaw, float look_pitch) noexcept {
    // A NaN would stay in the swing for good: a tick that brings one leaves
    // the body as it was, as the move's tick does.
    if (!std::isfinite(moved.x) || !std::isfinite(moved.z) || !std::isfinite(look_yaw) ||
        !std::isfinite(look_pitch)) {
        return;
    }

    // The swing follows the distance moved across, whatever moved the body.
    const double across = std::hypot(moved.x, moved.z);
    const double target_speed = std::min(across * swing_per_block, 1.0);
    motion.limb_speed += (target_speed - motion.limb_speed) * swing_easing;
    motion.limb_position += motion.limb_speed;

    motion.head_yaw = look_yaw;
    motion.head_pitch = look_pitch;
}

} // namespace vx
