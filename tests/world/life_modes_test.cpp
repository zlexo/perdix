#include "life_scene.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>

// RFC 0012's other modes, seeded and deterministic. Follow, as a tamed wolf
// in 26.2 (FollowOwnerGoal, the wolf's numbers): after you from 10 blocks
// away or more until within 2, aiming again at you every 10 ticks, his eyes
// on you; sprinting beyond 16 blocks where the wolf would teleport. Still:
// he stands, his eyes as in life. And in life, beyond 48 blocks he walks
// back to you until within 24.

namespace {

using vx::LifeGoal;
using vx::LifeMode;
using vx::Vec3d;
using vx::test::Scene;

double distance(Vec3d a, Vec3d b) {
    const Vec3d d = b - a;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// A scene in follow with you `away` blocks down -z.
Scene following(double away, std::uint64_t seed = 1) {
    Scene scene(seed);
    scene.life.set_mode(LifeMode::follow);
    scene.you.feet = {.x = 0.0, .y = 0.0, .z = -away};
    return scene;
}

} // namespace

TEST_CASE("in follow he starts after you from 10 blocks away and not from 9.9") {
    Scene near = following(9.9);
    static_cast<void>(near.step());
    CHECK_FALSE(near.life.running(LifeGoal::follow));
    Scene far = following(10.0);
    static_cast<void>(far.step());
    CHECK(far.life.running(LifeGoal::follow));
}

TEST_CASE("following he walks at you with his eyes on you and stops within 2 blocks") {
    Scene scene = following(14.0);
    int ticks = 0;
    for (; ticks < 400 && (ticks == 0 || scene.life.running(LifeGoal::follow)); ++ticks) {
        const vx::LifeTick ticked = scene.step();
        if (scene.life.running(LifeGoal::follow)) {
            CHECK(ticked.input.held.forward);
            CHECK_FALSE(ticked.input.held.sprint);
        }
    }
    CHECK(ticks < 400);
    // Seen within 2 at a choice, he lets go; he walked on up to 2 ticks
    // since the last choice and slides a little.
    const double left = distance(scene.perdix.position, scene.you.feet);
    CHECK(left <= 2.0);
    CHECK(left > 1.2);
    CHECK(std::abs(scene.motion.head_yaw) < 1e-4F);
}

TEST_CASE("following from beyond 16 blocks he sprints") {
    Scene scene = following(30.0);
    bool sprinted = false;
    for (int i = 0; i < 20; ++i) {
        const vx::LifeTick ticked = scene.step();
        CHECK(ticked.input.held.sprint);
        sprinted = sprinted || scene.perdix.sprinting;
    }
    CHECK(sprinted);
    Scene walking = following(15.0);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(walking.step().input.held.sprint);
    }
    // From farther than the leash's 48, it is still following, not the
    // life's going back.
    Scene far = following(60.0);
    static_cast<void>(far.step());
    CHECK(far.life.running(LifeGoal::follow));
    CHECK_FALSE(far.life.running(LifeGoal::go_back));
}

TEST_CASE("following he aims again at where you are every 10 ticks") {
    // Still for 101 ticks, past the life's first choices; then following
    // you 40 blocks off, walking sideways, so every new aim differs.
    Scene scene = following(5.0);
    scene.life.set_mode(LifeMode::still);
    for (int i = 0; i < 101; ++i) {
        static_cast<void>(scene.step());
    }
    scene.life.set_mode(LifeMode::follow);
    scene.you.feet = {.x = 0.0, .y = 0.0, .z = -40.0};
    while (!scene.life.running(LifeGoal::follow)) {
        static_cast<void>(scene.step());
    }
    std::optional<Vec3d> aim = scene.life.walk_target();
    int changes = 0;
    for (int tick = 1; tick <= 60; ++tick) {
        scene.you.feet.x += 0.1;
        static_cast<void>(scene.step());
        const std::optional<Vec3d> now = scene.life.walk_target();
        if (aim != now) {
            CHECK(tick % 10 == 0);
            ++changes;
        }
        aim = now;
    }
    CHECK(changes == 6);
}

TEST_CASE("in follow near you he lives his life and strolls and looks and comes back") {
    Scene scene = following(5.0);
    for (int i = 0; i < 20'000; ++i) {
        static_cast<void>(scene.step());
        CHECK(distance(scene.perdix.position, scene.you.feet) < 16.0);
    }
    CHECK(scene.life.starts(LifeGoal::stroll) > 50);
    CHECK(scene.life.starts(LifeGoal::look_at_player) > 20);
    CHECK(scene.life.starts(LifeGoal::follow) > 0);
}

