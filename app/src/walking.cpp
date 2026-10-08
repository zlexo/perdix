#include "walking.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace vx::app {
namespace {

// The most the clock catches up: a second of ticks.
constexpr std::chrono::nanoseconds most_behind = std::chrono::seconds(1);
// RFC 0010: the field of view widens by (1.3 + 1) / 2 sprinting.
constexpr double sprinting_fov = 1.15;
// How far below the lowest block a fall returns the player to its spawn.
constexpr double void_depth = 64.0;

Vec3d between_points(Vec3d from, Vec3d to, double t) noexcept {
    return from + ((to - from) * t);
}

} // namespace

Walking::Walking(Vec3d spawn, double lowest) noexcept
    : previous_(spawn), spawn_(spawn), respawn_below_(lowest - void_depth) {
    player_.position = spawn;
    scratch_.reserve(256);
}

void Walking::advance(std::chrono::nanoseconds elapsed, const TickInput& input,
                      const BlockBoxes& world) {
    // Clamped first, so no elapsed time, however long, overflows the sum.
    since_tick_ = std::min(
        since_tick_ + std::clamp(elapsed, std::chrono::nanoseconds(0), most_behind), most_behind);
    while (since_tick_ >= tick_length) {
        since_tick_ -= tick_length;
        tick(input, world);
    }
}

void Walking::tick(const TickInput& input, const BlockBoxes& world) {
    previous_ = player_.position;
    vx::tick(player_, input, world, numbers_, scratch_);
    if (player_.position.y < respawn_below_) {
        place(spawn_);
    }
    previous_eye_ = eye_;
    eye_ += (eye_height(player_.pose) - eye_) * 0.5;
    previous_fov_ = fov_;
    const double target = player_.sprinting ? sprinting_fov : 1.0;
    fov_ += (target - fov_) * 0.5;
    ++ticks_;
}

double Walking::between() const noexcept {
    return std::chrono::duration<double>(since_tick_) / std::chrono::duration<double>(tick_length);
}

Vec3d Walking::eye() const noexcept {
    const double t = between();
    return between_points(previous_, player_.position, t) +
           Vec3d{.x = 0.0, .y = previous_eye_ + ((eye_ - previous_eye_) * t), .z = 0.0};
}

double Walking::fov_factor() const noexcept {
    return previous_fov_ + ((fov_ - previous_fov_) * between());
}

double Walking::speed() const noexcept {
    const Vec3d moved = player_.position - previous_;
    constexpr double ticks_a_second = 20.0;
    return std::hypot(moved.x, moved.z) * ticks_a_second;
}

double Walking::vertical_speed() const noexcept {
    constexpr double ticks_a_second = 20.0;
    return (player_.position.y - previous_.y) * ticks_a_second;
}

void Walking::place(Vec3d feet) noexcept {
    // A new player there: no speed, no sprint, no wall hit or double tap
    // carried over from before.
    player_ = Player{};
    player_.position = feet;
    previous_ = feet;
    eye_ = eye_height(Pose::standing);
    previous_eye_ = eye_;
    fov_ = 1.0;
    previous_fov_ = 1.0;
}

} // namespace vx::app
