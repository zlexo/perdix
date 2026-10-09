#include <vx/world/coordinates.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <set>
#include <tuple>
#include <vector>

// RFC 0006's region: the terrain disc, the near cylinder, the margin, the
// plan ring and hysteresis, as pure functions.

namespace {

using vx::ChunkPos;
using vx::ChunkRange;
using vx::ColumnPos;
using vx::ColumnRuns;
using vx::Region;
using vx::RegionSettings;

// Terrain bands that look like hills: each column's from a hash of its
// position, 0 to 3 chunks low and 1 to 3 chunks tall, and none for a few
// columns, so the band's edge cases come up.
ChunkRange hill_band(ColumnPos column, std::uint32_t seed) {
    std::uint32_t hash = (static_cast<std::uint32_t>(column.x) * 0x9E37'79B9U) ^
                         (static_cast<std::uint32_t>(column.z) * 0x85EB'CA6BU) ^ seed;
    hash ^= hash >> 15U;
    hash *= 0x2C1B'3C6DU;
    hash ^= hash >> 12U;
    if (hash % 23 == 0) {
        return {};
    }
    const auto low = static_cast<std::int32_t>(hash % 4);
    return {.min_y = low, .max_y = low + static_cast<std::int32_t>((hash >> 8U) % 3)};
}

// The bands of the n by n columns centered on `column`, x fastest.
template <std::size_t Count, int Side>
std::array<ChunkRange, Count> bands_around(ColumnPos column, std::uint32_t seed) {
    std::array<ChunkRange, Count> bands{};
    constexpr int half = Side / 2;
    for (int dz = -half; dz <= half; ++dz) {
        for (int dx = -half; dx <= half; ++dx) {
            bands[static_cast<std::size_t>(((dz + half) * Side) + dx + half)] =
                hill_band({.x = column.x + dx, .z = column.z + dz}, seed);
        }
    }
    return bands;
}

bool drawn(const Region& region, ChunkPos chunk, std::uint32_t seed) {
    const ColumnPos column{.x = chunk.x, .z = chunk.z};
    return region.drawn(column, hill_band(column, seed)).contains(chunk.y);
}

bool loaded(const Region& region, ChunkPos chunk, std::uint32_t seed) {
    const ColumnPos column{.x = chunk.x, .z = chunk.z};
    return region.loaded(column, bands_around<9, 3>(column, seed)).contains(chunk.y);
}

bool planned(const Region& region, ChunkPos chunk, std::uint32_t seed) {
    const ColumnPos column{.x = chunk.x, .z = chunk.z};
    return region.planned(column, bands_around<25, 5>(column, seed)).contains(chunk.y);
}

// RFC 0006's drawn region written out: in the disc and its column's band, or
// in the near cylinder.
bool drawn_by_definition(const Region& region, ChunkPos chunk, std::uint32_t seed) {
    const RegionSettings& settings = region.settings();
    const std::int64_t dx = std::int64_t{chunk.x} - region.camera().x;
    const std::int64_t dz = std::int64_t{chunk.z} - region.camera().z;
    const std::int64_t square = (dx * dx) + (dz * dz);
    const bool in_disc = square <= std::int64_t{settings.view_distance} * settings.view_distance &&
                         hill_band({.x = chunk.x, .z = chunk.z}, seed).contains(chunk.y);
    const bool in_cylinder =
        square <= std::int64_t{settings.near_distance} * settings.near_distance &&
        std::abs(chunk.y - region.camera().y) <= settings.near_height;
    return in_disc || in_cylinder;
}

} // namespace

TEST_CASE("at 32 chunks the region draws 3,209 columns and loads 3,473, and keeps 432 more") {
    // RFC 0006's counts: the disc, the disc grown by one column every way,
    // diagonals included, and that at 34 chunks.
    const Region region({.x = 5, .y = 2, .z = -9}, {});
    const Region kept = region.grown(region.settings().hysteresis);
    std::size_t drawn_columns = 0;
    std::size_t loaded_columns = 0;
    std::size_t kept_columns = 0;
    std::size_t planned_columns = 0;
    for (std::int32_t z = -9 - 40; z <= -9 + 40; ++z) {
        for (std::int32_t x = 5 - 40; x <= 5 + 40; ++x) {
            const ColumnPos column{.x = x, .z = z};
            if (region.column_drawn(column)) {
                ++drawn_columns;
            }
            if (region.column_loaded(column)) {
                ++loaded_columns;
            }
            if (region.column_planned(column)) {
                ++planned_columns;
            }
            if (kept.column_loaded(column)) {
                ++kept_columns;
            }
        }
    }
    CHECK(drawn_columns == 3'209);
    CHECK(loaded_columns == 3'473);
    CHECK(kept_columns == 3'473 + 432);
    // The plan ring: within two columns of a drawn one, diagonals included.
    CHECK(planned_columns == 3'745);
}

TEST_CASE("a terrain band reaches down to its side neighbors' lowest surface") {
    const ChunkRange own{.min_y = 3, .max_y = 5};
    const std::array<ChunkRange, 4> sides = {ChunkRange{.min_y = 4, .max_y = 6}, ChunkRange{},
                                             ChunkRange{.min_y = -1, .max_y = 2},
                                             ChunkRange{.min_y = 7, .max_y = 9}};
    CHECK(vx::terrain_band(own, sides) == ChunkRange{.min_y = -1, .max_y = 5});
    // A column with no surface has no band, whatever its sides, even one
    // reaching below chunk 0.
    CHECK(vx::terrain_band({}, sides).empty());
    // Sides with no surface take nothing down.
    CHECK(vx::terrain_band(own, std::array<ChunkRange, 4>{}) == own);
}

TEST_CASE("a planned column keeps a run for each of its 25 bands, however far apart") {
    // terrain_band's bands overlap their side neighbors', so they make a few
    // runs; bands from anywhere else need not, and must still fit.
    const Region region({.x = 0, .y = -1'000, .z = 0}, {});
    std::array<ChunkRange, 25> bands{};
    for (std::size_t i = 0; i < bands.size(); ++i) {
        const auto y = static_cast<std::int32_t>(6 * i);
        bands[i] = {.min_y = y, .max_y = y};
    }
    const ColumnRuns runs = region.planned({.x = 0, .z = 0}, bands);
    CHECK(runs.runs().size() == 26);
    CHECK(runs.contains((6 * 24) + 2));
    CHECK(runs.contains(-1'000 - 8 - 2));
}

TEST_CASE("column runs merge what overlaps or touches, and keep the rest in order") {
    ColumnRuns runs;
    runs.add({.min_y = 10, .max_y = 12});
    runs.add({.min_y = -5, .max_y = -3});
    runs.add({});
    runs.add({.min_y = 20, .max_y = 21});
    REQUIRE(runs.runs().size() == 3);
    CHECK(runs.runs()[0] == ChunkRange{.min_y = -5, .max_y = -3});
    CHECK(runs.runs()[2] == ChunkRange{.min_y = 20, .max_y = 21});
    // Touching joins: 13 follows 12.
    runs.add({.min_y = 13, .max_y = 15});
    CHECK(runs.runs()[1] == ChunkRange{.min_y = 10, .max_y = 15});
    // One range bridging two runs makes one.
    runs.add({.min_y = 16, .max_y = 19});
    REQUIRE(runs.runs().size() == 2);
    CHECK(runs.runs()[1] == ChunkRange{.min_y = 10, .max_y = 21});
    CHECK(runs.chunks() == 3 + 12);
    CHECK(runs.contains(-4));
    CHECK_FALSE(runs.contains(0));
    CHECK(runs.contains(21));
    CHECK_FALSE(runs.contains(22));
}

TEST_CASE("for random cameras and bands, every drawn chunk's neighbors are loaded, and every "
          "loaded chunk's planned") {
    std::mt19937 random(0x5EED);
    std::uniform_int_distribution<std::int32_t> place(-1'000, 1'000);
    std::uniform_int_distribution<std::int32_t> height(-20, 40);
    std::uniform_int_distribution<std::int32_t> near(-12, 12);
    std::uniform_int_distribution<std::int32_t> tall(-3, 10);
    // The near cylinder inside the disc, and wider than it, as at the
    // smallest view distances.
    const std::array<RegionSettings, 2> settings = {
        RegionSettings{.view_distance = 6, .near_distance = 3, .near_height = 2, .hysteresis = 2},
        RegionSettings{.view_distance = 2, .near_distance = 4, .near_height = 3, .hysteresis = 2}};
    std::array<std::size_t, 2> drawn_chunks{}; // under each settings
    std::size_t loaded_chunks = 0;
    for (int trial = 0; trial < 60; ++trial) {
        const auto seed = static_cast<std::uint32_t>(trial);
        const ChunkPos camera{.x = place(random), .y = height(random), .z = place(random)};
        const Region region(camera, settings[static_cast<std::size_t>(trial % 2)]);
        for (int sample = 0; sample < 300; ++sample) {
            const ChunkPos chunk{
                .x = camera.x + near(random), .y = tall(random), .z = camera.z + near(random)};
            const bool is_drawn = drawn(region, chunk, seed);
            const bool is_loaded = loaded(region, chunk, seed);
            const bool is_planned = planned(region, chunk, seed);
            // Loaded is exactly within one chunk of a drawn one, and planned
            // within one of a loaded one.
            bool near_drawn = false;
            bool near_loaded = false;
            bool all_loaded = true;
            bool all_planned = true;
            for (std::int32_t dy = -1; dy <= 1; ++dy) {
                for (std::int32_t dz = -1; dz <= 1; ++dz) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const ChunkPos next{
                            .x = chunk.x + dx, .y = chunk.y + dy, .z = chunk.z + dz};
                        near_drawn = near_drawn || drawn(region, next, seed);
                        near_loaded = near_loaded || loaded(region, next, seed);
                        all_loaded = all_loaded && loaded(region, next, seed);
                        if (is_loaded) {
                            all_planned = all_planned && planned(region, next, seed);
                        }
                    }
                }
            }
            INFO("camera ", camera.x, ", ", camera.y, ", ", camera.z, ", chunk ", chunk.x, ", ",
                 chunk.y, ", ", chunk.z);
            REQUIRE(is_drawn == drawn_by_definition(region, chunk, seed));
            REQUIRE(is_loaded == near_drawn);
            REQUIRE(is_planned == near_loaded);
            if (is_drawn) {
                REQUIRE(all_loaded);
                ++drawn_chunks[static_cast<std::size_t>(trial % 2)];
            }
            if (is_loaded) {
                REQUIRE(all_planned);
                ++loaded_chunks;
            }
            // A chunk's column belongs where its chunks do.
            const ColumnPos column{.x = chunk.x, .z = chunk.z};
            REQUIRE((!is_drawn || region.column_drawn(column)));
            REQUIRE((!is_loaded || region.column_loaded(column)));
            REQUIRE((!is_planned || region.column_planned(column)));
        }
    }
    // The samples reached every kind of chunk, under both settings.
    INFO("drawn ", drawn_chunks[0], " and ", drawn_chunks[1], ", loaded ", loaded_chunks);
    CHECK(drawn_chunks[0] > 200);
    CHECK(drawn_chunks[1] > 40);
    CHECK(loaded_chunks > drawn_chunks[0] + drawn_chunks[1]);
}

namespace {

using ChunkSet = std::set<std::tuple<std::int32_t, std::int32_t, std::int32_t>>;

// The chunks `region` loads, out of the columns within `reach` of its camera.
ChunkSet loaded_set(const Region& region, std::int32_t reach, std::uint32_t seed) {
    ChunkSet chunks;
    const ChunkPos camera = region.camera();
    for (std::int32_t z = camera.z - reach; z <= camera.z + reach; ++z) {
        for (std::int32_t x = camera.x - reach; x <= camera.x + reach; ++x) {
            const ColumnPos column{.x = x, .z = z};
            const ColumnRuns runs = region.loaded(column, bands_around<9, 3>(column, seed));
            for (const ChunkRange& run : runs.runs()) {
                for (std::int32_t y = run.min_y; y <= run.max_y; ++y) {
                    chunks.emplace(x, y, z);
                }
            }
        }
    }
    return chunks;
}

// One step of streaming: keep what the grown region keeps, load what the
// region loads.
ChunkSet step(const ChunkSet& loaded, const Region& region, std::uint32_t seed) {
    const ChunkSet kept = loaded_set(region.grown(region.settings().hysteresis), 20, seed);
    ChunkSet next;
    for (const auto& chunk : loaded) {
        if (kept.contains(chunk)) {
            next.insert(chunk);
        }
    }
    for (const auto& chunk : loaded_set(region, 20, seed)) {
        next.insert(chunk);
    }
    return next;
}

} // namespace

TEST_CASE("crossing a chunk border back and forth loads and unloads nothing after the first time") {
    const RegionSettings settings{
        .view_distance = 8, .near_distance = 3, .near_height = 2, .hysteresis = 2};
    constexpr std::uint32_t seed = 7;
    const Region here({.x = 0, .y = 4, .z = 0}, settings);
    const Region there({.x = 1, .y = 4, .z = 0}, settings);
    const ChunkSet start = loaded_set(here, 20, seed);
    const ChunkSet crossed = step(start, there, seed);
    CHECK(crossed != start);
    // Back and forth again: the same chunks every time.
    const ChunkSet back = step(crossed, here, seed);
    CHECK(back == crossed);
    CHECK(step(back, there, seed) == crossed);
    // Two chunks on, the far edge goes.
    const ChunkSet farther = step(crossed, Region({.x = 3, .y = 4, .z = 0}, settings), seed);
    CHECK_FALSE(std::ranges::includes(farther, start));
    // Up and down across a chunk border: the near cylinder's height keeps
    // its hysteresis too.
    const Region above({.x = 0, .y = 5, .z = 0}, settings);
    const ChunkSet climbed = step(start, above, seed);
    CHECK(climbed != start);
    CHECK(step(climbed, here, seed) == climbed);
    CHECK(step(step(climbed, here, seed), above, seed) == climbed);
}

TEST_CASE("climbing keeps the terrain disc, and the near cylinder goes with the camera") {
    const ColumnPos column{.x = 20, .z = -10};
    const ChunkRange band{.min_y = 1, .max_y = 3};
    for (const std::int32_t height : {2, 30, 600, 62'500}) {
        INFO("camera at chunk height ", height);
        const Region region({.x = 0, .y = height, .z = 0}, {});
        const ColumnRuns runs = region.drawn(column, band);
        CHECK(runs.contains(1));
        CHECK(runs.contains(3));
        // Under the camera, the near cylinder: 8 below and 8 above.
        const ColumnRuns under = region.drawn({.x = 2, .z = 1}, band);
        CHECK(under.contains(height - 8));
        CHECK(under.contains(height + 8));
        CHECK_FALSE(under.contains(height + 9));
    }
}

TEST_CASE("nothing outside the 32-bit world belongs to a region at its edge") {
    for (const ChunkPos camera :
         {ChunkPos{.x = vx::highest_chunk, .y = vx::highest_chunk, .z = vx::highest_chunk},
          ChunkPos{.x = vx::lowest_chunk, .y = vx::lowest_chunk, .z = vx::lowest_chunk}}) {
        const Region region(camera, {});
        const ChunkRange everything{.min_y = vx::lowest_chunk, .max_y = vx::highest_chunk};
        std::array<ChunkRange, 25> bands{};
        bands.fill(everything);
        // Columns on both sides of the edge: past it, a region holds nothing.
        for (std::int32_t dz = -40; dz <= 40; dz += 7) {
            for (std::int32_t dx = -40; dx <= 40; dx += 7) {
                const ColumnPos column{.x = camera.x + dx, .z = camera.z + dz};
                const ColumnRuns runs =
                    region.planned(column, std::span<const ChunkRange, 25>(bands));
                for (const ChunkRange& run : runs.runs()) {
                    CHECK(run.min_y >= vx::lowest_chunk);
                    CHECK(run.max_y <= vx::highest_chunk);
                }
            }
        }
        // The edge's own column belongs, to the world's last chunk.
        const ColumnRuns own = region.drawn({.x = camera.x, .z = camera.z}, everything);
        REQUIRE(own.runs().size() == 1);
        CHECK(own.runs()[0] == everything);
        // One column past the edge is in nothing, on either axis.
        const std::int32_t past = camera.x == vx::highest_chunk ? camera.x + 1 : camera.x - 1;
        CHECK_FALSE(region.column_drawn({.x = past, .z = camera.z}));
        CHECK_FALSE(region.column_loaded({.x = past, .z = camera.z}));
        CHECK_FALSE(region.column_planned({.x = past, .z = camera.z}));
        CHECK_FALSE(region.column_drawn({.x = camera.x, .z = past}));
        CHECK_FALSE(region.column_planned({.x = camera.x, .z = past}));
    }
}
