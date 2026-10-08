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
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <map>
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

namespace {

// Loose boxes, each standing for a block with a slipperiness, for the rules
// a chunk's full cubes cannot show: slabs, taller boxes, blocks at different
// heights. Blocks it gives no slipperiness are 0.6.
class CellWorld final : public vx::BlockBoxes {
public:
    std::vector<Box> boxes;
    std::map<std::array<int, 3>, double> slipperiness_of;

    // A box from the bottom of block (x, y, z) up to `height`, and that
    // block's slipperiness.
    void add(int x, int y, int z, double slipperiness, double height = 1.0) {
        const vx::Vec3d low{
            .x = static_cast<double>(x), .y = static_cast<double>(y), .z = static_cast<double>(z)};
        boxes.push_back({.min = low, .max = low + vx::Vec3d{.x = 1.0, .y = height, .z = 1.0}});
        slipperiness_of[{x, y, z}] = slipperiness;
    }

    [[nodiscard]] double slipperiness(vx::BlockPos block) const override {
        const auto found = slipperiness_of.find({block.x, block.y, block.z});
        return found == slipperiness_of.end() ? 0.6 : found->second;
    }

    void collect(const Box& region, std::vector<Box>& out) const override {
        for (const Box& box : boxes) {
            if (box.max.x >= region.min.x && box.min.x <= region.max.x &&
                box.max.y >= region.min.y && box.min.y <= region.max.y &&
                box.max.z >= region.min.z && box.min.z <= region.max.z) {
                out.push_back(box);
            }
        }
    }
};

// W's first step from rest: default ground's 0.098, or ice's 0.022491.
constexpr double default_step = 0.098;
constexpr double ice_step = 0.022491;

// W's first step from rest at `feet`: after a tick standing there, so a
// block supports the player, as one that walked there would have; or, not
// `settled`, on the player's very first tick, with no block chosen yet.
double step_from(const vx::BlockBoxes& world, vx::Vec3d feet, bool settled = true) {
    vx::Player player = at_rest();
    player.position = feet;
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    if (settled) {
        vx::tick(player, {}, world, numbers, scratch);
    }
    const vx::Vec3d before = player.position;
    vx::tick(player, {.held = forward, .yaw = 0.0F}, world, numbers, scratch);
    return std::hypot(player.position.x - before.x, player.position.z - before.z);
}

// A strip of ice at x from 0 to 1, its top at y = 0.
CellWorld ice_strip() {
    CellWorld strip;
    for (int z = -2; z < 3; ++z) {
        strip.add(0, -1, z, 0.98);
    }
    return strip;
}

} // namespace

TEST_CASE("the block underfoot is the one the box stands on, though the center is over air") {
    // Since 1.20 (MC-1133): the player at x = 1.2, its box from 0.9 to 1.5,
    // stands on the strip of ice. On its very first tick no block has been
    // chosen, and 26.2 reads the one under its position (from its jar):
    // air, so default ground.
    const CellWorld strip = ice_strip();
    CHECK(step_from(strip, {.x = 1.2, .y = 0.0, .z = 0.5}) == doctest::Approx(ice_step));
    CHECK(step_from(strip, {.x = 1.2, .y = 0.0, .z = 0.5}, false) == doctest::Approx(default_step));
    // A hair inside the ice's top, as the collision's tolerance can leave a
    // box, the ice still holds the player up.
    CHECK(step_from(strip, {.x = 1.2, .y = -5e-8, .z = 0.5}) == doctest::Approx(ice_step));
}

TEST_CASE("ties underfoot go to the greater z, and to the greater z before the greater x") {
    // Stone along z = 0 and ice along z = 1: on the seam both centers are
    // 0.5 away, and the greater z, ice, supports the player (26.2's order,
    // from its jar).
    CellWorld rows;
    for (int x = -2; x < 3; ++x) {
        rows.add(x, -1, 0, 0.6);
        rows.add(x, -1, 1, 0.98);
    }
    CHECK(step_from(rows, {.x = 0.5, .y = 0.0, .z = 1.0}) == doctest::Approx(ice_step));
    CHECK(step_from(rows, {.x = 0.5, .y = 0.0, .z = 0.9}) == doctest::Approx(default_step));
    // Two blocks on a diagonal, equally far from (1, 0, 1): stone at x = 1,
    // z = 0, and ice at x = 0, z = 1. The greater z wins before the greater
    // x: ice.
    CellWorld diagonal;
    diagonal.add(1, -1, 0, 0.6);
    diagonal.add(0, -1, 1, 0.98);
    CHECK(step_from(diagonal, {.x = 1.0, .y = 0.0, .z = 1.0}) == doctest::Approx(ice_step));
}

