#include "demo_chunk.hpp"
#include "walking.hpp"

#include <vx/core/result.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/block_pack.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <vector>

namespace {

using namespace std::chrono_literals;

// A chunk whose bottom layer is a floor: its top is y = 1.
vx::ChunkStorage floor_chunk() {
    vx::ChunkStorage chunk;
    for (int x = 0; x < vx::chunk_size; ++x) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            chunk.set(vx::local_index(x, 0, z), static_cast<vx::BlockState>(1));
        }
    }
    return chunk;
}

constexpr vx::Vec3d on_the_floor{.x = 16.5, .y = 1.0, .z = 16.5};
constexpr vx::TickInput no_keys{};
constexpr vx::TickInput walk{.held = {.forward = true}, .yaw = 0.0F};
constexpr vx::TickInput sprint{.held = {.forward = true, .sprint = true}, .yaw = 0.0F};

} // namespace

TEST_CASE("the demo chunk's player lands on the grass and stays there") {
    const vx::render::BlockPack pack = vx::render::load_block_pack(VX_SHIPPED_PACK_DIR);
    REQUIRE(pack.registry);
    const vx::Result<vx::ChunkStorage> chunk = vx::app::demo_chunk(*pack.registry);
    REQUIRE(chunk.has_value());
    const vx::ChunkBoxes blocks(chunk.value(), {});
    // Two blocks over the app's spawn, on the grass.
    vx::app::Walking walking(vx::app::demo_spawn + vx::Vec3d{.x = 0.0, .y = 2.0, .z = 0.0}, 0.0);
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.player().on_ground);
    CHECK(walking.player().position.y == doctest::Approx(vx::app::demo_spawn.y));
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.player().position.y == doctest::Approx(vx::app::demo_spawn.y));
}

TEST_CASE("the clock runs 20 ticks a second at any frame rate, and catches up a second at most") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    for (const std::int64_t frames : {30, 60, 144}) {
        INFO(frames, " frames a second");
        vx::app::Walking walking(on_the_floor, 0.0);
        for (std::int64_t i = 0; i < frames; ++i) {
            walking.advance(std::chrono::nanoseconds(1'000'000'000 / frames), no_keys, blocks);
        }
        CHECK(walking.ticks() >= 19);
        CHECK(walking.ticks() <= 20);
    }
    vx::app::Walking stalled(on_the_floor, 0.0);
    stalled.advance(5s, no_keys, blocks);
    CHECK(stalled.ticks() == 20);
}

TEST_CASE("the camera follows the player smoothly between ticks, at eye height") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(500ms, walk, blocks);
    // Just after a tick, the camera is where the tick began.
    const vx::Vec3d at_the_tick = walking.eye();
    const vx::Vec3d after_it = walking.player().position;
    CHECK(at_the_tick.y == doctest::Approx(1.0 + 1.62));
    // Halfway to the next tick, it is halfway along the tick's move.
    walking.advance(25ms, walk, blocks);
    const vx::Vec3d halfway = walking.eye();
    CHECK(halfway.z == doctest::Approx(at_the_tick.z + ((after_it.z - at_the_tick.z) * 0.5)));
    CHECK(halfway.z < at_the_tick.z);
}

TEST_CASE("Ctrl with W starts a sprint, and letting go of W ends it") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(100ms, {.held = {.sprint = true}, .yaw = 0.0F}, blocks);
    CHECK_FALSE(walking.player().sprinting);
    walking.advance(100ms, sprint, blocks);
    CHECK(walking.player().sprinting);
    // Ctrl let go with W still held: the sprint goes on, as in the game.
    walking.advance(100ms, walk, blocks);
    CHECK(walking.player().sprinting);
    // W let go, even with Ctrl held again: the sprint ends.
    walking.advance(100ms, {.held = {.sprint = true}, .yaw = 0.0F}, blocks);
    CHECK_FALSE(walking.player().sprinting);
}

