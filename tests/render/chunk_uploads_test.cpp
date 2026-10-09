#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <utility>
#include <vector>

// RFC 0007's uploads without a GPU: an arena of quads in memory stands in
// for the buffer, and the frame clock is the test's.

namespace {

using vx::ChunkPos;
using vx::Quad;
using vx::render::ArenaSpace;
using vx::render::ChunkMeshUpdate;
using vx::render::ChunkUploads;
using vx::render::Uploaded;
using Sizes = std::array<std::array<std::uint32_t, 6>, 2>; // by pass, then face
using Time = ChunkUploads::Clock::time_point;
using std::chrono::microseconds;

// A mesh in RFC 0003's order whose quads name their mesh, pass, face and
// place in the group, so a test can tell where each landed.
class TestMesh {
public:
    TestMesh(const Sizes& sizes, std::uint32_t tag) {
        for (std::uint32_t pass = 0; pass < 2; ++pass) {
            for (std::uint32_t face = 0; face < 6; ++face) {
                for (std::uint32_t at = 0; at < sizes[pass][face]; ++at) {
                    quads_.push_back(
                        {.geometry = tag, .material = (pass << 24U) | (face << 16U) | at});
                }
            }
        }
        std::size_t at = 0;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (std::size_t face = 0; face < 6; ++face) {
                mesh_.ranges[pass][face] =
                    std::span<const Quad>(quads_).subspan(at, sizes[pass][face]);
                at += sizes[pass][face];
            }
        }
    }
    TestMesh(const TestMesh&) = delete;
    TestMesh& operator=(const TestMesh&) = delete;
    TestMesh(TestMesh&&) = delete;
    TestMesh& operator=(TestMesh&&) = delete;
    ~TestMesh() = default;

    [[nodiscard]] ChunkMeshUpdate update(ChunkPos pos, std::uint32_t generation) const {
        return {.pos = pos, .generation = generation, .mesh = mesh_};
    }

private:
    std::vector<Quad> quads_;
    vx::ChunkMesh mesh_{};
};

// The arena's memory, written as the GPU path would write the buffer.
class FakeArena final : public vx::render::QuadWriter {
public:
    explicit FakeArena(std::uint32_t capacity) : quads(capacity) {}

    vx::Result<void> write(std::uint32_t first, std::span<const Quad> written) override {
        if (fail || writes == fail_at) {
            return vx::Error{"the writer failed"};
        }
        REQUIRE(std::size_t{first} + written.size() <= quads.size());
        std::ranges::copy(written, quads.begin() + first);
        ++writes;
        return {};
    }

    std::vector<Quad> quads;
    std::size_t writes = 0;
    bool fail = false;
    // Fails from the write after this many on.
    std::size_t fail_at = std::numeric_limits<std::size_t>::max();
};

ArenaSpace make_space(std::uint32_t capacity, std::uint32_t slots) {
    vx::Result<ArenaSpace> space = ArenaSpace::create(capacity, slots);
    REQUIRE(space.has_value());
    return std::move(space).value();
}

constexpr ChunkPos here{.x = 1, .y = 2, .z = 3};
constexpr Sizes some{{{3, 1, 4, 1, 5, 9}, {2, 6, 0, 5, 3, 5}}};
constexpr Sizes few{{{1, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}}};
constexpr Sizes none{};

// RFC 0003's face order (+x, -x, +y, -y, +z, -z) for each of the arena's
// (+x, +y, +z, -x, -y, -z).
constexpr std::array<std::uint32_t, 6> mesher_face = {0, 2, 4, 1, 3, 5};

} // namespace

TEST_CASE("a mesh lands in its range, its groups in the arena's order") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    uploads.begin_frame(0, Time{});
    const TestMesh mesh(some, 7);
    REQUIRE(uploads.upload(mesh.update(here, 1), arena, Time{}).value() == Uploaded::written);
    const vx::render::ChunkMeshRecord* record = uploads.find(here);
    REQUIRE(record != nullptr);
    CHECK(record->generation == 1);
    CHECK(record->range.quads == 44);
    std::uint32_t at = record->range.first_quad;
    for (std::uint32_t pass = 0; pass < 2; ++pass) {
        for (std::uint32_t group = 0; group < 6; ++group) {
            const std::uint32_t face = mesher_face[group];
            INFO("pass ", pass, ", arena group ", group);
            CHECK(record->count[(pass * 6) + group] == some[pass][face]);
            for (std::uint32_t quad = 0; quad < some[pass][face]; ++quad) {
                CHECK(arena.quads[at].geometry == 7);
                CHECK(arena.quads[at].material == ((pass << 24U) | (face << 16U) | quad));
                ++at;
            }
        }
    }
    CHECK(uploads.stats().meshes == 1);
    CHECK(uploads.stats().quads == 44);
    CHECK(uploads.stats().uploads == 1);
    // Eleven groups hold quads, and each is one write.
    CHECK(arena.writes == 11);
}

