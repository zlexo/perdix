#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

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
        REQUIRE(map.erase(last_slot[erased]));
        CHECK_FALSE(map.find(last_slot[erased]).has_value());
        for (std::size_t i = 0; i < last_slot.size(); ++i) {
            if (i != erased) {
                CHECK(map.find(last_slot[i]) == static_cast<std::uint32_t>(i));
            }
        }
        // Nothing is left a slot further from home than it needs to be.
        CHECK(map.probe_stats().longest == 4);
    }
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
    CHECK_FALSE(map.find({.x = 1, .y = 2, .z = 3}).has_value());
    CHECK(map.probe_stats().longest == 0);
    CHECK(map.probe_stats().mean == 0.0);
}
