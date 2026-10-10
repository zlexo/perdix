#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
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

// You, far enough away that he never looks at you, and that no spot within
// his reach is inside the leash: he never strolls.
constexpr Vec3d far_away{.x = 0.0, .y = 0.0, .z = -40.0};
// You, 7 blocks over where he starts: never seen, and every spot near him
// inside the leash.
constexpr Vec3d overhead{.x = 0.0, .y = 7.0, .z = 0.0};

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
        scene.you.feet = overhead;
        std::vector<std::pair<vx::LifeTick, Vec3d>> ticks;
        for (int i = 0; i < 4'000; ++i) {
            const vx::LifeTick ticked = scene.step();
            ticks.emplace_back(ticked, scene.perdix.position);
        }
        CHECK(scene.life.starts(LifeGoal::stroll) > 10);
        return ticks;
    };
    const auto first = run(7);
    const auto again = run(7);
    const auto other = run(8);
    bool same = true;
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        const auto& [ticked, position] = first[i];
        same = same && ticked.look_yaw == again[i].first.look_yaw &&
               ticked.look_pitch == again[i].first.look_pitch &&
               ticked.input.yaw == again[i].first.input.yaw &&
               ticked.input.held.forward == again[i].first.input.held.forward &&
               position == again[i].second;
        differs = differs || position != other[i].second;
    }
    CHECK(same);
    CHECK(differs);
}
