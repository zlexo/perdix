#include <vx/render/arena_upkeep.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// RFC 0007's upkeep decides, once a frame, whether the arena compacts,
// repacks into a larger one, or is full.

namespace {

using vx::render::ArenaRange;
using vx::render::ArenaSpace;
using vx::render::ArenaUpkeep;
using vx::render::UpkeepStep;

ArenaSpace make(std::uint32_t capacity) {
    vx::Result<ArenaSpace> space = ArenaSpace::create(capacity, 2);
    REQUIRE(space.has_value());
    return std::move(space).value();
}

// `count` ranges of `quads`, then every other one freed: holes of `quads`
// between the rest.
std::vector<ArenaRange> scatter(ArenaSpace& space, int count, std::uint32_t quads) {
    std::vector<ArenaRange> kept;
    for (int at = 0; at < count; ++at) {
        const std::optional<ArenaRange> range = space.allocate(quads);
        REQUIRE(range.has_value());
        if (at % 2 == 0) {
            space.retire(*range, 0);
        } else {
            kept.push_back(*range);
        }
    }
    space.reclaim(0);
    return kept;
}

ArenaUpkeep make_upkeep(std::uint32_t max_capacity) {
    return ArenaUpkeep({.moves = 8,
                        .move_quads = std::uint32_t{1} << 20U,
                        .max_capacity = max_capacity,
                        .slots = 2});
}

constexpr vx::render::Compacted stuck{.meshes = 0, .quads = 0, .stuck = true};

} // namespace

