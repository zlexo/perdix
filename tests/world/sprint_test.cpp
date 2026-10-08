#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <numbers>
#include <optional>
#include <vector>

// RFC 0010's sprint rules and its 1-block step vector. As in the tick's
// other tests, the player faces -z at yaw 0, so the vectors' +z is -z here.

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::at_rest;
using vx::test::block_box;

constexpr double tolerance = 1e-5;

// One tick's result.
struct Step {
    Vec3d position;
    bool sprinting = false;
    bool on_ground = false;
    bool horizontal_collision = false;
};

// Runs a tick for each set of keys, in order, at `yaw`.
std::vector<Step> run(vx::Player& player, const std::vector<vx::MovementKeys>& keys,
                      const vx::BlockBoxes& world, float yaw = 0.0F) {
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    std::vector<Step> steps;
    for (const vx::MovementKeys& held : keys) {
        vx::tick(player, {.held = held, .yaw = yaw}, world, numbers, scratch);
        steps.push_back({.position = player.position,
                         .sprinting = player.sprinting,
                         .on_ground = player.on_ground,
                         .horizontal_collision = player.horizontal_collision});
    }
    return steps;
}

std::vector<vx::MovementKeys> repeat(const vx::MovementKeys& keys, std::size_t ticks) {
    return std::vector<vx::MovementKeys>(ticks, keys);
}

constexpr vx::MovementKeys w{.forward = true};
constexpr vx::MovementKeys ctrl_w{.forward = true, .sprint = true};

// Degrees as a yaw: positive turns left, toward -x.
float degrees(double angle) {
    return static_cast<float>(angle * std::numbers::pi / 180.0);
}

} // namespace

TEST_CASE("the sprint key starts a sprint with W held, speeding the player up that same tick") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    const std::vector<Step> steps = run(player, {ctrl_w}, world);
    CHECK(steps[0].sprinting);
    // Sprinting's 0.13 on the first tick, not walking's 0.1.
    CHECK(-steps[0].position.z == doctest::Approx(0.1274).epsilon(tolerance));
}

TEST_CASE("the sprint key held in the air starts a sprint, which accelerates 0.026 there") {
    const vx::test::FlatWorld world;
    vx::Player player;
    player.position.y = 100.0;
    const std::vector<Step> steps = run(player, {ctrl_w}, world);
    CHECK(steps[0].sprinting);
    CHECK(-steps[0].position.z == doctest::Approx(0.02548).epsilon(tolerance));
}

TEST_CASE("the sprint key starts nothing without W, with W and S, or crouching") {
    const vx::test::FlatWorld world;
    for (const vx::MovementKeys& keys :
         {vx::MovementKeys{.sprint = true}, vx::MovementKeys{.left = true, .sprint = true},
          vx::MovementKeys{.back = true, .sprint = true},
          vx::MovementKeys{.forward = true, .back = true, .sprint = true}}) {
        vx::Player player = at_rest();
        CHECK_FALSE(run(player, {keys}, world)[0].sprinting);
    }
    // Crouching, W gives 0.3 of the 0.8 a sprint needs.
    vx::Player crouching = at_rest();
    crouching.pose = vx::Pose::crouching;
    CHECK_FALSE(
        run(crouching, {{.forward = true, .sneak = true, .sprint = true}}, world)[0].sprinting);
}

TEST_CASE("W pressed twice within 6 ticks starts a sprint, and 7 apart does not") {
    // A fresh press opens a 7-tick window that counts down before it is
    // checked, so the second press must come within 6 ticks (26.2's Sprint
    // Window option, 7 by default).
    const vx::test::FlatWorld world;
    for (const std::size_t apart : std::initializer_list<std::size_t>{2, 6, 7}) {
        INFO(apart, " ticks apart");
        vx::Player player = at_rest();
        std::vector<vx::MovementKeys> keys = repeat({}, apart + 2);
        keys[0] = w;
        keys[apart] = w;
        keys[apart + 1] = w;
        const std::vector<Step> steps = run(player, keys, world);
        CHECK_FALSE(steps[0].sprinting);
        CHECK(steps[apart].sprinting == (apart <= 6));
        // Held on, W alone keeps a sprint going and starts none.
        CHECK(steps[apart + 1].sprinting == (apart <= 6));
    }
}

TEST_CASE("W pressed twice in the air sprints too: 26.2's double tap has no ground rule") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    std::vector<vx::MovementKeys> keys = repeat({}, 4);
    keys[0] = w;
    keys[1].jump = true; // off the ground from here
    keys[3] = w;
    const std::vector<Step> steps = run(player, keys, world);
    CHECK_FALSE(steps[3].on_ground);
    CHECK(steps[3].sprinting);
}

TEST_CASE("Shift held the tick before, or S held, closes W's double-tap window") {
    const vx::test::FlatWorld world;
    for (const vx::MovementKeys& between :
         {vx::MovementKeys{.sneak = true}, vx::MovementKeys{.back = true}}) {
        INFO("Shift ", between.sneak, ", S ", between.back);
        vx::Player player = at_rest();
        const std::vector<Step> steps = run(player, {w, between, {}, w}, world);
        CHECK_FALSE(steps[3].sprinting);
    }
    // Without either, the same presses sprint.
    vx::Player player = at_rest();
    CHECK(run(player, {w, {}, {}, w}, world)[3].sprinting);
}

