#include "walking.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>

namespace vx::app {
namespace {

// The most the clock catches up: a second of ticks.
constexpr std::chrono::nanoseconds most_behind = std::chrono::seconds(1);
// RFC 0010: the field of view widens by (1.3 + 1) / 2 sprinting, by 1.1
// flying, and by both sprint-flying.
constexpr double sprinting_fov = 1.15;
constexpr double flying_fov = 1.1;
// Minecraft 26.2's view bobbing, numbers from its jar: the amount at most
// (walking's speed carried into a tick is 0.118), its easing each tick, the
// walk distance a block of horizontal motion adds (one step a 1/0.6 of a
// block), the sideways shift per unit of amount, the lean and the tip in
// degrees per unit, and how far the tip's phase runs behind, in radians.
constexpr double bob_most = 0.1;
constexpr double bob_easing = 0.4;
constexpr double walk_per_block = 0.6;
constexpr double bob_sway = 0.5;
constexpr double bob_roll_degrees = 3.0;
constexpr double bob_tip_degrees = 5.0;
constexpr double bob_tip_lead = 0.2;
// How far below the lowest block a fall returns the player to its spawn.
constexpr double void_depth = 64.0;

Vec3d between_points(Vec3d from, Vec3d to, double t) noexcept {
    return from + ((to - from) * t);
}

} // namespace

Vec3d WalkingFrame::eye_at(double between) const noexcept {
    return between_points(previous, position, between) +
           Vec3d{.x = 0.0, .y = previous_eye + ((eye - previous_eye) * between), .z = 0.0};
}

double WalkingFrame::fov_factor_at(double between) const noexcept {
    return previous_fov + ((fov - previous_fov) * between);
}

Bob WalkingFrame::bob_at(double between, bool bobbing) const noexcept {
    if (!bobbing) {
        return {};
    }
    // The game reads the walk distance a fraction of a tick past the last
    // one, negated, and blends the amount between the last two ticks.
    const double walked_then = -(walked + ((walked - previous_walked) * between));
    const double amount = previous_bob + ((bob - previous_bob) * between);
    const double phase = std::numbers::pi * walked_then;
    constexpr double radians_per_degree = std::numbers::pi / 180.0;
    return {.shift_x = std::sin(phase) * amount * bob_sway,
            .shift_y = -std::abs(std::cos(phase) * amount),
            .turn_z = static_cast<float>(std::sin(phase) * amount * bob_roll_degrees *
                                         radians_per_degree),
            .turn_x = static_cast<float>(std::abs(std::cos(phase - bob_tip_lead) * amount) *
                                         bob_tip_degrees * radians_per_degree)};
}

double WalkingFrame::speed() const noexcept {
    const Vec3d moved = position - previous;
    constexpr double ticks_a_second = 20.0;
    return std::hypot(moved.x, moved.z) * ticks_a_second;
}

double WalkingFrame::vertical_speed() const noexcept {
    constexpr double ticks_a_second = 20.0;
    return (position.y - previous.y) * ticks_a_second;
}

Walking::Walking(Vec3d spawn, double lowest) noexcept
    : spawn_(spawn), respawn_below_(lowest - void_depth) {
    player_.position = spawn;
    frame_.previous = spawn;
    frame_.position = spawn;
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
    // The bob's amount, as the game takes it at the start of a tick: toward
    // the horizontal speed the player carries into it, at most 0.1, on the
    // ground; toward 0 off it.
    frame_.previous_bob = frame_.bob;
    const double carried = std::hypot(player_.velocity.x, player_.velocity.z);
    const double bob_target = player_.on_ground ? std::min(carried, bob_most) : 0.0;
    frame_.bob += (bob_target - frame_.bob) * bob_easing;
    frame_.previous = player_.position;
    vx::tick(player_, input, world, numbers_, scratch_);
    if (player_.position.y < respawn_below_) {
        place(spawn_);
    }
    frame_.position = player_.position;
    frame_.previous_eye = frame_.eye;
    frame_.eye += (eye_height(player_.pose) - frame_.eye) * 0.5;
    // Every move, on the ground or not, walks the bob's steps on.
    const Vec3d moved = player_.position - frame_.previous;
    frame_.previous_walked = frame_.walked;
    frame_.walked += walk_per_block * std::hypot(moved.x, moved.z);
    frame_.previous_fov = frame_.fov;
    const double target =
        (player_.flying ? flying_fov : 1.0) * (player_.sprinting ? sprinting_fov : 1.0);
    frame_.fov += (target - frame_.fov) * 0.5;
    ++frame_.ticks;
    report();
}

double Walking::between() const noexcept {
    return std::chrono::duration<double>(since_tick_) / std::chrono::duration<double>(tick_length);
}

void Walking::report() noexcept {
    frame_.on_ground = player_.on_ground;
    frame_.sprinting = player_.sprinting;
    frame_.crouching = player_.pose == Pose::crouching;
    frame_.flying = player_.flying;
}

void Walking::place(Vec3d feet) noexcept {
    // A new player there: no speed, no sprint, no wall hit or double tap
    // carried over from before.
    player_ = Player{};
    player_.position = feet;
    frame_.previous = feet;
    frame_.position = feet;
    frame_.eye = eye_height(Pose::standing);
    frame_.previous_eye = frame_.eye;
    frame_.fov = 1.0;
    frame_.previous_fov = 1.0;
    frame_.bob = 0.0;
    frame_.previous_bob = 0.0;
    frame_.previous_walked = frame_.walked;
    report();
}

} // namespace vx::app
