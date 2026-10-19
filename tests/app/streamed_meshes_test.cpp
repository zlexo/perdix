#include "streamed_meshes.hpp"
#include "world_loop.hpp"

#include <vx/core/frustum.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/result.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// The main thread's side of the world thread's meshes (RFC 0005, RFC 0007):
// meshes, batches and unloads offered to the arena's uploads in the order
// they came, each frame until the budget says later.

namespace {

using vx::ChunkPos;
using vx::Quad;
using vx::app::MeshUpdate;
using vx::app::ToMain;
using vx::render::Uploaded;

// The arena's quads in memory.
class Memory final : public vx::render::QuadWriter {
public:
    vx::Result<void> write(std::uint32_t first, std::span<const Quad> written) override {
        if (quads.size() < first + written.size()) {
            quads.resize(first + written.size());
        }
        std::ranges::copy(written, quads.begin() + first);
        return {};
    }
    std::vector<Quad> quads;
};

// A mesh as a mesh job sends it: `counts` quads in each of the twelve ranges,
// each quad naming its chunk and its place, in a buffer from the pool.
MeshUpdate mesh_of(vx::MeshResultPool& pool, ChunkPos pos, std::uint32_t generation,
                   const std::array<std::uint32_t, 12>& counts) {
    MeshUpdate update;
    update.pos = pos;
    update.generation = generation;
    update.counts = counts;
    std::uint32_t total = 0;
    for (const std::uint32_t count : counts) {
        total += count;
    }
    if (total > 0) {
        update.quads = pool.allocate(std::size_t{total} * sizeof(Quad));
        for (std::uint32_t at = 0; at < total; ++at) {
            const Quad quad{.geometry = static_cast<std::uint32_t>(pos.x), .material = at};
            std::memcpy(update.quads.data() + (std::size_t{at} * sizeof(Quad)), &quad,
                        sizeof(Quad));
        }
    }
    return update;
}

ToMain message_of(MeshUpdate&& update) {
    ToMain message;
    message.kind = ToMain::Kind::mesh;
    message.mesh = std::move(update);
    return message;
}

vx::render::ArenaSpace make_space(std::uint32_t quads) {
    vx::Result<vx::render::ArenaSpace> space = vx::render::ArenaSpace::create(quads, 2);
    REQUIRE(space.has_value());
    return std::move(space).value();
}

ToMain unload_of(ChunkPos pos, std::uint8_t level = 0) {
    ToMain unload;
    unload.kind = ToMain::Kind::unload;
    unload.pos = pos;
    unload.level = level;
    return unload;
}

// A mesh as mesh_of makes it, on RFC 0011's `level`.
MeshUpdate mesh_on(vx::MeshResultPool& pool, std::uint8_t level, ChunkPos pos,
                   std::uint32_t generation, const std::array<std::uint32_t, 12>& counts) {
    MeshUpdate update = mesh_of(pool, pos, generation, counts);
    update.level = level;
    return update;
}

ToMain batch_of(std::vector<MeshUpdate>&& meshes) {
    ToMain batch;
    batch.kind = ToMain::Kind::batch;
    batch.batch = std::move(meshes);
    return batch;
}

// One frame's offers: the uploads begin a frame, in two frame slots taken
// in turn, and take what they can.
struct Arena {
    explicit Arena(std::uint32_t quads = 4'096) : space(make_space(quads)) {}

    vx::render::ArenaSpace space;
    vx::render::ChunkUploads uploads{space};
    Memory memory;
    std::uint32_t frame = 0;

    void offer(vx::app::StreamedMeshes& meshes) {
        uploads.begin_frame(frame++ % 2, vx::render::ChunkUploads::Clock::now());
        vx::render::ChunkUploader uploader(uploads, memory);
        REQUIRE(meshes.apply(uploader).has_value());
    }
};

} // namespace

TEST_CASE("a mesh's ranges lie over its buffer in RFC 0003's order, back to back") {
    vx::MeshResultPool pool;
    const std::array<std::uint32_t, 12> counts{1, 0, 2, 0, 0, 3, 0, 0, 1, 0, 0, 0};
    const MeshUpdate update = mesh_of(pool, {.x = 7}, 1, counts);
    const vx::render::ChunkMeshUpdate chunk = vx::app::chunk_update(update);
    CHECK(chunk.pos == ChunkPos{.x = 7});
    CHECK(chunk.generation == 1);
    std::uint32_t next = 0;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face = 0; face < 6; ++face) {
            const std::span<const Quad> range = chunk.mesh.ranges[pass][face];
            REQUIRE(range.size() == counts[(pass * 6) + face]);
            for (const Quad& quad : range) {
                CHECK(quad.material == next++);
            }
        }
    }
    // An empty mesh has no buffer, and no quads.
    const MeshUpdate empty = mesh_of(pool, {.x = 8}, 1, {});
    CHECK(vx::app::chunk_update(empty).mesh.all().empty());
}