TEST_CASE("a sprint speeds the player up from the tick it starts") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    // RFC 0010: from rest on the ground, the first tick moves 0.098 blocks
    // walking and 0.1274 sprinting; sprinting tops out at 5.612 a second.
    walking.advance(50ms, sprint, blocks);
    CHECK(walking.speed() == doctest::Approx(0.1274 * 20.0));
    walking.advance(1s, sprint, blocks);
    CHECK(walking.speed() == doctest::Approx(5.6123).epsilon(1e-4));
}

TEST_CASE("the field of view eases toward 1.15 while sprinting, and back") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    // One tick halfway to 1.15: at the tick, the camera still has the last
    // tick's factor, and halfway through the next, half of the new one's.
    walking.advance(50ms, sprint, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.0));
    walking.advance(25ms, sprint, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.0375));
    walking.advance(1s, sprint, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.15));
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.0));
}

TEST_CASE("the speed shown is in blocks a second: walking tops out at 4.317") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    walking.advance(1s, walk, blocks);
    CHECK(walking.speed() == doctest::Approx(4.3172).epsilon(1e-4));
}

TEST_CASE("the speed shown is horizontal: a jump in place reads 0") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    walking.advance(50ms, {.held = {.jump = true}, .yaw = 0.0F}, blocks);
    REQUIRE(walking.player().position.y > on_the_floor.y);
    CHECK(walking.speed() == 0.0);
}

TEST_CASE("between ticks the camera goes from the last tick's position to this one's") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(450ms, walk, blocks);
    const vx::Vec3d before = walking.player().position;
    walking.advance(50ms, walk, blocks);
    const vx::Vec3d after = walking.player().position;
    REQUIRE(after.z < before.z);
    // At the tick, the eye is RFC 0010's 1.62 over where the tick began.
    CHECK(walking.eye().x == doctest::Approx(before.x));
    CHECK(walking.eye().y == doctest::Approx(before.y + 1.62));
    CHECK(walking.eye().z == doctest::Approx(before.z));
    // A fifth of the way to the next tick, a fifth of the way along the move.
    walking.advance(10ms, walk, blocks);
    CHECK(walking.eye().z == doctest::Approx(before.z + ((after.z - before.z) * 0.2)));
}

TEST_CASE("a frame that took no time, or less, runs no tick and takes none away") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(0ns, no_keys, blocks);
    walking.advance(-1s, no_keys, blocks);
    CHECK(walking.ticks() == 0);
    walking.advance(50ms, no_keys, blocks);
    CHECK(walking.ticks() == 1);
}

TEST_CASE("placing the player stops it where it is put, and the camera with it") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, sprint, blocks);
    REQUIRE(walking.player().on_ground);
    REQUIRE(walking.player().sprinting);
    // As the noclip camera lands it: in the air, over the floor.
    constexpr vx::Vec3d feet{.x = 10.5, .y = 5.0, .z = 10.5};
    walking.place(feet);
    CHECK(walking.player().position == feet);
    CHECK(walking.player().velocity == vx::Vec3d{});
    CHECK_FALSE(walking.player().on_ground);
    CHECK_FALSE(walking.player().sprinting);
    CHECK(walking.speed() == 0.0);
    // Halfway to the next tick, the camera is at the eye over the feet, not
    // on its way from where the player was.
    walking.advance(25ms, no_keys, blocks);
    CHECK(walking.eye().x == doctest::Approx(10.5));
    CHECK(walking.eye().y == doctest::Approx(5.0 + 1.62));
    CHECK(walking.eye().z == doctest::Approx(10.5));
}

