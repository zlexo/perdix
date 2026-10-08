#include "box_world.hpp"

#include <vx/core/vec3d.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_physics.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_boxes.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/movement.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <utility>
#include <vector>

// RFC 0010's blocks: slipperiness from the block that supports the player,
// and collision as each block's file says. The player faces -z. The ice
// vectors come from RFC 0010's formulas with a slipperiness of 0.98: an
// acceleration of 0.1 * (0.6 / 0.98) cubed and a drag of 0.98 * 0.91.

namespace {

using vx::Box;
using vx::test::at_rest;

constexpr double tolerance = 1e-5;

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

constexpr vx::MovementKeys forward{.forward = true};

// A test pack: ice, a poppy that collides with nothing, and stone.
vx::BlockRegistry test_pack() {
    vx::BlockLoad load = vx::BlockRegistry::load(std::vector<vx::SourceFile>{
        {.path = "assets/core/blocks/ice.toml",
         .text = "slipperiness = 0.98\n[textures]\nall = \"ice\"\n"},
        {.path = "assets/core/blocks/poppy.toml",
         .text = "collision = \"none\"\nopacity = \"cutout\"\n[textures]\nall = \"poppy\"\n"},
        {.path = "assets/core/blocks/stone.toml", .text = "[textures]\nall = \"stone\"\n"}});
    REQUIRE(load.registry.has_value());
    return *std::move(load.registry);
}

vx::BlockState state_of(const vx::BlockRegistry& registry, const char* key) {
    const std::optional<vx::BlockState> state = registry.find(key);
    REQUIRE(state.has_value());
    return *state;
}

} // namespace

TEST_CASE("on ice, W from rest gathers speed slowly, toward 4.157 blocks a second") {
    vx::test::FlatWorld world;
    world.floor_slipperiness = 0.98;
    vx::Player player = at_rest();
    const std::vector<double> moved = run_along_z(player, forward, 400, world);
    constexpr std::array<double, 5> game = {0.022491, 0.042548, 0.060435, 0.076386, 0.090612};
    for (std::size_t i = 0; i < game.size(); ++i) {
        INFO("tick ", i + 1);
        CHECK(moved[i] == doctest::Approx(game[i]).epsilon(tolerance));
    }
    CHECK(moved.back() == doctest::Approx(0.207862).epsilon(tolerance));
}

TEST_CASE("released on ice, the player slides 1.688 blocks over 37 ticks, where stone stops in 7") {
    vx::test::FlatWorld world;
    world.floor_slipperiness = 0.98;
    vx::Player player = at_rest();
    static_cast<void>(run_along_z(player, forward, 400, world));
    const std::vector<double> slide = run_along_z(player, {}, 40, world);
    CHECK(slide[0] == doctest::Approx(0.185371).epsilon(tolerance));
    CHECK(slide[1] == doctest::Approx(0.165314).epsilon(tolerance));
    CHECK(slide[2] == doctest::Approx(0.147427).epsilon(tolerance));
    double total = 0.0;
    for (const double step : slide) {
        total += step;
    }
    CHECK(total == doctest::Approx(1.688468).epsilon(tolerance));
    CHECK(slide[36] > 0.0);
    CHECK(slide[37] == 0.0);
}

TEST_CASE("a test pack's ice slides as RFC 0010 says, through the registry and a chunk") {
    const vx::BlockRegistry registry = test_pack();
    vx::ChunkStorage chunk;
    const vx::BlockState stone = state_of(registry, "core:stone");
    const vx::BlockState ice = state_of(registry, "core:ice");
    // A floor of stone at y = 0, with a column of ice at x = 1 down its
    // length: x from 0 to 1 is stone, from 1 to 2 ice.
    for (int x = 0; x < vx::chunk_size; ++x) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            chunk.set(vx::local_index(x, 0, z), x == 1 ? ice : stone);
        }
    }
    const vx::ChunkBoxes blocks(chunk, {}, registry.physics());
    CHECK(blocks.slipperiness({.x = 1, .y = 0, .z = 5}) == 0.98);
    CHECK(blocks.slipperiness({.x = 0, .y = 0, .z = 5}) == 0.6);
    // Outside the chunk, as for air.
    CHECK(blocks.slipperiness({.x = 1, .y = -1, .z = 5}) == 0.6);

    // The block that supports the player, chosen after each move on the
    // ground, is the one whose center is nearest its position, the greater
    // x on a tie (26.2's, from its jar): after a tick standing over both,
    // W's first step is stone's 0.098 or ice's 0.022491.
    struct Case {
        double x;
        double step;
    };
    for (const Case& game : {Case{.x = 0.9, .step = 0.098}, Case{.x = 1.0, .step = 0.022491},
                             Case{.x = 1.1, .step = 0.022491}, Case{.x = 1.5, .step = 0.022491}}) {
        INFO("x ", game.x);
        vx::Player player = at_rest();
        player.position = {.x = game.x, .y = 1.0, .z = 20.5};
        static_cast<void>(run_along_z(player, {}, 1, blocks));
        const std::vector<double> moved = run_along_z(player, forward, 1, blocks);
        CHECK(moved[0] == doctest::Approx(game.step).epsilon(tolerance));
    }
}

TEST_CASE("a block that collides with nothing gathers no box, and the player walks through it") {
    const vx::BlockRegistry registry = test_pack();
    vx::ChunkStorage chunk;
    const vx::BlockState stone = state_of(registry, "core:stone");
    for (int x = 0; x < vx::chunk_size; ++x) {
        for (int z = 0; z < vx::chunk_size; ++z) {
            chunk.set(vx::local_index(x, 0, z), stone);
        }
    }
    // A wall of poppies across the player's way, two high.
    for (int x = 0; x < vx::chunk_size; ++x) {
        chunk.set(vx::local_index(x, 1, 10), state_of(registry, "core:poppy"));
        chunk.set(vx::local_index(x, 2, 10), state_of(registry, "core:poppy"));
    }
    const vx::ChunkBoxes blocks(chunk, {}, registry.physics());
    std::vector<Box> out;
    // Inside the two poppies' cells, clear of the floor: nothing to gather.
    blocks.collect({.min = {.x = 4.1, .y = 1.1, .z = 10.1}, .max = {.x = 4.9, .y = 2.9, .z = 10.9}},
                   out);
    CHECK(out.empty());
    vx::Player player = at_rest();
    player.position = {.x = 4.5, .y = 1.0, .z = 14.5};
    static_cast<void>(run_along_z(player, forward, 40, blocks));
    CHECK(player.position.z < 8.0);
    CHECK_FALSE(player.horizontal_collision);
}
