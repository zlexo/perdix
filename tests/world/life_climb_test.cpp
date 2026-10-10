#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

// RFC 0012's life with auto-jump: a stroll that meets a step a block high
// climbs it the tick after the look ahead finds it; one that meets a wall
// 1.5 high ends stuck.

namespace {

using vx::test::block_box;

// A pit 3 blocks across with a half block underfoot, so every spot is up on
// the ground around it, a block above his feet or, with the ground 1.5
// high, too high to jump.
std::unique_ptr<vx::test::SceneOf<vx::test::LooseWorld>> pit(double ground) {
    auto scene = std::make_unique<vx::test::SceneOf<vx::test::LooseWorld>>(8);
    for (int x = -12; x <= 12; ++x) {
        for (int z = -12; z <= 12; ++z) {
            if (std::max(std::abs(x), std::abs(z)) <= 1) {
                scene->world.boxes.push_back(
                    {.min = {.x = static_cast<double>(x), .y = -0.5, .z = static_cast<double>(z)},
                     .max = {.x = x + 1.0, .y = 0.0, .z = z + 1.0}});
                continue;
            }
            scene->world.boxes.push_back(block_box(x, -1.0, z));
            scene->world.boxes.push_back(
                {.min = {.x = static_cast<double>(x), .y = 0.0, .z = static_cast<double>(z)},
                 .max = {.x = x + 1.0, .y = ground, .z = z + 1.0}});
        }
    }
    scene->perdix.position = {.x = 0.5, .y = 0.0, .z = 0.5};
    scene->you.feet = {.x = 0.5, .y = 8.0, .z = 0.5};
    return scene;
}

} // namespace

TEST_CASE("strolling out of a pit he jumps its edge but walled in 1.5 high never does") {
    SUBCASE("a block high: out at the first stroll") {
        const auto scene = pit(1.0);
        bool jumped = false;
        while (scene->life.starts(vx::LifeGoal::stroll) == 0) {
            static_cast<void>(scene->step());
        }
        for (int i = 0; i < 200 && scene->life.running(vx::LifeGoal::stroll); ++i) {
            jumped = scene->step().input.held.jump || jumped;
        }
        CHECK(jumped);
        CHECK(scene->perdix.position.y >= 1.0);
        CHECK_FALSE(scene->life.running(vx::LifeGoal::stroll));
    }
    SUBCASE("1.5 high: every stroll ends stuck") {
        const auto scene = pit(1.5);
        vx::test::Watch watch{.goal = vx::LifeGoal::stroll};
        watch.run(*scene, 6'000);
        watch.spells.pop_back();
        REQUIRE(watch.spells.size() > 5);
        // Stuck at the first check, 101 ticks in, or, when the stroll began
        // a block or more from the edge it walks into, at the second.
        for (const vx::test::Spell& spell : watch.spells) {
            CHECK((spell.length == 102 || spell.length == 204));
        }
        CHECK(scene->perdix.position.y == 0.0);
    }
}

TEST_CASE("started over, he forgets the jump his last move found") {
    // Two of the same life, one a tick ahead: when it jumps, the other has
    // that jump waiting.
    const auto ahead = pit(1.0);
    const auto scene = pit(1.0);
    bool waiting = false;
    for (int tick = 0; tick < 4'000 && !waiting; ++tick) {
        waiting = ahead->step().input.held.jump;
        if (!waiting) {
            static_cast<void>(scene->step());
        }
    }
    REQUIRE(waiting);
    scene->life.start_over(scene->motion.body_yaw);
    CHECK_FALSE(scene->step().input.held.jump);
}