TEST_CASE("a refusal the packed meshes would make room for starts compaction, and one they "
          "would not a repack 1.5 times larger") {
    ArenaSpace space = make(1'000);
    static_cast<void>(scatter(space, 10, 100)); // 500 live, holes of 100
    ArenaUpkeep upkeep = make_upkeep(10'000);
    // Nothing refused: nothing to do.
    CHECK(upkeep.step(space, 0) == UpkeepStep::none);
    // 300 quads fit nowhere, but the meshes packed would leave 500.
    CHECK(upkeep.step(space, 300) == UpkeepStep::compact);
    CHECK(upkeep.stats().compacting);
    // 600 would not fit even then: a repack, to 1.5 times the capacity.
    ArenaUpkeep growing = make_upkeep(10'000);
    CHECK(growing.step(space, 600) == UpkeepStep::repack);
    CHECK(growing.repack_capacity() == 1'500);
    CHECK_FALSE(growing.stats().full);
    // More than that is wanted: room for it all, and a quarter more.
    CHECK(growing.step(space, 1'500) == UpkeepStep::repack);
    CHECK(growing.repack_capacity() == 2'500);
}

TEST_CASE("a scattered arena is left alone while nothing is refused") {
    ArenaSpace space = make(1'000);
    static_cast<void>(scatter(space, 10, 90));
    REQUIRE(space.stats().fragmentation > 0.5);
    ArenaUpkeep upkeep = make_upkeep(10'000);
    for (int frame = 0; frame < 100; ++frame) {
        CHECK(upkeep.step(space, 0) == UpkeepStep::none);
    }
    CHECK_FALSE(upkeep.stats().compacting);
}

TEST_CASE("compaction runs while uploads are refused until they go in or it is stuck, and a "
          "refusal still there once every slot came round repacks") {
    ArenaSpace space = make(1'000);
    static_cast<void>(scatter(space, 10, 100));
    ArenaUpkeep upkeep = make_upkeep(10'000);
    REQUIRE(upkeep.step(space, 300) == UpkeepStep::compact);
    upkeep.compacted({.meshes = 2, .quads = 200, .stuck = false});
    CHECK(upkeep.stats().compacting);
    CHECK(upkeep.stats().moved == 200);
    CHECK(upkeep.stats().moved_total == 200);
    CHECK(upkeep.step(space, 300) == UpkeepStep::compact);
    CHECK(upkeep.stats().moved == 0);
    // The upload went in: the room is made.
    CHECK(upkeep.step(space, 0) == UpkeepStep::none);
    CHECK_FALSE(upkeep.stats().compacting);

    // Stuck: the highest mesh found no lower range. Refusals wait while the
    // ranges compaction emptied come back, two frames on, and the refused
    // upload tries again in that frame; one still refused then repacks.
    ArenaUpkeep blocked = make_upkeep(10'000);
    REQUIRE(blocked.step(space, 300) == UpkeepStep::compact);
    blocked.compacted(stuck);
    CHECK_FALSE(blocked.stats().compacting);
    CHECK(blocked.step(space, 300) == UpkeepStep::none);
    CHECK(blocked.step(space, 300) == UpkeepStep::none);
    CHECK(blocked.step(space, 300) == UpkeepStep::repack);
    CHECK(blocked.repack_capacity() == 1'500);
    // Settled without a refusal, it forgets: compaction may try again.
    ArenaUpkeep forgets = make_upkeep(10'000);
    REQUIRE(forgets.step(space, 300) == UpkeepStep::compact);
    forgets.compacted(stuck);
    for (int frame = 0; frame < 3; ++frame) {
        CHECK(forgets.step(space, 0) == UpkeepStep::none);
    }
    CHECK(forgets.step(space, 300) == UpkeepStep::compact);
}

TEST_CASE("compaction started by hand runs without refusals until it is stuck") {
    ArenaSpace space = make(1'000);
    static_cast<void>(scatter(space, 10, 100));
    ArenaUpkeep upkeep = make_upkeep(10'000);
    upkeep.start_compaction();
    CHECK(upkeep.step(space, 0) == UpkeepStep::compact);
    upkeep.compacted({.meshes = 3, .quads = 300, .stuck = false});
    CHECK(upkeep.step(space, 0) == UpkeepStep::compact);
    upkeep.compacted(stuck);
    CHECK_FALSE(upkeep.stats().compacting);
    for (int frame = 0; frame < 10; ++frame) {
        CHECK(upkeep.step(space, 0) == UpkeepStep::none);
    }
}

TEST_CASE("repacks stop at the maximum capacity, where the arena is full until a frame goes by "
          "without a refusal") {
    ArenaSpace space = make(1'000);
    const std::optional<ArenaRange> all = space.allocate(900);
    REQUIRE(all.has_value());
    // Room to grow, but not for everything wanted: a repack that still
    // leaves the arena full.
    ArenaUpkeep upkeep = make_upkeep(1'200);
    CHECK(upkeep.step(space, 400) == UpkeepStep::repack);
    CHECK(upkeep.repack_capacity() == 1'200);
    CHECK(upkeep.stats().full);
    // At the maximum already: nothing to do but say so, and count the frame.
    ArenaUpkeep capped = make_upkeep(1'000);
    CHECK(capped.step(space, 200) == UpkeepStep::none);
    CHECK(capped.stats().full);
    CHECK(capped.stats().full_frames == 1);
    for (int frame = 0; frame < 4; ++frame) {
        static_cast<void>(capped.step(space, 200));
    }
    CHECK(capped.stats().full_frames == 5);
    CHECK(capped.step(space, 0) == UpkeepStep::none);
    CHECK_FALSE(capped.stats().full);
    CHECK(capped.stats().full_frames == 5);
}

TEST_CASE("a repack the device could not make leaves the arena as it is, full") {
    ArenaSpace space = make(1'000);
    const std::optional<ArenaRange> all = space.allocate(900);
    REQUIRE(all.has_value());
    ArenaUpkeep upkeep = make_upkeep(1'000'000);
    REQUIRE(upkeep.step(space, 200) == UpkeepStep::repack);
    upkeep.repacked(false, 1'000);
    CHECK(upkeep.stats().full);
    CHECK(upkeep.stats().repacks == 0);
    // It tries no more.
    for (int frame = 0; frame < 5; ++frame) {
        CHECK(upkeep.step(space, 200) == UpkeepStep::none);
        CHECK(upkeep.stats().full);
    }
    ArenaUpkeep made = make_upkeep(1'000'000);
    REQUIRE(made.step(space, 200) == UpkeepStep::repack);
    made.repacked(true, 1'000);
    CHECK(made.stats().repacks == 1);
}
