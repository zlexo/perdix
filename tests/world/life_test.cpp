#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// RFC 0012's life, seeded and deterministic, in Minecraft 26.2's rhythm:
// the goals are chosen every second tick, and looking around is a chance
// of 2 in 100 at each choice when nothing else holds his eyes or legs, for
// 20 and up to 19 more ticks, counted every tick but ended only at a
// choice, so 22 to 40.

namespace {

using vx::LifeGoal;
using vx::Vec3d;
using vx::test::degrees;
using vx::test::Scene;
using vx::test::Spell;
using vx::test::Watch;

// You, far enough away that he never looks at you.
constexpr Vec3d far_away{.x = 0.0, .y = 0.0, .z = -30.0};

} // namespace

TEST_CASE("he looks around at 2 in 100 a choice at a level point a block off for 22 to 40 ticks") {
    Scene scene(21);
    scene.you.feet = far_away;
    Watch watch{.goal = LifeGoal::look_around};
    std::array<int, 4> quarters{};
    for (std::uint64_t tick = 0; tick < 60'000; ++tick) {
        watch.step(scene, tick);
        if (scene.life.running(LifeGoal::look_around)) {
            CHECK(scene.motion.head_pitch == 0.0F);
        }
        // Where a look around settles: its point's direction.
        if (!watch.spells.empty() && tick == watch.spells.back().start + 20) {
            const double yaw = degrees(static_cast<double>(scene.motion.head_yaw)) + 180.0;
            ++quarters.at(static_cast<std::size_t>(std::clamp(yaw, 0.0, 359.9) / 90.0));
        }
    }
    INFO(watch.tally.starts, " looks around in ", watch.tally.chances, " chances");
    CHECK(watch.tally.fits(0.02));
    watch.spells.pop_back();
    REQUIRE(watch.spells.size() > 100);
    std::uint64_t shortest = 1000;
    std::uint64_t longest = 0;
    for (const Spell& spell : watch.spells) {
        INFO("the look around from tick ", spell.start);
        CHECK(spell.length >= 22);
        CHECK(spell.length <= 40);
        shortest = std::min(shortest, spell.length);
        longest = std::max(longest, spell.length);
    }
    // The whole range: 20 and 39 ticks, run on to the next choice after.
    CHECK(shortest == 22);
    CHECK(longest == 40);
    for (const int quarter : quarters) {
        CHECK(quarter > 10);
    }
}

TEST_CASE("the same seed lives the same life and another seed another") {
    const auto run = [](std::uint64_t seed) {
        Scene scene(seed);
        scene.you.feet = far_away;
        std::vector<vx::LifeTick> ticks;
        for (int i = 0; i < 4'000; ++i) {
            ticks.push_back(scene.step());
        }
        return ticks;
    };
    const std::vector<vx::LifeTick> first = run(7);
    const std::vector<vx::LifeTick> again = run(7);
    const std::vector<vx::LifeTick> other = run(8);
    bool same = true;
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        same = same && first[i].look_yaw == again[i].look_yaw &&
               first[i].look_pitch == again[i].look_pitch &&
               first[i].input.yaw == again[i].input.yaw;
        differs = differs || first[i].look_yaw != other[i].look_yaw;
    }
    CHECK(same);
    CHECK(differs);
}