TEST_CASE("meshes go in the order they came, stop when the budget says later, and go on") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    std::vector<ToMain> messages;
    for (std::int32_t x = 0; x < 3; ++x) {
        messages.push_back(message_of(mesh_of(pool, {.x = x}, 1, {2})));
    }
    // A player's frame is the caller's: it stays.
    ToMain player;
    player.kind = ToMain::Kind::player;
    messages.push_back(std::move(player));
    meshes.receive(messages);
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().kind == ToMain::Kind::player);
    CHECK(meshes.waiting() == 3);
    // With no budget, a frame takes its first mesh alone.
    Arena arena;
    arena.uploads.set_budget(std::chrono::nanoseconds(0));
    arena.offer(meshes);
    CHECK(meshes.waiting() == 2);
    CHECK(meshes.done() == 1);
    REQUIRE(arena.uploads.find({.x = 0}) != nullptr);
    CHECK(arena.uploads.find({.x = 1}) == nullptr);
    arena.offer(meshes);
    arena.offer(meshes);
    CHECK(meshes.waiting() == 0);
    CHECK(meshes.done() == 3);
    CHECK(arena.uploads.records().size() == 3);
}

TEST_CASE("unloads go first, and a chunk's meshes that came before its unload are dropped") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::hours(1)); // a frame takes every mesh
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 3}, 1, {2})));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(arena.uploads.find({.x = 3}) != nullptr);
    // Chunk 1's first mesh came before its unload, its second after: shown
    // again. Chunk 4's only mesh came before its unload.
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 2, {3})));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 3, {1})));
    messages.push_back(message_of(mesh_of(pool, {.x = 4}, 4, {1})));
    messages.push_back(unload_of({.x = 1}));
    messages.push_back(unload_of({.x = 3}));
    messages.push_back(unload_of({.x = 4}));
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 5, {2})));
    meshes.receive(messages);
    CHECK(meshes.waiting() == 7);
    arena.offer(meshes);
    CHECK(meshes.waiting() == 0);
    CHECK(arena.uploads.find({.x = 3}) == nullptr);
    CHECK(arena.uploads.find({.x = 4}) == nullptr);
    REQUIRE(arena.uploads.find({.x = 1}) != nullptr);
    CHECK(arena.uploads.find({.x = 1})->generation == 5);
    CHECK(arena.uploads.find({.x = 2}) != nullptr);
    // Every mesh sent alone is done: three written, two dropped.
    CHECK(meshes.done() == 5);
    for (const vx::MeshPoolClass& size_class : pool.stats()) {
        CHECK(size_class.in_use == 0);
    }
}

TEST_CASE("an edit's batch goes ahead of the meshes waiting, whole whatever the budget, never "
          "counted") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::nanoseconds(0));
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 1, {2})));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 2, {2})));
    std::vector<MeshUpdate> edit;
    edit.push_back(mesh_of(pool, {.x = 3}, 3, {1}));
    edit.push_back(mesh_of(pool, {.x = 4}, 4, {1}));
    messages.push_back(batch_of(std::move(edit)));
    meshes.receive(messages);
    arena.offer(meshes);
    // The batch went whole; with no budget, its uploads left none for the
    // meshes, which follow in the next frames.
    CHECK(arena.uploads.find({.x = 3}) != nullptr);
    CHECK(arena.uploads.find({.x = 4}) != nullptr);
    CHECK(arena.uploads.find({.x = 1}) == nullptr);
    CHECK(meshes.done() == 0);
    arena.offer(meshes);
    arena.offer(meshes);
    CHECK(arena.uploads.find({.x = 2}) != nullptr);
    CHECK(meshes.done() == 2);
}

