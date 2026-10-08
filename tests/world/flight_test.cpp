#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <vector>

// RFC 0010's creative flight: Space pressed twice toggles it, landing ends
// it, it accelerates 0.05 (0.1 sprinting) against a drag of 0.91, and Space
// and Shift move it up and down by 0.15 a tick before the move, the vertical
// speed keeping 0.6 after it, with no gravity. The player faces -z.

namespace {

using vx::Box;
using vx::test::at_rest;
using vx::test::block_box;

constexpr double tolerance = 1e-5;

// A player flying high over the floor, at rest.
vx::Player flying() {
    vx::Player player;
    player.position.y = 10.0;
    player.flying = true;
    return player;
}

// One tick's result.
struct Step {
    vx::Vec3d moved;
    bool flying = false;
    bool on_ground = false;
};

// Runs a tick for each set of keys, in order.
std::vector<Step> run(vx::Player& player, const std::vector<vx::MovementKeys>& keys,
                      const vx::BlockBoxes& world) {
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    std::vector<Step> steps;
    for (const vx::MovementKeys& held : keys) {
        const vx::Vec3d before = player.position;
        vx::tick(player, {.held = held, .yaw = 0.0F}, world, numbers, scratch);
        steps.push_back({.moved = player.position - before,
                         .flying = player.flying,
                         .on_ground = player.on_ground});
    }
    return steps;
}

std::vector<vx::MovementKeys> repeat(const vx::MovementKeys& keys, std::size_t ticks) {
    return std::vector<vx::MovementKeys>(ticks, keys);
}

} // namespace

TEST_CASE("flying with W from rest: 0.049, 0.09359, 0.134167 a tick, toward 10.89 a second") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    const std::vector<Step> steps = run(player, repeat({.forward = true}, 200), world);
    constexpr std::array<double, 5> game = {0.049, 0.09359, 0.134167, 0.171092, 0.204694};
    for (std::size_t i = 0; i < game.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(-steps[i].moved.z == doctest::Approx(game[i]).epsilon(tolerance));
    }
    CHECK(-steps.back().moved.z == doctest::Approx(0.544444).epsilon(tolerance));
    // No gravity: the height never changed.
    CHECK(player.position.y == 10.0);
    CHECK(player.flying);
}

TEST_CASE("sprint-flying tops out at 21.78 blocks a second, and W with A at 11.11") {
    const vx::test::FlatWorld world;
    vx::Player sprinter = flying();
    const std::vector<Step> sprinted =
        run(sprinter, repeat({.forward = true, .sprint = true}, 200), world);
    CHECK(sprinter.sprinting);
    CHECK(-sprinted.back().moved.z == doctest::Approx(1.088889).epsilon(tolerance));

    vx::Player diagonal = flying();
    const std::vector<Step> steps =
        run(diagonal, repeat({.forward = true, .left = true}, 200), world);
    const vx::Vec3d last = steps.back().moved;
    CHECK(std::hypot(last.x, last.z) == doctest::Approx(0.555556).epsilon(tolerance));
}

TEST_CASE(
    "Space climbs 0.15, 0.24, 0.294 a tick toward 7.5 a second, and let go, the climb stops") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    std::vector<vx::MovementKeys> keys = repeat({.jump = true}, 100);
    const std::vector<vx::MovementKeys> released = repeat({}, 12);
    keys.insert(keys.end(), released.begin(), released.end());
    const std::vector<Step> steps = run(player, keys, world);
    constexpr std::array<double, 5> climb = {0.15, 0.24, 0.294, 0.3264, 0.34584};
    for (std::size_t i = 0; i < climb.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(steps[i].moved.y == doctest::Approx(climb[i]).epsilon(tolerance));
    }
    CHECK(steps[99].moved.y == doctest::Approx(0.375).epsilon(tolerance));
    // Released: 0.6 of the speed each tick, then 0 once under 0.003.
    constexpr std::array<double, 4> slowing = {0.225, 0.135, 0.081, 0.0486};
    for (std::size_t i = 0; i < slowing.size(); ++i) {
        INFO("tick ", i + 1, " after release");
        CHECK(steps[100 + i].moved.y == doctest::Approx(slowing[i]).epsilon(tolerance));
    }
    CHECK(steps[110].moved.y == 0.0);
    CHECK(steps[111].moved.y == 0.0);
}

