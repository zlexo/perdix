#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

// RFC 0010's jump vectors, checked to its 1e-5 blocks. As in the tick's
// other tests, the player faces -z (yaw 0), so distances along the vectors'
// +z are taken along -z.

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::at_rest;
using vx::test::block_box;

constexpr double tolerance = 1e-5;

// One tick's result: how far it moved along -z, and the height after it.
struct Step {
    double along = 0.0;
    double height = 0.0;
    bool on_ground = false;
};

// Runs a tick for each set of keys, in order.
std::vector<Step> run(vx::Player& player, const std::vector<vx::MovementKeys>& keys,
                      const vx::BlockBoxes& world) {
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    std::vector<Step> steps;
    for (const vx::MovementKeys& held : keys) {
        const double before = player.position.z;
        vx::tick(player, {.held = held, .yaw = 0.0F}, world, numbers, scratch);
        steps.push_back({.along = before - player.position.z,
                         .height = player.position.y,
                         .on_ground = player.on_ground});
    }
    return steps;
}

std::vector<vx::MovementKeys> repeat(const vx::MovementKeys& keys, std::size_t ticks) {
    return std::vector<vx::MovementKeys>(ticks, keys);
}

// The standing jump's height after each tick: RFC 0010's vector.
constexpr std::array<double, 12> jump_heights = {0.42,     0.7532,   1.001336, 1.166109,
                                                 1.249187, 1.252203, 1.176759, 1.024424,
                                                 0.796736, 0.495201, 0.121297, 0.0};

} // namespace

TEST_CASE("the standing jump peaks at 1.2522 on its 6th tick and lands on its 12th") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    const std::vector<Step> steps = run(player, repeat({.jump = true}, 12), world);
    for (std::size_t i = 0; i < jump_heights.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(steps[i].height == doctest::Approx(jump_heights[i]).epsilon(tolerance));
        CHECK(steps[i].on_ground == (i == 11));
    }
    // Not sprinting, there is no boost: the jump goes straight up.
    CHECK(player.position.x == 0.0);
    CHECK(player.position.z == 0.0);
}

TEST_CASE("a standing sprint-jump: 0.2 more along the facing on the jump's tick") {
    // W and the sprint held throughout, Space on the first tick only.
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.sprinting = true;
    std::vector<vx::MovementKeys> keys = repeat({.forward = true}, 14);
    keys[0].jump = true;
    const std::vector<Step> steps = run(player, keys, world);
    constexpr std::array<double, 14> along = {0.327400, 0.204240, 0.211339, 0.217798, 0.223676,
                                              0.229026, 0.233893, 0.238323, 0.242354, 0.246022,
                                              0.249360, 0.252398, 0.357082, 0.322367};
    for (std::size_t i = 0; i < along.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(steps[i].along == doctest::Approx(along[i]).epsilon(tolerance));
    }
    for (std::size_t i = 0; i < jump_heights.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(steps[i].height == doctest::Approx(jump_heights[i]).epsilon(tolerance));
    }
    CHECK(steps[11].on_ground);
}

TEST_CASE("sprint-jumping with every key held: a 12-tick cycle of 4.276 blocks, 7.127 a second") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.sprinting = true;
    const std::vector<Step> steps =
        run(player, repeat({.forward = true, .jump = true}, 12 * 12), world);
    // The jumps fall on ticks 1, 13, 25 and so on; by the last cycle the
    // speed carried from jump to jump has settled.
    const std::size_t last_cycle = 12 * 11;
    CHECK(steps[last_cycle].height == doctest::Approx(0.42).epsilon(tolerance));
    CHECK(steps[last_cycle].along == doctest::Approx(0.612183).epsilon(tolerance));
    CHECK(steps[last_cycle + 11].along == doctest::Approx(0.312949).epsilon(tolerance));
    double cycle = 0.0;
    for (std::size_t i = last_cycle; i < last_cycle + 12; ++i) {
        cycle += steps[i].along;
    }
    CHECK(cycle == doctest::Approx(4.276059).epsilon(tolerance));
    CHECK(cycle / 12.0 * 20.0 == doctest::Approx(7.126765).epsilon(tolerance));
}

TEST_CASE("the boost goes along the facing, whatever the other keys") {
    // Sprint-jumping with W and A: the boost is all along -z; only the
    // keys' acceleration goes along the diagonal.
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    vx::Player player = at_rest();
    player.sprinting = true;
    std::vector<Box> scratch;
    vx::tick(player, {.held = {.forward = true, .left = true, .jump = true}, .yaw = 0.0F}, world,
             numbers, scratch);
    const double keys = 0.13 / std::sqrt(2.0); // sprinting acceleration, 1 / root 2 on each axis
    CHECK(player.position.x == doctest::Approx(-keys).epsilon(tolerance));
    CHECK(player.position.z == doctest::Approx(-(0.2 + keys)).epsilon(tolerance));
}

