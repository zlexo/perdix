#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <vector>

// RFC 0010's crouching: the pose set for the next tick, forced under a low
// ceiling, and the edge guard while Shift is held. The player faces -z.

namespace {

using vx::Box;
using vx::test::at_rest;
using vx::test::block_box;

constexpr double tolerance = 1e-5;

// Runs `ticks` ticks of `keys` and returns how far each moved along -z.
std::vector<double> run_along_z(vx::Player& player, const vx::MovementKeys& keys, std::size_t ticks,
                                const vx::BlockBoxes& world) {
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    std::vector<double> moved;
    for (std::size_t i = 0; i < ticks; ++i) {
        const double before = player.position.z;
        vx::tick(player, {.held = keys, .yaw = 0.0F}, world, numbers, scratch);
        moved.push_back(before - player.position.z);
    }
    return moved;
}

constexpr vx::MovementKeys w{.forward = true};
constexpr vx::MovementKeys shift_w{.forward = true, .sneak = true};

// A ceiling 1.6 above the floor, from z = -10 to z = -2: room for the
// crouching box (1.5), not the standing one (1.8).
vx::test::FlatWorld low_gap() {
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -10; z < -2; ++z) {
            world.boxes.push_back(block_box(x, 1.6, z));
        }
    }
    return world;
}

} // namespace

TEST_CASE(
    "Shift crouches the player from the next tick: the tick it is pressed moves as standing") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, w, 100, world));
    const std::vector<double> moved = run_along_z(player, shift_w, 2, world);
    // The first tick with Shift still accelerates as standing, at top speed.
    CHECK(moved[0] == doctest::Approx(0.215859).epsilon(tolerance));
    CHECK(player.pose == vx::Pose::crouching);
    // The next is crouched: the carried speed's 0.546, plus 0.0294.
    CHECK(moved[1] == doctest::Approx(0.147259).epsilon(tolerance));
}

TEST_CASE("let go of Shift, the player stands from the next tick, where it fits") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, shift_w, 3, world));
    CHECK(player.pose == vx::Pose::crouching);
    static_cast<void>(run_along_z(player, w, 1, world));
    CHECK(player.pose == vx::Pose::standing);
}

TEST_CASE("standing, the player cannot walk under a 1.6-high ceiling, and crouched it can") {
    const vx::test::FlatWorld world = low_gap();
    vx::Player standing = at_rest();
    static_cast<void>(run_along_z(standing, w, 30, world));
    CHECK(standing.position.z == doctest::Approx(-1.7));
    CHECK(standing.horizontal_collision);

    vx::Player crouching = at_rest();
    static_cast<void>(run_along_z(crouching, shift_w, 60, world));
    CHECK(crouching.position.z < -3.0);
    CHECK_FALSE(crouching.horizontal_collision);
}

TEST_CASE("under a ceiling too low to stand, the player stays crouched without Shift until out") {
    // RFC 0010: the player crouches by itself where the standing box does not
    // fit and the crouching one does.
    const vx::test::FlatWorld world = low_gap();
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, shift_w, 70, world));
    REQUIRE(player.position.z < -4.0);
    REQUIRE(player.position.z > -9.0);
    // Shift let go under the ceiling: still crouched, still at the crouching
    // speed.
    const std::vector<double> under = run_along_z(player, w, 5, world);
    CHECK(player.pose == vx::Pose::crouching);
    CHECK(under[4] < 0.065);
    // Walked out from under it, it stands the tick after its box clears the
    // ceiling, and speeds up.
    std::vector<double> out;
    while (player.position.z > -10.4) {
        const std::vector<double> moved = run_along_z(player, w, 1, world);
        out.push_back(moved[0]);
        REQUIRE(out.size() < 200);
    }
    static_cast<void>(run_along_z(player, w, 1, world));
    CHECK(player.pose == vx::Pose::standing);
    const std::vector<double> after = run_along_z(player, w, 20, world);
    CHECK(after.back() == doctest::Approx(0.215859).epsilon(tolerance));
}

TEST_CASE("Shift held, W walks the player to a ledge's edge and no further") {
    // A platform 1 block high, its edge at z = -1; the box's front can pass
    // the edge until only its back still has the block under it.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -1; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
    }
    vx::Player player = at_rest();
    player.position.y = 1.0;
    static_cast<void>(run_along_z(player, shift_w, 60, world));
    CHECK(player.position.y == 1.0);
    CHECK(player.on_ground);
    // Its back edge, at z + 0.3, still over the block, within a step of 0.05.
    CHECK(player.position.z + 0.3 > -1.0);
    CHECK(player.position.z + 0.3 <= -0.95 + tolerance);
}

