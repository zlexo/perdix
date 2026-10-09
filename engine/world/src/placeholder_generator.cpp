#include <vx/world/placeholder_generator.hpp>

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/region.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace vx {
namespace {

// The noise's lattice values and weights are 16-bit fractions: one is 65,536.
constexpr std::int64_t one = std::int64_t{1} << 16U;

// x mod n, from 0 to n - 1 for negative x too.
constexpr std::int32_t floor_mod(std::int32_t x, std::int32_t n) noexcept {
    const std::int32_t mod = x % n;
    return mod < 0 ? mod + n : mod;
}

// x / n, rounded down for negative x too.
constexpr std::int32_t floor_div(std::int32_t x, std::int32_t n) noexcept {
    return (x - floor_mod(x, n)) / n;
}

// A lattice point's value, 0 to one - 1: SplitMix64's finalizer over the
// seed and the point.
std::int64_t lattice(std::uint64_t seed, std::int32_t x, std::int32_t z) noexcept {
    std::uint64_t key = seed ^ ((std::uint64_t{static_cast<std::uint32_t>(x)} << 32U) |
                                static_cast<std::uint32_t>(z));
    key = (key ^ (key >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    key = (key ^ (key >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    key ^= key >> 31U;
    return static_cast<std::int64_t>(key >> 48U);
}

// Smoothstep of `along` cells of `period`, as a fraction of one.
std::int64_t smooth(std::int64_t along, std::int64_t period) noexcept {
    // 3t^2 - 2t^3, with t = along / period.
    return ((3 * along * along * period) - (2 * along * along * along)) * one /
           (period * period * period);
}

std::int64_t mix(std::int64_t a, std::int64_t b, std::int64_t weight) noexcept {
    return a + (((b - a) * weight) / one);
}

// Value noise at block column (x, z), cells `period` blocks wide: 0 to one.
std::int64_t value_noise(std::uint64_t seed, std::int32_t x, std::int32_t z,
                         std::int32_t period) noexcept {
    const std::int32_t cell_x = floor_div(x, period);
    const std::int32_t cell_z = floor_div(z, period);
    const std::int64_t wx = smooth(floor_mod(x, period), period);
    const std::int64_t wz = smooth(floor_mod(z, period), period);
    const std::int64_t near =
        mix(lattice(seed, cell_x, cell_z), lattice(seed, cell_x + 1, cell_z), wx);
    const std::int64_t far =
        mix(lattice(seed, cell_x, cell_z + 1), lattice(seed, cell_x + 1, cell_z + 1), wx);
    return mix(near, far, wz);
}

bool in_spire(std::int32_t x, std::int32_t z) noexcept {
    return floor_mod(x, PlaceholderGenerator::spire_spacing) < PlaceholderGenerator::spire_width &&
           floor_mod(z, PlaceholderGenerator::spire_spacing) < PlaceholderGenerator::spire_width;
}

// Whether a chunk column holds a spire: spires start on multiples of 512,
// which are multiples of 32, so a chunk column holds a whole one or none.
bool column_has_spire(ColumnPos column) noexcept {
    constexpr std::int32_t chunks_apart = PlaceholderGenerator::spire_spacing / chunk_size;
    return floor_mod(column.x, chunks_apart) == 0 && floor_mod(column.z, chunks_apart) == 0;
}

} // namespace

std::int32_t PlaceholderGenerator::height(std::int32_t x, std::int32_t z) const noexcept {
    if (in_spire(x, z)) {
        return spire_height;
    }
    // Two octaves: broad hills, and smaller ones on them.
    const std::int64_t broad = value_noise(seed_, x, z, 128);
    const std::int64_t fine = value_noise(seed_ ^ 0x9E37'79B9'7F4A'7C15ULL, x, z, 32);
    const std::int64_t rise = (hill_rise * ((7 * broad) + (3 * fine))) / (10 * one);
    return lowest_ground + static_cast<std::int32_t>(rise);
}

void PlaceholderGenerator::column_heights(ColumnPos column,
                                          std::span<std::int32_t> heights) const noexcept {
    assert(heights.size() >= static_cast<std::size_t>(chunk_area));
    for (std::int32_t z = 0; z < chunk_size; ++z) {
        for (std::int32_t x = 0; x < chunk_size; ++x) {
            heights[static_cast<std::size_t>(x) + (static_cast<std::size_t>(z) * chunk_size)] =
                height((column.x * chunk_size) + x, (column.z * chunk_size) + z);
        }
    }
}

ChunkRange PlaceholderGenerator::surface_range(ColumnPos column, GenScratch& scratch) const {
    column_heights(column, scratch.heights);
    const auto [lowest, highest] = std::ranges::minmax(scratch.heights);
    // The top solid block of each block column is one below its height.
    return {.min_y = (lowest - 1) >> chunk_bits, .max_y = (highest - 1) >> chunk_bits};
}

std::optional<BlockState> PlaceholderGenerator::out_of_reach(ChunkPos pos) const noexcept {
    const std::int64_t bottom = std::int64_t{pos.y} * chunk_size;
    const std::int64_t top = bottom + chunk_size - 1;
    const std::int32_t tallest =
        column_has_spire({.x = pos.x, .z = pos.z}) ? spire_height : lowest_ground + hill_rise;
    if (bottom >= tallest) {
        return BlockState::air;
    }
    // Under every column's dirt, only stone: the ground is never lower than
    // lowest_ground, so its grass and dirt never reach below that less four.
    if (top < lowest_ground - (dirt_depth + 1)) {
        return blocks_.stone;
    }
    return std::nullopt;
}

ChunkPlan PlaceholderGenerator::plan(ChunkPos pos, GenScratch& /*scratch*/) const {
    return {.pos = pos};
}

ChunkStorage PlaceholderGenerator::build(std::span<const ChunkPlan* const, 27> plans,
                                         GenScratch& scratch) const {
    assert(plans[13] != nullptr);
    const ChunkPos pos = plans[13]->pos;
    column_heights({.x = pos.x, .z = pos.z}, scratch.heights);
    const std::int32_t bottom = pos.y * chunk_size;
    for (std::int32_t y = 0; y < chunk_size; ++y) {
        const std::int32_t block_y = bottom + y;
        for (std::int32_t z = 0; z < chunk_size; ++z) {
            for (std::int32_t x = 0; x < chunk_size; ++x) {
                const std::int32_t ground =
                    scratch.heights[static_cast<std::size_t>(x) +
                                    (static_cast<std::size_t>(z) * chunk_size)];
                BlockState block = BlockState::air;
                if (block_y < ground) {
                    const bool spire = ground == spire_height &&
                                       in_spire((pos.x * chunk_size) + x, (pos.z * chunk_size) + z);
                    if (spire || block_y < ground - (dirt_depth + 1)) {
                        block = blocks_.stone;
                    } else if (block_y < ground - 1) {
                        block = blocks_.dirt;
                    } else {
                        block = blocks_.grass;
                    }
                }
                scratch.blocks[local_index(x, y, z)] = block;
            }
        }
    }
    return ChunkStorage::from_dense(std::span<const BlockState, chunk_volume>(scratch.blocks),
                                    scratch.palette);
}

} // namespace vx