TEST_CASE("a tick held leaves the player where it is, and the camera still there") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    // Sprinting off: moving, with the field of view and the bob on their way.
    for (int tick = 0; tick < 5; ++tick) {
        walking.tick(sprint, blocks);
    }
    const vx::app::WalkingFrame moving = walking.frame();
    REQUIRE(moving.previous != moving.position);
    REQUIRE(moving.previous_fov != moving.fov);
    walking.hold();
    const vx::app::WalkingFrame& held = walking.frame();
    CHECK(held.position == moving.position);
    CHECK(held.ticks == moving.ticks);
    // Drawn anywhere between the ticks, the camera is where the last one
    // left it.
    CHECK(held.previous == held.position);
    CHECK(held.eye_at(0.0) == held.eye_at(1.0));
    CHECK(held.fov_factor_at(0.0) == held.fov_factor_at(1.0));
    const vx::app::Bob first = held.bob_at(0.0, true);
    const vx::app::Bob last = held.bob_at(1.0, true);
    CHECK(first.shift_x == last.shift_x);
    CHECK(first.shift_y == last.shift_y);
    CHECK(first.turn_z == last.turn_z);
    CHECK(first.turn_x == last.turn_x);
    CHECK(held.speed() == 0.0);
}

TEST_CASE("placing the player drops a sprint's wider field of view") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, sprint, blocks);
    REQUIRE(walking.fov_factor() == doctest::Approx(1.15));
    // The noclip camera drew at 1; landing the player keeps it there.
    walking.place(walking.player().position);
    walking.advance(16ms, no_keys, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.0));
}

TEST_CASE("falling 64 blocks below the world's lowest block returns the player to its spawn") {
    const vx::ChunkStorage empty;
    const vx::ChunkBoxes blocks(empty, {});
    vx::app::Walking walking({.x = 1.5, .y = 10.0, .z = 1.5}, 0.0);
    for (int second = 0; second < 3; ++second) {
        walking.advance(1s, no_keys, blocks);
    }
    // Without a return it would be some 90 blocks down by now; it passed
    // 64 below on the 52nd tick and has been falling from the spawn since.
    CHECK(walking.ticks() == 60);
    CHECK(walking.player().position.y > 0.0);
    CHECK(walking.player().position.y < 10.0);
}

TEST_CASE("the player returns to its spawn on the tick it falls 64 below the lowest block") {
    const vx::ChunkStorage empty;
    const vx::ChunkBoxes blocks(empty, {});
    // The lowest block at 65 puts the return at 1. From 1.1 the player falls
    // 0.0784 on its second tick, to 1.0216, and 0.155232 on its third, past 1.
    constexpr vx::Vec3d spawn{.x = 0.5, .y = 1.1, .z = 0.5};
    vx::app::Walking walking(spawn, 65.0);
    walking.advance(100ms, no_keys, blocks);
    CHECK(walking.player().position.y == doctest::Approx(1.0216));
    walking.advance(50ms, no_keys, blocks);
    CHECK(walking.player().position == spawn);
    CHECK(walking.player().velocity == vx::Vec3d{});
    // Between ticks the camera stays over the spawn, rather than sweeping up
    // from where the player fell.
    walking.advance(25ms, no_keys, blocks);
    CHECK(walking.eye().y == doctest::Approx(1.1 + 1.62));
}

TEST_CASE("the vertical speed shown is the last tick's rise: 0 standing, 8.4 on a jump's tick") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.vertical_speed() == 0.0);
    // The jump's first tick rises 0.42 blocks.
    walking.advance(50ms, {.held = {.jump = true}, .yaw = 0.0F}, blocks);
    CHECK(walking.vertical_speed() == doctest::Approx(0.42 * 20.0));
}

TEST_CASE("crouching, the eye eases down halfway each tick to 1.27, and back up to 1.62") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    const vx::TickInput shift{.held = {.sneak = true}, .yaw = 0.0F};
    // The tick with Shift crouches the player for the next, and the eye's
    // target drops: halfway, 1.445. Halfway to the next tick, the camera is
    // halfway there from 1.62.
    walking.advance(50ms, shift, blocks);
    CHECK(walking.player().pose == vx::Pose::crouching);
    // Just after that tick, the camera is still where the tick began.
    CHECK(walking.eye().y == doctest::Approx(1.0 + 1.62));
    walking.advance(25ms, shift, blocks);
    CHECK(walking.eye().y == doctest::Approx(1.0 + 1.5325));
    walking.advance(1s, shift, blocks);
    CHECK(walking.eye().y == doctest::Approx(1.0 + 1.27));
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.player().pose == vx::Pose::standing);
    CHECK(walking.eye().y == doctest::Approx(1.0 + 1.62));
}

