#include <vx/world/body_motion.hpp>

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vx {
namespace {

constexpr double pi = std::numbers::pi;
constexpr double full_turn = 2.0 * pi;

constexpr double radians(double degrees) noexcept {
    return degrees * pi / 180.0;
}

// Minecraft 26.2's numbers for a player's body, from its jar. The walk
// swing's speed eases this far toward its target each tick, a target of
// this much per block moved across, at most 1.
constexpr double swing_easing = 0.4;
constexpr double swing_per_block = 4.0;
// A move turns the body only when the square of its distance across is
// more than this, compared as a float, as the game compares it: a hair over
// 0.05 squared, so a move of exactly 0.05 blocks does not count.
constexpr float least_turning_move_squared = 0.0025000002F;
// A move this far from the look, turning either way, is walking backward:
// the body faces away from it.
constexpr double backward_from = radians(95.0);
constexpr double backward_to = radians(265.0);
// The body turns this much of the way to its target each tick, and stays
// within this of the look.
constexpr double body_easing = 0.3;
constexpr double most_off_look = radians(50.0);

// How far `to` lies from `from` turning left, from 0 up to a full turn.
double leftward(double from, double to) noexcept {
    const double turn = std::fmod(to - from, full_turn);
    return turn < 0.0 ? turn + full_turn : turn;
}

} // namespace

double wrap_turn(double angle) noexcept {
    const double turn = std::fmod(angle, full_turn);
    if (turn <= -pi) {
        return turn + full_turn;
    }
    if (turn > pi) {
        return turn - full_turn;
    }
    return turn;
}

BodyMotion body_facing(float look_yaw, float look_pitch) noexcept {
    return {.body_yaw = static_cast<float>(wrap_turn(static_cast<double>(look_yaw))),
            .head_yaw = look_yaw,
            .head_pitch = look_pitch};
}

void animate(BodyMotion& motion, Vec3d moved, float look_yaw, float look_pitch) noexcept {
    // A NaN would stay in the swing and the body for good: a tick that
    // brings one leaves the body as it was, as the move's tick does.
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

    // The body aims along a move that went far enough, or against it when it
    // went backward; otherwise it keeps its facing, and only the look's hold
    // turns it.
    const auto look = static_cast<double>(look_yaw);
    auto body = static_cast<double>(motion.body_yaw);
    double target = body;
    if (static_cast<float>((moved.x * moved.x) + (moved.z * moved.z)) >
        least_turning_move_squared) {
        // The engine's yaw of the move: forward is (-sin yaw, -cos yaw).
        const double way = std::atan2(-moved.x, -moved.z);
        const double off_look = leftward(look, way);
        const bool backward = off_look > backward_from && off_look < backward_to;
        target = backward ? way + pi : way;
    }
    body += wrap_turn(target - body) * body_easing;
    const double from_look = wrap_turn(look - body);
    if (std::abs(from_look) > most_off_look) {
        body += from_look - std::copysign(most_off_look, from_look);
    }
    motion.body_yaw = static_cast<float>(wrap_turn(body));
}

BodyAt body_at(const BodyMotion& previous, const BodyMotion& current, double between) noexcept {
    const auto turned = [between](float from, float to) {
        const auto start = static_cast<double>(from);
        return static_cast<float>(
            wrap_turn(start + (wrap_turn(static_cast<double>(to) - start) * between)));
    };
    const double speed =
        previous.limb_speed + ((current.limb_speed - previous.limb_speed) * between);
    return {.limb_position = current.limb_position - (current.limb_speed * (1.0 - between)),
            .limb_amount = std::min(speed, 1.0),
            .body_yaw = turned(previous.body_yaw, current.body_yaw),
            .head_yaw = turned(previous.head_yaw, current.head_yaw),
            .head_pitch = static_cast<float>(
                static_cast<double>(previous.head_pitch) +
                (static_cast<double>(current.head_pitch - previous.head_pitch) * between))};
}

} // namespace vx
