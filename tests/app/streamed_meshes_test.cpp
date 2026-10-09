#include "streamed_meshes.hpp"
#include "world_loop.hpp"

#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/result.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>

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

ToMain unload_of(ChunkPos pos) {
    ToMain unload;
    unload.kind = ToMain::Kind::unload;
    unload.pos = pos;
    return unload;
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
