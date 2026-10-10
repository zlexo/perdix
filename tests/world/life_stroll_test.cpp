#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

// RFC 0012's stroll, seeded and deterministic: a chance of 1 in 60 at each
// choice to pick a spot (stroll_spot_test.cpp has the picking), then a walk
// straight at it, the walking yaw turning at most 90 degrees a tick, until
// he is within a block of it or covers less than a block between two checks
// more than 100 ticks apart.

namespace {

using vx::LifeGoal;
using vx::Vec3d;
using vx::test::Scene;
using vx::test::Watch;

// You, 7 blocks over where he starts: never seen, and every spot near him
// inside the leash.
constexpr Vec3d overhead{.x = 0.0, .y = 7.0, .z = 0.0};

bool idle(const vx::MovementKeys& keys) {
    return !keys.forward && !keys.back && !keys.left && !keys.right && !keys.jump && !keys.sneak &&
           !keys.sprint;
}

double distance(Vec3d a, Vec3d b) {
    const Vec3d d = b - a;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

} // namespace

TEST_CASE("strolls start at 1 chance in 60 at each choice") {
    // Over the RFC's 12,000 ticks, and over 60,000, which tells 1 in 60
    // from 1 in 50 or 70.
    for (const std::uint64_t ticks : {12'000U, 60'000U}) {
        INFO(ticks, " ticks");
        Scene scene(1);
        scene.you.feet = overhead;
        Watch watch{.goal = LifeGoal::stroll};
        watch.run(scene, ticks);
        INFO(watch.tally.starts, " strolls in ", watch.tally.chances, " chances");
        CHECK(watch.tally.starts > static_cast<int>(ticks / 300));
        CHECK(watch.tally.fits(1.0 / 60.0));
    }
}

TEST_CASE("he walks at the spot at walking pace turning at most 90 degrees a tick") {
    Scene scene(4);
    scene.you.feet = overhead;
    float last_yaw = 0.0F;
    int walked = 0;
    for (int i = 0; i < 12'000; ++i) {
        const std::optional<Vec3d> spot = scene.life.walk_target();
        const Vec3d at = scene.perdix.position;
        const vx::LifeTick ticked = scene.step();
        const std::optional<Vec3d> walking_to = scene.life.walk_target();
        if (ticked.input.held.forward) {
            ++walked;
            REQUIRE(walking_to);
            CHECK_FALSE(ticked.input.held.sprint);
            CHECK_FALSE(ticked.input.held.jump);
            const double turned =
                std::abs(vx::wrap_turn(static_cast<double>(ticked.input.yaw - last_yaw)));
            CHECK(turned <= vx::test::radians(90.0) + 1e-6);
            // Once it points at the spot, it keeps to it.
            if (spot && spot == walking_to && turned < vx::test::radians(89.0)) {
                CHECK(ticked.input.yaw == doctest::Approx(vx::test::yaw_toward(at, *spot)));
            }
        } else {
            CHECK(idle(ticked.input.held));
        }
        last_yaw = ticked.input.yaw;
    }
    CHECK(walked > 1000);
}

TEST_CASE("within a block of the spot he stops and the stroll ends at the next choice") {
    Scene scene(5);
    scene.you.feet = overhead;
    int arrivals = 0;
    for (int i = 0; i < 12'000; ++i) {
        const std::optional<Vec3d> spot = scene.life.walk_target();
        const bool near = spot && distance(scene.perdix.position, *spot) <= 1.0;
        const vx::LifeTick ticked = scene.step();
        if (!near) {
            continue;
        }
        ++arrivals;
        CHECK_FALSE(ticked.input.held.forward);
        CHECK_FALSE(scene.life.walk_target());
        // The next choice, a tick or two on, ends it, unless it starts
        // another.
        const std::uint64_t strolls = scene.life.starts(LifeGoal::stroll);
        static_cast<void>(scene.step());
        static_cast<void>(scene.step());
        CHECK((!scene.life.running(LifeGoal::stroll) ||
               scene.life.starts(LifeGoal::stroll) > strolls));
    }
    CHECK(arrivals > 40);
}

TEST_CASE("a stroll that covers less than a block in 100 ticks ends stuck") {
    // A cell a block across with a half block underfoot, walled in two
    // blocks high: every spot is outside the walls or on them, and no walk
    // gets him there.
    vx::test::SceneOf<vx::test::LooseWorld> scene(6);
    for (int x = -12; x <= 12; ++x) {
        for (int z = -12; z <= 12; ++z) {
            const int ring = std::max(std::abs(x), std::abs(z));
            if (ring == 0) {
                scene.world.boxes.push_back({.min = {.x = 0.0, .y = -0.5, .z = 0.0},
                                             .max = {.x = 1.0, .y = 0.0, .z = 1.0}});
                continue;
            }
            scene.world.boxes.push_back(vx::test::block_box(x, -1.0, z));
            if (ring == 1) {
                scene.world.boxes.push_back(vx::test::block_box(x, 0.0, z));
                scene.world.boxes.push_back(vx::test::block_box(x, 1.0, z));
            }
        }
    }
    scene.perdix.position = {.x = 0.5, .y = 0.0, .z = 0.5};
    scene.you.feet = {.x = 0.5, .y = 7.0, .z = 0.5};
    Watch watch{.goal = LifeGoal::stroll};
    watch.run(scene, 12'000);
    watch.spells.pop_back(); // the last may still be running
    REQUIRE(watch.spells.size() > 20);
    for (const vx::test::Spell& spell : watch.spells) {
        INFO("the stroll from tick ", spell.start);
        // Stuck at the check 101 ticks in, and ended at the choice after.
        CHECK(spell.length == 102);
    }
    CHECK(std::abs(scene.perdix.position.x - 0.5) < 0.25);
    CHECK(std::abs(scene.perdix.position.z - 0.5) < 0.25);
}
