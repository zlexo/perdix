#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace vx::app {

// The player walking at RFC 0010's 20 ticks a second, on the main thread
// until M3's world thread takes the ticks over: a clock that runs every tick
// that falls due, with the keys held then, and the last two ticks' positions
// for the camera to draw between, so walking is smooth at any frame rate.
class Walking {
public:
    static constexpr std::chrono::nanoseconds tick_length = std::chrono::milliseconds(50);

    // A player standing at `spawn`, returned there whenever it falls 64
    // blocks below `lowest`, the world's lowest block.
    Walking(Vec3d spawn, double lowest) noexcept;

    // Runs every tick that falls due in `elapsed`, each with `input`. More
    // than a second behind, the rest is skipped, as RFC 0005's world thread
    // skips ticks. The tick's own rules (RFC 0010) start and end sprints and
    // crouches.
    void advance(std::chrono::nanoseconds elapsed, const TickInput& input, const BlockBoxes& world);

    // The camera's position for this frame: between the last tick's position
    // and this one's, as far as the time since this tick goes, at eye height,
    // which eases halfway to the pose's each tick, 1.62 standing and 1.27
    // crouching.
    [[nodiscard]] Vec3d eye() const noexcept;
    // The field of view's factor for this frame, eased as the game eases it:
    // halfway to its target each tick, 1 walking and 1.15 sprinting.
    [[nodiscard]] double fov_factor() const noexcept;
    // The player's horizontal speed over the last tick, in blocks a second.
    [[nodiscard]] double speed() const noexcept;
    // How far the player rose over the last tick, in blocks a second: below
    // 0 falling, and 0 standing on the ground.
    [[nodiscard]] double vertical_speed() const noexcept;

    [[nodiscard]] const Player& player() const noexcept { return player_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

    // Puts the player's feet at `feet`, still, standing and not sprinting,
    // with the eye at 1.62 and the field of view back at 1, as when the
    // noclip camera lands it.
    void place(Vec3d feet) noexcept;

private:
    // How far between the last two ticks this frame falls: 0 to 1.
    [[nodiscard]] double between() const noexcept;
    void tick(const TickInput& input, const BlockBoxes& world);

    Player player_;
    Vec3d previous_; // the position before the last tick
    Vec3d spawn_;
    double respawn_below_;
    std::chrono::nanoseconds since_tick_{0};
    double eye_ = eye_height(Pose::standing);
    double previous_eye_ = eye_height(Pose::standing);
    double fov_ = 1.0;
    double previous_fov_ = 1.0;
    MovementNumbers numbers_;
    std::vector<Box> scratch_;
    std::uint64_t ticks_ = 0;
};

} // namespace vx::app
