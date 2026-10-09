#include <vx/render/chunk_arena.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

// RFC 0007's arena space: ranges come back only once the frame that retired
// them is done, and the statistics say what is live, retired and free.

namespace {

using vx::render::ArenaRange;
using vx::render::ArenaSpace;

ArenaSpace make(std::uint32_t capacity, std::uint32_t slots) {
    vx::Result<ArenaSpace> space = ArenaSpace::create(capacity, slots);
    REQUIRE(space.has_value());
    return std::move(space.value());
}

} // namespace

TEST_CASE("a range retired in a frame is handed out again only once that frame's slot comes "
          "round") {
    ArenaSpace space = make(100, 2);
    const std::optional<ArenaRange> whole = space.allocate(100);
    REQUIRE(whole.has_value());
    CHECK(whole->first_quad == 0);
    CHECK_FALSE(space.allocate(1).has_value());
    // Retired while slot 0 records: frames in flight may still draw it.
    space.retire(*whole, 0);
    CHECK_FALSE(space.allocate(1).has_value());
    space.reclaim(1);
    CHECK_FALSE(space.allocate(1).has_value());
    // Slot 0 comes round: its frame is done.
    space.reclaim(0);
    const std::optional<ArenaRange> again = space.allocate(100);
    REQUIRE(again.has_value());
    CHECK(again->first_quad == 0);
}