TEST_CASE("an unload takes its chunk out of a batch that came before it, not one after it") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    std::vector<ToMain> messages;
    std::vector<MeshUpdate> before;
    before.push_back(mesh_of(pool, {.x = 1}, 1, {1}));
    before.push_back(mesh_of(pool, {.x = 2}, 2, {1}));
    messages.push_back(batch_of(std::move(before)));
    messages.push_back(unload_of({.x = 1}));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(arena.uploads.find({.x = 1}) == nullptr);
    CHECK(arena.uploads.find({.x = 2}) != nullptr);
    // Unloaded, then shown again and edited: the batch after the unload
    // stays whole.
    messages.push_back(unload_of({.x = 2}));
    std::vector<MeshUpdate> after;
    after.push_back(mesh_of(pool, {.x = 2}, 4, {1}));
    messages.push_back(batch_of(std::move(after)));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(arena.uploads.find({.x = 2}) != nullptr);
    CHECK(arena.uploads.find({.x = 2})->generation == 4);
    for (const vx::MeshPoolClass& size_class : pool.stats()) {
        CHECK(size_class.in_use == 0);
    }
}

TEST_CASE("a mesh that does not fit waits, and the unloads behind it make it room") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena(100);
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 1, {60})));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 2, {60})));
    meshes.receive(messages);
    arena.offer(meshes);
    // Chunk 2's mesh does not fit beside chunk 1's: it waits.
    CHECK(arena.uploads.find({.x = 1}) != nullptr);
    CHECK(meshes.waiting() == 1);
    CHECK(meshes.done() == 1);
    messages.push_back(unload_of({.x = 1}));
    meshes.receive(messages);
    // Chunk 1's range comes back once its frame slot comes round again.
    for (int frame = 0; frame < 4 && meshes.waiting() > 0; ++frame) {
        arena.offer(meshes);
    }
    CHECK(meshes.waiting() == 0);
    CHECK(arena.uploads.find({.x = 1}) == nullptr);
    CHECK(arena.uploads.find({.x = 2}) != nullptr);
    CHECK(meshes.done() == 2);
}

TEST_CASE("a mesh waiting for room goes with its chunk's unload, and what follows goes on") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena(100);
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 1, {60})));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 2, {60})));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(meshes.waiting() == 1);
    messages.push_back(unload_of({.x = 2}));
    messages.push_back(message_of(mesh_of(pool, {.x = 3}, 3, {30})));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(meshes.waiting() == 0);
    CHECK(arena.uploads.find({.x = 2}) == nullptr);
    CHECK(arena.uploads.find({.x = 3}) != nullptr);
    // Written, dropped, written.
    CHECK(meshes.done() == 3);
    for (const vx::MeshPoolClass& size_class : pool.stats()) {
        CHECK(size_class.in_use == 0);
    }
}

TEST_CASE(
    "a batch that does not fit waits whole, ahead of its chunks' meshes, and the edits after it") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena(100);
    arena.uploads.set_budget(std::chrono::hours(1));
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 1, {60})));
    meshes.receive(messages);
    arena.offer(meshes);
    // An edit's batch of 50 quads does not fit beside chunk 1's 60; another
    // edit follows it, then a newer mesh of one of the first batch's chunks.
    std::vector<MeshUpdate> first;
    first.push_back(mesh_of(pool, {.x = 2}, 2, {25}));
    first.push_back(mesh_of(pool, {.x = 3}, 3, {25}));
    messages.push_back(batch_of(std::move(first)));
    std::vector<MeshUpdate> second;
    second.push_back(mesh_of(pool, {.x = 4}, 4, {10}));
    messages.push_back(batch_of(std::move(second)));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 5, {10})));
    meshes.receive(messages);
    arena.offer(meshes);
    // Nothing went: chunk 2's newer mesh would show without the edit's other
    // half.
    CHECK(arena.uploads.find({.x = 2}) == nullptr);
    CHECK(arena.uploads.find({.x = 4}) == nullptr);
    CHECK(meshes.waiting() == 3);
    // Chunk 1 unloads; once its range comes back, both batches go in the
    // same frame, whole, and the mesh after them.
    messages.push_back(unload_of({.x = 1}));
    meshes.receive(messages);
    for (int frame = 0; frame < 4 && meshes.waiting() > 0; ++frame) {
        arena.offer(meshes);
        const bool edited = arena.uploads.find({.x = 3}) != nullptr;
        CHECK(edited == (arena.uploads.find({.x = 4}) != nullptr));
        CHECK(edited == (arena.uploads.find({.x = 2}) != nullptr));
    }
    CHECK(meshes.waiting() == 0);
    REQUIRE(arena.uploads.find({.x = 2}) != nullptr);
    CHECK(arena.uploads.find({.x = 2})->generation == 5);
    CHECK(arena.uploads.find({.x = 3}) != nullptr);
    CHECK(arena.uploads.find({.x = 4}) != nullptr);
    CHECK(meshes.done() == 2);
    for (const vx::MeshPoolClass& size_class : pool.stats()) {
        CHECK(size_class.in_use == 0);
    }
}