TEST_CASE("the window runs on after a double tap: let go and press again inside it, and the sprint "
          "starts again") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    const std::vector<Step> steps = run(player, {w, {}, w, {}, w}, world);
    CHECK(steps[2].sprinting);
    CHECK_FALSE(steps[3].sprinting);
    CHECK(steps[4].sprinting);
}

TEST_CASE("a fresh push forward is W's input rising from none: S let go with W held counts") {
    // W and S together push nowhere; letting go of S is the first push, and
    // W pressed again inside the window sprints.
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    const std::vector<Step> steps = run(player, {{.forward = true, .back = true}, w, {}, w}, world);
    CHECK_FALSE(steps[1].sprinting);
    CHECK(steps[3].sprinting);
}

TEST_CASE("a sprint stops when W goes, or with S held too, and goes on with A or D added") {
    const vx::test::FlatWorld world;
    struct Case {
        vx::MovementKeys keys;
        bool sprinting;
    };
    for (const Case& game :
         {Case{.keys = {}, .sprinting = false}, Case{.keys = {.back = true}, .sprinting = false},
          Case{.keys = {.forward = true, .back = true}, .sprinting = false},
          Case{.keys = {.left = true}, .sprinting = false},
          Case{.keys = {.forward = true}, .sprinting = true},
          Case{.keys = {.forward = true, .left = true}, .sprinting = true},
          Case{.keys = {.forward = true, .right = true}, .sprinting = true}}) {
        vx::Player player = at_rest();
        const std::vector<Step> steps = run(player, {ctrl_w, game.keys}, world);
        CHECK(steps[0].sprinting);
        CHECK(steps[1].sprinting == game.sprinting);
    }
}

TEST_CASE("a wall grazed at 5 degrees keeps a sprint, and one hit at 45 ends it the next tick") {
    // A wall 2 blocks high along z, its face at x = -1, and the player's box
    // against it: turned left, the keys push into the wall, and the player
    // slides along it straight down -z.
    vx::test::FlatWorld world;
    for (int z = -40; z < 2; ++z) {
        world.boxes.push_back(block_box(-2.0, 0.0, z));
        world.boxes.push_back(block_box(-2.0, 1.0, z));
    }
    const auto against_the_wall = [] {
        vx::Player player = at_rest();
        player.position.x = -0.7;
        return player;
    };
    vx::Player grazing = against_the_wall();
    const std::vector<Step> grazed = run(grazing, repeat(ctrl_w, 20), world, degrees(5.0));
    for (std::size_t i = 0; i < grazed.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(grazed[i].horizontal_collision);
        CHECK(grazed[i].sprinting);
    }
    CHECK(grazing.position.x == doctest::Approx(-0.7));

    vx::Player hitting = against_the_wall();
    const std::vector<Step> hit = run(hitting, repeat(ctrl_w, 3), world, degrees(45.0));
    CHECK(hit[0].horizontal_collision);
    CHECK(hit[0].sprinting);
    // Held against the wall, the sprint key cannot start it again.
    CHECK_FALSE(hit[1].sprinting);
    CHECK_FALSE(hit[2].sprinting);
}

TEST_CASE("sprinting onto a 1-block step: Space on ticks 4 to 9 clears its face, 1 to 3 land "
          "short, and 10 or 11 hit it and climb on") {
    // RFC 0010's step vector: a step 1 block high whose face is 3 blocks
    // ahead, sprinting from rest with the sprint key and W held, and Space
    // on one tick.
    vx::test::FlatWorld world;
    for (int x = -2; x < 2; ++x) {
        for (int z = -30; z < -3; ++z) {
            world.boxes.push_back(block_box(x, 0.0, z));
        }
    }
    for (std::size_t jump = 1; jump <= 11; ++jump) {
        INFO("Space on tick ", jump);
        vx::Player player = at_rest();
        std::vector<vx::MovementKeys> keys = repeat(ctrl_w, 30);
        keys[jump - 1].jump = true;
        const std::vector<Step> steps = run(player, keys, world);
        // The landing: the first tick after the jump's to end on the ground.
        std::size_t landing = jump;
        while (landing < steps.size() && !steps[landing].on_ground) {
            ++landing;
        }
        REQUIRE(landing < steps.size());
        std::optional<std::size_t> hit;
        for (std::size_t i = 0; i <= landing; ++i) {
            if (steps[i].horizontal_collision && !hit) {
                hit = i;
            }
        }
        if (jump <= 3) {
            CHECK(steps[landing].position.y == 0.0);
        } else if (jump <= 9) {
            CHECK_FALSE(hit.has_value());
            CHECK(steps[landing].position.y == doctest::Approx(1.0));
        } else {
            // Under the 8-degree rule, measured against the motion after the
            // collision, Space on tick 10 meets the face moving straight at
            // it (0 degrees, a graze) and clears the step the next tick, so
            // the sprint survives; on tick 11 the box then stands dead
            // against the face for a tick, which ends it. RFC 0010's
            // simulator ended a sprint on any hit, so its vector has both
            // losing it.
            REQUIRE(hit.has_value());
            bool lost = false;
            for (std::size_t i = *hit + 1; i <= landing; ++i) {
                lost = lost || !steps[i].sprinting;
            }
            CHECK(lost == (jump == 11));
            CHECK(steps[landing].position.y == doctest::Approx(1.0));
        }
        if (jump == 7) {
            // The vector's clean case: on top at tick 15, 4.111729 along.
            CHECK(landing + 1 == 15);
            CHECK(-steps[landing].position.z == doctest::Approx(4.111729).epsilon(tolerance));
        }
    }
}

