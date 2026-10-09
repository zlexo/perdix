#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

// RFC 0006's chunk map: open addressing over a power-of-two array, erasing
// by shifting a probe run back, and its hash.

namespace {

using vx::ChunkMap;
using vx::ChunkPos;

// Chunk coordinates run from the lowest block's >> 5 to the highest's
// (RFC 0002): 27 bits each.
constexpr std::int32_t lowest = std::numeric_limits<std::int32_t>::min() >> vx::chunk_bits;
constexpr std::int32_t highest = std::numeric_limits<std::int32_t>::max() >> vx::chunk_bits;

// Positions this far apart on an axis share a hash.
constexpr std::int32_t alias = 1 << 21;

struct PosHash {
    std::size_t operator()(ChunkPos pos) const noexcept { return vx::chunk_hash(pos); }
};

// A position from a mix that crowds runs and tests the edges: a small cube
// around the origin, the edges of the range, positions sharing a hash with
// the small cube's, and anywhere in the range.
ChunkPos random_pos(std::mt19937& random) {
    std::uniform_int_distribution<int> kind(0, 3);
    std::uniform_int_distribution<std::int32_t> small(-8, 7);
    std::uniform_int_distribution<std::int32_t> near_edge(0, 3);
    std::uniform_int_distribution<std::int32_t> anywhere(lowest, highest);
    std::uniform_int_distribution<int> coin(0, 1);
    switch (kind(random)) {
    case 0:
        return {.x = small(random), .y = small(random), .z = small(random)};
    case 1: {
        const auto edge = [&] {
            return coin(random) == 0 ? lowest + near_edge(random) : highest - near_edge(random);
        };
        return {.x = edge(), .y = edge(), .z = edge()};
    }
    case 2:
        return {.x = small(random) + (alias * (coin(random) == 0 ? 1 : -1)),
                .y = small(random),
                .z = small(random) - alias};
    default:
        return {.x = anywhere(random), .y = anywhere(random), .z = anywhere(random)};
    }
}

// The slots a map holding `count` positions should have: the smallest power
// of two, at least 16, with `count` at most half of it.
std::size_t expected_capacity(std::size_t count) {
    std::size_t capacity = 16;
    while (capacity / 2 < count) {
        capacity *= 2;
    }
    return capacity;
}

// A region as streaming loads it, roughly: the columns within 33 chunks of
// its center, 5 chunks deep. Its row at `dz` runs from the center's x less
// this to the center's x plus this.
std::int32_t half_row(std::int32_t dz) {
    std::int32_t half = 0;
    while (((half + 1) * (half + 1)) + (dz * dz) <= 33 * 33) {
        ++half;
    }
    return half;
}

constexpr std::int32_t region_depth = 5;

std::vector<ChunkPos> region_around(std::int32_t center_x) {
    std::vector<ChunkPos> region;
    for (std::int32_t dz = -33; dz <= 33; ++dz) {
        const std::int32_t half = half_row(dz);
        for (std::int32_t x = center_x - half; x <= center_x + half; ++x) {
            for (std::int32_t y = 0; y < region_depth; ++y) {
                region.push_back({.x = x, .y = y, .z = dz});
            }
        }
    }
    return region;
}

} // namespace

