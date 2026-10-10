#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// RFC 0012's two looking goals, seeded and deterministic, in Minecraft
// 26.2's rhythm: the goals are chosen every second tick; looking at you is a
// chance of 2 in 100 at each choice while you are within 6 blocks (4.8
// sneaking) and in sight, and lasts 40 and up to 39 more ticks counted in
// choices, so 40 to 80; looking around is 2 in 100 when nothing else holds
// his eyes or legs, and lasts 20 and up to 19 more ticks, counted every
// tick but ended only at a choice, so 22 to 40.

namespace {

using vx::LifeGoal;
using vx::Vec3d;
using vx::test::degrees;
using vx::test::PillarScene;
using vx::test::Spell;
using vx::test::Watch;

// You 5 blocks ahead of Perdix, who stands on a pillar in the middle of
// nowhere, so he never strolls away.
constexpr Vec3d five_ahead{.x = 0.5, .y = 0.0, .z = -4.5};

Vec3d ahead(double distance) {
    return {.x = 0.5, .y = 0.0, .z = 0.5 - distance};
}

} // namespace

TEST_CASE("with you 5 blocks away in his sight he looks at you within a few hundred ticks") {
    std::uint64_t total = 0;
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        INFO("seed ", seed);
        PillarScene scene(seed);
        scene.you.feet = five_ahead;
        std::uint64_t tick = 0;
        while (!scene.life.running(LifeGoal::look_at_player) && tick < 1000) {
            static_cast<void>(scene.step());
            ++tick;
        }
        CHECK(scene.life.running(LifeGoal::look_at_player));
        total += tick;
    }
    // A chance of 2 in 100 every second tick: 100 ticks on average.
    CHECK(total / 20 < 300);
}

TEST_CASE("he looks at you for 40 to 80 ticks with his head on your eyes") {
    PillarScene scene(11);
    scene.you.feet = five_ahead;
    Watch watch{.goal = LifeGoal::look_at_player};
    std::uint64_t longest = 0;
    std::uint64_t shortest = 1000;
    for (std::uint64_t tick = 0; tick < 20'000; ++tick) {
        watch.step(scene, tick);
        // Once his head has had time to turn, it is on your eyes: straight
        // ahead, level, as both stand on the same floor.
        if (scene.life.running(LifeGoal::look_at_player) && !watch.spells.empty() &&
            tick - watch.spells.back().start >= 18) {
            CHECK(std::abs(scene.motion.head_yaw) < 1e-5F);
            CHECK(scene.motion.head_pitch == 0.0F);
        }
    }
    REQUIRE(watch.spells.size() > 20);
    watch.spells.pop_back(); // the last may still be running
    for (const Spell& spell : watch.spells) {
        INFO("the look from tick ", spell.start);
        CHECK(spell.length >= 40);
        CHECK(spell.length <= 80);
        CHECK(spell.length % 2 == 0);
        longest = std::max(longest, spell.length);
        shortest = std::min(shortest, spell.length);
    }
    // The whole range: 40 and 79 halved, rounded up, two ticks a choice.
    CHECK(shortest == 40);
    CHECK(longest == 80);
}