TEST_CASE("the holes meshes waiting leave: the newest of a chunk with quads, none on screen, in "
          "view, each chunk once") {
    vx::MeshResultPool pool;
    Arena arena;
    vx::app::StreamedMeshes meshes;
    constexpr std::array<std::uint32_t, 12> some{1};
    // At the middle of chunk 0, looking along +x.
    vx::Viewer viewer = vx::viewer_at({});
    viewer.view = vx::Frustum({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = 1.0},
                              {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7, 16.0 / 9.0, 0.05);
    const auto holes = [&] {
        return meshes.holes(arena.uploads, viewer, vx::app::near_holes_distance);
    };
    // One on screen already.
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 5}, 1, some)));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(meshes.waiting() == 0);
    CHECK(holes().all == 0);
    // Ahead, near: one with two meshes, one empty, one unloaded after its
    // mesh came, one on screen with a newer one waiting, and one in a batch.
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 1, some)));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 2, some)));
    messages.push_back(message_of(mesh_of(pool, {.x = 3}, 1, {})));
    messages.push_back(message_of(mesh_of(pool, {.x = 4}, 1, some)));
    messages.push_back(unload_of({.x = 4}));
    messages.push_back(message_of(mesh_of(pool, {.x = 5}, 2, some)));
    std::vector<MeshUpdate> batch;
    batch.push_back(mesh_of(pool, {.x = 6}, 1, some));
    messages.push_back(batch_of(std::move(batch)));
    // Behind, and ahead beyond 256 blocks.
    messages.push_back(message_of(mesh_of(pool, {.x = -3}, 1, some)));
    messages.push_back(message_of(mesh_of(pool, {.x = 12}, 1, some)));
    // A newer empty one: nothing left to draw.
    messages.push_back(message_of(mesh_of(pool, {.x = 7}, 1, some)));
    messages.push_back(message_of(mesh_of(pool, {.x = 7}, 2, {})));
    meshes.receive(messages);
    const vx::app::Holes waiting = holes();
    CHECK(waiting.all == 3);  // 2, 6 and 12
    CHECK(waiting.near == 2); // 2 and 6
    // Drawn, they are holes no more.
    for (int frame = 0; frame < 20 && meshes.waiting() > 0; ++frame) {
        arena.offer(meshes);
    }
    REQUIRE(meshes.waiting() == 0);
    CHECK(holes().all == 0);
}

TEST_CASE("samples go to their reader in order: after the meshes before them, before those after") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    const auto sample_of = [](std::uint32_t id) {
        ToMain sample;
        sample.kind = ToMain::Kind::sample;
        sample.sample.id = id;
        return sample;
    };
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_of(pool, {.x = 1}, 1, {1})));
    messages.push_back(sample_of(1));
    messages.push_back(message_of(mesh_of(pool, {.x = 2}, 1, {1})));
    messages.push_back(unload_of({.x = 1}));
    messages.push_back(sample_of(2));
    ToMain player;
    player.kind = ToMain::Kind::player;
    messages.push_back(std::move(player));
    // What waits when each sample is read.
    std::vector<std::pair<std::uint32_t, std::size_t>> seen;
    meshes.receive(messages, [&](const vx::app::WorldSample& sample) {
        seen.emplace_back(sample.id, meshes.waiting());
    });
    REQUIRE(seen.size() == 2);
    CHECK(seen[0] == std::pair<std::uint32_t, std::size_t>{1, 1});
    CHECK(seen[1] == std::pair<std::uint32_t, std::size_t>{2, 3});
    // The rest stay for the caller.
    REQUIRE(messages.size() == 1);
    CHECK(messages.front().kind == ToMain::Kind::player);
}