TEST_CASE(
    "under a ceiling a held jump waits out the cooldown, and pressed again jumps every 3 ticks") {
    // Under a ceiling 2 blocks up, a jump rises 0.2, hits it, and lands on
    // its 3rd tick. Held, the cooldown makes the next jump wait for the 11th
    // tick; pressed again on landing, the player jumps every 3 ticks.
    vx::test::FlatWorld corridor;
    for (int x = -2; x < 3; ++x) {
        for (int z = -60; z < 3; ++z) {
            corridor.boxes.push_back(block_box(x, 2.0, z));
        }
    }
    // A player that may not fly: in creative, Space pressed twice within 7
    // ticks would start a flight instead.
    const auto jump_ticks = [&](const std::vector<vx::MovementKeys>& keys) {
        vx::Player player = at_rest();
        player.may_fly = false;
        const std::vector<Step> steps = run(player, keys, corridor);
        std::vector<std::size_t> jumps;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].height == doctest::Approx(0.2)) {
                jumps.push_back(i + 1);
            }
        }
        return jumps;
    };
    const std::vector<std::size_t> held = jump_ticks(repeat({.jump = true}, 30));
    CHECK(held == std::vector<std::size_t>{1, 11, 21});

    std::vector<vx::MovementKeys> pressed = repeat({}, 30);
    for (std::size_t i = 0; i < pressed.size(); i += 3) {
        pressed[i].jump = true;
    }
    CHECK(jump_ticks(pressed) == std::vector<std::size_t>{1, 4, 7, 10, 13, 16, 19, 22, 25, 28});

    // The jump in the corridor, tick by tick: up 0.2 to the ceiling, then
    // gravity's steps, and down on the 3rd.
    vx::Player player = at_rest();
    const std::vector<Step> steps = run(player, repeat({.jump = true}, 3), corridor);
    CHECK(steps[0].height == doctest::Approx(0.2).epsilon(tolerance));
    CHECK(steps[1].height == doctest::Approx(0.1216).epsilon(tolerance));
    CHECK(steps[2].height == 0.0);
    // A head against the ceiling is not on the ground.
    CHECK_FALSE(steps[0].on_ground);
    CHECK_FALSE(steps[1].on_ground);
    CHECK(steps[2].on_ground);
}

TEST_CASE("Space does nothing in the air") {
    const vx::test::FlatWorld world;
    vx::Player player;
    player.position.y = 5.0;
    const std::vector<Step> steps = run(player, repeat({.jump = true}, 3), world);
    CHECK(steps[0].height == 5.0);
    CHECK(player.velocity.y < 0.0);
}

TEST_CASE("the boost turns with the yaw, and goes along the facing even when moving another way") {
    // Sprinting toward -z at top speed, the player turns a quarter left and
    // sprint-jumps with W and A on the same tick: the boost goes along the
    // new facing, -x, and the keys along -x and +z; the speed carried keeps
    // going toward -z.
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    vx::Player player = at_rest();
    player.sprinting = true;
    static_cast<void>(run(player, repeat({.forward = true}, 100), world));
    std::vector<Box> scratch;
    const Vec3d before = player.position;
    vx::tick(player,
             {.held = {.forward = true, .left = true, .jump = true},
              .yaw = std::numbers::pi_v<float> / 2.0F},
             world, numbers, scratch);
    const Vec3d moved = player.position - before;
    CHECK(moved.x == doctest::Approx(-0.291924).epsilon(tolerance));
    CHECK(moved.z == doctest::Approx(-0.061293).epsilon(tolerance));
    CHECK(moved.y == doctest::Approx(0.42).epsilon(tolerance));
}

TEST_CASE("a jump into a slab steps onto it instead, as the game's jump cancel does") {
    // Walking at a slab 0.5 tall from z = -1 on, Space on the tick that
    // reaches it: the tick began on the ground, so the box steps onto the
    // slab rather than rising 0.42 into its side, and the jump's speed is
    // gone.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -12; z < -1; ++z) {
            world.boxes.push_back(vx::test::low_box(x, z, 0.5));
        }
    }
    vx::Player player = at_rest();
    std::vector<vx::MovementKeys> keys = repeat({.forward = true}, 6);
    keys[4].jump = true;
    const std::vector<Step> steps = run(player, keys, world);
    CHECK(steps[4].height == 0.5);
    CHECK(steps[4].along == doctest::Approx(0.205385).epsilon(tolerance));
    CHECK_FALSE(steps[4].on_ground);
    CHECK(steps[5].height == 0.5);
    CHECK(steps[5].along == doctest::Approx(0.131740).epsilon(tolerance));
    CHECK(steps[5].on_ground);
}

TEST_CASE("a jump never slows a faster rise: it leaves at least 0.42, as 26.2's does") {
    // On the ground and already rising 0.6 a tick, as something pushing the
    // player up would leave it: Space keeps the 0.6. Nothing in Perdix
    // pushes a player up yet, so only this shows it.
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.velocity.y = 0.6;
    const std::vector<Step> steps = run(player, {{.jump = true}}, world);
    CHECK(steps[0].height == doctest::Approx(0.6).epsilon(tolerance));
}

TEST_CASE("Shift held in the air does not change a walking jump's arc") {
    // Flight's Shift sinks; a walking player's only crouches.
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    std::vector<vx::MovementKeys> keys = repeat({.sneak = true}, 12);
    keys[0].jump = true;
    const std::vector<Step> steps = run(player, keys, world);
    for (std::size_t i = 0; i < jump_heights.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(steps[i].height == doctest::Approx(jump_heights[i]).epsilon(tolerance));
    }
}