TEST_CASE("in still he never moves and his eyes still follow you") {
    Scene scene(2);
    scene.life.set_mode(LifeMode::still);
    scene.you.feet = {.x = 0.0, .y = 0.0, .z = -5.0};
    for (int i = 0; i < 20'000; ++i) {
        // You come and go, near and far.
        scene.you.feet.z = (i / 2'000) % 2 == 0 ? -5.0 : -30.0;
        const vx::LifeTick ticked = scene.step();
        CHECK_FALSE(ticked.input.held.forward);
        CHECK(scene.perdix.position == Vec3d{});
    }
    CHECK(scene.life.starts(LifeGoal::look_at_player) > 20);
    CHECK(scene.life.starts(LifeGoal::look_around) == 0);
    CHECK(scene.life.starts(LifeGoal::stroll) == 0);
    CHECK(scene.life.starts(LifeGoal::follow) == 0);
}

TEST_CASE("a mode set mid-stroll stops the stroll at once") {
    Scene scene(3);
    scene.you.feet = {.x = 0.0, .y = 7.0, .z = 0.0};
    while (!scene.life.walk_target()) {
        static_cast<void>(scene.step());
    }
    scene.life.set_mode(LifeMode::still);
    CHECK_FALSE(scene.life.running(LifeGoal::stroll));
    CHECK_FALSE(scene.life.walk_target());
    CHECK_FALSE(scene.step().input.held.forward);
    CHECK(scene.life.mode() == LifeMode::still);
}

TEST_CASE("put elsewhere mid-stroll he starts over, his head where he now faces") {
    Scene scene(3);
    scene.you.feet = {.x = 0.0, .y = 7.0, .z = 0.0};
    while (!scene.life.walk_target()) {
        static_cast<void>(scene.step());
    }
    // As the app brings him: his body put down 5 blocks away, turned about.
    const auto facing = static_cast<float>(
        vx::wrap_turn(static_cast<double>(scene.motion.head_yaw) + vx::test::pi));
    scene.perdix.position = {.x = 5.0, .y = 0.0, .z = 5.0};
    scene.motion = vx::body_facing(facing, 0.0F);
    scene.life.start_over(facing);
    CHECK_FALSE(scene.life.walk_target());
    for (const LifeGoal goal : {LifeGoal::go_back, LifeGoal::follow, LifeGoal::stroll,
                                LifeGoal::look_at_player, LifeGoal::look_around}) {
        CHECK_FALSE(scene.life.running(goal));
    }
    // His head turns from there, at most 10 degrees a tick, not from where
    // it looked before.
    const vx::LifeTick next = scene.step();
    CHECK(std::abs(vx::wrap_turn(static_cast<double>(next.look_yaw - facing))) <=
          vx::test::radians(10.0) + 1e-5);
    // No walk began, so the keys push the way he faces.
    REQUIRE_FALSE(scene.life.walk_target());
    CHECK(next.input.yaw == facing);
    CHECK_FALSE(next.input.held.jump);
    CHECK(scene.life.mode() == LifeMode::life);
}

TEST_CASE("in life beyond 48 blocks he walks back to you until within 24") {
    for (const double away : {47.0, 49.0}) {
        INFO(away, " blocks away");
        Scene scene(4);
        scene.you.feet = {.x = 0.0, .y = 0.0, .z = -away};
        static_cast<void>(scene.step());
        CHECK(scene.life.running(LifeGoal::go_back) == (away > 48.0));
    }
    Scene scene(4);
    scene.you.feet = {.x = 0.0, .y = 0.0, .z = -60.0};
    int ticks = 0;
    for (; ticks < 1'000 && (ticks == 0 || scene.life.running(LifeGoal::go_back)); ++ticks) {
        const vx::LifeTick ticked = scene.step();
        if (scene.life.running(LifeGoal::go_back)) {
            CHECK(ticked.input.held.forward);
            CHECK(ticked.input.held.sprint);
        }
    }
    CHECK(ticks < 1'000);
    const double left = distance(scene.perdix.position, scene.you.feet);
    CHECK(left <= 24.0);
    CHECK(left > 22.0);
    CHECK(scene.life.starts(LifeGoal::stroll) == 0);
}
