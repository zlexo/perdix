#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/body_motion.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

// RFC 0012's swing and body, driven by RFC 0010's movement over flat ground,
// so every move is one the tick really makes. The expected values are worked
// out from the RFC's rules and RFC 0010's walking vectors, not from the
// code. As in the movement tests, the player looks down -z (yaw 0) unless a
// case turns it.

namespace {

using vx::Vec3d;

constexpr double pi = std::numbers::pi;

constexpr double radians(double degrees) {
    return degrees * pi / 180.0;
}

// A player on the flat world with a body: each step runs RFC 0010's tick,
// then animates the body by the move the tick made.
struct Walker {
    vx::test::FlatWorld world;
    vx::MovementNumbers numbers;
    vx::Player player = vx::test::at_rest();
    vx::BodyMotion motion = vx::body_facing(0.0F, 0.0F);
    std::vector<vx::Box> scratch;

    void step(const vx::MovementKeys& keys, float yaw = 0.0F) {
        const Vec3d before = player.position;
        vx::tick(player, {.held = keys, .yaw = yaw}, world, numbers, scratch);
        vx::animate(motion, player.position - before, yaw, 0.0F);
    }
};

constexpr vx::MovementKeys forward{.forward = true};

// The swing's speed after each of the first 12 ticks from rest, from RFC
// 0010's moves for those ticks: the speed eases 0.4 of the way to four
// times the move, at most 1.
constexpr std::array<double, 12> walking_speeds = {0.156800, 0.336493, 0.491052, 0.609311,
                                                   0.694203, 0.752746, 0.792025, 0.817861,
                                                   0.834602, 0.845323, 0.852125, 0.856408};
constexpr std::array<double, 12> sprinting_speeds = {0.203840, 0.437440, 0.638368, 0.783021,
                                                     0.869812, 0.921887, 0.953132, 0.971879,
                                                     0.983128, 0.989877, 0.993926, 0.996356};
constexpr std::array<double, 12> sneaking_speeds = {0.047040, 0.100947, 0.147316, 0.182793,
                                                    0.208260, 0.225823, 0.237607, 0.245358,
                                                    0.250380, 0.253596, 0.255636, 0.256921};

struct Gait {
    const char* name;
    vx::MovementKeys keys;
    bool sprinting;
    bool crouching;
    std::array<double, 12> speeds;
    double position_after_12; // the speeds added up
    double settled;           // four times the gait's top speed, at most 1
};

} // namespace

TEST_CASE("the walk swing eases toward 0.863 walking and 1 sprinting and 0.259 sneaking") {
    const std::array<Gait, 3> gaits = {
        Gait{.name = "walking",
             .keys = forward,
             .sprinting = false,
             .crouching = false,
             .speeds = walking_speeds,
             .position_after_12 = 8.038949,
             .settled = 4.0 * 0.215859},
        Gait{.name = "sprinting",
             .keys = forward,
             .sprinting = true,
             .crouching = false,
             .speeds = sprinting_speeds,
             .position_after_12 = 9.742667,
             .settled = 1.0},
        Gait{.name = "sneaking",
             .keys = {.forward = true, .sneak = true},
             .sprinting = false,
             .crouching = true,
             .speeds = sneaking_speeds,
             .position_after_12 = 2.411675,
             .settled = 4.0 * 0.064758},
    };
    for (const Gait& gait : gaits) {
        INFO(gait.name);
        Walker walker;
        walker.player.sprinting = gait.sprinting;
        walker.player.pose = gait.crouching ? vx::Pose::crouching : vx::Pose::standing;
        for (std::size_t i = 0; i < gait.speeds.size(); ++i) {
            INFO("tick ", i + 1);
            walker.step(gait.keys);
            CHECK(walker.motion.limb_speed == doctest::Approx(gait.speeds[i]).epsilon(1e-5));
        }
        CHECK(walker.motion.limb_position == doctest::Approx(gait.position_after_12).epsilon(1e-5));
        // Twenty ticks in, the speed is within a hundredth of where it
        // settles; a hundred in, it is there.
        for (int i = 12; i < 20; ++i) {
            walker.step(gait.keys);
        }
        CHECK(std::abs(walker.motion.limb_speed - gait.settled) < 0.01);
        for (int i = 20; i < 100; ++i) {
            walker.step(gait.keys);
        }
        CHECK(walker.motion.limb_speed == doctest::Approx(gait.settled).epsilon(1e-5));
    }
}

TEST_CASE("walking into a wall swings nothing and turns nothing") {
    // A wall against the front of the player's box: W moves it nowhere.
    Walker walker;
    walker.world.boxes.push_back(
        {.min = {.x = -2.0, .y = 0.0, .z = -1.3}, .max = {.x = 3.0, .y = 2.0, .z = -0.3}});
    walker.motion.body_yaw = static_cast<float>(radians(30.0));
    for (int i = 0; i < 20; ++i) {
        walker.step(forward);
    }
    CHECK(walker.player.position == Vec3d{});
    CHECK(walker.motion.limb_speed == 0.0);
    CHECK(walker.motion.limb_position == 0.0);
    CHECK(walker.motion.body_yaw == static_cast<float>(radians(30.0)));
}

TEST_CASE("the head takes the tick's look") {
    vx::BodyMotion motion = vx::body_facing(0.5F, -0.25F);
    CHECK(motion.head_yaw == 0.5F);
    CHECK(motion.head_pitch == -0.25F);
    CHECK(motion.body_yaw == 0.5F);
    CHECK(motion.limb_speed == 0.0);
    CHECK(motion.limb_position == 0.0);
    vx::animate(motion, {}, -1.0F, 0.75F);
    CHECK(motion.head_yaw == -1.0F);
    CHECK(motion.head_pitch == 0.75F);
}

TEST_CASE("a move or a look that is not finite leaves the body as it was") {
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr float nanf = std::numeric_limits<float>::quiet_NaN();
    vx::BodyMotion motion = vx::body_facing(0.25F, 0.0F);
    vx::animate(motion, {.x = 0.1, .y = 0.0, .z = 0.0}, 0.25F, 0.0F);
    const vx::BodyMotion before = motion;
    vx::animate(motion, {.x = nan, .y = 0.0, .z = 0.0}, 0.25F, 0.0F);
    vx::animate(motion, {.x = 0.1, .y = 0.0, .z = 0.0}, nanf, 0.0F);
    vx::animate(motion, {.x = 0.1, .y = 0.0, .z = 0.0}, 0.25F, nanf);
    CHECK(motion.limb_speed == before.limb_speed);
    CHECK(motion.limb_position == before.limb_position);
    CHECK(motion.body_yaw == before.body_yaw);
    CHECK(motion.head_yaw == before.head_yaw);
    CHECK(motion.head_pitch == before.head_pitch);
}