TEST_CASE("the statistics count live, retired and free quads, and how scattered the free ones "
          "are") {
    ArenaSpace space = make(1'000, 3);
    std::vector<ArenaRange> ranges;
    for (int at = 0; at < 10; ++at) {
        const std::optional<ArenaRange> range = space.allocate(100);
        REQUIRE(range.has_value());
        ranges.push_back(*range);
    }
    vx::render::ArenaStats stats = space.stats();
    CHECK(stats.capacity == 1'000);
    CHECK(stats.live == 1'000);
    CHECK(stats.ranges == 10);
    CHECK(stats.free == 0);
    CHECK(stats.fragmentation == 0.0);
    // Every other range: five holes of 100 quads.
    for (std::size_t at = 0; at < ranges.size(); at += 2) {
        space.retire(ranges[at], 1);
    }
    stats = space.stats();
    CHECK(stats.live == 500);
    CHECK(stats.retired == 500);
    CHECK(stats.free == 0); // still held for their frame
    space.reclaim(1);
    stats = space.stats();
    CHECK(stats.retired == 0);
    CHECK(stats.free == 500);
    CHECK(stats.largest_free == 100);
    CHECK(stats.fragmentation == doctest::Approx(0.8));
    // Nothing of 101 quads fits, though 500 are free.
    CHECK_FALSE(space.allocate(101).has_value());
    CHECK(space.allocate(100).has_value());
}

TEST_CASE("the lowest free range that fits is where compaction moves a mesh") {
    ArenaSpace space = make(1'200, 2);
    std::vector<ArenaRange> ranges;
    for (int at = 0; at < 10; ++at) {
        const std::optional<ArenaRange> range = space.allocate(100);
        REQUIRE(range.has_value());
        ranges.push_back(*range);
    }
    // Holes at 100 (100 quads) and 400 (200 quads), and 200 free at the end.
    for (const std::size_t at : {1U, 4U, 5U}) {
        space.retire(ranges[at], 0);
    }
    space.reclaim(0);
    const auto lowest = [&space](std::uint32_t quads) {
        const std::optional<ArenaRange> range = space.allocate_lowest(quads);
        REQUIRE(range.has_value());
        return range->first_quad;
    };
    CHECK(lowest(150) == 400);
    CHECK(lowest(100) == 100);
    // 50 left at 550: 60 goes to the end, 50 fills it.
    CHECK(lowest(60) == 1'000);
    CHECK(lowest(50) == 550);
    CHECK_FALSE(space.allocate_lowest(141).has_value());
}

TEST_CASE("a cancelled range is free at once, as if it had never been handed out") {
    ArenaSpace space = make(300, 2);
    const std::optional<ArenaRange> kept = space.allocate(100);
    const std::optional<ArenaRange> tried = space.allocate_lowest(200);
    REQUIRE(kept.has_value());
    REQUIRE(tried.has_value());
    CHECK_FALSE(space.allocate(1).has_value());
    space.cancel(*tried);
    const vx::render::ArenaStats stats = space.stats();
    CHECK(stats.live == 100);
    CHECK(stats.ranges == 1);
    CHECK(stats.retired == 0);
    CHECK(stats.free == 200);
    const std::optional<ArenaRange> again = space.allocate(200);
    REQUIRE(again.has_value());
    CHECK(again->first_quad == tried->first_quad);
}

TEST_CASE("under random allocations, retires and frames, no range overlaps one still in use") {
    constexpr std::uint32_t capacity = 4'096;
    constexpr std::uint32_t slots = 3;
    ArenaSpace space = make(capacity, slots);
    // Each quad: free, or which range holds it until its frame is done.
    std::vector<int> owner(capacity, -1);
    std::vector<ArenaRange> live;
    std::vector<std::vector<ArenaRange>> retired(slots);
    std::mt19937 random(11);
    std::uniform_int_distribution<std::uint32_t> size(1, 200);
    std::uniform_int_distribution<int> roll(0, 9);
    int next_owner = 0;
    std::uint32_t slot = 0;
    std::size_t allocated = 0;
    for (int step = 0; step < 20'000; ++step) {
        const int kind = roll(random);
        if (kind < 5) {
            const std::uint32_t quads = size(random);
            // Two in five the lowest range that fits, which the model
            // finds by looking from the start.
            const bool lowest = kind < 2;
            const std::optional<ArenaRange> range =
                lowest ? space.allocate_lowest(quads) : space.allocate(quads);
            if (lowest) {
                std::optional<std::uint32_t> expected;
                std::uint32_t run = 0;
                for (std::uint32_t at = 0; at < capacity && !expected; ++at) {
                    run = owner[at] == -1 ? run + 1 : 0;
                    if (run == quads) {
                        expected = at + 1 - quads;
                    }
                }
                REQUIRE(range.has_value() == expected.has_value());
                if (range) {
                    REQUIRE(range->first_quad == *expected);
                }
            }
            if (!range) {
                continue;
            }
            ++allocated;
            REQUIRE(range->first_quad + range->quads <= capacity);
            for (std::uint32_t at = range->first_quad; at < range->first_quad + range->quads;
                 ++at) {
                REQUIRE(owner[at] == -1);
                owner[at] = next_owner;
            }
            ++next_owner;
            live.push_back(*range);
        } else if (kind < 8 && !live.empty()) {
            const std::size_t pick = random() % live.size();
            space.retire(live[pick], slot);
            retired[slot].push_back(live[pick]);
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
        } else {
            // The next frame: its slot comes round, and its retired quads
            // are free again.
            slot = (slot + 1) % slots;
            space.reclaim(slot);
            for (const ArenaRange& range : retired[slot]) {
                for (std::uint32_t at = range.first_quad; at < range.first_quad + range.quads;
                     ++at) {
                    owner[at] = -1;
                }
            }
            retired[slot].clear();
        }
        const vx::render::ArenaStats stats = space.stats();
        std::uint32_t live_quads = 0;
        for (const ArenaRange& range : live) {
            live_quads += range.quads;
        }
        REQUIRE(stats.live == live_quads);
        REQUIRE(stats.ranges == live.size());
        std::uint32_t retired_quads = 0;
        for (const std::vector<ArenaRange>& in_slot : retired) {
            for (const ArenaRange& range : in_slot) {
                retired_quads += range.quads;
            }
        }
        REQUIRE(stats.retired == retired_quads);
        // The free quads, and the longest run of them: freed ranges merge
        // with their free neighbors, so that is the largest free range.
        std::uint32_t free_quads = 0;
        std::uint32_t largest = 0;
        std::uint32_t run = 0;
        for (const int held : owner) {
            if (held == -1) {
                ++free_quads;
                ++run;
                largest = std::max(largest, run);
            } else {
                run = 0;
            }
        }
        REQUIRE(stats.free == free_quads);
        REQUIRE(stats.largest_free == largest);
        const double fragmentation =
            free_quads == 0
                ? 0.0
                : 1.0 - (static_cast<double>(largest) / static_cast<double>(free_quads));
        REQUIRE(stats.fragmentation == doctest::Approx(fragmentation));
    }
    CHECK(allocated > 2'000);
}
