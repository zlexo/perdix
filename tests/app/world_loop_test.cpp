#include "world_loop.hpp"

#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

// RFC 0005's world loop over RFC 0006's streamer: meshes reach the main
// thread in one ordered stream, and the end state does not depend on how
// many workers ran the jobs.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ChunkState;
using vx::app::ToMain;
using vx::app::ToWorld;
using vx::app::WorldLoop;

constexpr BlockState stone = static_cast<BlockState>(1);
constexpr vx::PlaceholderBlocks blocks{
    .grass = static_cast<BlockState>(3), .dirt = static_cast<BlockState>(2), .stone = stone};

// Air, then stone, dirt and grass, each opaque with a texture of its own.
std::vector<vx::BlockVisual> visuals() {
    const auto opaque = [](std::uint16_t texture) {
        return vx::BlockVisual{.opacity = vx::Opacity::opaque,
                               .cull_group = 0,
                               .texture = {texture, texture, texture, texture, texture, texture}};
    };
    return {vx::BlockVisual{}, opaque(1), opaque(2), opaque(3)};
}

constexpr vx::RegionSettings small{
    .view_distance = 3, .near_distance = 1, .near_height = 1, .hysteresis = 2};

constexpr ChunkPos above_hills{.x = 3, .y = 2, .z = 3};

using Chunk = std::tuple<std::int32_t, std::int32_t, std::int32_t>;

Chunk chunk_of(ChunkPos pos) {
    return {pos.x, pos.y, pos.z};
}

// Messages are filled in field by field: each kind uses a few fields.
void move_camera(WorldLoop& loop, ChunkPos camera) {
    ToWorld message;
    message.kind = ToWorld::Kind::camera;
    message.camera = camera;
    loop.to_world().push(std::move(message));
}

// The chunks drawn and complete: those the main thread should have meshes of.
std::set<Chunk> shown(const WorldLoop& loop) {
    std::set<Chunk> out;
    const vx::ChunkRecords& records = loop.streamer().records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index) && records[index].drawn &&
            records[index].state == ChunkState::complete) {
            out.insert(chunk_of(records[index].pos));
        }
    }
    return out;
}

} // namespace

TEST_CASE("the region streams in, and each drawn chunk's mesh reaches the main thread once") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    std::set<Chunk> meshed;
    for (const ToMain& message : messages) {
        REQUIRE(message.kind == ToMain::Kind::mesh);
        CHECK(meshed.insert(chunk_of(message.mesh.pos)).second);
    }
    CHECK(meshed == shown(loop));
    CHECK(meshed.size() > 27);
    CHECK(loop.meshes_in_flight() == 0);
}
