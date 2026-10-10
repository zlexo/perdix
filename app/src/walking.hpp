#pragma once

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

namespace vx::app {

// This frame's view bobbing (RFC 0010's item 8) as Minecraft 26.2 applies
// it to the view, numbers from its jar: the scene shifted in view space, x
// right and y up, in blocks, then turned around the view's z axis and then
// its x axis, in radians. The camera undoes it.
struct Bob {
    double shift_x = 0.0;
    double shift_y = 0.0;
    float turn_z = 0.0F;
    float turn_x = 0.0F;
};

// What the camera needs of the player's last two ticks (RFC 0010), to draw
// between them so walking is smooth at any frame rate: made after each tick
// on the world thread, which runs the ticks (RFC 0005), and read on the
// main thread.
struct WalkingFrame {
    Vec3d previous; // the position before the last tick
    Vec3d position;
    double previous_eye = eye_height(Pose::standing);
    double eye = eye_height(Pose::standing);
    double previous_fov = 1.0;
    double fov = 1.0;
    double previous_bob = 0.0; // the bob's amount
    double bob = 0.0;
    double previous_walked = 0.0; // the bob's walk distance
    double walked = 0.0;
    // The body's animation after the tick before and the last one (RFC
    // 0012): its walk swing, its turn and where its head looks.
    BodyMotion previous_motion{};
    BodyMotion motion{};
    bool on_ground = false;
    bool sprinting = false;
    bool crouching = false;
    bool flying = false;
    std::uint64_t ticks = 0; // run so far

    // The camera's position `between` (0 to 1) of the way from the last
    // tick to the next: between the last tick's position and this one's,
    // at eye height, which eases halfway to the pose's each tick, 1.62
    // standing and 1.27 crouching.
    [[nodiscard]] Vec3d eye_at(double between) const noexcept;
    // The field of view's factor then, eased as the game eases it: halfway
    // to its target each tick: 1 walking, 1.15 sprinting, 1.1 flying and
    // 1.265 sprint-flying.
    [[nodiscard]] double fov_factor_at(double between) const noexcept;
    // The view bobbing then: on the ground the camera sways, lifts, leans
    // and tips with each step, as the game's does; none when `bobbing` is
    // off.
    [[nodiscard]] Bob bob_at(double between, bool bobbing) const noexcept;
    // The horizontal speed over the last tick, in blocks a second.
    [[nodiscard]] double speed() const noexcept;
    // How far the player rose over the last tick, in blocks a second: below
    // 0 falling, and 0 standing on the ground.
    [[nodiscard]] double vertical_speed() const noexcept;
};

// The player walking at RFC 0010's 20 ticks a second: on the world thread,
// whose clock runs each tick with the keys held then (tick()), or on a
// clock of its own that runs every tick falling due (advance()), as the
// tests drive it. Each tick leaves a WalkingFrame for the camera.
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
    // One tick, with the keys held now: the world thread's, on its clock.
    // The head looks along the keys' yaw and pitch.
    void tick(const TickInput& input, const BlockBoxes& world);
    // The same, the head looking along `look_yaw` and `look_pitch` instead,
    // as Perdix's does while he walks one way and looks another (RFC 0012).
    void tick(const TickInput& input, const BlockBoxes& world, float look_yaw, float look_pitch);
    // A tick the player waits through instead, keys and all, while its move
    // could touch a chunk not loaded yet (RFC 0010): it stays where it is,
    // keeping its speed, and the frame shows it still.
    void hold() noexcept;

    // The last two ticks, for the camera.
    [[nodiscard]] const WalkingFrame& frame() const noexcept { return frame_; }
    // This frame's, as far between the ticks as advance()'s clock is.
    [[nodiscard]] Vec3d eye() const noexcept { return frame_.eye_at(between()); }
    [[nodiscard]] double fov_factor() const noexcept { return frame_.fov_factor_at(between()); }
    [[nodiscard]] Bob bob() const noexcept { return frame_.bob_at(between(), bobbing); }
    [[nodiscard]] double speed() const noexcept { return frame_.speed(); }
    [[nodiscard]] double vertical_speed() const noexcept { return frame_.vertical_speed(); }

    // The game's View Bobbing option, on by default; the overlay switches it.
    bool bobbing = true;

    // Whether Space pressed twice toggles creative flight (RFC 0010), as it
    // does for you. Perdix's auto-jump presses Space, and the game never
    // counts those toward a double tap, so he may not fly (RFC 0012).
    void set_may_fly(bool may_fly) noexcept;
    // Turns the body and its look to `yaw` and `pitch` at once, the swing at
    // rest: a body as it appears.
    void face(float yaw, float pitch) noexcept;

    [[nodiscard]] const Player& player() const noexcept { return player_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return frame_.ticks; }

    // Puts the player's feet at `feet`, still, standing, not sprinting and not
    // flying, with no Space press waiting for a second, the eye at 1.62 and
    // the field of view back at 1, as when the noclip camera lands it.
    void place(Vec3d feet) noexcept;

private:
    // How far between the last two ticks this frame falls: 0 to 1.
    [[nodiscard]] double between() const noexcept;
    // The frame's flags from the player's state.
    void report() noexcept;

    Player player_;
    bool may_fly_ = true;
    WalkingFrame frame_;
    Vec3d spawn_;
    double respawn_below_;
    std::chrono::nanoseconds since_tick_{0};
    MovementNumbers numbers_;
    std::vector<Box> scratch_;
};

} // namespace vx::app