TEST_CASE("a newer mesh replaces the old, whose range waits for its frame slot") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh first(some, 1);
    const TestMesh second(few, 2);
    uploads.begin_frame(0, Time{});
    REQUIRE(uploads.upload(first.update(here, 1), arena, Time{}).value() == Uploaded::written);
    REQUIRE(uploads.upload(second.update(here, 2), arena, Time{}).value() == Uploaded::written);
    REQUIRE(uploads.find(here) != nullptr);
    CHECK(uploads.find(here)->generation == 2);
    CHECK(uploads.find(here)->range.quads == 1);
    CHECK(uploads.records().size() == 1);
    CHECK(uploads.stats().quads == 1);
    // The first range is retired in slot 0, and free again only when slot 0
    // comes round.
    CHECK(space.stats().retired == 44);
    uploads.begin_frame(1, Time{});
    uploads.begin_frame(2, Time{});
    CHECK(space.stats().retired == 44);
    uploads.begin_frame(0, Time{});
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().live == 1);
}

TEST_CASE("an update no newer than the arena's is dropped, also once an empty mesh removed it") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh mesh(some, 1);
    const TestMesh empty(none, 0);
    uploads.begin_frame(0, Time{});
    REQUIRE(uploads.upload(mesh.update(here, 5), arena, Time{}).value() == Uploaded::written);
    CHECK(uploads.upload(mesh.update(here, 5), arena, Time{}).value() == Uploaded::dropped);
    CHECK(uploads.upload(mesh.update(here, 4), arena, Time{}).value() == Uploaded::dropped);
    CHECK(uploads.stats().dropped == 2);
    // Nothing left to draw: the mesh goes, and an older one waiting behind
    // the empty one is dropped too.
    CHECK(uploads.upload(empty.update(here, 6), arena, Time{}).value() == Uploaded::removed);
    CHECK(uploads.find(here) == nullptr);
    CHECK(uploads.stats().meshes == 0);
    CHECK(space.stats().retired == 44);
    CHECK(uploads.upload(mesh.update(here, 5), arena, Time{}).value() == Uploaded::dropped);
    CHECK(uploads.upload(mesh.update(here, 7), arena, Time{}).value() == Uploaded::written);
    // Unloaded, the chunk is forgotten: its next life starts afresh.
    uploads.remove(here);
    CHECK(uploads.find(here) == nullptr);
    CHECK(uploads.upload(mesh.update(here, 1), arena, Time{}).value() == Uploaded::written);
}

