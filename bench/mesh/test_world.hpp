#pragma once

#include <vx/world/block_state.hpp>

#include <cmath>
#include <cstdint>

// The test world of RFC 0003's performance budget, made as the meshing
// prototype of 2026-10-02 made it, so the engine's numbers compare with the
// prototype's column: seeded value-noise terrain, rolling or rough, a flat
// layered world and two checkerboards. Each is a function of a block's
// world position.
namespace vx::bench::world {

// States numbered as the prototype numbered them, scattered over the 16-bit
// space so nothing relies on a state being a small number.
inline constexpr BlockState air = BlockState::air;
inline constexpr auto stone = static_cast<BlockState>(1);
inline constexpr auto granite = static_cast<BlockState>(2);
inline constexpr auto andesite = static_cast<BlockState>(6);
inline constexpr auto grass = static_cast<BlockState>(9);
inline constexpr auto dirt = static_cast<BlockState>(10);
inline constexpr auto water = static_cast<BlockState>(86);
inline constexpr auto sand = static_cast<BlockState>(118);
inline constexpr auto gravel = static_cast<BlockState>(124);
inline constexpr auto iron_ore = static_cast<BlockState>(131);
inline constexpr auto coal_ore = static_cast<BlockState>(133);
inline constexpr auto glass = static_cast<BlockState>(200);
inline constexpr auto leaves = static_cast<BlockState>(201);

// SplitMix64's finalizer, and a seeded hash of a lattice point.
inline std::uint64_t mix(std::uint64_t value) {
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

inline std::uint64_t hash(std::uint64_t seed, std::int64_t a, std::int64_t b, std::int64_t c) {
    std::uint64_t value = mix(seed + 0x9E3779B97F4A7C15ULL);
    value = mix(value ^ (static_cast<std::uint64_t>(a) * 0xD6E8FEB86659FD93ULL));
    value = mix(value ^ (static_cast<std::uint64_t>(b) * 0xA0761D6478BD642FULL));
    return mix(value ^ (static_cast<std::uint64_t>(c) * 0xE7037ED1A0B428DBULL));
}

inline double unit(std::uint64_t value) {
    return static_cast<double>(value >> 11U) * 0x1.0p-53;
}

inline double smooth(double t) {
    return t * t * (3.0 - (2.0 * t));
}

inline double lerp(double a, double b, double t) {
    return a + ((b - a) * t);
}

// Value noise: a hashed value at each lattice point, smoothly interpolated.
inline double noise2(std::uint64_t seed, double x, double z) {
    const double fx = std::floor(x);
    const double fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx);
    const auto iz = static_cast<std::int64_t>(fz);
    const double tx = smooth(x - fx);
    const double tz = smooth(z - fz);
    const auto at = [&](int dx, int dz) { return unit(hash(seed, ix + dx, 0, iz + dz)); };
    return lerp(lerp(at(0, 0), at(1, 0), tx), lerp(at(0, 1), at(1, 1), tx), tz);
}

inline double noise3(std::uint64_t seed, double x, double y, double z) {
    const double fx = std::floor(x);
    const double fy = std::floor(y);
    const double fz = std::floor(z);
    const auto ix = static_cast<std::int64_t>(fx);
    const auto iy = static_cast<std::int64_t>(fy);
    const auto iz = static_cast<std::int64_t>(fz);
    const double tx = smooth(x - fx);
    const double ty = smooth(y - fy);
    const double tz = smooth(z - fz);
    const auto at = [&](int dx, int dy, int dz) {
        return unit(hash(seed, ix + dx, iy + dy, iz + dz));
    };
    const double low =
        lerp(lerp(at(0, 0, 0), at(1, 0, 0), tx), lerp(at(0, 0, 1), at(1, 0, 1), tx), tz);
    const double high =
        lerp(lerp(at(0, 1, 0), at(1, 1, 0), tx), lerp(at(0, 1, 1), at(1, 1, 1), tx), tz);
    return lerp(low, high, ty);
}

inline constexpr std::uint64_t seed = 20261002;
inline constexpr int sea_level = 13;

// Rolling hills, 8 to 26 blocks high in the chunk at the origin.
inline int terrain_height(int x, int z) {
    const double n =
        (0.65 * noise2(seed, x / 24.0, z / 24.0)) + (0.35 * noise2(seed + 1, x / 9.0, z / 9.0));
    return 8 + static_cast<int>(std::lround(18.0 * n));
}

// Rougher hills, 4 to 28 high: the upper end of typical.
inline int rough_height(int x, int z) {
    const double n =
        (0.6 * noise2(seed + 7, x / 12.0, z / 12.0)) + (0.4 * noise2(seed + 8, x / 5.0, z / 5.0));
    return 4 + static_cast<int>(std::lround(24.0 * n));
}

// About ten states: grass, dirt or sand on top, water below sea level, and
// stone with blobs of granite and andesite, gravel and ores.
template <int (*Height)(int, int)> BlockState layered(int x, int y, int z) {
    const int height = Height(x, z);
    if (y > height) {
        return y <= sea_level ? water : air;
    }
    const int depth = height - y;
    const bool beach = height <= sea_level + 1;
    if (depth == 0) {
        if (height < sea_level - 2) {
            return gravel;
        }
        return beach ? sand : grass;
    }
    if (depth <= 3) {
        return beach ? sand : dirt;
    }
    const double blob = noise3(seed + 2, x / 6.0, y / 6.0, z / 6.0);
    if (blob > 0.80) {
        return granite;
    }
    if (blob < 0.18) {
        return andesite;
    }
    const std::uint64_t roll = hash(seed + 3, x, y, z) % 1000;
    if (roll < 12) {
        return coal_ore;
    }
    if (roll < 18) {
        return iron_ore;
    }
    if (roll < 26) {
        return gravel;
    }
    return stone;
}

inline BlockState terrain(int x, int y, int z) {
    return layered<terrain_height>(x, y, z);
}

inline BlockState rough_terrain(int x, int y, int z) {
    return layered<rough_height>(x, y, z);
}

// Solid below y = 16: grass, three layers of dirt, stone.
inline BlockState flat(int /*x*/, int y, int /*z*/) {
    if (y >= 16) {
        return air;
    }
    if (y == 15) {
        return grass;
    }
    return y >= 12 ? dirt : stone;
}

// Stone where x + y + z is even: 98,304 faces in a chunk, none of which merge.
inline BlockState checkerboard(int x, int y, int z) {
    return ((x + y + z) & 1) == 0 ? stone : air;
}

// Glass and leaves alternating. Neither hides the other, so every block shows
// all six faces: 196,608, the most a chunk can have.
inline BlockState cutout_checkerboard(int x, int y, int z) {
    return ((x + y + z) & 1) == 0 ? glass : leaves;
}

using Function = BlockState (*)(int x, int y, int z);

} // namespace vx::bench::world
