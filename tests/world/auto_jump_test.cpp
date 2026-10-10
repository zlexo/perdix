#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <numbers>
#include <vector>

// RFC 0012's auto-jump, as 26.2's LocalPlayer does it: after a move on the
// ground with a key that moves him, not sneaking and not going backward,
// with room over his head, two probes 0.51 over his feet and 0.3 either
// side reach 7 times his speed ahead; the first box they cross, in the
// game's order, must rise more than 0.5 and at most 1.2 with nothing over
// it, and he jumps on the next tick. The player faces -z (yaw 0).

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::block_box;

constexpr vx::MovementKeys forward{.forward = true};

// A player walking on flat ground with the game's auto-jump: each step runs
// RFC 0010's tick with the keys, and Space when the last move found a step.
struct Climber {
    vx::test::FlatWorld world;
    vx::MovementNumbers numbers;
    vx::Player player = vx::test::at_rest();
    std::vector<Box> scratch;
    bool jump_next = false;
    float pitch = 0.0F;

    // Whether this tick's move finds a step to jump.
    bool step(vx::MovementKeys keys, float yaw = 0.0F) {
        keys.jump = keys.jump || jump_next;
        const vx::TickInput input{.held = keys, .yaw = yaw};
        const Vec3d before = player.position;
        vx::tick(player, input, world, numbers, scratch);
        jump_next =
            vx::auto_jumps(player, input, pitch, player.position - before, world, numbers, scratch);
        return jump_next;
    }

    // Whether any of `ticks` ticks of `keys` finds a step.
    bool ever(const vx::MovementKeys& keys, int ticks, float yaw = 0.0F) {
        bool found = false;
        for (int i = 0; i < ticks; ++i) {
            found = step(keys, yaw) || found;
        }
        return found;
    }
};

// A step a block high and wide, its face toward the player at z = -2, and
// raised ground behind it.
void lay_step(vx::test::FlatWorld& world) {
    for (int x = -2; x <= 2; ++x) {
        for (int z = -12; z <= -3; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
    }
}

// Whether a standing player at `feet`, W held and not yet moving, finds a
// step among `boxes`.
bool finds(const std::vector<Box>& boxes, Vec3d feet = {}, bool sprinting = false,
           const vx::MovementKeys& keys = forward, float pitch = 0.0F) {
    vx::test::FlatWorld world;
    world.boxes = boxes;
    vx::Player player = vx::test::at_rest();
    player.position = feet;
    player.sprinting = sprinting;
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    return vx::auto_jumps(player, {.held = keys, .yaw = 0.0F}, pitch, {}, world, numbers, scratch);
}

// A box across the player's way, from `near_face` back a block, from
// `bottom` up to `top`.
Box across(double near_face, double bottom, double top) {
    return {.min = {.x = -1.0, .y = bottom, .z = near_face - 1.0},
            .max = {.x = 1.0, .y = top, .z = near_face}};
}

} // namespace

TEST_CASE("walking at a 1-block step he jumps the tick after the probe meets it and climbs") {
    Climber climber;
    lay_step(climber.world);
    int found_at = -1;
    for (int i = 0; i < 40 && found_at < 0; ++i) {
        const double before = climber.player.position.z;
        if (climber.step(forward)) {
            found_at = i;
            // This move's probe, from his feet a move and 0.7 on, reaches
            // past the step's face.
            const double moved = climber.player.position.z - before;
            CHECK(climber.player.position.z + moved - 0.7 < -2.0);
        }
    }
    REQUIRE(found_at >= 0);
    CHECK(climber.player.position.y == 0.0);
    static_cast<void>(climber.step(forward));
    CHECK(climber.player.position.y == doctest::Approx(0.42));
    for (int i = 0; i < 30; ++i) {
        static_cast<void>(climber.step(forward));
    }
    CHECK(climber.player.on_ground);
    CHECK(climber.player.position.y == 1.0);
    CHECK(climber.player.position.z < -3.0);
}

TEST_CASE("he never jumps a wall 1.5 blocks high or a step with a block on it or under a roof") {
    SUBCASE("a wall 1.5 blocks high") {
        Climber climber;
        climber.world.boxes.push_back(across(-2.0, 0.0, 1.5));
        CHECK_FALSE(climber.ever(forward, 40));
        CHECK(climber.player.position.y == 0.0);
    }
    SUBCASE("a step with a block on it") {
        Climber climber;
        lay_step(climber.world);
        for (int x = -2; x <= 2; ++x) {
            climber.world.boxes.push_back(block_box(x, 1.0, -3.0));
        }
        CHECK_FALSE(climber.ever(forward, 40));
    }
    SUBCASE("a roof two blocks up over his head") {
        Climber climber;
        lay_step(climber.world);
        for (int x = -2; x <= 2; ++x) {
            for (int z = -3; z <= 1; ++z) {
                climber.world.boxes.push_back(block_box(x, 2.0, z));
            }
        }
        CHECK_FALSE(climber.ever(forward, 40));
    }
}