TEST_CASE("a chunk is its position and its level: each level's mesh is its own, and an unload "
          "takes only its level's") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::hours(1));
    constexpr ChunkPos at{.x = 2, .y = 1, .z = -3};
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_on(pool, 0, at, 1, {2})));
    messages.push_back(message_of(mesh_on(pool, 1, at, 2, {3})));
    messages.push_back(message_of(mesh_on(pool, 5, at, 3, {1})));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(arena.uploads.find(at, 0) != nullptr);
    REQUIRE(arena.uploads.find(at, 1) != nullptr);
    REQUIRE(arena.uploads.find(at, 5) != nullptr);
    CHECK(arena.uploads.find(at, 1)->level == 1);
    // Level 1's unload, and a level-5 mesh that came before level 0's: the
    // level-1 chunk goes, the others stay, the level-5 mesh is shown.
    messages.push_back(message_of(mesh_on(pool, 5, at, 4, {2})));
    messages.push_back(unload_of(at, 1));
    messages.push_back(message_of(mesh_on(pool, 0, at, 5, {1})));
    messages.push_back(unload_of(at, 0));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(meshes.waiting() == 0);
    CHECK(arena.uploads.find(at, 1) == nullptr);
    CHECK(arena.uploads.find(at, 0) == nullptr);
    REQUIRE(arena.uploads.find(at, 5) != nullptr);
    CHECK(arena.uploads.find(at, 5)->generation == 4);
    // Every mesh sent alone is done: four written, one dropped.
    CHECK(meshes.done() == 5);
}

TEST_CASE("an unload takes its own level's chunk out of a waiting batch, not another level's") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::hours(1));
    constexpr ChunkPos at{.x = 4};
    std::vector<ToMain> messages;
    std::vector<MeshUpdate> batch;
    batch.push_back(mesh_on(pool, 0, at, 1, {1}));
    batch.push_back(mesh_on(pool, 0, {.x = 5}, 1, {1}));
    messages.push_back(batch_of(std::move(batch)));
    messages.push_back(unload_of(at, 2));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(arena.uploads.find(at, 0) != nullptr);
    CHECK(arena.uploads.find({.x = 5}, 0) != nullptr);
}

TEST_CASE("the holes a coarse mesh leaves count its own box: in view and near at its level's "
          "size") {
    vx::MeshResultPool pool;
    Arena arena;
    vx::app::StreamedMeshes meshes;
    constexpr std::array<std::uint32_t, 12> some{1};
    // At the middle of chunk 0, looking along +x.
    vx::Viewer viewer = vx::viewer_at({});
    viewer.view = vx::Frustum({.x = 1.0, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = 1.0},
                              {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7, 16.0 / 9.0, 0.05);
    arena.uploads.set_budget(std::chrono::hours(1));
    // On screen: a level-0 mesh at x = 3.
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_on(pool, 0, {.x = 3}, 1, some)));
    meshes.receive(messages);
    arena.offer(meshes);
    REQUIRE(arena.uploads.find({.x = 3}, 0) != nullptr);
    // Waiting at the same position, two more chunks, neither newer than the
    // other nor covered by the level-0 mesh on screen: level 1's spans 192
    // to 256 blocks ahead, near; level 2's spans 384 to 512 ahead, in view
    // and not near, where its level-0 namesake is 96 ahead and near.
    messages.push_back(message_of(mesh_on(pool, 2, {.x = 3}, 1, some)));
    messages.push_back(message_of(mesh_on(pool, 1, {.x = 3}, 2, some)));
    meshes.receive(messages);
    const vx::app::Holes waiting =
        meshes.holes(arena.uploads, viewer, vx::app::near_holes_distance);
    CHECK(waiting.all == 2);
    CHECK(waiting.near == 1);
}