TEST_CASE("placing the player stands it up, with the eye at 1.62 at once") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, {.held = {.sneak = true}, .yaw = 0.0F}, blocks);
    REQUIRE(walking.player().pose == vx::Pose::crouching);
    walking.place(on_the_floor);
    CHECK(walking.player().pose == vx::Pose::standing);
    CHECK(walking.eye().y == doctest::Approx(1.0 + 1.62));
}

TEST_CASE("Space pressed twice flies, the field of view easing to 1.1, and 1.265 sprint-flying") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    const vx::TickInput space{.held = {.jump = true}, .yaw = 0.0F};
    walking.advance(50ms, space, blocks);
    walking.advance(50ms, no_keys, blocks);
    walking.advance(50ms, space, blocks);
    REQUIRE(walking.player().flying);
    walking.advance(1s, no_keys, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.1));
    walking.advance(1s, sprint, blocks);
    CHECK(walking.fov_factor() == doctest::Approx(1.265));
    // Placed, as the noclip camera lands it, the player walks again.
    walking.place(walking.player().position);
    CHECK_FALSE(walking.player().flying);
}

TEST_CASE("placing the player closes Space's window: one press after it does not fly") {
    // Space pressed, then the noclip camera there and back before the window
    // closes: no tick runs in the noclip camera, so a window left open would
    // still be open, and the next single press would fly.
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    const vx::TickInput space{.held = {.jump = true}, .yaw = 0.0F};
    walking.advance(50ms, space, blocks);
    walking.advance(50ms, no_keys, blocks);
    walking.place(on_the_floor);
    walking.advance(50ms, space, blocks);
    CHECK_FALSE(walking.player().flying);
}

TEST_CASE("standing still the view does not bob, and walking it sways, lifts, leans and tips "
          "as the game's does") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(1s, no_keys, blocks);
    const vx::app::Bob still = walking.bob();
    CHECK(still.shift_x == 0.0);
    CHECK(still.shift_y == 0.0);
    CHECK(still.turn_z == 0.0F);
    CHECK(still.turn_x == 0.0F);
    // Walking carries 0.118 a tick into each tick, so the amount reaches its
    // most, 0.1: a shift of 0.05 sideways and 0.1 up and down, a lean of 0.3
    // degrees and a tip of 0.5, as Minecraft 26.2's.
    walking.advance(2s, walk, blocks);
    constexpr double degree = 3.141592653589793 / 180.0;
    std::array<double, 4> most{};
    for (int frame = 0; frame < 100; ++frame) {
        walking.advance(10ms, walk, blocks);
        const vx::app::Bob bob = walking.bob();
        most[0] = std::max(most[0], std::abs(bob.shift_x));
        most[1] = std::max(most[1], std::abs(bob.shift_y));
        most[2] = std::max(most[2], std::abs(static_cast<double>(bob.turn_z)));
        most[3] = std::max(most[3], std::abs(static_cast<double>(bob.turn_x)));
    }
    const std::array<double, 4> game = {0.05, 0.1, 0.3 * degree, 0.5 * degree};
    for (std::size_t i = 0; i < game.size(); ++i) {
        INFO("part ", i);
        CHECK(most[i] <= game[i] * 1.0001);
        CHECK(most[i] > game[i] * 0.98);
    }
}

