#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/region.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vx {

// What a chunk's build needs to know beforehand (RFC 0008): structures and
// carvers that reach into its neighbors. The placeholder plans nothing.
struct ChunkPlan {
    ChunkPos pos;
};

// A generation job's working memory, one for each worker (RFC 0005): a
// chunk's blocks as a plain array, the table that compresses them, and the
// surface heights of its 1,024 block columns.
struct GenScratch {
    std::vector<BlockState> blocks = std::vector<BlockState>(chunk_volume);
    PaletteScratch palette;
    std::vector<std::int32_t> heights = std::vector<std::int32_t>(chunk_area);
};

// What streaming needs from a generator (RFC 0006): RFC 0008's two passes and
// its out-of-reach test, and each column's surface range. Pure functions of
// the seed, the parameters and a position, so any thread may call them, in
// any order, with the same results.
class Generator {
public:
    virtual ~Generator() = default;

    // The chunks of a column where anything solid can meet the sky, trees
    // included: wider than the truth at worst, never narrower. A job.
    [[nodiscard]] virtual ChunkRange surface_range(ColumnPos column, GenScratch& scratch) const = 0;
    // The state filling a chunk no terrain can reach, else none. The world
    // thread's, about a microsecond.
    [[nodiscard]] virtual std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept = 0;
    // A job.
    [[nodiscard]] virtual ChunkPlan plan(ChunkPos pos, GenScratch& scratch) const = 0;
    // Bumped whenever what the generator makes changes, as when RFC 0008's
    // parameters do: streaming then regenerates every unedited chunk,
    // nearest first. The world thread's.
    [[nodiscard]] virtual std::uint32_t version() const noexcept { return 0; }
    // A job: the blocks of the chunk at the middle of `plans`, the 27 plans
    // around and including it, x fastest, then z, then y, so plans[13] is
    // its own; null where a neighbor is out of reach.
    [[nodiscard]] virtual ChunkStorage build(std::span<const ChunkPlan* const, 27> plans,
                                             GenScratch& scratch) const = 0;

    // RFC 0011's coarse levels, 1 to levels() - 1: a level-L chunk's cells
    // are 2^L blocks wide and its position is on level L's grid (lod.hpp);
    // level 0 is the calls above. The same contracts hold: pure functions,
    // surface ranges never narrower than the cells' surfaces, and a chunk
    // out of reach builds to that state. Coarse chunks have no plans: a
    // coarse build reads nothing of its neighbors.

    // How many levels the generator makes, level 0 included: 1 makes none
    // coarser, and the calls below are then never made.
    [[nodiscard]] virtual std::uint8_t levels() const noexcept { return 1; }
    // A job.
    [[nodiscard]] virtual ChunkRange lod_surface_range(ColumnPos /*column*/, std::uint8_t /*level*/,
                                                       GenScratch& /*scratch*/) const {
        return {};
    }
    // The world thread's.
    [[nodiscard]] virtual std::optional<BlockState>
    lod_out_of_reach(LodPos /*pos*/) const noexcept {
        return BlockState::air;
    }
    // A job.
    [[nodiscard]] virtual ChunkStorage lod_build(LodPos /*pos*/, GenScratch& /*scratch*/) const {
        return ChunkStorage::uniform(BlockState::air);
    }
};

} // namespace vx
