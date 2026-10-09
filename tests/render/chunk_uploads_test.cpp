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

// The GPU's copies of a frame's moves, from one arena's quads into
// another's, or within one: a frame's moves never read what another writes.
void copy_moves(std::span<const vx::render::ArenaMove> moves, const std::vector<Quad>& from,
                std::vector<Quad>& to) {
    for (const vx::render::ArenaMove& move : moves) {
        REQUIRE(std::size_t{move.from} + move.quads <= from.size());
        REQUIRE(std::size_t{move.to} + move.quads <= to.size());
        std::copy_n(from.begin() + move.from, move.quads, to.begin() + move.to);
    }
}

// Vulkan's rule for one copy within a buffer, which the GPU makes of a
// frame's compaction: no move's destination touches any move's source.
bool sources_and_destinations_apart(std::span<const vx::render::ArenaMove> moves) {
    for (const vx::render::ArenaMove& into : moves) {
        for (const vx::render::ArenaMove& out : moves) {
            if (into.to < out.from + out.quads && out.from < into.to + into.quads) {
                return false;
            }
        }
    }
    return true;
}

// Whether each record's range holds its mesh's quads, tagged with the chunk's
// x: `meshes` by x.
bool every_mesh_in_place(const ChunkUploads& uploads, const std::vector<Quad>& arena,
                         const std::map<int, const TestMesh*>& meshes) {
    for (const vx::render::ChunkMeshRecord& record : uploads.records()) {
        const vx::ChunkMesh mesh = meshes.at(record.pos.x)->update(record.pos, 1).mesh;
        std::uint32_t at = record.range.first_quad;
        for (const vx::Pass pass : {vx::Pass::opaque, vx::Pass::cutout}) {
            for (const vx::Face face : vx::render::arena_faces) {
                for (const Quad quad : mesh.range(pass, face)) {
                    if (arena.at(at) != quad) {
                        return false;
                    }
                    ++at;
                }
            }
        }
    }
    return true;
}

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
    // A frame whose budget went before its first upload, as when the frame
    // waited for its image, still takes that one, and no more.
    const Time late = next + microseconds(16'000);
    uploads.begin_frame(2, late);
    CHECK(uploads.upload(mesh.update({.x = 4}, 1), arena, late + microseconds(9'000)).value() ==
          Uploaded::written);
    CHECK(uploads.upload(mesh.update({.x = 5}, 1), arena, late + microseconds(9'000)).value() ==
          Uploaded::later);
    uploads.set_budget(std::chrono::nanoseconds(0));
    uploads.begin_frame(0, late + microseconds(16'000));
    CHECK(uploads.upload(mesh.update({.x = 5}, 1), arena, late + microseconds(16'000)).value() ==
          Uploaded::written);
    CHECK(uploads.upload(mesh.update({.x = 6}, 1), arena, late + microseconds(16'000)).value() ==
          Uploaded::later);
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
    // Two large meshes: the first finds the hole, the second nothing. The
    // first's range goes back at once, never written: held for a frame slot,
    // it would hide room the upkeep looks for.
    const std::array<ChunkMeshUpdate, 2> two_big = {big.update({.x = 5}, 7),
                                                    big.update({.x = 6}, 8)};
    CHECK(uploads.upload_batch(two_big, arena).value() == Uploaded::full);
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
    // The older update's range went back at once, never written.
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().live == 44);
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

TEST_CASE("a refused upload says how much room it wanted: its mesh's, or its whole batch's") {
    ArenaSpace space = make_space(50, 3);
    FakeArena arena(50);
    ChunkUploads uploads(space);
    uploads.begin_frame(0, Time{});
    const TestMesh big(some, 1); // 44 quads
    const TestMesh one(few, 2);
    REQUIRE(uploads.upload(big.update(here, 1), arena, Time{}).value() == Uploaded::written);
    CHECK(uploads.stats().wanted == 0);
    REQUIRE(uploads.upload(big.update({.x = 9}, 1), arena, Time{}).value() == Uploaded::full);
    CHECK(uploads.stats().wanted == 44);
    // Two meshes of one quad fit alone; with a third mesh of 44 the batch
    // wants all 46 at once.
    const std::array<ChunkMeshUpdate, 3> batch = {
        one.update({.x = 10}, 1), big.update({.x = 11}, 1), one.update({.x = 12}, 1)};
    REQUIRE(uploads.upload_batch(batch, arena).value() == Uploaded::full);
    CHECK(uploads.stats().wanted == 46);
    // A later, smaller refusal keeps the frame's largest; the next frame
    // starts over.
    REQUIRE(uploads.upload(big.update({.x = 13}, 1), arena, Time{}).value() == Uploaded::full);
    CHECK(uploads.stats().wanted == 46);
    uploads.begin_frame(1, Time{});
    CHECK(uploads.stats().wanted == 0);
}

TEST_CASE("compaction moves the highest meshes into the lowest ranges that fit them, and each "
          "keeps its quads") {
    ArenaSpace space = make_space(450, 3);
    FakeArena arena(450);
    ChunkUploads uploads(space);
    // Ten meshes of 44 quads, then the lower ones of every other pair go:
    // holes of 44 between the rest, and 10 free at the end.
    const TestMesh mesh(some, 7);
    std::map<int, const TestMesh*> meshes;
    uploads.begin_frame(0, Time{});
    for (int x = 0; x < 10; ++x) {
        REQUIRE(uploads.upload(mesh.update({.x = x}, 1), arena, Time{}).value() ==
                Uploaded::written);
        meshes[x] = &mesh;
    }
    std::map<int, std::uint32_t> before;
    for (const vx::render::ChunkMeshRecord& record : uploads.records()) {
        before[record.pos.x] = record.range.first_quad;
    }
    for (int x = 0; x < 10; x += 2) {
        uploads.remove({.x = x});
        meshes.erase(x);
    }
    for (std::uint32_t slot : {1U, 2U, 0U}) {
        uploads.begin_frame(slot, Time{});
    }
    REQUIRE(space.stats().fragmentation > 0.5);
    std::vector<vx::render::ArenaMove> moves;
    const vx::render::Compacted done = uploads.compact(3, 450, moves);
    CHECK(done.meshes == 3);
    CHECK(done.quads == 132);
    CHECK_FALSE(done.stuck);
    REQUIRE(moves.size() == 3);
    // The three highest went down, highest first, each to the lowest hole.
    std::vector<std::uint32_t> highest;
    for (const auto& [x, first] : before) {
        if (x % 2 == 1) {
            highest.push_back(first);
        }
    }
    std::ranges::sort(highest, std::ranges::greater{});
    for (std::size_t at = 0; at < moves.size(); ++at) {
        CHECK(moves[at].from == highest[at]);
        CHECK(moves[at].to < moves[at].from);
        CHECK(moves[at].quads == 44);
    }
    CHECK(moves[0].to == 0);
    CHECK(moves[1].to == 88);
    CHECK(moves[2].to == 176);
    CHECK(sources_and_destinations_apart(moves));
    copy_moves(moves, std::vector<Quad>(arena.quads), arena.quads);
    CHECK(every_mesh_in_place(uploads, arena.quads, meshes));
    // The old ranges wait for the frame.
    CHECK(space.stats().retired == 132);
    CHECK(space.stats().live == 5 * 44);
}

TEST_CASE("compaction is stuck once the highest mesh cannot go lower") {
    ArenaSpace space = make_space(1'000, 3);
    FakeArena arena(1'000);
    ChunkUploads uploads(space);
    const TestMesh big(some, 7); // 44 quads
    const TestMesh small(few, 8);
    uploads.begin_frame(0, Time{});
    // Small, big, small, big, ...: removing the small ones leaves holes of
    // one quad, where no big mesh fits.
    for (int x = 0; x < 8; ++x) {
        const TestMesh& mesh = x % 2 == 0 ? small : big;
        REQUIRE(uploads.upload(mesh.update({.x = x}, 1), arena, Time{}).value() ==
                Uploaded::written);
    }
    std::vector<vx::render::ArenaMove> moves;
    // Packed from the start: nothing lower is free.
    CHECK(uploads.compact(2, 1'000, moves).stuck);
    CHECK(moves.empty());
    for (int x = 0; x < 8; x += 2) {
        uploads.remove({.x = x});
    }
    for (std::uint32_t slot : {1U, 2U, 0U}) {
        uploads.begin_frame(slot, Time{});
    }
    const vx::render::ArenaStats fragmented = space.stats();
    const vx::render::Compacted stuck = uploads.compact(8, 1'000, moves);
    CHECK(stuck.stuck);
    CHECK(stuck.meshes == 0);
    CHECK(moves.empty());
    // The range tried and not taken went straight back.
    CHECK(space.stats().live == fragmented.live);
    CHECK(space.stats().free == fragmented.free);

    // Remove one big mesh low down: a hole of 45 opens, and the highest
    // mesh moves into it; the next one finds no other and is stuck.
    uploads.remove({.x = 1});
    for (std::uint32_t slot : {1U, 2U, 0U}) {
        uploads.begin_frame(slot, Time{});
    }
    const vx::render::Compacted one = uploads.compact(8, 1'000, moves);
    CHECK(one.meshes == 1);
    CHECK(one.stuck);
}

TEST_CASE("a frame moves its share of quads, and its first mesh whatever its size") {
    // Five meshes of 44 quads; the lowest two go, leaving 88 free at the
    // start.
    const auto scattered = [](ArenaSpace& space, ChunkUploads& uploads, FakeArena& arena,
                              const TestMesh& mesh) {
        uploads.begin_frame(0, Time{});
        for (int x = 0; x < 5; ++x) {
            REQUIRE(uploads.upload(mesh.update({.x = x}, 1), arena, Time{}).value() ==
                    Uploaded::written);
        }
        uploads.remove({.x = 0});
        uploads.remove({.x = 1});
        for (std::uint32_t slot : {1U, 2U, 0U}) {
            uploads.begin_frame(slot, Time{});
        }
        REQUIRE(space.stats().largest_free >= 88);
    };
    const TestMesh mesh(some, 7);
    {
        ArenaSpace space = make_space(400, 3);
        FakeArena arena(400);
        ChunkUploads uploads(space);
        scattered(space, uploads, arena, mesh);
        // Room for one 44-quad move in 50: the second would pass the share.
        std::vector<vx::render::ArenaMove> moves;
        const vx::render::Compacted done = uploads.compact(8, 50, moves);
        CHECK(done.meshes == 1);
        CHECK_FALSE(done.stuck);
    }
    {
        ArenaSpace space = make_space(400, 3);
        FakeArena arena(400);
        ChunkUploads uploads(space);
        scattered(space, uploads, arena, mesh);
        // A share under one mesh still moves the first: compaction never
        // spins without moving.
        std::vector<vx::render::ArenaMove> moves;
        const vx::render::Compacted done = uploads.compact(8, 43, moves);
        CHECK(done.meshes == 1);
        CHECK(done.quads == 44);
        CHECK_FALSE(done.stuck);
    }
}

TEST_CASE("a repack packs every mesh into a fresh space in their order, and each keeps its "
          "quads") {
    ArenaSpace space = make_space(300, 3);
    FakeArena arena(300);
    ChunkUploads uploads(space);
    const TestMesh big(some, 7);
    const TestMesh small(few, 8);
    std::map<int, const TestMesh*> meshes;
    uploads.begin_frame(0, Time{});
    for (int x = 0; x < 6; ++x) {
        const TestMesh& mesh = x % 3 == 0 ? small : big;
        REQUIRE(uploads.upload(mesh.update({.x = x}, 1), arena, Time{}).value() ==
                Uploaded::written);
        meshes[x] = &mesh;
    }
    uploads.remove({.x = 1});
    meshes.erase(1);
    std::map<std::uint32_t, int> order; // chunk x by first quad, before
    for (const vx::render::ChunkMeshRecord& record : uploads.records()) {
        order[record.range.first_quad] = record.pos.x;
    }

    // Too small for what is live: nothing changes.
    ArenaSpace cramped = make_space(100, 3);
    std::vector<vx::render::ArenaMove> moves;
    CHECK_FALSE(uploads.repack(cramped, moves));
    CHECK(moves.empty());
    CHECK(cramped.stats().live == 0);
    CHECK(every_mesh_in_place(uploads, arena.quads, meshes));

    ArenaSpace fresh = make_space(450, 3);
    REQUIRE(uploads.repack(fresh, moves));
    REQUIRE(moves.size() == 5);
    std::vector<Quad> grown(450);
    copy_moves(moves, arena.quads, grown);
    // The new buffer takes the old one's place, and the fresh space the old
    // space's: its retired ranges go with it.
    arena.quads = grown;
    space = std::move(fresh);
    CHECK(every_mesh_in_place(uploads, arena.quads, meshes));
    // Packed from the start, in the order they lay.
    std::uint32_t next = 0;
    for (const auto& [first, x] : order) {
        const vx::render::ChunkMeshRecord* record = uploads.find({.x = x});
        REQUIRE(record != nullptr);
        CHECK(record->range.first_quad == next);
        next += record->range.quads;
    }
    CHECK(space.stats().live == next);
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().free == 450 - next);
    // Uploads go on in the new space: a replaced mesh's range retires there.
    REQUIRE(uploads.upload(big.update({.x = 0}, 2), arena, Time{}).value() == Uploaded::written);
    meshes[0] = &big;
    CHECK(space.stats().retired == 1);
    CHECK(every_mesh_in_place(uploads, arena.quads, meshes));
}

TEST_CASE("under random uploads, batches, stale offers, removals, frames, compaction and "
          "repacks, each record's range holds its newest mesh, and the arena holds those "
          "ranges alone") {
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
    std::size_t compactions = 0;
    std::size_t repacks = 0;
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
        } else if (kind < 8) {
            // Compaction: the GPU copies within the arena.
            std::vector<vx::render::ArenaMove> moves;
            static_cast<void>(uploads.compact(4, capacity, moves));
            REQUIRE(sources_and_destinations_apart(moves));
            copy_moves(moves, std::vector<Quad>(arena.quads), arena.quads);
            compactions += moves.empty() ? 0 : 1;
        } else if (kind < 9) {
            // A repack into a new buffer of the same size, from which the
            // old one's retired ranges are gone.
            vx::Result<ArenaSpace> fresh = ArenaSpace::create(capacity, 3);
            REQUIRE(fresh.has_value());
            std::vector<vx::render::ArenaMove> moves;
            REQUIRE(uploads.repack(fresh.value(), moves));
            std::vector<Quad> moved(capacity);
            copy_moves(moves, arena.quads, moved);
            arena.quads = moved;
            space = std::move(fresh).value();
            ++repacks;
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
    CHECK(compactions > 20);
    CHECK(repacks > 20);
    // Every slot comes round: nothing waits, and all but the records' ranges
    // is free.
    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        uploads.begin_frame(slot, Time{});
    }
    CHECK(space.stats().retired == 0);
    CHECK(space.stats().free == capacity - space.stats().live);
}
