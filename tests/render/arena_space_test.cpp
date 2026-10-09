#include <vx/render/chunk_arena.hpp>

#include <doctest/doctest.h>

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
            const std::optional<ArenaRange> range = space.allocate(quads);
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
    }
    CHECK(allocated > 2'000);
}