TEST_CASE("looking at you is a chance of 2 in 100 at each choice") {
    PillarScene scene(5);
    scene.you.feet = five_ahead;
    Watch watch{.goal = LifeGoal::look_at_player};
    watch.run(scene, 60'000);
    INFO(watch.tally.starts, " looks in ", watch.tally.chances, " chances");
    CHECK(watch.tally.chances > 10'000);
    CHECK(watch.tally.fits(0.02));
}

TEST_CASE("he never looks at you through a wall or from 7 blocks") {
    SUBCASE("a wall between you") {
        PillarScene scene(3);
        scene.you.feet = five_ahead;
        scene.world.boxes.push_back(
            {.min = {.x = -3.0, .y = -2.0, .z = -2.9}, .max = {.x = 3.0, .y = 3.0, .z = -2.1}});
        for (int i = 0; i < 20'000; ++i) {
            static_cast<void>(scene.step());
        }
        CHECK(scene.life.starts(LifeGoal::look_at_player) == 0);
        CHECK(scene.life.starts(LifeGoal::look_around) > 0);
    }
    SUBCASE("7 blocks away") {
        PillarScene scene(3);
        scene.you.feet = ahead(7.0);
        for (int i = 0; i < 20'000; ++i) {
            static_cast<void>(scene.step());
        }
        CHECK(scene.life.starts(LifeGoal::look_at_player) == 0);
    }
}

TEST_CASE("sneaking hides you from more than 4.8 blocks away") {
    const auto looks = [](double distance) {
        PillarScene scene(4);
        scene.you = {.feet = ahead(distance), .eye_height = 1.27, .sneaking = true};
        for (int i = 0; i < 20'000; ++i) {
            static_cast<void>(scene.step());
        }
        return scene.life.starts(LifeGoal::look_at_player);
    };
    CHECK(looks(4.9) == 0);
    CHECK(looks(4.7) > 0);
}

TEST_CASE("a look goes on through a sneak or a wall and ends once you are farther than 6") {
    // The same seed reaches the same first look in each case.
    const auto first_look = [] {
        auto scene = std::make_unique<PillarScene>(9);
        scene->you.feet = five_ahead;
        while (!scene->life.running(LifeGoal::look_at_player)) {
            static_cast<void>(scene->step());
        }
        return scene;
    };
    SUBCASE("you sneak out of the 4.8 a start needs") {
        const auto scene = first_look();
        scene->you.eye_height = 1.27;
        scene->you.sneaking = true;
        for (int i = 0; i < 30; ++i) {
            static_cast<void>(scene->step());
        }
        CHECK(scene->life.running(LifeGoal::look_at_player));
    }
    SUBCASE("a wall comes between") {
        const auto scene = first_look();
        scene->world.boxes.push_back(
            {.min = {.x = -3.0, .y = -2.0, .z = -2.9}, .max = {.x = 3.0, .y = 3.0, .z = -2.1}});
        for (int i = 0; i < 30; ++i) {
            static_cast<void>(scene->step());
        }
        CHECK(scene->life.running(LifeGoal::look_at_player));
    }
    SUBCASE("you step back to 6.1 blocks") {
        const auto scene = first_look();
        scene->you.feet = ahead(6.1);
        static_cast<void>(scene->step());
        static_cast<void>(scene->step());
        CHECK_FALSE(scene->life.running(LifeGoal::look_at_player));
    }
}

TEST_CASE("he never looks around while he looks at you") {
    PillarScene scene(13);
    scene.you.feet = five_ahead;
    for (int i = 0; i < 20'000; ++i) {
        static_cast<void>(scene.step());
        CHECK_FALSE((scene.life.running(LifeGoal::look_at_player) &&
                     scene.life.running(LifeGoal::look_around)));
    }
    CHECK(scene.life.starts(LifeGoal::look_around) > 0);
}

TEST_CASE("his head turns at most 10 degrees a tick as you walk round him") {
    PillarScene scene(17);
    double fastest = 0.0;
    float last = scene.motion.head_yaw;
    for (int i = 0; i < 20'000; ++i) {
        const double angle = vx::test::radians(3.0 * i);
        scene.you.feet = {
            .x = 0.5 + (4.5 * std::sin(angle)), .y = 0.0, .z = 0.5 + (4.5 * std::cos(angle))};
        const vx::LifeTick ticked = scene.step();
        const double turn = std::abs(vx::wrap_turn(static_cast<double>(ticked.look_yaw - last)));
        fastest = std::max(fastest, turn);
        last = ticked.look_yaw;
    }
    CHECK(degrees(fastest) <= 10.0001);
    CHECK(degrees(fastest) > 9.99);
}
