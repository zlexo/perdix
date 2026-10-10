#include <vx/world/life.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vx {
namespace {

constexpr double pi = std::numbers::pi;

constexpr double radians(double degrees) noexcept {
    return degrees * pi / 180.0;
}

// Minecraft 26.2's look control, from its jar: the head's yaw turns at most
// this much a tick, toward a point or back to the body; the pitch stays
// within this of level; a point holds this many ticks. A point nearer than
// the game's 1e-5 (as a float) across, or up and down, leaves the head's
// yaw, or its pitch, as it is.
constexpr double head_turn = radians(10.0);
constexpr double most_pitch = radians(40.0);
constexpr int point_hold = 2;
constexpr double least_offset = 9.999999747378752e-6;

// `from` turned toward `to` by at most `most`, the shorter way round.
double turned_toward(double from, double to, double most) noexcept {
    return wrap_turn(from + std::clamp(wrap_turn(to - from), -most, most));
}

} // namespace

LookControl::LookControl(float yaw) noexcept
    : yaw_(static_cast<float>(wrap_turn(static_cast<double>(yaw)))) {}

void LookControl::look_at(Vec3d point) noexcept {
    point_ = point;
    hold_ = point_hold;
}

void LookControl::tick(Vec3d eye, float body_yaw) noexcept {
    // Level unless a point holds, as the game resets a mob's pitch each tick.
    pitch_ = 0.0F;
    if (hold_ <= 0) {
        yaw_ = static_cast<float>(
            turned_toward(static_cast<double>(yaw_), static_cast<double>(body_yaw), head_turn));
        return;
    }
    --hold_;
    const Vec3d to = point_ - eye;
    const double across = std::hypot(to.x, to.z);
    if (std::abs(to.x) > least_offset || std::abs(to.z) > least_offset) {
        // The engine's yaw of the way to the point: forward is (-sin, -cos).
        const double toward = std::atan2(-to.x, -to.z);
        yaw_ = static_cast<float>(turned_toward(static_cast<double>(yaw_), toward, head_turn));
    }
    if (std::abs(to.y) > least_offset || across > least_offset) {
        pitch_ = static_cast<float>(std::clamp(std::atan2(to.y, across), -most_pitch, most_pitch));
    }
}

} // namespace vx
