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