TEST_CASE("the nearest center underfoot is measured in three dimensions, the greater y on a tie") {
    // A box 0.25 tall in block (0, 1, 0) and one 1.25 tall in block (1, 0, 1),
    // both tops at y = 1.25, so the block that slides is at y = 0 in the
    // supporting column. From (1.0625, 1.25, 1.0625) the low box's center is
    // nearer in three dimensions (0.6953 squared, against 0.9453), though
    // not in x and z alone: it supports, and the block under it is ice.
    CellWorld levels;
    levels.add(0, 0, 0, 0.98);
    levels.add(0, 1, 0, 0.6, 0.25);
    levels.add(1, 0, 1, 0.6, 1.25);
    CHECK(step_from(levels, {.x = 1.0625, .y = 1.25, .z = 1.0625}) == doctest::Approx(ice_step));
    // From (1.125, 1.25, 1.125) both are 0.84375 away, squared: the greater
    // y, the low box, supports, and the block under it is stone, though the
    // tall box is ice.
    CellWorld tie;
    tie.add(0, 0, 0, 0.6);
    tie.add(0, 1, 0, 0.6, 0.25);
    tie.add(1, 0, 1, 0.98, 1.25);
    CHECK(step_from(tie, {.x = 1.125, .y = 1.25, .z = 1.125}) == doctest::Approx(default_step));
}

TEST_CASE("a box under the feet supports the player, and a nearer one lower beside it does not") {
    // Stone up to y = 1, and beside it an ice slab, its top 0.5 below the
    // feet: the slab's center is nearer, but only the stone is under them.
    CellWorld step;
    step.add(0, 0, 0, 0.6);
    step.add(1, 0, 0, 0.98, 0.5);
    CHECK(step_from(step, {.x = 1.2, .y = 1.0, .z = 0.5}) == doctest::Approx(default_step));
}

TEST_CASE("over ice, a block up to half a block tall slides as ice, and a taller one does not") {
    // The block 0.500001 below the feet (26.2, from its jar; since 1.15): a
    // slab on ice slides, a bed (0.5625) does not.
    struct Case {
        double height;
        double step;
    };
    for (const Case& game :
         {Case{.height = 0.4375, .step = ice_step}, Case{.height = 0.5, .step = ice_step},
          Case{.height = 0.5625, .step = default_step}}) {
        INFO("height ", game.height);
        CellWorld world;
        for (int x = -2; x < 3; ++x) {
            for (int z = -2; z < 3; ++z) {
                world.add(x, -1, z, 0.98);
                world.add(x, 0, z, 0.6, game.height);
            }
        }
        CHECK(step_from(world, {.x = 0.5, .y = game.height, .z = 0.5}) ==
              doctest::Approx(game.step));
    }
}

TEST_CASE("a tick that starts on ice and ends over stone keeps ice's drag") {
    // Ice at x from 0 to 1 and stone past it. Moving 0.9 along +x with no
    // key held, the tick ends over stone, and its drag is still ice's.
    CellWorld world;
    for (int z = -2; z < 3; ++z) {
        world.add(0, -1, z, 0.98);
        for (int x = 1; x < 4; ++x) {
            world.add(x, -1, z, 0.6);
        }
    }
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    vx::Player player = at_rest();
    player.position = {.x = 0.5, .y = 0.0, .z = 0.5};
    vx::tick(player, {}, world, numbers, scratch);
    player.velocity.x = 0.9;
    vx::tick(player, {}, world, numbers, scratch);
    CHECK(player.position.x == doctest::Approx(1.4));
    CHECK(player.velocity.x == doctest::Approx(0.9 * 0.98 * 0.91));
    // The next tick drags as stone.
    vx::tick(player, {}, world, numbers, scratch);
    CHECK(player.velocity.x == doctest::Approx(0.9 * 0.98 * 0.91 * 0.6 * 0.91));
}

TEST_CASE("walked off an edge, the block the box came from supports the player for a tick") {
    // Ice to x = 3, nothing past it. A move of 0.6 along +x from x = 2.8
    // ends on the ground (Y moves first, while the box is over the ice) with
    // nothing under the box. 26.2 then looks under where the box came from
    // (from its jar) and finds the edge's ice, so the next tick drags as on
    // ice; the move after that leaves the ground, and no block supports it.
    CellWorld world;
    for (int x = 0; x < 3; ++x) {
        for (int z = -2; z < 3; ++z) {
            world.add(x, -1, z, 0.98);
        }
    }
    const vx::MovementNumbers numbers;
    std::vector<Box> scratch;
    vx::Player player = at_rest();
    player.position = {.x = 2.8, .y = 0.0, .z = 0.5};
    player.velocity.x = 0.6;
    vx::tick(player, {}, world, numbers, scratch);
    REQUIRE(player.on_ground);
    REQUIRE(player.supporting.has_value());
    CHECK(*player.supporting == vx::BlockPos{.x = 2, .y = -1, .z = 0});
    const double carried = player.velocity.x;
    vx::tick(player, {}, world, numbers, scratch);
    CHECK(player.velocity.x == doctest::Approx(carried * 0.98 * 0.91));
    CHECK_FALSE(player.on_ground);
    CHECK_FALSE(player.supporting.has_value());

    // A player whose last move on the ground found no block under it does
    // not look back (26.2, from its jar).
    vx::Player found_none = at_rest();
    found_none.position = {.x = 2.8, .y = 0.0, .z = 0.5};
    found_none.velocity.x = 0.6;
    found_none.ground_without_block = true;
    vx::tick(found_none, {}, world, numbers, scratch);
    REQUIRE(found_none.on_ground);
    CHECK_FALSE(found_none.supporting.has_value());
    CHECK(found_none.ground_without_block);
}
