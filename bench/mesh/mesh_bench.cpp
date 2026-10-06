#include "../benchmarks.hpp"
#include "test_world.hpp"

#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>

#include <nanobench.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace vx::bench {
namespace {

// What the test world's states look like, as a block registry would say:
// the prototype's textures, water drawn as nothing until M7, and glass and
// leaves as cutouts of two cull groups. Every one of the 65,536 states has
// an entry.
std::vector<BlockVisual> visual_table() {
    std::vector<BlockVisual> visuals(65'536);
    const auto set = [&](BlockState state, Opacity opacity, std::uint8_t group,
                         std::array<std::uint16_t, 6> texture) {
        visuals[static_cast<std::size_t>(state)] = {
            .opacity = opacity, .cull_group = group, .texture = texture};
    };
    const auto all = [](std::uint16_t layer) {
        return std::array<std::uint16_t, 6>{layer, layer, layer, layer, layer, layer};
    };
    set(world::stone, Opacity::opaque, 0, all(1));
    set(world::granite, Opacity::opaque, 0, all(2));
    set(world::andesite, Opacity::opaque, 0, all(3));
    set(world::grass, Opacity::opaque, 0, {5, 5, 4, 6, 5, 5}); // top, sides, dirt below
    set(world::dirt, Opacity::opaque, 0, all(6));
    set(world::sand, Opacity::opaque, 0, all(7));
    set(world::gravel, Opacity::opaque, 0, all(8));
    set(world::iron_ore, Opacity::opaque, 0, all(9));
    set(world::coal_ore, Opacity::opaque, 0, all(10));
    set(world::water, Opacity::air, 0, all(11));
    set(world::glass, Opacity::cutout, 1, all(12));
    set(world::leaves, Opacity::cutout, 2, all(13));
    return visuals;
}

// The chunk at the origin with its border, all from `world`: its
// neighbors are there, as streaming guarantees (RFC 0006).
std::unique_ptr<PaddedChunk> padded(world::Function world) {
    auto chunk = std::make_unique<PaddedChunk>();
    for (int y = 0; y < padded_size; ++y) {
        for (int z = 0; z < padded_size; ++z) {
            for (int x = 0; x < padded_size; ++x) {
                chunk->blocks[padded_index(x, y, z)] = world(x - 1, y - 1, z - 1);
            }
        }
    }
    return chunk;
}

} // namespace

void mesh(ankerl::nanobench::Bench& bench) {
    const std::vector<BlockVisual> visuals = visual_table();
    MeshScratch scratch;
    struct Case {
        const char* name;
        world::Function world;
    };
    for (const Case& test :
         {Case{.name = "typical terrain", .world = world::terrain},
          Case{.name = "rough terrain", .world = world::rough_terrain},
          Case{.name = "flat layers", .world = world::flat},
          Case{.name = "opaque checkerboard", .world = world::checkerboard},
          Case{.name = "cutout checkerboard", .world = world::cutout_checkerboard}}) {
        const std::unique_ptr<PaddedChunk> chunk = padded(test.world);
        // RFC 0003 budgets the output too: under 32 KiB for typical terrain.
        const std::size_t quads = mesh_chunk(*chunk, visuals, scratch).all().size();
        const std::size_t faces =
            mesh_chunk(*chunk, visuals, scratch, {.merge = false}).all().size();
        std::cout << "mesh, " << test.name << ": " << faces << " faces, " << quads << " quads, "
                  << quads * sizeof(Quad) << " bytes\n";
        bench.batch(1).unit("chunk").run(std::string("mesh: ") + test.name, [&] {
            const ChunkMesh mesh = mesh_chunk(*chunk, visuals, scratch);
            ankerl::nanobench::doNotOptimizeAway(mesh.all().size());
        });
        // What merging costs: the same chunk, a quad a face.
        bench.batch(1).unit("chunk").run(std::string("mesh: ") + test.name + ", unmerged", [&] {
            const ChunkMesh mesh = mesh_chunk(*chunk, visuals, scratch, {.merge = false});
            ankerl::nanobench::doNotOptimizeAway(mesh.all().size());
        });
    }
}

} // namespace vx::bench