TEST_CASE("Shift keeps a sprint: crouched, the player sprints at 1.684 blocks a second") {
    // 26.2 keeps the sprint (1.21.4 ended it; 1.21.5 reverted that): the
    // crouching speed times the sprint's 1.3, 0.0294 * 1.3 / 0.454 a tick.
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, {.forward = true, .sprint = true}, 1, world));
    REQUIRE(player.sprinting);
    const std::vector<double> moved =
        run_along_z(player, {.forward = true, .sneak = true, .sprint = true}, 100, world);
    CHECK(player.sprinting);
    CHECK(player.pose == vx::Pose::crouching);
    CHECK(moved.back() == doctest::Approx(0.084185).epsilon(tolerance));
}

TEST_CASE(
    "Shift holds the player at an edge from the tick it is pressed, a tick before the crouch") {
    // The guard follows the key, not the pose, which crouches only from the
    // next tick. A platform 1 block high, its edge at z = -1, and the player
    // still on it, carrying more than enough speed to go over.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -1; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
    }
    vx::Player player = at_rest();
    player.position = {.x = 0.0, .y = 1.0, .z = -1.2};
    player.velocity.z = -0.2;
    static_cast<void>(run_along_z(player, shift_w, 5, world));
    CHECK(player.position.y == 1.0);
    CHECK(player.position.z + 0.3 > -1.0);
}

TEST_CASE("a crouch the ceiling forces holds no edge: without Shift the player walks off") {
    // A platform 1 block high, its edge at z = -1, under a ceiling 1.6 above
    // it that runs on past the edge, so the player stays crouched by itself.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -1; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
        for (int z = -4; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 2.6, z));
        }
    }
    vx::Player player = at_rest();
    player.position.y = 1.0;
    player.pose = vx::Pose::crouching;
    static_cast<void>(run_along_z(player, w, 1, world));
    REQUIRE(player.pose == vx::Pose::crouching);
    static_cast<void>(run_along_z(player, w, 40, world));
    CHECK(player.position.y == 0.0);
}

TEST_CASE("a body sees from 1.62 above its feet standing, and from 1.27 crouching") {
    CHECK(vx::eye_height(vx::Pose::standing) == doctest::Approx(1.62));
    CHECK(vx::eye_height(vx::Pose::crouching) == doctest::Approx(1.27));
}

TEST_CASE("let go of Shift on the tick the box goes under a low ceiling: the pose is taken where "
          "the move ended") {
    const vx::test::FlatWorld world = low_gap();
    vx::Player player = at_rest();
    // Crouched up to the ceiling's edge at z = -2, the box's front less
    // than a tick's move from it.
    for (int i = 0; player.position.z - 0.3 > -2.0 + 0.04; ++i) {
        REQUIRE(i < 100);
        static_cast<void>(run_along_z(player, shift_w, 1, world));
    }
    // One tick without Shift carries the front under the ceiling, where
    // only the crouching box fits.
    static_cast<void>(run_along_z(player, w, 1, world));
    REQUIRE(player.position.z - 0.3 < -2.0);
    CHECK(player.pose == vx::Pose::crouching);
}

TEST_CASE("inside a block, where neither box fits, the player stands: a forced crouch needs room") {
    // RFC 0010: crouched where the standing box does not fit and the
    // crouching one does; otherwise standing.
    vx::test::FlatWorld world;
    world.boxes.push_back(
        {.min = {.x = -0.5, .y = 0.5, .z = -0.5}, .max = {.x = 0.5, .y = 1.0, .z = 0.5}});
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, {}, 1, world));
    CHECK(player.pose == vx::Pose::standing);
}

TEST_CASE("a tick at a position that is not finite asks the world for nothing, as a move does") {
    // The pose's check reads the world around the player, and a position
    // that is not finite has none.
    vx::test::BoxWorld world;
    world.boxes.push_back(block_box(0.0, -1.0, 0.0));
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    for (const double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        INFO("x = ", bad);
        vx::Player player = at_rest();
        player.position.x = bad;
        world.regions.clear();
        vx::tick(player, {.held = {.forward = true}, .yaw = 0.0F}, world, numbers, scratch);
        CHECK(world.regions.empty());
        CHECK(player.pose == vx::Pose::standing);
    }
}