TEST_CASE("an edit's batch waiting for room holds back its own chunks' meshes, not another "
          "level's at the same position") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena(100);
    constexpr ChunkPos at{.x = 1};
    std::vector<ToMain> messages;
    // A batch too big for the arena waits for room.
    std::vector<MeshUpdate> batch;
    batch.push_back(mesh_on(pool, 0, at, 1, {60}));
    batch.push_back(mesh_on(pool, 0, {.x = 2}, 1, {60}));
    messages.push_back(batch_of(std::move(batch)));
    messages.push_back(message_of(mesh_on(pool, 1, at, 2, {10})));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(arena.uploads.find(at, 0) == nullptr);
    CHECK(arena.uploads.find(at, 1) != nullptr);
    // Level 0's mesh of a batch member waits behind the batch.
    messages.push_back(message_of(mesh_on(pool, 0, at, 3, {10})));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(arena.uploads.find(at, 0) == nullptr);
}

namespace {

ToMain swap_of(std::vector<vx::LodPos> show, std::vector<vx::LodPos> hide) {
    ToMain swap;
    swap.kind = ToMain::Kind::swap;
    swap.show = std::move(show);
    swap.hide = std::move(hide);
    return swap;
}

ChunkPos at(vx::LodPos pos) {
    return {.x = pos.x, .y = pos.y, .z = pos.z};
}

bool shown(const Arena& arena, vx::LodPos pos) {
    const vx::render::ChunkMeshRecord* record = arena.uploads.find(at(pos), pos.level);
    return record != nullptr && record->shown;
}

} // namespace

TEST_CASE("RFC 0011's swaps wait until every chunk they show has a mesh in the arena, then hide "
          "and show at once") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::hours(1));
    arena.uploads.use_swaps();
    constexpr vx::LodPos coarse{.x = 1, .y = 0, .z = 0, .level = 1};
    constexpr vx::LodPos west{.x = 2, .y = 0, .z = 0, .level = 0};
    constexpr vx::LodPos east{.x = 3, .y = 0, .z = 0, .level = 0};
    constexpr std::array<std::uint32_t, 12> some{2};
    std::vector<ToMain> messages;
    messages.push_back(message_of(mesh_on(pool, coarse.level, at(coarse), 1, some)));
    messages.push_back(swap_of({coarse}, {}));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(shown(arena, coarse));
    // The coarse chunk's children, one mesh still on its way: nothing
    // changes, the child already in the arena hidden.
    messages.push_back(message_of(mesh_on(pool, 0, at(west), 2, some)));
    messages.push_back(swap_of({west, east}, {coarse}));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(shown(arena, coarse));
    REQUIRE(arena.uploads.find(at(west)) != nullptr);
    CHECK_FALSE(shown(arena, west));
    CHECK(meshes.waiting() == 1);
    // The last mesh: both children show, and the coarse chunk goes, in the
    // same frame.
    messages.push_back(message_of(mesh_on(pool, 0, at(east), 3, some)));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK_FALSE(shown(arena, coarse));
    CHECK(shown(arena, west));
    CHECK(shown(arena, east));
    CHECK(meshes.waiting() == 0);
}

TEST_CASE("swaps go in order, and an unload takes its chunk out of a waiting swap") {
    vx::MeshResultPool pool;
    vx::app::StreamedMeshes meshes;
    Arena arena;
    arena.uploads.set_budget(std::chrono::hours(1));
    arena.uploads.use_swaps();
    constexpr vx::LodPos first{.x = 5, .y = 1, .z = 0, .level = 2};
    constexpr vx::LodPos second{.x = 6, .y = 1, .z = 0, .level = 2};
    constexpr vx::LodPos gone{.x = 7, .y = 1, .z = 0, .level = 2};
    constexpr std::array<std::uint32_t, 12> some{1};
    std::vector<ToMain> messages;
    // The first swap waits for a mesh that will not come; the second's is
    // there, and waits behind it.
    messages.push_back(message_of(mesh_on(pool, second.level, at(second), 1, some)));
    messages.push_back(swap_of({first, gone}, {}));
    messages.push_back(swap_of({second}, {}));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK_FALSE(shown(arena, second));
    // The first's mesh comes and the chunk it was still waiting for unloads:
    // both swaps go.
    messages.push_back(message_of(mesh_on(pool, first.level, at(first), 2, some)));
    messages.push_back(unload_of(at(gone), gone.level));
    meshes.receive(messages);
    arena.offer(meshes);
    CHECK(shown(arena, first));
    CHECK(shown(arena, second));
    CHECK(meshes.waiting() == 0);
}
