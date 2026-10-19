#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/region.hpp>

#include <cstdint>
#include <optional>
#include <span>

namespace vx {

// The states the placeholder builds with: the core pack's (RFC 0004).
struct PlaceholderBlocks {
    BlockState grass;
    BlockState dirt;
    BlockState stone;
};

// RFC 0006's generator until RFC 0008's terrain: rolling hills from two
// octaves of value noise, 16 to 111 blocks high, grass over three blocks of
// dirt over stone; and a 16 by 16 stone spire up to y = 640 every 512 blocks
// on both axes, so tall columns and the climb below them are exercised. Its
// noise is integer arithmetic, so a seed gives the same world on every
// machine. It plans nothing, its builds ignore their neighbors, and its
// surface ranges are exact.
class PlaceholderGenerator final : public Generator {
public:
    PlaceholderGenerator(std::uint64_t seed, const PlaceholderBlocks& blocks) noexcept
        : seed_(seed), blocks_(blocks) {}

    // The height of block column (x, z): the y of the lowest air above the
    // ground, everything below it solid.
    [[nodiscard]] std::int32_t height(std::int32_t x, std::int32_t z) const noexcept;

    [[nodiscard]] ChunkRange surface_range(ColumnPos column, GenScratch& scratch) const override;
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override;
    [[nodiscard]] ChunkPlan plan(ChunkPos pos, GenScratch& scratch) const override;
    [[nodiscard]] ChunkStorage build(std::span<const ChunkPlan* const, 27> plans,
                                     GenScratch& scratch) const override;

    // RFC 0011's coarse levels, all five. A cell takes the ground's height
    // at the middle of its column, without the smaller octave where its
    // 32-block hills are under two cells (level 5), whose mean stands in. A
    // cell is solid when its middle is under the ground, and takes the block
    // the top of its solid part has at level 0, so grass caps the hills. The
    // 16-block spires are whole cells to level 4 and fall between the cells'
    // middles at level 5. Level 0 by these rules is the calls above.
    [[nodiscard]] std::uint8_t levels() const noexcept override { return lod_levels; }
    [[nodiscard]] ChunkRange lod_surface_range(ColumnPos column, std::uint8_t level,
                                               GenScratch& scratch) const override;
    [[nodiscard]] std::optional<BlockState> lod_out_of_reach(LodPos pos) const noexcept override;
    [[nodiscard]] ChunkStorage lod_build(LodPos pos, GenScratch& scratch) const override;
    // The ground's height under cell (x, z) of `level`, in blocks.
    [[nodiscard]] std::int32_t cell_height(std::int64_t x, std::int64_t z,
                                           std::uint8_t level) const noexcept;

    // The hills' heights run from this to just under 112; the spires reach
    // 640.
    static constexpr std::int32_t lowest_ground = 16;
    static constexpr std::int32_t hill_rise = 96;
    static constexpr std::int32_t spire_height = 640;
    static constexpr std::int32_t spire_spacing = 512;
    static constexpr std::int32_t spire_width = 16;
    static constexpr std::int32_t dirt_depth = 3; // under the grass

private:
    // The heights of a chunk column's 1,024 block columns, into `heights`,
    // x fastest.
    void column_heights(ColumnPos column, std::span<std::int32_t> heights) const noexcept;
    // The ground's heights under a chunk column's 1,024 cells at `level`.
    void cell_heights(ColumnPos column, std::uint8_t level,
                      std::span<std::int32_t> heights) const noexcept;
    // The hills' height at block column (x, z), with or without the smaller
    // octave.
    [[nodiscard]] std::int32_t hills(std::int32_t x, std::int32_t z,
                                     bool small_octave) const noexcept;

    std::uint64_t seed_;
    PlaceholderBlocks blocks_;
};

} // namespace vx
