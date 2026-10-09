#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
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

    std::uint64_t seed_;
    PlaceholderBlocks blocks_;
};

} // namespace vx