TEST_CASE("Shift sinks as Space climbs, and a flying player does not crouch") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    const std::vector<Step> steps = run(player, repeat({.sneak = true}, 3), world);
    CHECK(steps[0].moved.y == doctest::Approx(-0.15).epsilon(tolerance));
    CHECK(steps[1].moved.y == doctest::Approx(-0.24).epsilon(tolerance));
    CHECK(player.pose == vx::Pose::standing);
    // Space and Shift together hold the height.
    vx::Player both = flying();
    const std::vector<Step> held = run(both, repeat({.jump = true, .sneak = true}, 5), world);
    CHECK(both.position.y == 10.0);
    CHECK(held[4].moved.y == 0.0);
}

TEST_CASE("Space pressed twice within 6 ticks starts flight, 7 apart does not, and twice more ends "
          "it") {
    const vx::test::FlatWorld world;
    for (const std::size_t apart : std::initializer_list<std::size_t>{2, 6, 7}) {
        INFO(apart, " ticks apart");
        vx::Player player = at_rest();
        std::vector<vx::MovementKeys> keys = repeat({}, apart + 1);
        keys[0].jump = true; // the first press jumps, from the ground
        keys[apart].jump = true;
        const std::vector<Step> steps = run(player, keys, world);
        CHECK(steps[apart].flying == (apart <= 6));
    }
    // Flying, two more presses end it, and the player falls.
    vx::Player player = flying();
    const std::vector<Step> steps =
        run(player, {{.jump = true}, {}, {.jump = true}, {}, {}}, world);
    CHECK_FALSE(steps[2].flying);
    CHECK(steps[4].moved.y < 0.0);
}

TEST_CASE("only a player that may fly takes off") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.may_fly = false;
    const std::vector<Step> steps = run(player, {{.jump = true}, {}, {.jump = true}}, world);
    CHECK_FALSE(steps[2].flying);
}

TEST_CASE("landing ends a flight") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    player.position.y = 0.3;
    const std::vector<Step> steps = run(player, repeat({.sneak = true}, 4), world);
    // Down 0.15, then the next 0.24 cut short by the floor: landed, and no
    // longer flying.
    CHECK(steps[0].flying);
    CHECK(steps[1].on_ground);
    CHECK_FALSE(steps[1].flying);
    CHECK(player.position.y == 0.0);
}

TEST_CASE("flying, Shift does not hold the player back at an edge") {
    // A platform 1 block high, its edge at z = -1, and the player flying half
    // a block over it, Space and Shift together holding its height: within
    // the edge guard's 0.6, yet it flies on past the edge.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -1; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
    }
    vx::Player player = flying();
    player.position.y = 1.5;
    static_cast<void>(
        run(player, repeat({.forward = true, .jump = true, .sneak = true}, 20), world));
    CHECK(player.position.z < -1.3 - 1.0);
    CHECK(player.position.y == 1.5);
    CHECK(player.flying);
}

TEST_CASE("Space pressed a third time opens a new window rather than toggling back") {
    // The press that toggles closes the window: the next one opens another,
    // and only a fourth within it toggles again.
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    const vx::MovementKeys space{.jump = true};
    const std::vector<Step> steps = run(player, {space, {}, space, {}, space, {}, space}, world);
    CHECK_FALSE(steps[2].flying);
    CHECK_FALSE(steps[4].flying);
    CHECK(steps[6].flying);
}

TEST_CASE("a ceiling does not end a flight") {
    // A roof 2 blocks over the floor: Space flies the head into it, and the
    // player keeps flying under it.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -2; z < 2; ++z) {
            world.boxes.push_back(block_box(x, 2.0, z));
        }
    }
    vx::Player player = flying();
    player.position.y = 0.1;
    const std::vector<Step> steps = run(player, repeat({.jump = true}, 5), world);
    CHECK(steps[0].moved.y == doctest::Approx(0.1).epsilon(tolerance));
    CHECK(player.position.y == doctest::Approx(0.2).epsilon(tolerance));
    CHECK(player.flying);
    CHECK_FALSE(player.on_ground);
}