TEST_CASE("he does not jump walking backward or sneaking but does strafing") {
    SUBCASE("backward into a step behind him") {
        Climber climber;
        climber.world.boxes.push_back(
            {.min = {.x = -1.0, .y = 0.0, .z = 2.0}, .max = {.x = 1.0, .y = 1.0, .z = 3.0}});
        CHECK_FALSE(climber.ever({.back = true}, 40));
    }
    SUBCASE("sneaking") {
        Climber climber;
        lay_step(climber.world);
        CHECK_FALSE(climber.ever({.forward = true, .sneak = true}, 80));
    }
    SUBCASE("strafing right into a step to his right") {
        Climber climber;
        climber.world.boxes.push_back(
            {.min = {.x = 2.0, .y = 0.0, .z = -1.0}, .max = {.x = 3.0, .y = 1.0, .z = 1.0}});
        CHECK(climber.ever({.right = true}, 40));
    }
}

TEST_CASE("looking almost straight down he jumps even walking backward") {
    // The game takes the look with its pitch: walking backward, the way
    // against the look is -cos(pitch), past -0.15 only below 81.4 degrees.
    const std::vector<Box> behind{
        {.min = {.x = -1.0, .y = 0.0, .z = 0.5}, .max = {.x = 1.0, .y = 1.0, .z = 1.5}}};
    const auto down = [](double degrees) {
        return static_cast<float>(-degrees * std::numbers::pi / 180.0);
    };
    CHECK_FALSE(finds(behind, {}, false, {.back = true}, down(80.0)));
    CHECK(finds(behind, {}, false, {.back = true}, down(82.0)));
    CHECK(finds(behind, {}, false, {.back = true}, down(90.0)));
    CHECK_FALSE(finds(behind, {}, false, {.back = true}, 0.0F));
}

TEST_CASE("the probes run 0.51 over his feet and 0.3 either side") {
    CHECK(finds({across(-0.6, 0.505, 1.0)}));
    CHECK_FALSE(finds({across(-0.6, 0.515, 1.0)}));
    const auto post = [](double from_x, double to_x) {
        return Box{.min = {.x = from_x, .y = 0.0, .z = -0.8},
                   .max = {.x = to_x, .y = 1.0, .z = -0.5}};
    };
    CHECK(finds({post(0.25, 0.35)}));
    CHECK(finds({post(-0.35, -0.25)}));
    CHECK_FALSE(finds({post(0.31, 0.41)}));
    CHECK_FALSE(finds({post(-0.1, 0.1)}));
}

TEST_CASE("the probes reach 7 times his speed: 0.7 walking and 0.91 sprinting") {
    CHECK(finds({across(-0.65, 0.0, 1.0)}));
    CHECK_FALSE(finds({across(-0.75, 0.0, 1.0)}));
    CHECK(finds({across(-0.85, 0.0, 1.0)}, {}, true));
    CHECK_FALSE(finds({across(-0.95, 0.0, 1.0)}, {}, true));
}

TEST_CASE("an obstacle must rise more than 0.5 and at most 1.2 above his feet") {
    CHECK_FALSE(finds({across(-0.5, 0.0, 0.5)}));
    CHECK(finds({across(-0.5, 0.0, 0.55)}));
    CHECK(finds({across(-0.5, 0.0, 1.2)}));
    CHECK_FALSE(finds({across(-0.5, 0.0, 1.25)}));
}

TEST_CASE("the first box in the game's order counts and not the nearest") {
    // A step a block high, and a wall two high a block behind it, both in
    // reach of a long move: the game meets boxes by z from the lowest, so
    // going toward -z the wall comes first and there is no jump; going
    // toward +z the step does.
    const auto run = [](double facing, double step_z, double wall_z, Vec3d feet, Vec3d moved) {
        vx::test::FlatWorld world;
        world.boxes = {block_box(0.0, 0.0, step_z), block_box(0.0, 0.0, wall_z),
                       block_box(0.0, 1.0, wall_z)};
        vx::Player player = vx::test::at_rest();
        player.position = feet;
        const vx::MovementNumbers numbers;
        std::vector<Box> scratch;
        return vx::auto_jumps(player, {.held = forward, .yaw = static_cast<float>(facing)}, 0.0F,
                              moved, world, numbers, scratch);
    };
    CHECK_FALSE(
        run(0.0, -1.0, -2.0, {.x = 0.5, .y = 0.0, .z = 0.3}, {.x = 0.0, .y = 0.0, .z = -1.0}));
    CHECK(run(std::numbers::pi, 1.0, 2.0, {.x = 0.5, .y = 0.0, .z = 0.7},
              {.x = 0.0, .y = 0.0, .z = 1.0}));
}
