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

double degrees(float radians) {
    return static_cast<double>(radians) * 180.0 / pi;
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

TEST_CASE("walking forward from a body 40 degrees off closes 0.3 of the gap a tick") {
    for (const double off : {40.0, -40.0}) {
        INFO("the body ", off, " degrees off");
        Walker walker;
        walker.motion.body_yaw = static_cast<float>(radians(off));
        double expected = off;
        for (int i = 0; i < 10; ++i) {
            INFO("tick ", i + 1);
            walker.step(forward);
            expected *= 0.7;
            CHECK(degrees(walker.motion.body_yaw) == doctest::Approx(expected).epsilon(1e-5));
        }
    }
}

TEST_CASE("strafing turns the body toward the way moved until it is 50 degrees off the look") {
    // A, left: the move goes 90 degrees left of the look. The body turns 0.3
    // of the way to it each tick, 27 then 45.9 degrees, and the third tick's
    // 59.13 is held back to 50; D the same to the right.
    for (const bool left : {true, false}) {
        INFO((left ? "A" : "D"));
        Walker walker;
        const double side = left ? 1.0 : -1.0;
        const std::array<double, 3> first = {27.0, 45.9, 50.0};
        for (std::size_t i = 0; i < first.size(); ++i) {
            INFO("tick ", i + 1);
            walker.step({.left = left, .right = !left});
            CHECK(degrees(walker.motion.body_yaw) ==
                  doctest::Approx(side * first[i]).epsilon(1e-5));
        }
        for (int i = 3; i < 20; ++i) {
            walker.step({.left = left, .right = !left});
        }
        CHECK(degrees(walker.motion.body_yaw) == doctest::Approx(side * 50.0).epsilon(1e-5));
        CHECK(walker.motion.head_yaw == 0.0F);
    }
}

TEST_CASE("walking backward keeps the body on the look") {
    Walker walker;
    for (int i = 0; i < 20; ++i) {
        walker.step({.back = true});
        CHECK(walker.motion.body_yaw == 0.0F);
    }
    CHECK(walker.player.position.z > 2.0);
    // A body 30 degrees off eases back to the look, not around to face the
    // way it walks.
    walker.motion.body_yaw = static_cast<float>(radians(30.0));
    double expected = 30.0;
    for (int i = 0; i < 5; ++i) {
        walker.step({.back = true});
        expected *= 0.7;
        CHECK(degrees(walker.motion.body_yaw) == doctest::Approx(expected).epsilon(1e-5));
    }
}

TEST_CASE("standing while the look turns 70 degrees moves the body only by the 20 beyond 50") {
    SUBCASE("in one tick") {
        Walker walker;
        walker.step({}, static_cast<float>(radians(70.0)));
        CHECK(degrees(walker.motion.body_yaw) == doctest::Approx(20.0).epsilon(1e-5));
    }
    SUBCASE("ten degrees a tick either way") {
        for (const double side : {1.0, -1.0}) {
            Walker walker;
            for (int i = 1; i <= 7; ++i) {
                walker.step({}, static_cast<float>(radians(side * 10.0 * i)));
                const double held = std::max(0.0, (10.0 * i) - 50.0);
                CHECK(degrees(walker.motion.body_yaw) ==
                      doctest::Approx(side * held).epsilon(1e-5));
            }
            // Looking back does not bring the body back: it is within 50.
            walker.step({}, 0.0F);
            CHECK(degrees(walker.motion.body_yaw) == doctest::Approx(side * 20.0).epsilon(1e-5));
        }
    }
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

TEST_CASE("the body turns toward the way moved in the engine's yaw with positive to the left") {
    // Looking and walking down -x is yaw +90 degrees, a left turn from -z;
    // down +x is -90. The body starts down -z and turns 0.3 of the way, held
    // within 50 degrees of the look: 40, then 55 and 65.5 degrees.
    for (const double side : {1.0, -1.0}) {
        INFO("toward ", side > 0.0 ? "-x" : "+x");
        vx::BodyMotion motion = vx::body_facing(0.0F, 0.0F);
        const auto look = static_cast<float>(radians(side * 90.0));
        const Vec3d moved{.x = -side * 0.2, .y = 0.0, .z = 0.0};
        for (const double expected : {40.0, 55.0, 65.5}) {
            vx::animate(motion, moved, look, 0.0F);
            CHECK(degrees(motion.body_yaw) == doctest::Approx(side * expected).epsilon(1e-5));
        }
    }
    // Down +z, behind the start, is half a turn: the body comes round to it
    // the shorter way, whichever side it starts on.
    for (const double start : {170.0, -170.0}) {
        vx::BodyMotion motion = vx::body_facing(static_cast<float>(radians(start)), 0.0F);
        const auto look = static_cast<float>(pi);
        for (int i = 0; i < 40; ++i) {
            vx::animate(motion, {.x = 0.0, .y = 0.0, .z = 0.2}, look, 0.0F);
        }
        CHECK(std::abs(std::abs(degrees(motion.body_yaw)) - 180.0) < 1e-4);
    }
}

TEST_CASE("a move of 0.05 blocks or less leaves the body's facing alone") {
    // The body 30 degrees off the look, moving along the look: a move that
    // counts turns it to 21 degrees, one that does not leaves it at 30.
    const auto after_move = [](double distance) {
        vx::BodyMotion motion = vx::body_facing(0.0F, 0.0F);
        motion.body_yaw = static_cast<float>(radians(30.0));
        vx::animate(motion, {.x = 0.0, .y = 0.0, .z = -distance}, 0.0F, 0.0F);
        return degrees(motion.body_yaw);
    };
    CHECK(after_move(0.04) == doctest::Approx(30.0).epsilon(1e-5));
    CHECK(after_move(0.05) == doctest::Approx(30.0).epsilon(1e-5));
    CHECK(after_move(0.0501) == doctest::Approx(21.0).epsilon(1e-5));
    // Vertical motion does not count: a fall turns nothing.
    vx::BodyMotion falling = vx::body_facing(0.0F, 0.0F);
    falling.body_yaw = static_cast<float>(radians(30.0));
    vx::animate(falling, {.x = 0.0, .y = -3.0, .z = 0.01}, 0.0F, 0.0F);
    CHECK(degrees(falling.body_yaw) == doctest::Approx(30.0).epsilon(1e-5));
    CHECK(falling.limb_speed == doctest::Approx(0.4 * 0.04));
}

TEST_CASE("a move more than 95 degrees from the look either way counts as walking backward") {
    // From a body on the look, a move 94 degrees off is followed, 0.3 of the
    // way: 28.2 degrees toward it. At 96 the body turns toward the opposite
    // way instead, 84 degrees the other side of the look: 25.2 degrees away.
    // The same mirrored, 264 and 266 degrees round to the left.
    const auto after_move = [](double move_degrees) {
        vx::BodyMotion motion = vx::body_facing(0.0F, 0.0F);
        const double way = radians(move_degrees);
        vx::animate(motion, {.x = -0.2 * std::sin(way), .y = 0.0, .z = -0.2 * std::cos(way)}, 0.0F,
                    0.0F);
        return degrees(motion.body_yaw);
    };
    CHECK(after_move(94.0) == doctest::Approx(28.2).epsilon(1e-5));
    CHECK(after_move(96.0) == doctest::Approx(-25.2).epsilon(1e-5));
    CHECK(after_move(264.0) == doctest::Approx(25.2).epsilon(1e-5));
    CHECK(after_move(266.0) == doctest::Approx(-28.2).epsilon(1e-5));
    CHECK(after_move(180.0) == doctest::Approx(0.0));
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

TEST_CASE("between ticks the swing runs up to the tick's phase at the tick's speed") {
    const vx::BodyMotion previous{.limb_speed = 0.5, .limb_position = 1.0};
    const vx::BodyMotion current{.limb_speed = 0.6, .limb_position = 1.6};
    const vx::BodyAt start = vx::body_at(previous, current, 0.0);
    CHECK(start.limb_position == doctest::Approx(1.0));
    CHECK(start.limb_amount == doctest::Approx(0.5));
    const vx::BodyAt quarter = vx::body_at(previous, current, 0.25);
    CHECK(quarter.limb_position == doctest::Approx(1.15));
    CHECK(quarter.limb_amount == doctest::Approx(0.525));
    const vx::BodyAt end = vx::body_at(previous, current, 1.0);
    CHECK(end.limb_position == doctest::Approx(1.6));
    CHECK(end.limb_amount == doctest::Approx(0.6));
    // The amount is at most 1, whatever the speeds.
    const vx::BodyMotion faster{.limb_speed = 1.4, .limb_position = 3.0};
    CHECK(vx::body_at(current, faster, 0.75).limb_amount == 1.0);
}

TEST_CASE("between ticks the yaws turn the shorter way round and the pitch is blended") {
    const auto at = [](double angle) { return static_cast<float>(radians(angle)); };
    const vx::BodyMotion previous{
        .body_yaw = at(170.0), .head_yaw = at(-170.0), .head_pitch = 0.2F};
    const vx::BodyMotion current{
        .body_yaw = at(-170.0), .head_yaw = at(170.0), .head_pitch = -0.4F};
    const vx::BodyAt quarter = vx::body_at(previous, current, 0.25);
    CHECK(degrees(quarter.body_yaw) == doctest::Approx(175.0).epsilon(1e-5));
    CHECK(degrees(quarter.head_yaw) == doctest::Approx(-175.0).epsilon(1e-5));
    CHECK(quarter.head_pitch == doctest::Approx(0.05));
    const vx::BodyAt three_quarters = vx::body_at(previous, current, 0.75);
    CHECK(degrees(three_quarters.body_yaw) == doctest::Approx(-175.0).epsilon(1e-5));
    CHECK(degrees(three_quarters.head_yaw) == doctest::Approx(175.0).epsilon(1e-5));
    CHECK(three_quarters.head_pitch == doctest::Approx(-0.25));
}

TEST_CASE("between two walking ticks the body runs from one tick's pose to the next") {
    Walker walker;
    walker.motion.body_yaw = static_cast<float>(radians(40.0));
    for (int i = 0; i < 3; ++i) {
        walker.step(forward);
    }
    const vx::BodyMotion previous = walker.motion;
    walker.step(forward, static_cast<float>(radians(10.0)));
    const vx::BodyMotion& current = walker.motion;
    const vx::BodyAt start = vx::body_at(previous, current, 0.0);
    CHECK(start.limb_position == doctest::Approx(previous.limb_position));
    CHECK(start.limb_amount == doctest::Approx(previous.limb_speed));
    CHECK(start.body_yaw == doctest::Approx(previous.body_yaw));
    CHECK(start.head_yaw == doctest::Approx(previous.head_yaw));
    const vx::BodyAt end = vx::body_at(previous, current, 1.0);
    CHECK(end.limb_position == doctest::Approx(current.limb_position));
    CHECK(end.limb_amount == doctest::Approx(current.limb_speed));
    CHECK(end.body_yaw == doctest::Approx(current.body_yaw));
    CHECK(end.head_yaw == doctest::Approx(current.head_yaw));
}