TEST_CASE("a chunk map agrees with std::unordered_map through seeded inserts, erases and finds") {
    std::mt19937 random(0x5EED);
    std::uniform_int_distribution<int> operation(0, 99);
    std::uniform_int_distribution<std::uint32_t> record(0, ChunkMap::no_record - 1);
    ChunkMap map;
    std::unordered_map<ChunkPos, std::uint32_t, PosHash> reference;
    for (int step = 0; step < 200'000; ++step) {
        const ChunkPos pos = random_pos(random);
        const int roll = operation(random);
        if (roll < 45) {
            const std::uint32_t value = record(random);
            const bool inserted = reference.try_emplace(pos, value).second;
            REQUIRE(map.insert(pos, value) == inserted);
        } else if (roll < 80) {
            REQUIRE(map.erase(pos) == (reference.erase(pos) == 1));
        } else {
            const auto found = reference.find(pos);
            REQUIRE(map.find(pos) == (found == reference.end()
                                          ? std::nullopt
                                          : std::optional<std::uint32_t>(found->second)));
        }
        REQUIRE(map.size() == reference.size());
    }
    CHECK(map.size() > 1'000);
    for (const auto& [pos, value] : reference) {
        CHECK(map.find(pos) == value);
    }
    CHECK(std::has_single_bit(map.capacity()));
    CHECK(map.size() <= map.capacity() / 2);
}

TEST_CASE("the table doubles before it is more than half full, and not before") {
    ChunkMap map;
    CHECK(map.capacity() == 0);
    CHECK(map.memory_bytes() == 0);
    for (std::int32_t x = 0; x < 3'000; ++x) {
        REQUIRE(map.insert({.x = x, .y = -x, .z = 7}, static_cast<std::uint32_t>(x)));
        INFO("size ", map.size());
        REQUIRE(map.capacity() == expected_capacity(map.size()));
        if (map.size() == map.capacity() / 2) {
            // Half full, an insert refused for a mapped position does not
            // double it either.
            REQUIRE_FALSE(map.insert({.x = 0, .y = 0, .z = 7}, 1));
            REQUIRE(map.capacity() == expected_capacity(map.size()));
        }
    }
    // 16 bytes a slot: at 17,900 chunks, RFC 0006's 65,536 slots are 1 MiB.
    CHECK(map.memory_bytes() == map.capacity() * 16);
    ChunkMap region;
    region.reserve(17'900);
    CHECK(region.capacity() == 65'536);
    CHECK(region.memory_bytes() == std::size_t{1} << 20U);
    // Exactly half full is allowed: 65,536 slots hold 32,768 positions.
    region.reserve(32'768);
    CHECK(region.capacity() == 65'536);
}

TEST_CASE("positions that share a hash are still told apart") {
    const ChunkPos first{.x = 5, .y = -3, .z = 7};
    const std::vector<ChunkPos> sharing = {first,
                                           {.x = 5 + alias, .y = -3, .z = 7},
                                           {.x = 5, .y = -3 + alias, .z = 7},
                                           {.x = 5, .y = -3, .z = 7 - alias},
                                           {.x = 5 - (4 * alias), .y = -3 + alias, .z = 7 + alias}};
    ChunkMap map;
    for (std::size_t i = 0; i < sharing.size(); ++i) {
        CHECK(vx::chunk_hash(sharing[i]) == vx::chunk_hash(first));
        REQUIRE(map.insert(sharing[i], static_cast<std::uint32_t>(i)));
    }
    REQUIRE(map.erase(sharing[1]));
    CHECK_FALSE(map.find(sharing[1]).has_value());
    for (std::size_t i = 0; i < sharing.size(); ++i) {
        if (i != 1) {
            CHECK(map.find(sharing[i]) == static_cast<std::uint32_t>(i));
        }
    }
    REQUIRE(map.insert(sharing[1], 99));
    CHECK(map.find(sharing[1]) == 99U);
}

TEST_CASE("erasing in a run that wraps past the table's end keeps the run reachable") {
    // Six positions whose hash names the last of 16 slots: they fill slots
    // 15 and 0 to 4, so their run wraps.
    std::vector<ChunkPos> last_slot;
    for (std::int32_t x = 0; last_slot.size() < 6; ++x) {
        const ChunkPos pos{.x = x, .y = 0, .z = 0};
        if ((vx::chunk_hash(pos) & 15U) == 15U) {
            last_slot.push_back(pos);
        }
    }
    for (std::size_t erased = 0; erased < last_slot.size(); ++erased) {
        INFO("erased ", erased);
        ChunkMap map;
        for (std::size_t i = 0; i < last_slot.size(); ++i) {
            REQUIRE(map.insert(last_slot[i], static_cast<std::uint32_t>(i)));
        }
        REQUIRE(map.capacity() == 16);
        CHECK(map.probe_stats().longest == 5);
        CHECK(map.probe_stats().mean == 2.5); // 0 to 5 slots from home
        REQUIRE(map.erase(last_slot[erased]));
        CHECK_FALSE(map.find(last_slot[erased]).has_value());
        for (std::size_t i = 0; i < last_slot.size(); ++i) {
            if (i != erased) {
                CHECK(map.find(last_slot[i]) == static_cast<std::uint32_t>(i));
            }
        }
        // Nothing is left a slot further from home than it needs to be.
        CHECK(map.probe_stats().longest == 4);
        CHECK(map.probe_stats().mean == 2.0);
    }
}

TEST_CASE("probe runs stay short after hours of streaming") {
    // A region flies 2,000 chunks east: every step frees the slice it leaves
    // and maps the slice it enters, 670,000 erases and as many inserts.
    ChunkMap map;
    std::uint32_t record = 0;
    for (const ChunkPos pos : region_around(0)) {
        REQUIRE(map.insert(pos, record++));
    }
    const std::size_t region_size = map.size();
    REQUIRE(map.capacity() == 65'536);
    std::size_t moves = 0;
    for (std::int32_t center = 1; center <= 2'000; ++center) {
        for (std::int32_t dz = -33; dz <= 33; ++dz) {
            const std::int32_t half = half_row(dz);
            for (std::int32_t y = 0; y < region_depth; ++y) {
                REQUIRE(map.erase({.x = center - 1 - half, .y = y, .z = dz}));
                REQUIRE(map.insert({.x = center + half, .y = y, .z = dz}, record++));
                ++moves;
            }
        }
        REQUIRE(map.size() == region_size);
    }
    CHECK(moves == 670'000);
    CHECK(map.capacity() == 65'536);
    // As short as in a map built fresh from the same positions: a quarter
    // full, a position sits about 0.2 slots from home.
    ChunkMap fresh;
    for (const ChunkPos pos : region_around(2'000)) {
        REQUIRE(fresh.insert(pos, 0));
    }
    const vx::ProbeStats churned = map.probe_stats();
    const vx::ProbeStats built = fresh.probe_stats();
    INFO("mean ", churned.mean, " against ", built.mean, ", longest ", churned.longest, " against ",
         built.longest);
    CHECK(churned.mean < 0.4);
    CHECK(churned.mean < built.mean * 1.5);
    CHECK(churned.longest <= (built.longest * 2) + 2);
    for (const ChunkPos pos : region_around(2'000)) {
        REQUIRE(map.find(pos).has_value());
    }
}

TEST_CASE(
    "the 2,097,152 positions of a 128-chunk cube share no hash, at the origin and the edges") {
    for (const std::int32_t corner : {-64, lowest, highest - 127}) {
        INFO("cube from ", corner);
        std::vector<std::uint64_t> hashes;
        hashes.reserve(std::size_t{128} * 128 * 128);
        for (std::int32_t x = 0; x < 128; ++x) {
            for (std::int32_t y = 0; y < 128; ++y) {
                for (std::int32_t z = 0; z < 128; ++z) {
                    hashes.push_back(
                        vx::chunk_hash({.x = corner + x, .y = corner + y, .z = corner + z}));
                }
            }
        }
        std::ranges::sort(hashes);
        CHECK(std::ranges::adjacent_find(hashes) == hashes.end());
    }
}

TEST_CASE("positions within 2^21 chunks of each other on every axis never share a hash") {
    // The finalizer is a bijection, so only the 63 packed bits matter: each
    // coordinate's low 21 bits, side by side. Coordinates that differ in
    // their highest or lowest packed bits would collide if the fields
    // overlapped or lost a bit.
    const std::vector<std::int32_t> values = {
        0, 1, 2, 1 << 10, 1 << 19, 1 << 20, (1 << 20) + 1, alias - 2, alias - 1};
    for (const std::int32_t offset : {0, lowest, highest - alias + 1, -alias}) {
        INFO("offset ", offset);
        std::vector<std::uint64_t> hashes;
        for (const std::int32_t x : values) {
            for (const std::int32_t y : values) {
                for (const std::int32_t z : values) {
                    hashes.push_back(
                        vx::chunk_hash({.x = offset + x, .y = offset + y, .z = offset + z}));
                }
            }
        }
        std::ranges::sort(hashes);
        CHECK(std::ranges::adjacent_find(hashes) == hashes.end());
    }
}

TEST_CASE("the hash is RFC 0006's, to the bit") {
    // Worked out outside the engine from the RFC's code. The tests above pass
    // for any well-mixed bijection, so only this one sees a change to the
    // packing order, a constant or a shift.
    CHECK(vx::chunk_hash({}) == 0U);
    CHECK(vx::chunk_hash({.x = 1, .y = 2, .z = 3}) == 0x4708'9D13'0101'72F1ULL);
    CHECK(vx::chunk_hash({.x = -1, .y = -2, .z = -3}) == 0x672C'020C'AED8'AF9DULL);
    CHECK(vx::chunk_hash({.x = std::numeric_limits<std::int32_t>::min(),
                          .y = std::numeric_limits<std::int32_t>::max(),
                          .z = lowest}) == 0x0435'B385'7FD8'9883ULL);
}

TEST_CASE("insert refuses a mapped position, erase an unmapped one, and clear keeps the slots") {
    ChunkMap map;
    CHECK_FALSE(map.erase({}));
    CHECK_FALSE(map.find({}).has_value());
    REQUIRE(map.insert({.x = 1, .y = 2, .z = 3}, 10));
    CHECK_FALSE(map.insert({.x = 1, .y = 2, .z = 3}, 11));
    CHECK(map.find({.x = 1, .y = 2, .z = 3}) == 10U);
    CHECK_FALSE(map.erase({.x = 3, .y = 2, .z = 1}));
    CHECK(map.size() == 1);
    map.reserve(1'000);
    const std::size_t reserved = map.capacity();
    CHECK(reserved == expected_capacity(1'000));
    for (std::int32_t x = 0; x < 1'000; ++x) {
        static_cast<void>(map.insert({.x = x, .y = 0, .z = 0}, 0));
    }
    CHECK(map.capacity() == reserved);
    map.clear();
    CHECK(map.empty());
    CHECK(map.capacity() == reserved);
    map.reserve(1); // never shrinks
    CHECK(map.capacity() == reserved);
    CHECK_FALSE(map.find({.x = 1, .y = 2, .z = 3}).has_value());
    CHECK(map.probe_stats().longest == 0);
    CHECK(map.probe_stats().mean == 0.0);
}

TEST_CASE("an empty slot never answers for the origin") {
    // An empty slot holds the position (0, 0, 0): only its record tells it
    // from the origin's. Here the origin's own slot is empty.
    ChunkMap map;
    const ChunkPos other{.x = 1, .y = 2, .z = 3};
    REQUIRE(map.insert(other, 1));
    REQUIRE((vx::chunk_hash({}) & 15U) != (vx::chunk_hash(other) & 15U));
    CHECK_FALSE(map.find({}).has_value());
    CHECK_FALSE(map.erase({}));
    REQUIRE(map.insert({}, 7));
    CHECK(map.find({}) == 7U);
    REQUIRE(map.erase({}));
    CHECK_FALSE(map.find({}).has_value());
    CHECK(map.size() == 1);
    CHECK(map.find(other) == 1U);
}

TEST_CASE("erasing where runs cross the table's end, with mixed homes, keeps the rest reachable") {
    // Positions whose own slot is one of the last four or the first three of
    // 16: eight of them fill runs that cross the end, with mixed homes.
    std::vector<ChunkPos> near_end;
    for (std::int32_t x = 0; near_end.size() < std::size_t{8} * 200; ++x) {
        const ChunkPos pos{.x = x, .y = 0, .z = 0};
        const std::uint64_t home = vx::chunk_hash(pos) & 15U;
        if (home >= 12U || home <= 2U) {
            near_end.push_back(pos);
        }
    }
    for (std::size_t first = 0; first < near_end.size(); first += 8) {
        for (std::size_t erased = first; erased < first + 8; ++erased) {
            ChunkMap map;
            ChunkMap without;
            for (std::size_t i = first; i < first + 8; ++i) {
                const auto record = static_cast<std::uint32_t>(i);
                REQUIRE(map.insert(near_end[i], record));
                if (i != erased) {
                    REQUIRE(without.insert(near_end[i], record));
                }
            }
            REQUIRE(map.capacity() == 16);
            REQUIRE(map.erase(near_end[erased]));
            REQUIRE_FALSE(map.find(near_end[erased]).has_value());
            for (std::size_t i = first; i < first + 8; ++i) {
                if (i != erased) {
                    REQUIRE(map.find(near_end[i]) == static_cast<std::uint32_t>(i));
                }
            }
            // Linear probing fills the same slots whatever the order of
            // inserts, so a right erase leaves the same total distance from
            // home as a map built without that position.
            REQUIRE(map.probe_stats().mean == without.probe_stats().mean);
        }
    }
}
