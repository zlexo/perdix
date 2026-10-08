#include "demo_chunk.hpp"

#include <vx/core/result.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vx::app {
namespace {

using Blocks = std::array<BlockState, chunk_volume>;

// Fills the box from `low` to `high`, both included, with `state`.
void fill(Blocks& blocks, LocalPos low, LocalPos high, BlockState state) {
    for (int y = low.y; y <= high.y; ++y) {
        for (int z = low.z; z <= high.z; ++z) {
            for (int x = low.x; x <= high.x; ++x) {
                blocks[local_index(x, y, z)] = state;
            }
        }
    }
}

} // namespace

Result<ChunkStorage> demo_chunk(const BlockRegistry& registry) {
    std::string missing;
    const auto block = [&](std::string_view key) {
        const std::optional<BlockState> state = registry.find(key);
        if (!state) {
            missing += (missing.empty() ? "" : ", ") + std::string(key);
        }
        return state.value_or(BlockState::air);
    };
    const BlockState stone = block("core:stone");
    const BlockState dirt = block("core:dirt");
    const BlockState grass = block("core:grass");
    const BlockState sand = block("core:sand");
    const BlockState cobblestone = block("core:cobblestone");
    const BlockState planks = block("core:oak_planks");
    const BlockState leaves = block("core:oak_leaves");
    const BlockState glass = block("core:glass");
    const BlockState log_y = block("core:oak_log[axis=y]");
    const BlockState log_x = block("core:oak_log[axis=x]");
    const BlockState log_z = block("core:oak_log[axis=z]");
    if (!missing.empty()) {
        return Error{"the block pack has no " + missing + ", which the demo chunk is built of"};
    }

    auto blocks = std::make_unique<Blocks>();
    blocks->fill(BlockState::air);
    // The ground: stone, dirt, then grass on top at y = 6, with a beach of
    // sand in one corner, a step lower.
    fill(*blocks, {.x = 0, .y = 0, .z = 0}, {.x = 31, .y = 3, .z = 31}, stone);
    fill(*blocks, {.x = 0, .y = 4, .z = 0}, {.x = 31, .y = 5, .z = 31}, dirt);
    fill(*blocks, {.x = 0, .y = 6, .z = 0}, {.x = 31, .y = 6, .z = 31}, grass);
    fill(*blocks, {.x = 22, .y = 4, .z = 0}, {.x = 31, .y = 5, .z = 9}, sand);
    fill(*blocks, {.x = 22, .y = 6, .z = 0}, {.x = 31, .y = 6, .z = 9}, BlockState::air);

    // An oak: a trunk five high, under a crown of leaves.
    fill(*blocks, {.x = 6, .y = 10, .z = 6}, {.x = 10, .y = 11, .z = 10}, leaves);
    fill(*blocks, {.x = 7, .y = 12, .z = 7}, {.x = 9, .y = 12, .z = 9}, leaves);
    fill(*blocks, {.x = 8, .y = 13, .z = 8}, {.x = 8, .y = 13, .z = 8}, leaves);
    for (const LocalPos corner :
         {LocalPos{.x = 6, .y = 11, .z = 6}, LocalPos{.x = 10, .y = 11, .z = 6},
          LocalPos{.x = 6, .y = 11, .z = 10}, LocalPos{.x = 10, .y = 11, .z = 10}}) {
        (*blocks)[local_index(corner.x, corner.y, corner.z)] = BlockState::air;
    }
    fill(*blocks, {.x = 8, .y = 7, .z = 8}, {.x = 8, .y = 11, .z = 8}, log_y);

    // Felled logs: one along x, one along z.
    fill(*blocks, {.x = 3, .y = 7, .z = 20}, {.x = 9, .y = 7, .z = 20}, log_x);
    fill(*blocks, {.x = 13, .y = 7, .z = 18}, {.x = 13, .y = 7, .z = 25}, log_z);

    // A hut: cobblestone corners, plank walls and roof, a glass window on
    // the side facing +z and a doorway on the side facing -x.
    constexpr int x0 = 18;
    constexpr int x1 = 25;
    constexpr int z0 = 15;
    constexpr int z1 = 21;
    fill(*blocks, {.x = x0, .y = 7, .z = z0}, {.x = x1, .y = 10, .z = z1}, planks);
    fill(*blocks, {.x = x0 + 1, .y = 7, .z = z0 + 1}, {.x = x1 - 1, .y = 10, .z = z1 - 1},
         BlockState::air);
    for (const int x : {x0, x1}) {
        for (const int z : {z0, z1}) {
            fill(*blocks, {.x = x, .y = 7, .z = z}, {.x = x, .y = 10, .z = z}, cobblestone);
        }
    }
    fill(*blocks, {.x = x0, .y = 11, .z = z0}, {.x = x1, .y = 11, .z = z1}, planks);
    fill(*blocks, {.x = x0 + 2, .y = 8, .z = z1}, {.x = x1 - 2, .y = 9, .z = z1}, glass);
    fill(*blocks, {.x = x0, .y = 7, .z = z0 + 3}, {.x = x0, .y = 8, .z = z0 + 3}, BlockState::air);

    PaletteScratch scratch;
    return ChunkStorage::from_dense(*blocks, scratch);
}

} // namespace vx::app
