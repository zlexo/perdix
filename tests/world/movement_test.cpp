#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <vector>

// RFC 0010's vectors: what the game's formulas give, tick by tick, which
// reproduce the speeds the wiki publishes. They are checked to the RFC's
// 1e-5 blocks, which doctest's Approx scales by 1 plus the value: up to
// 1.7e-5 for a tick's move. The game rounds some numbers through floats. The
// vectors are written for a player walking toward +z; here the player faces
// -z (yaw 0, where the sine and cosine are exact), so z runs negative.

namespace {

using vx::Box;
using vx::Vec3d;
using vx::test::at_rest;
using vx::test::block_box;

constexpr double tolerance = 1e-5;
constexpr std::size_t vector_ticks = 12;

// Runs `ticks` ticks of `keys` and returns how far each moved along -z.
std::vector<double> run_along_z(vx::Player& player, const vx::MovementKeys& keys, std::size_t ticks,
                                const vx::BlockBoxes& world) {
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    std::vector<double> moved;
    for (std::size_t i = 0; i < ticks; ++i) {
        const double before = player.position.z;
        vx::tick(player, {.held = keys, .yaw = 0.0F}, world, numbers, scratch);
        moved.push_back(before - player.position.z);
    }
    return moved;
}

void check_vector(const std::vector<double>& moved, const std::array<double, vector_ticks>& game) {
    REQUIRE(moved.size() >= game.size());
    for (std::size_t i = 0; i < game.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(moved[i] == doctest::Approx(game[i]).epsilon(tolerance));
    }
}

constexpr vx::MovementKeys forward{.forward = true};

} // namespace

TEST_CASE(
    "walking from rest matches the game tick by tick, and tops out at 4.317 blocks a second") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    const std::vector<double> moved = run_along_z(player, forward, 100, world);
    check_vector(moved, {0.098000, 0.151508, 0.180723, 0.196675, 0.205385, 0.210140, 0.212736,
                         0.214154, 0.214928, 0.215351, 0.215582, 0.215708});
    CHECK(moved.back() == doctest::Approx(0.215859).epsilon(tolerance));
    CHECK(player.on_ground);
    CHECK(player.position.y == 0.0);
}

TEST_CASE("sprinting from rest tops out at 5.612 blocks a second") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.sprinting = true;
    const std::vector<double> moved = run_along_z(player, forward, 100, world);
    check_vector(moved, {0.127400, 0.196960, 0.234940, 0.255677, 0.267000, 0.273182, 0.276557,
                         0.278400, 0.279407, 0.279956, 0.280256, 0.280420});
    CHECK(moved.back() == doctest::Approx(0.280617).epsilon(tolerance));
}

TEST_CASE("sneaking from rest tops out at 1.295 blocks a second") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    player.pose = vx::Pose::crouching;
    const std::vector<double> moved = run_along_z(player, forward, 100, world);
    check_vector(moved, {0.029400, 0.045452, 0.054217, 0.059002, 0.061615, 0.063042, 0.063821,
                         0.064246, 0.064478, 0.064605, 0.064674, 0.064712});
    CHECK(moved.back() == doctest::Approx(0.064758).epsilon(tolerance));
}

TEST_CASE("a diagonal is 2% faster walking and 41% faster sneaking, as in the game") {
    // The keys' vector is cut to length 1 only when it is longer: W with A
    // is 0.98 times root 2, cut to 1; sneaking, 0.294 times root 2 is kept.
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    const auto top_speed = [&](vx::Pose pose) {
        vx::Player player = at_rest();
        player.pose = pose;
        std::vector<Box> scratch;
        Vec3d before;
        for (int i = 0; i < 100; ++i) {
            before = player.position;
            vx::tick(player, {.held = {.forward = true, .left = true}, .yaw = 0.0F}, world, numbers,
                     scratch);
        }
        const Vec3d last = player.position - before;
        return std::hypot(last.x, last.z);
    };
    CHECK(top_speed(vx::Pose::standing) == doctest::Approx(0.220264).epsilon(tolerance));
    CHECK(top_speed(vx::Pose::crouching) == doctest::Approx(0.091581).epsilon(tolerance));
}

TEST_CASE("releasing W slides on for 7 ticks, then stops") {
    const vx::test::FlatWorld world;
    for (const bool sprinting : {false, true}) {
        INFO("sprinting ", sprinting);
        vx::Player player = at_rest();
        player.sprinting = sprinting;
        static_cast<void>(run_along_z(player, forward, 100, world));
        const std::vector<double> slide = run_along_z(player, {}, 9, world);
        const std::array<double, 8> game = sprinting
                                               ? std::array{0.153217, 0.083656, 0.045676, 0.024939,
                                                            0.013617, 0.007435, 0.004059, 0.0}
                                               : std::array{0.117859, 0.064351, 0.035136, 0.019184,
                                                            0.010475, 0.005719, 0.003123, 0.0};
        for (std::size_t i = 0; i < game.size(); ++i) {
            INFO("tick ", i + 1);
            CHECK(slide[i] == doctest::Approx(game[i]).epsilon(tolerance));
        }
        CHECK(slide[8] == 0.0);
    }
}