TEST_CASE("the bob takes one step for each 1/0.6 of a block walked") {
    // The lift peaks where the walk distance, 0.6 of the blocks moved, is a
    // whole number: every 1.667 blocks. Frames of 10 ms catch each peak to
    // within 0.022 blocks.
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking({.x = 16.5, .y = 1.0, .z = 30.5}, 0.0);
    walking.advance(2s, walk, blocks);
    std::vector<double> peaks;
    double best = 0.0;
    double best_at = 0.0;
    for (int frame = 0; frame < 300; ++frame) {
        walking.advance(10ms, walk, blocks);
        const double lift = std::abs(walking.bob().shift_y);
        const double along = -walking.eye().z;
        if (lift > 0.0995) {
            if (lift > best) {
                best = lift;
                best_at = along;
            }
        } else if (best > 0.0) {
            peaks.push_back(best_at);
            best = 0.0;
        }
    }
    REQUIRE(peaks.size() >= 3);
    for (std::size_t i = 1; i < peaks.size(); ++i) {
        INFO("step ", i);
        CHECK(peaks[i] - peaks[i - 1] == doctest::Approx(1.0 / 0.6).epsilon(0.03));
    }
}

TEST_CASE("the lean follows the sway, and the tip runs 0.2 of a step's phase behind the lift") {
    // 26.2's (from its jar): the sway and the lean go with the phase's sine,
    // the lift with its cosine, and the tip with the cosine of the phase 0.2
    // earlier. The sway and the lift give the phase's sine and the size of
    // its cosine, so the phase is one of two.
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking({.x = 16.5, .y = 1.0, .z = 30.5}, 0.0);
    walking.advance(2s, walk, blocks);
    constexpr double degree = std::numbers::pi / 180.0;
    for (int frame = 0; frame < 100; ++frame) {
        INFO("frame ", frame);
        walking.advance(10ms, walk, blocks);
        const vx::app::Bob bob = walking.bob();
        const double amount = std::hypot(2.0 * bob.shift_x, bob.shift_y);
        REQUIRE(amount > 0.09);
        const double sine = 2.0 * bob.shift_x / amount;
        const double cosine = std::abs(bob.shift_y) / amount;
        CHECK(static_cast<double>(bob.turn_z) / (amount * 3.0 * degree) ==
              doctest::Approx(sine).epsilon(1e-5));
        const double tip = static_cast<double>(bob.turn_x) / (amount * 5.0 * degree);
        double nearest = 1.0;
        for (const double phase : {std::atan2(sine, cosine), std::atan2(sine, -cosine)}) {
            nearest = std::min(nearest, std::abs(tip - std::abs(std::cos(phase - 0.2))));
        }
        CHECK(nearest < 1e-4);
    }
}

TEST_CASE("in the air, or switched off, the bob dies away") {
    const vx::ChunkStorage chunk = floor_chunk();
    const vx::ChunkBoxes blocks(chunk, {});
    vx::app::Walking walking(on_the_floor, 0.0);
    walking.advance(2s, walk, blocks);
    // A jump: the tick that jumps began on the ground and keeps the amount
    // at 0.1; each tick after it keeps 0.6 of it, easing 0.4 of the way to 0.
    // Drawn right at the 11th tick, the camera shows the 10th's amount: 0.1
    // times 0.6 to the 9th, 0.001, a hundredth of walking's. Whatever the
    // step's phase, the amount is the lift and twice the sway, squared and
    // summed, square-rooted.
    walking.advance(550ms, {.held = {.forward = true, .jump = true}, .yaw = 0.0F}, blocks);
    REQUIRE_FALSE(walking.player().on_ground);
    const vx::app::Bob up = walking.bob();
    CHECK(std::hypot(2.0 * up.shift_x, up.shift_y) ==
          doctest::Approx(0.1 * std::pow(0.6, 9)).epsilon(1e-6));
    // Switched off, nothing at all, walking or not.
    vx::app::Walking off(on_the_floor, 0.0);
    off.bobbing = false;
    off.advance(2s, walk, blocks);
    off.advance(25ms, walk, blocks);
    const vx::app::Bob none = off.bob();
    CHECK(none.shift_x == 0.0);
    CHECK(none.shift_y == 0.0);
    CHECK(none.turn_z == 0.0F);
    CHECK(none.turn_x == 0.0F);
}