TEST_CASE("uploads stop once the frame's budget is spent, and go on the next frame") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh mesh(few, 1);
    const TestMesh empty(none, 0);
    const Time start{};
    uploads.begin_frame(0, start);
    CHECK(uploads.upload(mesh.update({.x = 0}, 1), arena, start).value() == Uploaded::written);
    CHECK(uploads.upload(mesh.update({.x = 1}, 1), arena, start + microseconds(999)).value() ==
          Uploaded::written);
    CHECK(uploads.upload(mesh.update({.x = 2}, 1), arena, start + microseconds(1'000)).value() ==
          Uploaded::later);
    CHECK(uploads.stats().refused == 1);
    // Drops and empty meshes write nothing, so the budget does not stop them.
    CHECK(uploads.upload(mesh.update({.x = 0}, 1), arena, start + microseconds(5'000)).value() ==
          Uploaded::dropped);
    CHECK(uploads.upload(empty.update({.x = 1}, 2), arena, start + microseconds(5'000)).value() ==
          Uploaded::removed);
    // The next frame, and a raised budget, as a loading screen would.
    const Time next = start + microseconds(16'000);
    uploads.begin_frame(1, next);
    CHECK(uploads.stats().refused == 0);
    CHECK(uploads.upload(mesh.update({.x = 2}, 1), arena, next).value() == Uploaded::written);
    uploads.set_budget(std::chrono::milliseconds(8));
    CHECK(uploads.upload(mesh.update({.x = 3}, 1), arena, next + microseconds(5'000)).value() ==
          Uploaded::written);
    CHECK(uploads.stats().uploads == 2);
}

TEST_CASE("an edit's batch goes in whole whatever the budget, or not at all without room") {
    ArenaSpace space = make_space(100, 3);
    FakeArena arena(100);
    ChunkUploads uploads(space);
    const TestMesh big(some, 1);  // 44 quads
    const TestMesh small(few, 2); // 1 quad
    const Time start{};
    uploads.begin_frame(0, start);
    REQUIRE(uploads.upload(big.update({.x = 0}, 1), arena, start).value() == Uploaded::written);
    // Whatever the budget, a batch of three goes in, in this frame.
    const std::array<ChunkMeshUpdate, 3> batch = {
        small.update({.x = 1}, 2), small.update({.x = 2}, 3), big.update({.x = 0}, 4)};
    CHECK(uploads.upload_batch(batch, arena).value() == Uploaded::written);
    CHECK(uploads.records().size() == 3);
    CHECK(uploads.find({.x = 0})->generation == 4);
    // 90 quads live or retired: a batch needing 45 more does not fit, and
    // nothing of it is written or kept.
    const std::size_t writes = arena.writes;
    const std::array<ChunkMeshUpdate, 2> too_big = {small.update({.x = 3}, 5),
                                                    big.update({.x = 4}, 6)};
    CHECK(uploads.upload_batch(too_big, arena).value() == Uploaded::full);
    CHECK(arena.writes == writes);
    CHECK(uploads.find({.x = 3}) == nullptr);
    CHECK(uploads.records().size() == 3);
    CHECK(space.stats().live == 46);
    // Slot 0 comes round: the old mesh's 44 quads are free again, a hole
    // the batch's large mesh takes, since a batch places its largest first:
    // its small one, placed first, would have split that hole.
    uploads.begin_frame(1, start);
    uploads.begin_frame(0, start);
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().free == 54);
    CHECK(uploads.upload_batch(too_big, arena).value() == Uploaded::written);
    CHECK(uploads.records().size() == 5);
}

TEST_CASE("a batch's update older than another of its chunk in the batch keeps no range") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh newer(some, 2); // 44 quads
    const TestMesh older(few, 1);  // 1 quad
    const TestMesh empty(none, 0);
    uploads.begin_frame(0, Time{});
    // RFC 0005's world thread puts a chunk in a batch once; should one come
    // twice, the newest update wins and the older takes no room.
    const std::array<ChunkMeshUpdate, 4> batch = {newer.update(here, 5), older.update(here, 4),
                                                  empty.update({.x = 9}, 7),
                                                  older.update({.x = 9}, 6)};
    REQUIRE(uploads.upload_batch(batch, arena).value() == Uploaded::written);
    REQUIRE(uploads.find(here) != nullptr);
    CHECK(uploads.find(here)->generation == 5);
    CHECK(uploads.find({.x = 9}) == nullptr);
    CHECK(uploads.stats().dropped == 2);
    // Once every slot has come round, the arena holds that one mesh alone.
    uploads.begin_frame(1, Time{});
    uploads.begin_frame(2, Time{});
    uploads.begin_frame(0, Time{});
    CHECK(space.stats().live == 44);
    CHECK(space.stats().ranges == 1);
    CHECK(space.stats().retired == 0);
}

TEST_CASE("a batch's stale update needs no room") {
    ArenaSpace space = make_space(50, 3);
    FakeArena arena(50);
    ChunkUploads uploads(space);
    const TestMesh big(some, 1);  // 44 quads
    const TestMesh small(few, 2); // 1 quad
    uploads.begin_frame(0, Time{});
    REQUIRE(uploads.upload(big.update(here, 5), arena, Time{}).value() == Uploaded::written);
    // 6 quads free: the stale mesh's 44 would not fit, and it needs none.
    const std::array<ChunkMeshUpdate, 2> batch = {big.update(here, 4), small.update({.x = 9}, 6)};
    CHECK(uploads.upload_batch(batch, arena).value() == Uploaded::written);
    CHECK(uploads.find(here)->generation == 5);
    CHECK(uploads.find({.x = 9}) != nullptr);
    CHECK(uploads.stats().dropped == 1);
}

TEST_CASE("a writer's error writes no record, and its range goes back with its slot") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh mesh(some, 1);
    uploads.begin_frame(0, Time{});
    arena.fail = true;
    const vx::Result<Uploaded> failed = uploads.upload(mesh.update(here, 1), arena, Time{});
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message == "the writer failed");
    CHECK(uploads.find(here) == nullptr);
    CHECK(uploads.stats().meshes == 0);
    CHECK(space.stats().retired == 44);
    uploads.begin_frame(1, Time{});
    uploads.begin_frame(0, Time{});
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().live == 0);
}

TEST_CASE("a writer's error in a batch keeps the meshes before it and gives back every range "
          "after") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh mesh(some, 1); // 44 quads in eleven groups, a write each
    uploads.begin_frame(0, Time{});
    const std::array<ChunkMeshUpdate, 3> batch = {
        mesh.update({.x = 0}, 1), mesh.update({.x = 1}, 2), mesh.update({.x = 2}, 3)};
    // The first mesh goes in whole, the second fails halfway.
    arena.fail_at = 11 + 5;
    const vx::Result<Uploaded> failed = uploads.upload_batch(batch, arena);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().message == "the writer failed");
    CHECK(uploads.find({.x = 0}) != nullptr);
    CHECK(uploads.find({.x = 1}) == nullptr);
    CHECK(uploads.find({.x = 2}) == nullptr);
    // The second and third ranges go back with the slot, each once.
    uploads.begin_frame(1, Time{});
    uploads.begin_frame(2, Time{});
    uploads.begin_frame(0, Time{});
    CHECK(space.stats().live == 44);
    CHECK(space.stats().ranges == 1);
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().free == 1'024 - 44);
}

TEST_CASE("an emptied chunk's generation rises with each empty mesh and goes when it unloads") {
    ArenaSpace space = make_space(1'024, 3);
    FakeArena arena(1'024);
    ChunkUploads uploads(space);
    const TestMesh mesh(some, 1);
    const TestMesh empty(none, 0);
    uploads.begin_frame(0, Time{});
    CHECK(uploads.upload(empty.update(here, 5), arena, Time{}).value() == Uploaded::removed);
    CHECK(uploads.upload(empty.update(here, 8), arena, Time{}).value() == Uploaded::removed);
    // Newer than the first empty mesh, older than the second.
    CHECK(uploads.upload(mesh.update(here, 6), arena, Time{}).value() == Uploaded::dropped);
    // Unloaded, the chunk is forgotten, its empty meshes too.
    uploads.remove(here);
    CHECK(uploads.upload(mesh.update(here, 1), arena, Time{}).value() == Uploaded::written);
}

TEST_CASE("under random uploads, batches, stale offers, removals and frames, each record's "
          "range holds its newest mesh, and the arena holds those ranges alone") {
    // Three frame slots, one more than RFC 0001's frames in flight; every
    // frame, the slot it reuses is finished.
    constexpr std::uint32_t capacity = 4'096;
    ArenaSpace space = make_space(capacity, 3);
    FakeArena arena(capacity);
    ChunkUploads uploads(space);
    std::mt19937 random(0x0A11'0CA7);
    std::uniform_int_distribution<int> roll(0, 99);
    std::uniform_int_distribution<std::uint32_t> group(0, 12);
    std::uniform_int_distribution<int> chunk(0, 15);
    // The meshes alive, so their spans stay valid.
    std::vector<std::unique_ptr<TestMesh>> meshes;
    // Each chunk's newest generation the uploads know, its mesh's or its
    // empty one's, until it unloads; and its mesh's, which tags its quads.
    std::map<int, std::uint32_t> known;
    std::map<int, std::uint32_t> newest;
    std::uint32_t generation = 0;
    std::uint32_t frame = 0;
    std::size_t stale_offers = 0;
    std::size_t batches = 0;
    // A mesh of random groups for chunk `x`, its quads tagged `tag`.
    const auto make_update = [&](int x, std::uint32_t tag) {
        Sizes sizes{};
        for (auto& faces : sizes) {
            for (std::uint32_t& size : faces) {
                size = roll(random) < 60 ? 0 : group(random);
            }
        }
        meshes.push_back(std::make_unique<TestMesh>(sizes, tag));
        return meshes.back()->update({.x = x}, tag);
    };
    // What the uploads make of an update they take, kept in the model.
    const auto expect = [&](const ChunkMeshUpdate& update) {
        const int x = update.pos.x;
        if (known.contains(x) && update.generation <= known[x]) {
            return Uploaded::dropped;
        }
        known[x] = update.generation;
        std::size_t quads = 0;
        for (const auto& faces : update.mesh.ranges) {
            for (const std::span<const Quad> range : faces) {
                quads += range.size();
            }
        }
        if (quads == 0) {
            newest.erase(x);
            return Uploaded::removed;
        }
        newest[x] = update.generation;
        return Uploaded::written;
    };
    uploads.begin_frame(0, Time{});
    for (int step = 0; step < 5'000; ++step) {
        const int kind = roll(random);
        const int x = chunk(random);
        if (kind < 5) {
            ++frame;
            uploads.begin_frame(frame % 3, Time{});
        } else if (kind < 10) {
            uploads.remove({.x = x});
            known.erase(x);
            newest.erase(x);
        } else if (kind < 20 && known.contains(x)) {
            // An update that waited, offered after a newer one went in.
            const std::uint32_t old =
                std::uniform_int_distribution<std::uint32_t>(1, known[x])(random);
            const vx::Result<Uploaded> done = uploads.upload(make_update(x, old), arena, Time{});
            REQUIRE(done.has_value());
            CHECK(done.value() == Uploaded::dropped);
            ++stale_offers;
        } else if (kind < 35) {
            // An edit's batch of one to three, a chunk maybe twice, in any
            // order.
            std::vector<ChunkMeshUpdate> batch;
            const int members = 1 + (roll(random) % 3);
            for (int at = 0; at < members; ++at) {
                const int member = at > 0 && roll(random) < 50 ? batch[0].pos.x : chunk(random);
                ++generation;
                batch.push_back(make_update(member, generation));
            }
            std::ranges::shuffle(batch, random);
            const vx::Result<Uploaded> done = uploads.upload_batch(batch, arena);
            REQUIRE(done.has_value());
            if (done.value() == Uploaded::written) {
                for (const ChunkMeshUpdate& member : batch) {
                    expect(member);
                }
            } else {
                REQUIRE(done.value() == Uploaded::full);
            }
            ++batches;
        } else {
            ++generation;
            const ChunkMeshUpdate update = make_update(x, generation);
            const vx::Result<Uploaded> done = uploads.upload(update, arena, Time{});
            REQUIRE(done.has_value());
            if (done.value() != Uploaded::full) {
                CHECK(done.value() == expect(update));
            }
        }
        // Every record's range holds its own quads, none overlap, the
        // records are exactly the chunks with a mesh, and the arena's live
        // quads are their ranges, nothing more.
        std::vector<std::pair<std::uint32_t, std::uint32_t>> ranges;
        std::uint64_t quads = 0;
        for (const vx::render::ChunkMeshRecord& record : uploads.records()) {
            REQUIRE(newest.contains(record.pos.x));
            REQUIRE(record.generation == newest[record.pos.x]);
            for (std::uint32_t at = 0; at < record.range.quads; ++at) {
                REQUIRE(arena.quads[record.range.first_quad + at].geometry == record.generation);
            }
            ranges.emplace_back(record.range.first_quad, record.range.quads);
            quads += record.range.quads;
        }
        REQUIRE(uploads.records().size() == newest.size());
        std::ranges::sort(ranges);
        for (std::size_t at = 1; at < ranges.size(); ++at) {
            REQUIRE(ranges[at - 1].first + ranges[at - 1].second <= ranges[at].first);
        }
        REQUIRE(uploads.stats().meshes == uploads.records().size());
        REQUIRE(uploads.stats().quads == quads);
        REQUIRE(space.stats().live == quads);
        REQUIRE(space.stats().ranges == uploads.records().size());
    }
    CHECK(frame > 100);
    CHECK(generation > 4'000);
    CHECK(stale_offers > 200);
    CHECK(batches > 500);
    // Every slot comes round: nothing waits, and all but the records' ranges
    // is free.
    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        uploads.begin_frame(slot, Time{});
    }
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().free == capacity - space.stats().live);
}