TEST_CASE("the momentum threshold takes the horizontal speed as a whole, since 1.21.5") {
    // Walking 3 degrees off an axis, the sideways part of the speed is under
    // 0.003 on its own, but the speed is not: 26.2 keeps it, where earlier
    // versions zeroed each axis alone and bent the path toward the axis. The
    // values are the game's, whose sine comes from a table; the exact sine
    // here moves them by 2.3e-6 at most, inside the tolerance.
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    const float three_degrees = static_cast<float>(3.0 * std::numbers::pi / 180.0);
    vx::Player player = at_rest();
    std::vector<Box> scratch;
    std::vector<double> sideways;
    const auto step = [&](const vx::MovementKeys& keys) {
        const double before = player.position.x;
        vx::tick(player, {.held = keys, .yaw = three_degrees}, world, numbers, scratch);
        sideways.push_back(before - player.position.x);
    };
    for (std::size_t i = 0; i < vector_ticks; ++i) {
        step(forward);
    }
    CHECK(sideways[0] == doctest::Approx(0.005128).epsilon(tolerance));
    CHECK(sideways[1] == doctest::Approx(0.007927).epsilon(tolerance));
    CHECK(sideways[11] == doctest::Approx(0.011287).epsilon(tolerance));
    // Released, the sideways speed decays with the rest for 7 ticks.
    for (int i = 0; i < 8; ++i) {
        step({});
    }
    const std::array<double, 8> decay = {0.006162, 0.003365, 0.001837, 0.001003,
                                         0.000548, 0.000299, 0.000163, 0.0};
    for (std::size_t i = 0; i < decay.size(); ++i) {
        INFO("tick ", i + 1, " after release");
        CHECK(sideways[vector_ticks + i] == doctest::Approx(decay[i]).epsilon(tolerance));
    }
}

TEST_CASE("standing still, the box stays on the ground with gravity's step carried") {
    const vx::test::FlatWorld world;
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, {}, 10, world));
    CHECK(player.position == Vec3d{});
    CHECK(player.on_ground);
    CHECK(player.velocity.y == doctest::Approx(-0.0784));
}

TEST_CASE("falling approaches 3.92 blocks a tick, 78.4 a second") {
    const vx::test::FlatWorld world;
    vx::Player player;
    player.position.y = 10'000.0;
    static_cast<void>(run_along_z(player, {}, 400, world));
    CHECK_FALSE(player.on_ground);
    CHECK(player.velocity.y == doctest::Approx(-3.92).epsilon(1e-3));
}

TEST_CASE("a wall stops only the part of the velocity it cut short") {
    // Walking diagonally into a wall across z: z stops, x carries on.
    vx::test::FlatWorld world;
    for (int x = -20; x < 4; ++x) {
        world.boxes.push_back(block_box(x, 0.0, -2.0));
        world.boxes.push_back(block_box(x, 1.0, -2.0));
    }
    const vx::MovementNumbers numbers;
    vx::Player player = at_rest();
    player.position = {.x = 0.5, .y = 0.0, .z = 0.5};
    std::vector<Box> scratch;
    for (int i = 0; i < 20; ++i) {
        vx::tick(player, {.held = {.forward = true, .left = true}, .yaw = 0.0F}, world, numbers,
                 scratch);
    }
    CHECK(player.position.z == doctest::Approx(-0.7));
    CHECK(player.velocity.z == 0.0);
    CHECK(player.velocity.x < 0.0);
    CHECK(player.horizontal_collision);
}

TEST_CASE("a tick gathers into the caller's scratch, and keeps its memory once it has room") {
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    vx::Player player = at_rest();
    std::vector<Box> scratch;
    scratch.reserve(64);
    const Box* const memory = scratch.data();
    for (int i = 0; i < 50; ++i) {
        vx::tick(player, {.held = forward, .yaw = 0.0F}, world, numbers, scratch);
    }
    CHECK_FALSE(scratch.empty());
    CHECK(scratch.data() == memory);
}

TEST_CASE("a yaw that is not finite leaves the player as it was, as a move that is not finite "
          "does") {
    // Even with no key held, a NaN facing times a 0 input is NaN: one such
    // tick would leave the velocity NaN for good, and every later move would
    // do nothing.
    const vx::test::FlatWorld world;
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    for (const float yaw :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        INFO("yaw ", yaw);
        vx::Player player = at_rest();
        vx::tick(player, {.held = forward, .yaw = yaw}, world, numbers, scratch);
        CHECK(player.position == Vec3d{});
        CHECK(player.velocity == Vec3d{.x = 0.0, .y = -0.0784, .z = 0.0});
        CHECK(player.on_ground);
        // And the next tick walks as from rest.
        vx::tick(player, {.held = forward, .yaw = 0.0F}, world, numbers, scratch);
        CHECK(player.position.z == doctest::Approx(-0.098));
    }
}
