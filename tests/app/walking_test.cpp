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

#include <chrono>
#include <cstdint>
#include <initializer_list>

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