TEST_CASE(
    "taking off from the ground jumps too, as 26.2's toggle does, and accelerates as flight") {
    // Space pressed in the air just before landing opens the window, and
    // pressed again on the ground two ticks later takes off from a tick that
    // began on the ground: 26.2's toggle jumps then, to 0.42, and flight's
    // Space adds its 0.15 before the move, so the player rises 0.57; W
    // pushes at flight's acceleration and drag, so the next tick moves
    // flight's own 0.09359.
    const vx::test::FlatWorld world;
    vx::Player player;
    player.position.y = 0.1;
    player.velocity.y = -0.2;
    const std::vector<Step> steps = run(
        player, {{.jump = true}, {}, {.forward = true, .jump = true}, {.forward = true}}, world);
    REQUIRE(steps[1].on_ground);
    CHECK(steps[2].flying);
    CHECK(steps[2].moved.y == doctest::Approx(0.42 + 0.15).epsilon(tolerance));
    CHECK(-steps[2].moved.z == doctest::Approx(0.049).epsilon(tolerance));
    CHECK(-steps[3].moved.z == doctest::Approx(0.09359).epsilon(tolerance));
}

TEST_CASE("flying, the threshold ends a glide: 5.474 blocks over 55 ticks after W") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    static_cast<void>(run(player, repeat({.forward = true}, 300), world));
    const double start = player.position.z;
    const std::vector<Step> steps = run(player, repeat({}, 70), world);
    CHECK(start - player.position.z == doctest::Approx(5.474175).epsilon(tolerance));
    CHECK(steps[54].moved.z < 0.0);
    for (std::size_t i = 55; i < steps.size(); ++i) {
        INFO("tick ", i + 1, " after release");
        CHECK(steps[i].moved.z == 0.0);
    }
}

TEST_CASE("landing with Shift held crouches from the next tick") {
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    player.position.y = 0.3;
    const std::vector<Step> steps = run(player, repeat({.sneak = true}, 2), world);
    REQUIRE_FALSE(steps[1].flying);
    CHECK(player.pose == vx::Pose::crouching);
}

TEST_CASE("the tick after a landing begins on the ground, so Space then jumps") {
    // Flying keeps 0.6 of the vertical speed from before the move, whatever
    // the move ran into: landing at -0.24 leaves -0.144, which presses the
    // player on the ground the next tick, as standing's -0.0784 does.
    const vx::test::FlatWorld world;
    vx::Player player = flying();
    player.position.y = 0.3;
    const std::vector<Step> landing = run(player, repeat({.sneak = true}, 2), world);
    REQUIRE_FALSE(landing[1].flying);
    CHECK(player.velocity.y == doctest::Approx(-0.144).epsilon(tolerance));
    const std::vector<Step> steps = run(player, {{}, {.jump = true}}, world);
    CHECK(steps[0].on_ground);
    CHECK(steps[1].moved.y == doctest::Approx(0.42).epsilon(tolerance));
}

TEST_CASE("held against a ceiling, the climb keeps its speed for when the ceiling ends") {
    // Under a roof 2 blocks up, the head against it, then out from under it
    // with Space still held: the climb goes on at 0.375 a tick at once, the
    // speed it reached while held back.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        world.boxes.push_back(block_box(x, 2.0, -1.0));
    }
    vx::Player player = flying();
    player.position = {.x = 0.0, .y = 0.1, .z = -0.5};
    static_cast<void>(run(player, repeat({.jump = true}, 30), world));
    CHECK(player.position.y == doctest::Approx(0.2).epsilon(tolerance));
    player.position.z = 1.0; // out from under the roof
    const std::vector<Step> out = run(player, repeat({.jump = true}, 1), world);
    CHECK(out[0].moved.y == doctest::Approx(0.375).epsilon(tolerance));
}
