#include "world_loop.hpp"
#include "world_thread.hpp"

#include <vx/core/frustum.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/life.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

// RFC 0005's world loop over RFC 0006's streamer: meshes reach the main
// thread in one ordered stream, an edit's remeshes together, and the end
// state does not depend on how many workers ran the jobs.

namespace {

using vx::BlockState;
using vx::ChunkPos;
using vx::ChunkState;
using vx::app::SampleKind;
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
    message.camera = vx::viewer_at(camera);
    loop.to_world().push(std::move(message));
}

void edit(WorldLoop& loop, vx::BlockPos block, BlockState state) {
    ToWorld message;
    message.kind = ToWorld::Kind::edit;
    message.block = block;
    message.state = state;
    loop.to_world().push(std::move(message));
}

void stop(WorldLoop& loop) {
    ToWorld message;
    message.kind = ToWorld::Kind::stop;
    loop.to_world().push(std::move(message));
}

void uploaded(WorldLoop& loop, std::uint64_t count) {
    ToWorld message;
    message.kind = ToWorld::Kind::uploaded;
    message.uploaded = count;
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

// A hash of a mesh: its counts by pass and face, then its quads' bytes.
std::uint64_t mesh_hash(std::span<const std::uint32_t, 12> counts, const std::byte* quads) {
    std::uint64_t value = 0xcbf2'9ce4'8422'2325ULL;
    std::size_t total = 0;
    for (const std::uint32_t count : counts) {
        value = (value ^ count) * 0x100'0000'01B3ULL;
        total += count;
    }
    for (std::size_t at = 0; at < total * sizeof(vx::Quad); ++at) {
        value = (value ^ std::to_integer<std::uint64_t>(quads[at])) * 0x100'0000'01B3ULL;
    }
    return value;
}

// What the main thread holds after the messages: each chunk's newest mesh,
// by its quads' bytes and counts.
class MainThread {
public:
    void receive(std::vector<ToMain>& messages) {
        for (ToMain& message : messages) {
            switch (message.kind) {
            case ToMain::Kind::mesh:
                take(message.mesh);
                ++meshes;
                break;
            case ToMain::Kind::batch:
                for (vx::app::MeshUpdate& update : message.batch) {
                    take(update);
                }
                ++batches;
                last_batch.clear();
                for (const vx::app::MeshUpdate& update : message.batch) {
                    last_batch.push_back(chunk_of(update.pos));
                }
                break;
            case ToMain::Kind::unload:
                held.erase(chunk_of(message.pos));
                ++unloads;
                break;
            case ToMain::Kind::player:
                player = message.player;
                tick_times.push_back(message.tick_at);
                break;
            case ToMain::Kind::perdix:
                perdix = message.player;
                ++perdix_frames;
                break;
            case ToMain::Kind::landed:
                landings.push_back(message.landed);
                break;
            case ToMain::Kind::sample:
                samples.push_back(message.sample);
                break;
            case ToMain::Kind::report:
                reports.push_back(message.report);
                break;
            case ToMain::Kind::swap:
                ++swaps;
                break;
            }
        }
    }

    // A hash of everything held, in chunk order.
    [[nodiscard]] std::uint64_t hash() const {
        std::uint64_t value = 0xcbf2'9ce4'8422'2325ULL;
        for (const auto& [chunk, mesh] : held) {
            const auto& [x, y, z] = chunk;
            for (const std::uint64_t part :
                 {std::uint64_t(std::uint32_t(x)), std::uint64_t(std::uint32_t(y)),
                  std::uint64_t(std::uint32_t(z)), mesh.bytes}) {
                value = (value ^ part) * 0x100'0000'01B3ULL;
            }
        }
        return value;
    }

    struct Held {
        std::uint32_t generation = 0;
        std::uint64_t bytes = 0; // a hash of the counts and quads
    };
    std::map<Chunk, Held> held;
    std::size_t meshes = 0;
    std::size_t batches = 0;
    std::size_t unloads = 0;
    std::vector<Chunk> last_batch;
    vx::app::WalkingFrame player;
    vx::app::WalkingFrame perdix{};
    std::size_t perdix_frames = 0;
    std::vector<std::chrono::steady_clock::time_point> tick_times;
    std::vector<bool> landings;
    std::vector<vx::app::WorldSample> samples;
    std::vector<vx::app::WorldReport> reports;
    std::size_t swaps = 0;

private:
    void take(const vx::app::MeshUpdate& update) {
        Held& slot = held[chunk_of(update.pos)];
        // An older mesh than the one held is dropped, as RFC 0007 drops it.
        if (update.generation < slot.generation) {
            return;
        }
        slot.generation = update.generation;
        // An empty mesh has no buffer.
        const bool empty =
            std::ranges::all_of(update.counts, [](std::uint32_t count) { return count == 0; });
        CHECK(update.quads.empty() == empty);
        slot.bytes = mesh_hash(update.counts, update.quads.data());
    }
};

// RFC 0003's mesher run here over the blocks the loop holds now: the mesh the
// main thread should hold for a chunk once the loop has settled, on level 0
// or one of RFC 0011's coarse levels.
class Mesher {
public:
    std::uint64_t hash(const WorldLoop& loop, ChunkPos pos,
                       std::span<const vx::BlockVisual> visuals, vx::MeshOptions options = {},
                       std::uint8_t level = 0) {
        std::array<vx::ChunkStorage, 27> uniform;
        const auto chunk_at = [&](int dx, int dy, int dz) -> const vx::ChunkStorage* {
            const vx::ChunkRecord* record =
                loop.streamer(level).find({.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz});
            REQUIRE(record != nullptr);
            if (record->blocks != nullptr) {
                return &record->blocks->value();
            }
            vx::ChunkStorage& made =
                uniform[static_cast<std::size_t>((dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1)))];
            made = vx::ChunkStorage::uniform(record->uniform);
            return &made;
        };
        vx::ChunkNeighborhood neighborhood(*chunk_at(0, 0, 0));
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 || dy != 0 || dz != 0) {
                        neighborhood.set(dx, dy, dz, chunk_at(dx, dy, dz));
                    }
                }
            }
        }
        vx::assemble_padded(neighborhood, *padded_);
        const vx::ChunkMesh mesh = vx::mesh_chunk(*padded_, visuals, scratch_, options);
        std::array<std::uint32_t, 12> counts{};
        for (const vx::Pass pass : {vx::Pass::opaque, vx::Pass::cutout}) {
            for (std::size_t face = 0; face < 6; ++face) {
                counts[(static_cast<std::size_t>(pass) * 6) + face] = static_cast<std::uint32_t>(
                    mesh.range(pass, static_cast<vx::Face>(face)).size());
            }
        }
        return mesh_hash(counts, std::as_bytes(mesh.all()).data());
    }

private:
    std::unique_ptr<vx::PaddedChunk> padded_ = std::make_unique<vx::PaddedChunk>();
    vx::MeshScratch scratch_;
};

// Chunks with blocks: once no job runs, the only versions alive, unless a
// pin was never given back.
std::size_t chunks_with_blocks(const WorldLoop& loop) {
    std::size_t count = 0;
    const vx::ChunkRecords& records = loop.streamer().records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (records.live(index) && records[index].blocks != nullptr) {
            ++count;
        }
    }
    return count;
}

// A hash of every chunk the loop holds: position, state and blocks.
std::uint64_t chunk_hash(const WorldLoop& loop) {
    std::map<Chunk, std::uint64_t> each;
    const vx::ChunkRecords& records = loop.streamer().records();
    std::vector<BlockState> dense(vx::chunk_volume);
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!records.live(index)) {
            continue;
        }
        const vx::ChunkRecord& record = records[index];
        std::uint64_t value = static_cast<std::uint64_t>(record.state);
        if (record.blocks != nullptr) {
            record.blocks->value().to_dense(std::span<BlockState, vx::chunk_volume>(dense));
            for (const BlockState block : dense) {
                value = (value ^ static_cast<std::uint64_t>(block)) * 0x100'0000'01B3ULL;
            }
        } else {
            value = (value ^ static_cast<std::uint64_t>(record.uniform)) * 0x100'0000'01B3ULL;
        }
        each[chunk_of(record.pos)] = value;
    }
    std::uint64_t value = 0xcbf2'9ce4'8422'2325ULL;
    for (const auto& [chunk, part] : each) {
        const auto& [x, y, z] = chunk;
        value =
            (value ^ std::uint64_t(std::uint32_t(x) + (std::uint64_t(std::uint32_t(z)) << 32U))) *
            0x100'0000'01B3ULL;
        value = (value ^ std::uint64_t(std::uint32_t(y))) * 0x100'0000'01B3ULL;
        value = (value ^ part) * 0x100'0000'01B3ULL;
    }
    return value;
}

// A drawn, complete chunk in column (x, 3) at the ground, and the block on its
// west border at the middle of its height and depth.
std::optional<ChunkPos> ground_chunk(const WorldLoop& loop, std::int32_t x) {
    const std::set<Chunk> drawn = shown(loop);
    for (std::int32_t y = 0; y <= 3; ++y) {
        if (drawn.contains({x, y, 3}) && drawn.contains({x - 1, y, 3})) {
            return ChunkPos{.x = x, .y = y, .z = 3};
        }
    }
    return std::nullopt;
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

TEST_CASE("a break on a chunk border delivers both meshes in one batch") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    const std::optional<ChunkPos> east = ground_chunk(loop, 1);
    REQUIRE(east.has_value());
    // The block on chunk x = 1's west face: its neighbor at x = 0 pads it.
    edit(loop, {.x = 32, .y = (east->y * 32) + 16, .z = (3 * 32) + 16}, BlockState::air);
    loop.settle();
    loop.take(messages);
    MainThread main;
    main.receive(messages);
    CHECK(main.batches == 1);
    CHECK(main.meshes == 0);
    std::vector<Chunk> expected = {chunk_of(*east), {0, east->y, 3}};
    std::ranges::sort(expected);
    std::ranges::sort(main.last_batch);
    CHECK(main.last_batch == expected);
}

TEST_CASE("a border break's batch waits for the newest remesh, and loses a chunk that unloads") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    const std::optional<ChunkPos> east = ground_chunk(loop, 1);
    REQUIRE(east.has_value());
    const std::int32_t y = (east->y * 32) + 16;
    SUBCASE("a newer remesh of one of them lands first") {
        edit(loop, {.x = 32, .y = y, .z = 112}, BlockState::air);
        loop.pass(); // the two remeshes start
        CHECK(jobs.queued(vx::JobLevel::urgent) == 2);
        // A second edit inside the east chunk only, while they run.
        edit(loop, {.x = 40, .y = y, .z = 112}, BlockState::air);
        loop.settle();
        loop.take(messages);
        MainThread main;
        main.receive(messages);
        CHECK(main.batches == 1);
        CHECK(main.meshes == 0);
        CHECK(main.last_batch.size() == 2);
    }
    SUBCASE("the other chunk unloads before the batch is done") {
        edit(loop, {.x = 32, .y = y, .z = 112}, BlockState::air);
        loop.pass(); // the two remeshes start
        // Six columns east of x = 0, outside the region grown for
        // hysteresis; x = 1 is five away, still inside.
        move_camera(loop, {.x = 6, .y = above_hills.y, .z = 3});
        loop.settle();
        loop.take(messages);
        MainThread main;
        main.receive(messages);
        CHECK(main.batches == 1);
        CHECK(main.last_batch == std::vector<Chunk>{chunk_of(*east)});
        CHECK(main.unloads > 0);
        // The canceled remesh gave its pins back: a write to the chunk it
        // read, still loaded beside x = 1, leaves no frozen copy behind.
        edit(loop, {.x = 16, .y = y, .z = 112}, BlockState::air);
        loop.settle();
        CHECK(loop.streamer().stats().block_versions == chunks_with_blocks(loop));
    }
}

TEST_CASE("a break next to a chunk that is not drawn sends its batch without that chunk") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    // Column x = 6 is at the view distance; x = 7 beyond it is loaded, so x = 6
    // can be meshed, but not drawn. A break on their border changes both.
    const std::set<Chunk> drawn = shown(loop);
    std::optional<std::int32_t> ground;
    for (std::int32_t y = -2; y <= 5 && !ground; ++y) {
        const vx::ChunkRecord* beyond = loop.streamer().find({.x = 7, .y = y, .z = 3});
        if (drawn.contains({6, y, 3}) && beyond != nullptr && !beyond->drawn &&
            vx::counts_as_ready(beyond->state)) {
            ground = y;
        }
    }
    REQUIRE(ground.has_value());
    edit(loop, {.x = (7 * 32) - 1, .y = (*ground * 32) + 16, .z = 112}, BlockState::air);
    loop.settle();
    loop.take(messages);
    MainThread main;
    main.receive(messages);
    CHECK(main.batches == 1);
    CHECK(main.last_batch == std::vector<Chunk>{{6, *ground, 3}});
}

TEST_CASE("the loop goes with remeshes queued, or done and not taken in, and every pin back") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // In debug builds the streamer's destructor asserts that every job record
    // came back, and the pool's that every buffer did.
    for (const bool run : {false, true}) {
        WorldLoop loop(generator, table, jobs, pool, {.region = small});
        move_camera(loop, above_hills);
        loop.settle();
        const std::optional<ChunkPos> east = ground_chunk(loop, 1);
        REQUIRE(east.has_value());
        edit(loop, {.x = 32, .y = (east->y * 32) + 16, .z = 112}, BlockState::air);
        loop.pass(); // the two remeshes are queued
        REQUIRE(loop.meshes_in_flight() == 2);
        if (run) {
            jobs.run_until_idle(); // done, their results not taken in
        }
    }
}

TEST_CASE("streaming meshes wait while too many sent are not uploaded, and edits never wait") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // Four jobs a pass, so that however fast the chunks come in, the jobs
    // started before the limit holds them are a few, and meshes wait.
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = small, .queue_limit = 4, .unuploaded_limit = 10});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    MainThread main;
    main.receive(messages);
    const std::size_t first = main.meshes;
    CHECK(first >= 10);
    CHECK(first < shown(loop).size());
    // An edit's remeshes go out all the same.
    const std::optional<ChunkPos> east = ground_chunk(loop, 1);
    REQUIRE(east.has_value());
    edit(loop, {.x = 32, .y = (east->y * 32) + 16, .z = 112}, BlockState::air);
    loop.settle();
    loop.take(messages);
    main.receive(messages);
    CHECK(main.batches == 1);
    // Uploaded: the rest follow.
    for (int round = 0; round < 100 && main.held.size() < shown(loop).size(); ++round) {
        uploaded(loop, main.meshes);
        loop.settle();
        loop.take(messages);
        main.receive(messages);
    }
    std::set<Chunk> held;
    for (const auto& [chunk, mesh] : main.held) {
        held.insert(chunk);
    }
    CHECK(held == shown(loop));
    // A count past what was sent, as when a batch's meshes are counted too,
    // never holds streaming back.
    uploaded(loop, main.meshes + 2);
    loop.pass();
    CHECK(loop.unuploaded() == 0);
}

TEST_CASE("loading, editing, unloading and reloading end the same with 0, 1 and 6 workers") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    std::vector<std::uint64_t> chunks;
    std::vector<std::uint64_t> meshes;
    for (const std::uint32_t workers : {0U, 1U, 6U}) {
        vx::JobSystem jobs({.workers = workers});
        vx::MeshResultPool pool;
        WorldLoop loop(generator, table, jobs, pool, {.region = small});
        MainThread main;
        std::vector<ToMain> messages;
        const auto settle = [&] {
            loop.settle();
            loop.take(messages);
            main.receive(messages);
        };
        move_camera(loop, above_hills);
        settle();
        edit(loop, {.x = 100, .y = 70, .z = 100}, stone);
        edit(loop, {.x = 96, .y = 64, .z = 96}, BlockState::air);
        settle();
        move_camera(loop, {.x = above_hills.x + 12, .y = above_hills.y, .z = above_hills.z});
        settle();
        move_camera(loop, above_hills);
        settle();
        edit(loop, {.x = 120, .y = 70, .z = 90}, stone);
        settle();
        INFO("workers ", workers);
        CHECK(main.held.size() > 27);
        chunks.push_back(chunk_hash(loop));
        meshes.push_back(main.hash());
        // Every pin was given back: no version outlives its chunk's blocks.
        CHECK(loop.streamer().stats().block_versions == chunks_with_blocks(loop));
        if (workers == 0) {
            // What the main thread holds is a mesh of each chunk shown and of
            // nothing else, as the mesher makes it from the blocks now. The
            // other runs match this one.
            std::set<Chunk> held;
            Mesher mesher;
            for (const auto& [chunk, mesh] : main.held) {
                held.insert(chunk);
                const auto& [x, y, z] = chunk;
                CHECK(mesh.bytes == mesher.hash(loop, {.x = x, .y = y, .z = z}, table));
            }
            CHECK(held == shown(loop));
        }
    }
    CHECK(chunks[1] == chunks[0]);
    CHECK(chunks[2] == chunks[0]);
    CHECK(meshes[1] == meshes[0]);
    CHECK(meshes[2] == meshes[0]);
}

TEST_CASE("ticks come 20 a second on the loop's clock, and over a second behind the rest are "
          "skipped") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    const std::chrono::steady_clock::time_point start{};
    using std::chrono::milliseconds;
    CHECK(loop.step(start) == start + milliseconds(50));
    CHECK(loop.ticks() == 0);
    loop.step(start + milliseconds(49));
    CHECK(loop.ticks() == 0);
    loop.step(start + milliseconds(50));
    CHECK(loop.ticks() == 1);
    // Late by 120 ms: the three ticks due run, back to back.
    CHECK(loop.step(start + milliseconds(170)) == start + milliseconds(200));
    CHECK(loop.ticks() == 3);
    CHECK(loop.skipped_ticks() == 0);
    // Five seconds late: a tick runs now and the rest are skipped.
    const std::chrono::steady_clock::time_point late = start + milliseconds(5'200);
    CHECK(loop.step(late) == late + milliseconds(50));
    CHECK(loop.ticks() == 4);
    CHECK(loop.skipped_ticks() == 100);
}

namespace {

// Each reading moves it on by 100 us: a streaming step takes in five results.
class TickingClock final : public vx::JobClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        at += std::chrono::microseconds(100);
        return at;
    }

private:
    mutable std::chrono::steady_clock::time_point at;
};

} // namespace

TEST_CASE("while the streamer has results left from its step, the loop runs again at once") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    const TickingClock clock;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .clock = &clock});
    move_camera(loop, above_hills);
    const std::chrono::steady_clock::time_point start{};
    using std::chrono::milliseconds;
    // The first pass feeds the columns' surface jobs: nothing to take in yet.
    CHECK(loop.step(start) == start + milliseconds(50));
    jobs.run_until_idle();
    // Their results come five a step: the world thread is not to sleep.
    CHECK(loop.step(start) == start);
    CHECK(loop.streamer().results_waiting() > 0);
    loop.settle();
    CHECK(loop.step(start + milliseconds(1)) == start + milliseconds(50));
}

TEST_CASE("after a stop no tick runs, and settling returns with jobs still in flight") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.pass(); // the first jobs are queued
    stop(loop);
    const std::chrono::steady_clock::time_point start{};
    loop.step(start);
    loop.step(start + std::chrono::milliseconds(500));
    REQUIRE(loop.ticks() == 0);
    // Results are no longer taken in, so this would never settle: the loop's
    // destructor waits for the jobs instead.
    loop.settle();
    CHECK(loop.stopping());
}

TEST_CASE("the loop catches up while the near cylinder lacks meshes, and stops once it has them") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    const std::chrono::steady_clock::time_point start{};
    using std::chrono::milliseconds;
    loop.step(start);
    CHECK_FALSE(loop.catching_up()); // no camera yet: nothing is missing
    move_camera(loop, above_hills);
    loop.step(start + milliseconds(50));
    CHECK(loop.catching_up());
    CHECK(jobs.catching_up()); // the workers are told
    loop.settle();
    loop.step(start + milliseconds(100));
    CHECK_FALSE(loop.catching_up());
    CHECK_FALSE(jobs.catching_up());
}

TEST_CASE("on its own thread the loop streams the region in, and stops when told") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({.workers = 2});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    MainThread main;
    {
        const vx::app::WorldThread thread(loop);
        move_camera(loop, above_hills);
        std::vector<ToMain> messages;
        // Until no mesh has come for half a second; at most 30 seconds.
        auto quiet_since = std::chrono::steady_clock::now();
        const auto give_up = quiet_since + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() - quiet_since < std::chrono::milliseconds(500) &&
               std::chrono::steady_clock::now() < give_up) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            loop.take(messages);
            if (!messages.empty()) {
                main.receive(messages);
                quiet_since = std::chrono::steady_clock::now();
            }
        }
    }
    // Stopped and joined: the loop is ours to read.
    std::set<Chunk> held;
    for (const auto& [chunk, mesh] : main.held) {
        held.insert(chunk);
    }
    CHECK(held == shown(loop));
    CHECK(held.size() > 27);
    CHECK(loop.ticks() > 0);
}

namespace {

// The placeholder, counting the jobs that call it. With no workers every job
// runs on the test's thread, so a plain count does.
class CountingGenerator final : public vx::Generator {
public:
    explicit CountingGenerator(const vx::Generator& inner) noexcept : inner_(&inner) {}

    [[nodiscard]] vx::ChunkRange surface_range(vx::ColumnPos column,
                                               vx::GenScratch& scratch) const override {
        ++calls;
        return inner_->surface_range(column, scratch);
    }
    [[nodiscard]] std::optional<BlockState> out_of_reach(ChunkPos pos) const noexcept override {
        return inner_->out_of_reach(pos);
    }
    [[nodiscard]] vx::ChunkPlan plan(ChunkPos pos, vx::GenScratch& scratch) const override {
        ++calls;
        return inner_->plan(pos, scratch);
    }
    [[nodiscard]] vx::ChunkStorage build(std::span<const vx::ChunkPlan* const, 27> plans,
                                         vx::GenScratch& scratch) const override {
        ++calls;
        return inner_->build(plans, scratch);
    }

    mutable int calls = 0;

private:
    const vx::Generator* inner_;
};

} // namespace

TEST_CASE("the loop goes without running the jobs still queued") {
    const vx::PlaceholderGenerator placeholder(0x5EED, blocks);
    const CountingGenerator generator(placeholder);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    int calls = 0;
    {
        WorldLoop loop(generator, table, jobs, pool, {.region = small});
        move_camera(loop, above_hills);
        loop.pass(); // the columns' surfaces are queued
        REQUIRE(jobs.queued(vx::JobLevel::normal) > 0);
        calls = generator.calls;
    }
    CHECK(generator.calls == calls);
}

namespace {

// A viewer at the chunk's middle looking along +x, or along -x: a 70-degree
// view, as the app's.
vx::Viewer looking_along_x(ChunkPos chunk, double sign) {
    vx::Viewer viewer = vx::viewer_at(chunk);
    viewer.forward = {.x = sign, .y = 0.0, .z = 0.0};
    viewer.view = vx::Frustum({.x = sign, .y = 0.0, .z = 0.0}, {.x = 0.0, .y = 0.0, .z = sign},
                              {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7002075382097097, 16.0 / 9.0, 0.05);
    return viewer;
}

void turn_to(WorldLoop& loop, const vx::Viewer& viewer) {
    ToWorld message;
    message.kind = ToWorld::Kind::camera;
    message.camera = viewer;
    loop.to_world().push(std::move(message));
}

} // namespace

TEST_CASE("streaming meshes go out lowest score first, ranked again when the view turns") {
    // One job queued at a time, so the backlog alone picks the next, and
    // one mesh out at a time: none goes until the main thread has the last.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    const vx::app::WorldLoopSettings one_by_one{
        .region = small, .queue_limit = 1, .unuploaded_limit = 1};
    // Streams the region in looking east, then, turned to `turned` or not,
    // lets the waiting meshes out one by one: their scores, in send order.
    const auto meshes_out = [&](const std::optional<vx::Viewer>& turned) {
        vx::JobSystem jobs({});
        vx::MeshResultPool pool;
        WorldLoop loop(generator, table, jobs, pool, one_by_one);
        turn_to(loop, looking_along_x(above_hills, 1.0));
        loop.settle();
        std::vector<ToMain> messages;
        loop.take(messages);
        MainThread main;
        main.receive(messages);
        REQUIRE(main.meshes == 1);
        if (turned) {
            turn_to(loop, *turned);
            loop.settle();
        }
        std::vector<std::uint32_t> scores;
        for (int round = 0; round < 5'000 && main.held.size() < shown(loop).size(); ++round) {
            uploaded(loop, main.meshes);
            loop.settle();
            loop.take(messages);
            for (const ToMain& message : messages) {
                if (message.kind == ToMain::Kind::mesh) {
                    scores.push_back(loop.streamer().score(message.mesh.pos));
                }
            }
            main.receive(messages);
        }
        return scores;
    };
    for (const std::optional<vx::Viewer>& turned :
         {std::optional<vx::Viewer>(), std::optional(looking_along_x(above_hills, -1.0))}) {
        INFO("turned around: ", turned.has_value());
        const std::vector<std::uint32_t> scores = meshes_out(turned);
        REQUIRE(scores.size() > 30);
        CHECK(std::ranges::is_sorted(scores));
        // Both sides of the view were waiting: the ranking had work to do.
        CHECK(scores.front() < scores.back());
    }
}

TEST_CASE("uniform air and enclosed stone go out as empty meshes, without a job") {
    // Above the hills, chunks of air; underground, stone among stone.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    for (const ChunkPos camera : {above_hills, ChunkPos{.x = 3, .y = -3, .z = 3}}) {
        INFO("camera at y ", camera.y);
        vx::JobSystem jobs({});
        vx::MeshResultPool pool;
        WorldLoop loop(generator, table, jobs, pool, {.region = small});
        move_camera(loop, camera);
        loop.settle();
        std::vector<ToMain> messages;
        loop.take(messages);
        MainThread main;
        main.receive(messages);
        // The chunks shown whose meshes are known to be empty: those the
        // loop must have sent without meshing.
        const vx::ChunkRecords& records = loop.streamer().records();
        const auto uniform_stone = [&](ChunkPos pos) {
            const vx::ChunkRecord* record = loop.streamer().find(pos);
            return record != nullptr && record->blocks == nullptr && record->uniform == stone;
        };
        std::size_t known_empty = 0;
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (!records.live(index) || !records[index].drawn ||
                records[index].state != ChunkState::complete || records[index].blocks != nullptr) {
                continue;
            }
            const ChunkPos pos = records[index].pos;
            const bool enclosed =
                uniform_stone(pos) &&
                std::ranges::all_of(
                    std::array<ChunkPos, 6>{
                        ChunkPos{pos.x + 1, pos.y, pos.z}, ChunkPos{pos.x - 1, pos.y, pos.z},
                        ChunkPos{pos.x, pos.y + 1, pos.z}, ChunkPos{pos.x, pos.y - 1, pos.z},
                        ChunkPos{pos.x, pos.y, pos.z + 1}, ChunkPos{pos.x, pos.y, pos.z - 1}},
                    uniform_stone);
            if (records[index].uniform == BlockState::air || enclosed) {
                ++known_empty;
            }
        }
        CHECK(known_empty > 0);
        CHECK(loop.meshes_skipped() == known_empty);
        // Every chunk shown has its mesh, one each, the skipped ones empty.
        CHECK(main.meshes == shown(loop).size());
        std::set<Chunk> held;
        for (const auto& [chunk, mesh] : main.held) {
            held.insert(chunk);
        }
        CHECK(held == shown(loop));
    }
}

TEST_CASE("held back by the main thread, the meshes known empty at the front still go, and no job "
          "starts") {
    // High over the hills, which stay under block 112, the nearest chunks
    // are air: with no mesh allowed out unuploaded, those still go, without
    // a job, up to the first that needs one.
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = small, .queue_limit = 256, .unuploaded_limit = 0});
    move_camera(loop, {.x = 3, .y = 5, .z = 3});
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    MainThread main;
    main.receive(messages);
    CHECK(loop.meshes_skipped() > 0);
    CHECK(main.meshes == loop.meshes_skipped());
    CHECK(loop.meshes_in_flight() == 0);
    CHECK(main.held.size() < shown(loop).size());
    // Nothing runs and nothing waits for a job, yet the region is not
    // complete: chunks drawn have no mesh sent, which both kinds of sample
    // see.
    REQUIRE(loop.streamer().stats().jobs_in_flight == 0);
    for (const SampleKind kind : {SampleKind::completion, SampleKind::checked}) {
        ToWorld request;
        request.kind = ToWorld::Kind::sample;
        request.sample_kind = kind;
        loop.to_world().push(std::move(request));
        loop.pass();
        loop.take(messages);
        main.receive(messages);
        REQUIRE_FALSE(main.samples.empty());
        CHECK_FALSE(main.samples.back().complete);
        // A completion request is answered with the totals alone.
        CHECK(main.samples.back().kind == kind);
        CHECK(main.samples.back().broken == 0);
    }
    CHECK(main.samples.back().holes > 0);
}

namespace {

// A loop with a player standing over block column (100, 100), streamed in
// around it unless `streamed` is false, and the main thread's view of what it
// sends. With a `perdix` mode, Perdix too, as far over column (100, 95), 5
// blocks north of the player, facing `perdix_facing`: the player.
struct Walker {
    vx::PlaceholderGenerator generator{0x5EED, blocks};
    std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs{{}};
    vx::MeshResultPool pool;
    std::int32_t ground = generator.height(100, 100);
    std::optional<WorldLoop> loop;
    MainThread main;
    std::chrono::steady_clock::time_point now{};

    explicit Walker(double above, bool streamed = true,
                    std::optional<vx::LifeMode> perdix = std::nullopt,
                    float perdix_facing = south) {
        vx::app::WorldLoopSettings settings{.region = small};
        settings.player = vx::app::WorldLoopSettings::PlayerStart{
            .spawn = {.x = 100.5, .y = ground + above, .z = 100.5}, .lowest = -64.0};
        if (perdix) {
            settings.perdix = vx::app::WorldLoopSettings::PerdixStart{.spawn = perdix_spawn(above),
                                                                      .facing = perdix_facing,
                                                                      .mode = *perdix,
                                                                      .seed = 0x5EED};
        }
        loop.emplace(generator, table, jobs, pool, settings);
        if (streamed) {
            stream_in();
        }
    }

    // The region streamed in around the player.
    void stream_in() {
        move_camera(*loop, {.x = 3, .y = ground / 32, .z = 3});
        loop->settle();
    }

    // `count` ticks on the loop's clock, 50 ms apart.
    void ticks(int count) {
        for (int tick = 0; tick < count; ++tick) {
            now += WorldLoop::tick_length;
            loop->step(now);
        }
        std::vector<ToMain> messages;
        loop->take(messages);
        main.receive(messages);
    }

    void send(const vx::TickInput& input) {
        ToWorld message;
        message.kind = ToWorld::Kind::input;
        message.input = input;
        loop->to_world().push(std::move(message));
    }

    void land(vx::Vec3d feet) {
        ToWorld message;
        message.kind = ToWorld::Kind::land;
        message.feet = feet;
        handle(std::move(message));
    }

    // A message handled between ticks, and what it sends back.
    void handle(ToWorld message) {
        loop->to_world().push(std::move(message));
        loop->pass();
        std::vector<ToMain> messages;
        loop->take(messages);
        main.receive(messages);
    }

    // Where Perdix starts, `above` the ground over column (100, 95).
    [[nodiscard]] vx::Vec3d perdix_spawn(double above) const {
        return {.x = 100.5, .y = generator.height(100, 95) + above, .z = 95.5};
    }

    static constexpr float south = std::numbers::pi_v<float>;
};

} // namespace

TEST_CASE("the player falls onto the streamed hills on the loop's ticks, and walks with the keys") {
    Walker walker(3.0);
    // The first step starts the clock; each after it runs a tick, and every
    // tick's frame goes to the main thread with when it fell due.
    walker.ticks(41);
    REQUIRE(walker.main.tick_times.size() == 40);
    for (std::size_t at = 1; at < walker.main.tick_times.size(); ++at) {
        CHECK(walker.main.tick_times[at] - walker.main.tick_times[at - 1] ==
              WorldLoop::tick_length);
    }
    CHECK(walker.main.player.on_ground);
    CHECK(walker.main.player.position.y == doctest::Approx(walker.ground));
    CHECK(walker.main.player.ticks == 40);
    // W held, facing north (-z): a second of walking, 4.3 blocks.
    walker.send({.held = {.forward = true}, .yaw = 0.0F});
    walker.ticks(20);
    CHECK(walker.main.player.position.z < 100.5 - 3.0);
    CHECK(walker.main.player.position.x == doctest::Approx(100.5));
    // Let go: it stops.
    walker.send({});
    walker.ticks(20);
    CHECK(walker.main.player.speed() == doctest::Approx(0.0));
}

TEST_CASE("a player in a chunk not ready yet waits there, keys and all, until it is") {
    // Nothing streamed in: the chunk the player starts in is a solid cube to
    // moves, which cannot hold up a body already inside it.
    Walker walker(3.0, false);
    const vx::Vec3d spawn{.x = 100.5, .y = walker.ground + 3.0, .z = 100.5};
    walker.send({.held = {.forward = true}, .yaw = 0.0F});
    walker.ticks(41);
    // Every tick still sends its frame, with the player where it started.
    CHECK(walker.main.tick_times.size() == 40);
    CHECK(walker.main.player.position == spawn);
    CHECK(walker.main.player.previous == spawn);
    CHECK(walker.main.player.ticks == 0);
    // Streamed in, it falls onto the ground below and walks from there.
    walker.send({});
    walker.stream_in();
    walker.ticks(20);
    CHECK(walker.main.player.ticks == 20);
    CHECK(walker.main.player.on_ground);
    CHECK(walker.main.player.position.y == doctest::Approx(walker.ground));
    walker.send({.held = {.forward = true}, .yaw = 0.0F});
    walker.ticks(20);
    CHECK(walker.main.player.position.z < 100.5 - 3.0);
}

TEST_CASE(
    "a player whose move could touch a chunk not loaded waits short of it, keeping its speed") {
    // Streamed in around chunk x = 3: chunks up to x = 7 are loaded, and x =
    // 8, from block 256 on, is not.
    Walker walker(3.0);
    walker.ticks(41);
    std::int32_t top = 0;
    for (std::int32_t x = 248; x < 280; ++x) {
        top = std::max(top, walker.generator.height(x, 100));
    }
    const vx::Vec3d start{.x = 248.5, .y = top + 4.0, .z = 100.5};
    walker.land(start);
    REQUIRE(walker.main.landings.back());
    // Space twice to fly, then east (+x) over the hills.
    constexpr float east = -1.5707964F;
    walker.send({.held = {.jump = true}, .yaw = east});
    walker.ticks(1);
    walker.send({.held = {}, .yaw = east});
    walker.ticks(1);
    walker.send({.held = {.jump = true}, .yaw = east});
    walker.ticks(1);
    REQUIRE(walker.main.player.flying);
    walker.send({.held = {.forward = true}, .yaw = east});
    walker.ticks(40);
    // Stopped short of block 256 by what its next move could touch, still,
    // and with its speed kept for when it goes on.
    const vx::app::WalkingFrame held = walker.main.player;
    CHECK(held.position.x > start.x + 3.0);
    CHECK(held.position.x < 256.0 - 1.0);
    CHECK(held.previous == held.position);
    REQUIRE(walker.loop->walking() != nullptr);
    CHECK(walker.loop->walking()->player().velocity.x > 0.2);
    // Streamed in further east, it flies on.
    move_camera(*walker.loop, {.x = 7, .y = walker.ground / 32, .z = 3});
    walker.loop->settle();
    walker.ticks(20);
    CHECK(walker.main.player.position.x > 256.0);
}

TEST_CASE("the noclip camera lands the player only where a standing player fits") {
    Walker walker(3.0);
    walker.ticks(41);
    // Inside the ground: refused, and the player stays where it was.
    const vx::Vec3d before = walker.main.player.position;
    walker.land({.x = 100.5, .y = walker.ground - 3.0, .z = 100.5});
    REQUIRE(walker.main.landings.size() == 1);
    CHECK_FALSE(walker.main.landings.back());
    CHECK(walker.main.player.position == before);
    // In a chunk still loading, past the streamed region: refused too.
    walker.land({.x = 400.5, .y = walker.ground + 5.0, .z = 100.5});
    REQUIRE(walker.main.landings.size() == 2);
    CHECK_FALSE(walker.main.landings.back());
    CHECK(walker.main.player.position == before);
    // In the air above it: landed there at once, still, before any tick.
    const vx::Vec3d air{.x = 101.5, .y = walker.ground + 5.0, .z = 98.5};
    walker.land(air);
    REQUIRE(walker.main.landings.size() == 3);
    CHECK(walker.main.landings.back());
    CHECK(walker.main.player.position == air);
    CHECK(walker.main.player.previous == air);
    // Then it falls to the ground on the ticks.
    walker.ticks(40);
    CHECK(walker.main.player.on_ground);
    CHECK(walker.main.player.position.y == doctest::Approx(walker.generator.height(101, 98)));
}

TEST_CASE("Perdix ticks beside the player, falls onto the ground and sends every tick's frame") {
    Walker walker(3.0, true, vx::LifeMode::still);
    walker.ticks(41);
    CHECK(walker.main.perdix_frames == 40);
    CHECK(walker.main.perdix.ticks == 40);
    CHECK(walker.main.perdix.on_ground);
    CHECK(walker.main.perdix.position.x == 100.5);
    CHECK(walker.main.perdix.position.z == 95.5);
    CHECK(walker.main.perdix.position.y == doctest::Approx(walker.generator.height(100, 95)));
    // Facing the player, as he appeared; his auto-jump's presses never make
    // him fly.
    CHECK(std::abs(walker.main.perdix.motion.body_yaw) ==
          doctest::Approx(static_cast<double>(Walker::south)).epsilon(1e-4));
    REQUIRE(walker.loop->perdix() != nullptr);
    CHECK_FALSE(walker.loop->perdix()->player().may_fly);
    REQUIRE(walker.loop->walking() != nullptr);
    CHECK(walker.loop->walking()->player().may_fly);
}

TEST_CASE("Perdix stands in still, and in his life strolls off on his own") {
    Walker walker(0.0, true, vx::LifeMode::still);
    const vx::Vec3d spawn = walker.perdix_spawn(0.0);
    const auto away = [&] {
        const vx::Vec3d at = walker.main.perdix.position;
        return std::hypot(at.x - spawn.x, at.z - spawn.z);
    };
    walker.ticks(401);
    REQUIRE(walker.loop->perdix_life() != nullptr);
    CHECK(walker.loop->perdix_life()->mode() == vx::LifeMode::still);
    CHECK(away() == 0.0);
    CHECK(walker.loop->perdix_life()->starts(vx::LifeGoal::stroll) == 0);

    ToWorld message;
    message.kind = ToWorld::Kind::perdix_mode;
    message.perdix_mode = vx::LifeMode::life;
    walker.handle(std::move(message));
    CHECK(walker.loop->perdix_life()->mode() == vx::LifeMode::life);
    // A minute of his life, looked at each second.
    double farthest = 0.0;
    for (int second = 0; second < 60; ++second) {
        walker.ticks(20);
        farthest = std::max(farthest, away());
    }
    CHECK(walker.loop->perdix_life()->starts(vx::LifeGoal::stroll) > 0);
    CHECK(farthest > 2.0);
    CHECK(walker.main.perdix_frames == 1600);
}

TEST_CASE("Perdix turns his head to look at you, on the life's look, and stands") {
    // Facing north, away from the player 5 blocks south of him.
    Walker walker(0.0, true, vx::LifeMode::still, 0.0F);
    bool looked_back = false;
    for (int second = 0; second < 120 && !looked_back; ++second) {
        walker.ticks(20);
        looked_back = std::abs(walker.main.perdix.motion.head_yaw) > 2.5F;
    }
    CHECK(looked_back);
    REQUIRE(walker.loop->perdix_life() != nullptr);
    CHECK(walker.loop->perdix_life()->starts(vx::LifeGoal::look_at_player) > 0);
    CHECK(walker.main.perdix.position.x == 100.5);
    CHECK(walker.main.perdix.position.z == 95.5);
}

TEST_CASE("following you up the hill, Perdix climbs its steps on the auto-jump") {
    Walker walker(0.0, true, vx::LifeMode::follow);
    walker.ticks(21);
    // The player 14 blocks east and 8 up the hill, whose steps there are a
    // block each: too far, so he follows.
    const vx::Vec3d you{.x = 114.5, .y = walker.generator.height(114, 95) + 0.5, .z = 95.5};
    walker.land(you);
    REQUIRE(walker.main.landings.back());
    double closest = 100.0;
    double highest = 0.0;
    for (int second = 0; second < 15; ++second) {
        walker.ticks(20);
        const vx::Vec3d at = walker.main.perdix.position;
        closest = std::min(closest, std::hypot(at.x - you.x, at.z - you.z));
        highest = std::max(highest, at.y);
    }
    CHECK(closest < 3.0);
    CHECK(highest > walker.perdix_spawn(0.0).y + 6.0);
    REQUIRE(walker.loop->perdix_life() != nullptr);
    CHECK(walker.loop->perdix_life()->starts(vx::LifeGoal::follow) > 0);
}

TEST_CASE("Perdix in a chunk not ready yet waits, as the player does") {
    Walker walker(3.0, false, vx::LifeMode::still);
    walker.ticks(41);
    CHECK(walker.main.perdix_frames == 40);
    CHECK(walker.main.perdix.ticks == 0);
    CHECK(walker.main.perdix.position == walker.perdix_spawn(3.0));
    CHECK(walker.main.perdix.previous == walker.perdix_spawn(3.0));
    walker.stream_in();
    walker.ticks(20);
    CHECK(walker.main.perdix.ticks == 20);
    CHECK(walker.main.perdix.on_ground);
    CHECK(walker.main.perdix.position.y == doctest::Approx(walker.generator.height(100, 95)));
}

TEST_CASE("brought to you, Perdix stands on the ground 5 blocks ahead, facing you") {
    Walker walker(0.0, true, vx::LifeMode::still);
    walker.ticks(41);
    const std::size_t frames = walker.main.perdix_frames;
    constexpr float east = -1.5707964F;
    ToWorld bring;
    bring.kind = ToWorld::Kind::bring_perdix;
    bring.feet = walker.main.player.position;
    bring.input.yaw = east;
    walker.handle(bring);
    // Sent at once, before any tick: 5 blocks east, standing, facing west.
    CHECK(walker.main.perdix_frames == frames + 1);
    CHECK(walker.main.perdix.position.x == doctest::Approx(105.5));
    CHECK(walker.main.perdix.position.z == doctest::Approx(100.5));
    CHECK(walker.main.perdix.position.y == doctest::Approx(walker.generator.height(105, 100)));
    CHECK(walker.main.perdix.previous == walker.main.perdix.position);
    CHECK(walker.main.perdix.motion.body_yaw ==
          doctest::Approx(static_cast<double>(-east)).epsilon(1e-4));
    // A new body there, which still may not fly, and his life starts over
    // there: on the next tick his head turns from where he now faces.
    REQUIRE(walker.loop->perdix() != nullptr);
    CHECK_FALSE(walker.loop->perdix()->player().may_fly);
    walker.ticks(1);
    CHECK(std::abs(walker.main.perdix.motion.head_yaw + east) <= 0.1746F);
    walker.ticks(19);
    CHECK(walker.main.perdix.on_ground);
    CHECK(walker.main.perdix.position.x == doctest::Approx(105.5));

    // From a free camera 40 blocks up, looking north, down to the ground.
    bring.feet = {.x = 100.5, .y = walker.ground + 40.0, .z = 100.5};
    bring.input.yaw = 0.0F;
    walker.handle(bring);
    CHECK(walker.main.perdix.position.x == doctest::Approx(100.5));
    CHECK(walker.main.perdix.position.z == doctest::Approx(95.5));
    CHECK(walker.main.perdix.position.y == doctest::Approx(walker.generator.height(100, 95)));

    // Ahead of feet in a chunk still loading, nowhere fits: he stays.
    const vx::Vec3d before = walker.main.perdix.position;
    bring.feet = {.x = 400.5, .y = walker.ground + 5.0, .z = 100.5};
    walker.handle(bring);
    CHECK(walker.main.perdix.position == before);
}

TEST_CASE("Perdix comes only with a player") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    vx::app::WorldLoopSettings settings{.region = small};
    settings.perdix = vx::app::WorldLoopSettings::PerdixStart{.spawn = {}};
    const WorldLoop loop(generator, table, jobs, pool, settings);
    CHECK(loop.perdix() == nullptr);
    CHECK(loop.perdix_life() == nullptr);
}

TEST_CASE("switching merging off meshes every chunk shown again, unmerged") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    MainThread main;
    main.receive(messages);
    const std::uint64_t skipped = loop.meshes_skipped();
    REQUIRE(skipped > 0);
    ToWorld off;
    off.kind = ToWorld::Kind::meshing;
    off.meshing = {.merge = false};
    loop.to_world().push(std::move(off));
    loop.settle();
    // An empty mesh is empty with any options: none went again.
    CHECK(loop.meshes_skipped() == skipped);
    loop.take(messages);
    main.receive(messages);
    // Each chunk shown holds RFC 0003's unmerged mesh now, and merging made
    // a difference somewhere.
    Mesher mesher;
    std::size_t changed = 0;
    for (const Chunk& chunk : shown(loop)) {
        const ChunkPos pos{
            .x = std::get<0>(chunk), .y = std::get<1>(chunk), .z = std::get<2>(chunk)};
        REQUIRE(main.held.contains(chunk));
        const std::uint64_t unmerged = mesher.hash(loop, pos, table, {.merge = false});
        CHECK(main.held[chunk].bytes == unmerged);
        if (unmerged != mesher.hash(loop, pos, table)) {
            ++changed;
        }
    }
    CHECK(changed > 0);
}

TEST_CASE("a sample counts the holes in view until the region is complete, and the totals so far, "
          "and the checks find nothing broken, moving or not") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    MainThread main;
    // Every full request checks RFC 0006's invariants too.
    std::uint32_t asked = 0;
    const auto ask = [&](SampleKind kind = SampleKind::checked) {
        ToWorld request;
        request.kind = ToWorld::Kind::sample;
        request.sample_id = ++asked;
        request.sample_kind = kind;
        loop.to_world().push(std::move(request));
        loop.pass();
        std::vector<ToMain> messages;
        loop.take(messages);
        main.receive(messages);
        REQUIRE_FALSE(main.samples.empty());
        const vx::app::WorldSample answer = main.samples.back();
        CHECK(answer.id == asked);
        CHECK(answer.broken == 0);
        return answer;
    };
    turn_to(loop, looking_along_x(above_hills, 1.0));
    // Nothing known yet: streaming is not idle, so the region is not complete.
    CHECK_FALSE(ask().complete);
    CHECK_FALSE(ask(SampleKind::completion).complete);
    // The surfaces in, a pass later: the drawn region is known, and none of
    // its chunks that need a mesh has one. A request is answered as its pass
    // starts.
    jobs.run_until_idle();
    loop.pass();
    // Asked only whether it is complete: no, with the totals and nothing
    // more, the longest step and pass left for the next full sample.
    const vx::app::WorldSample light = ask(SampleKind::completion);
    CHECK(light.kind == SampleKind::completion);
    CHECK_FALSE(light.complete);
    CHECK(light.generated == loop.streamer().stats().generated);
    CHECK(light.holes == 0);
    CHECK(light.chunk_bytes == 0);
    CHECK(light.longest_step == std::chrono::nanoseconds(0));
    CHECK(light.longest_pass == std::chrono::nanoseconds(0));
    const vx::app::WorldSample loading = ask();
    CHECK(loading.kind == SampleKind::checked);
    CHECK_FALSE(loading.complete);
    CHECK(loading.holes > 0);
    CHECK(loading.near_holes > 0);
    CHECK(loading.near_holes <= loading.holes);
    // Settled: asked whether it is complete, the answer is a full sample,
    // which a phase waiting for the region ends on.
    loop.settle();
    const vx::app::WorldSample settled = ask(SampleKind::completion);
    CHECK(settled.kind == SampleKind::full);
    CHECK(settled.complete);
    CHECK(settled.chunk_bytes == loop.streamer().chunk_bytes());
    // It took the longest step and pass of the whole load: those of the
    // full sample after it are an idle pass's, a few clock ticks or none.
    CHECK(settled.longest_step > std::chrono::nanoseconds(0));
    CHECK(settled.longest_pass > std::chrono::nanoseconds(0));
    // A full one: no hole, and the totals are the streamer's and the loop's.
    const vx::app::WorldSample done = ask();
    CHECK(done.complete);
    CHECK(done.at > loading.at);
    CHECK(done.holes == 0);
    CHECK(done.near_holes == 0);
    const vx::StreamingStats stats = loop.streamer().stats();
    CHECK(done.generated == stats.generated);
    CHECK(done.uniform == stats.uniform);
    CHECK(done.skipped == loop.meshes_skipped());
    CHECK(done.meshed + done.skipped >= shown(loop).size());
    CHECK(done.chunk_bytes == loop.streamer().chunk_bytes());
    // A chunk at a time, then three at once: the chunks left behind stop
    // showing, some kept a while by the hysteresis, and others leave.
    for (std::int32_t step = 1; step <= 8; ++step) {
        turn_to(loop,
                looking_along_x({.x = above_hills.x + step, .y = above_hills.y, .z = above_hills.z},
                                1.0));
        CHECK_FALSE(ask().complete);
        loop.settle();
        CHECK(ask().complete);
    }
    turn_to(loop, looking_along_x({.x = above_hills.x + 11, .y = above_hills.y, .z = above_hills.z},
                                  1.0));
    CHECK_FALSE(ask().complete);
    loop.settle();
    CHECK(ask().complete);
    // Far away at once, then loading there: the old region goes with the
    // move, and nothing breaks while the new one streams in.
    turn_to(loop, looking_along_x(
                      {.x = above_hills.x + 1'000, .y = above_hills.y, .z = above_hills.z}, 1.0));
    CHECK_FALSE(ask().complete);
    jobs.run_until_idle();
    loop.pass();
    CHECK_FALSE(ask().complete);
    loop.settle();
    CHECK(ask().complete);
}

TEST_CASE("the render distance moves the loop's region: the main thread holds what is shown, and "
          "the checks find nothing broken") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    MainThread main;
    std::vector<ToMain> messages;
    // Settled, with every mesh the loop sends taken in and reported done.
    const auto settle = [&] {
        loop.settle();
        for (int round = 0; round < 100; ++round) {
            loop.take(messages);
            main.receive(messages);
            uploaded(loop, main.meshes);
            loop.settle();
            if (loop.unuploaded() == 0 && loop.meshes_in_flight() == 0) {
                break;
            }
        }
        loop.take(messages);
        main.receive(messages);
    };
    const auto held = [&] {
        std::set<Chunk> chunks;
        for (const auto& [chunk, mesh] : main.held) {
            chunks.insert(chunk);
        }
        return chunks;
    };
    const auto checked = [&] {
        ToWorld request;
        request.kind = ToWorld::Kind::sample;
        request.sample_kind = SampleKind::checked;
        loop.to_world().push(std::move(request));
        loop.pass();
        loop.take(messages);
        main.receive(messages);
        REQUIRE_FALSE(main.samples.empty());
        return main.samples.back();
    };
    const auto set_distance = [&](std::int32_t chunks) {
        ToWorld message;
        message.kind = ToWorld::Kind::view_distance;
        message.view_distance = chunks;
        loop.to_world().push(std::move(message));
        settle();
    };
    move_camera(loop, above_hills);
    settle();
    const std::set<Chunk> at_three = shown(loop);
    CHECK(held() == at_three);
    // Farther, past what the hysteresis around 3 would keep: shown as a
    // loop started at 6, and complete.
    set_distance(6);
    vx::JobSystem fresh_jobs({});
    vx::MeshResultPool fresh_pool;
    vx::RegionSettings six = small;
    six.view_distance = 6;
    WorldLoop fresh(generator, table, fresh_jobs, fresh_pool, {.region = six});
    move_camera(fresh, above_hills);
    fresh.settle();
    CHECK(shown(loop) == shown(fresh));
    CHECK(shown(loop).size() > at_three.size());
    CHECK(held() == shown(loop));
    vx::app::WorldSample sample = checked();
    CHECK(sample.broken == 0);
    CHECK(sample.complete);
    // Nearer: the chunks past the hysteresis leave, their meshes with them,
    // and those within it stay, more than a loop started at 2 shows.
    set_distance(2);
    CHECK(shown(loop).size() < shown(fresh).size());
    vx::JobSystem two_jobs({});
    vx::MeshResultPool two_pool;
    vx::RegionSettings two = small;
    two.view_distance = 2;
    WorldLoop fresh_two(generator, table, two_jobs, two_pool, {.region = two});
    move_camera(fresh_two, above_hills);
    fresh_two.settle();
    CHECK(shown(loop).size() > shown(fresh_two).size());
    CHECK(held() == shown(loop));
    sample = checked();
    CHECK(sample.broken == 0);
    CHECK(sample.complete);
}

namespace {

// A clock the test moves on.
class ManualClock final : public vx::JobClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override { return at; }
    std::chrono::steady_clock::time_point at;
};

void send_arena(WorldLoop& loop, std::uint64_t full_frames, double share, bool full = true) {
    ToWorld message;
    message.kind = ToWorld::Kind::arena;
    message.arena_full_frames = full_frames;
    message.arena_share = share;
    message.arena_full = full;
    loop.to_world().push(std::move(message));
}

vx::app::WorldSample ask_sample(WorldLoop& loop, MainThread& main) {
    ToWorld message;
    message.kind = ToWorld::Kind::sample;
    message.sample_kind = SampleKind::full;
    loop.to_world().push(std::move(message));
    loop.pass();
    std::vector<ToMain> messages;
    loop.take(messages);
    main.receive(messages);
    REQUIRE_FALSE(main.samples.empty());
    return main.samples.back();
}

vx::app::WorldReport ask_report(WorldLoop& loop, MainThread& main) {
    ToWorld message;
    message.kind = ToWorld::Kind::report;
    loop.to_world().push(std::move(message));
    loop.pass();
    std::vector<ToMain> messages;
    loop.take(messages);
    main.receive(messages);
    REQUIRE_FALSE(main.reports.empty());
    return main.reports.back();
}

constexpr vx::RegionSettings six{
    .view_distance = 6, .near_distance = 1, .near_height = 1, .hysteresis = 2};

} // namespace

TEST_CASE("over its memory budget the loop lowers the streamer's hysteresis, then its view "
          "distance, a step each check, and its report says why") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    ManualClock clock;
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = six, .memory_budget = 1, .clock = &clock});
    move_camera(loop, above_hills);
    loop.settle();
    MainThread main;
    // The first pass checked: chunk data was already over one byte.
    CHECK(loop.streamer().limits() == vx::RegionLimits{.view_distance = 6, .hysteresis = 1});
    // A sample says the view distance the streamer holds (the flythrough's
    // report checks it).
    CHECK(ask_sample(loop, main).view_distance == 6);
    // No further check before 250 ms have passed on the loop's clock.
    loop.pass();
    CHECK(loop.streamer().limits().hysteresis == 1);
    const vx::RegionLimits expected[] = {{.view_distance = 6, .hysteresis = 0},
                                         {.view_distance = 5, .hysteresis = 0},
                                         {.view_distance = 4, .hysteresis = 0},
                                         {.view_distance = 4, .hysteresis = 0}};
    for (const vx::RegionLimits& limits : expected) {
        clock.at += WorldLoop::memory_check_interval;
        loop.settle();
        CHECK(loop.streamer().limits() == limits);
    }
    CHECK(ask_sample(loop, main).view_distance == 4);
    const vx::app::WorldReport report = ask_report(loop, main);
    CHECK(report.limits == vx::RegionLimits{.view_distance = 4, .hysteresis = 0});
    CHECK(report.chosen == vx::RegionLimits{.view_distance = 6, .hysteresis = 2});
    CHECK(report.limited_by == vx::LimitedBy::chunk_memory);
    CHECK(report.budget == 1);
    CHECK(report.chunk_bytes > 0);
    std::size_t chunks = 0;
    for (const std::size_t count : report.chunks) {
        chunks += count;
    }
    CHECK(chunks == loop.streamer().records().size());
    // The player's slider caps it, nearer still.
    ToWorld nearer;
    nearer.kind = ToWorld::Kind::view_distance;
    nearer.view_distance = 3;
    loop.to_world().push(std::move(nearer));
    loop.settle();
    CHECK(loop.streamer().limits().view_distance == 3);
}

TEST_CASE("an arena full in frames since the last check lowers the limits, an old count does "
          "not, and five calm seconds bring them back to the player's") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    ManualClock clock;
    WorldLoop loop(generator, table, jobs, pool, {.region = six, .clock = &clock});
    move_camera(loop, above_hills);
    loop.settle();
    MainThread main;
    REQUIRE(loop.streamer().limits() == vx::RegionLimits{.view_distance = 6, .hysteresis = 2});
    const auto check = [&] {
        clock.at += WorldLoop::memory_check_interval;
        loop.settle();
        return loop.streamer().limits();
    };
    // Full in 3 frames: one step down, and the report says why.
    send_arena(loop, 3, 1.0);
    CHECK(check() == vx::RegionLimits{.view_distance = 6, .hysteresis = 1});
    CHECK(ask_report(loop, main).limited_by == vx::LimitedBy::mesh_memory);
    // The same count again, as while the window is minimized and no frame
    // runs the upkeep: no further step, and with the share near the limit,
    // nothing comes back either.
    for (int at = 0; at < 30; ++at) {
        send_arena(loop, 3, 0.95);
        CHECK(check() == vx::RegionLimits{.view_distance = 6, .hysteresis = 1});
    }
    // The slider moves while the budget holds the region back: nearer caps
    // it at once, and the player's hysteresis is what comes back.
    ToWorld nearer;
    nearer.kind = ToWorld::Kind::view_distance;
    nearer.view_distance = 5;
    loop.to_world().push(std::move(nearer));
    CHECK(check() == vx::RegionLimits{.view_distance = 5, .hysteresis = 1});
    // Room again: after five calm seconds, one step back, to the player's.
    send_arena(loop, 3, 0.1, false);
    for (int at = 0; at < 20; ++at) {
        CHECK(check() == vx::RegionLimits{.view_distance = 5, .hysteresis = 1});
    }
    CHECK(check() == vx::RegionLimits{.view_distance = 5, .hysteresis = 2});
    const vx::app::WorldReport report = ask_report(loop, main);
    CHECK(report.limited_by == vx::LimitedBy::nothing);
    CHECK(report.chosen == vx::RegionLimits{.view_distance = 5, .hysteresis = 2});
}

TEST_CASE("a lowering's own full frames, reported once the arena has room again, are not a "
          "second fill, and a new fill still is") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    ManualClock clock;
    WorldLoop loop(generator, table, jobs, pool, {.region = six, .clock = &clock});
    move_camera(loop, above_hills);
    loop.settle();
    REQUIRE(loop.streamer().limits() == vx::RegionLimits{.view_distance = 6, .hysteresis = 2});
    const auto check = [&] {
        clock.at += WorldLoop::memory_check_interval;
        loop.settle();
        return loop.streamer().limits();
    };
    // The arena fills: one step down.
    send_arena(loop, 3, 1.0);
    CHECK(check() == vx::RegionLimits{.view_distance = 6, .hysteresis = 1});
    // Frames kept ending full until the lowering's unloads applied, so the
    // count rose; by the report the arena has room. The same fill: no step.
    send_arena(loop, 5, 0.9, false);
    CHECK(check() == vx::RegionLimits{.view_distance = 6, .hysteresis = 1});
    // A new fill, still full when reported: the next step down.
    send_arena(loop, 7, 1.0);
    CHECK(check() == vx::RegionLimits{.view_distance = 6, .hysteresis = 0});
}

// RFC 0011's levels: a streamer on each, and every mesh and unload with its
// level.

namespace {

using LevelChunk = std::tuple<int, std::int32_t, std::int32_t, std::int32_t>;

LevelChunk level_chunk_of(std::uint8_t level, ChunkPos pos) {
    return {int{level}, pos.x, pos.y, pos.z};
}

// The chunks of every level drawn and complete.
std::set<LevelChunk> shown_on_levels(const WorldLoop& loop) {
    std::set<LevelChunk> out;
    for (std::uint8_t level = 0; level < loop.levels(); ++level) {
        const vx::ChunkRecords& records = loop.streamer(level).records();
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index) && records[index].drawn &&
                records[index].state == ChunkState::complete) {
                out.insert(level_chunk_of(level, records[index].pos));
            }
        }
    }
    return out;
}

// The rings as the loop applies them to a region.
vx::LodRings rings_for(vx::LodRings rings, const vx::RegionSettings& region) {
    rings.view_distance = region.view_distance;
    rings.near_distance = region.near_distance;
    rings.near_height = region.near_height;
    return rings;
}

// The main thread's meshes by level and chunk, each its newest, as RFC
// 0011's uploads keep them.
class LevelMeshes {
public:
    void receive(std::vector<ToMain>& messages) {
        for (ToMain& message : messages) {
            switch (message.kind) {
            case ToMain::Kind::mesh:
                take(message.mesh);
                break;
            case ToMain::Kind::batch:
                for (const vx::app::MeshUpdate& update : message.batch) {
                    take(update);
                }
                break;
            case ToMain::Kind::unload:
                held.erase(level_chunk_of(message.level, message.pos));
                shown.erase(level_chunk_of(message.level, message.pos));
                unloads.insert(int{message.level});
                break;
            case ToMain::Kind::swap:
                swap(message);
                break;
            default:
                break;
            }
        }
    }

    struct Held {
        std::uint32_t generation = 0;
        std::uint64_t bytes = 0; // a hash of the counts and quads
        bool empty = true;
        // RFC 0011's open border sets as the update brought them, and every
        // quad.
        std::array<std::uint32_t, 12> counts{};
        std::array<std::uint32_t, 12> border{};
        std::array<std::uint32_t, 12> open{};
        std::vector<vx::Quad> quads;
    };
    std::map<LevelChunk, Held> held;
    std::set<int> unloads; // the levels unloads came for
    // RFC 0011's swaps, applied as they come: the chunks drawn, and the
    // chunks shown with no mesh with faces held when their swap came.
    std::set<LevelChunk> shown;
    std::size_t swaps = 0;
    std::size_t shown_bare = 0;
    std::vector<LevelChunk> hidden; // by the swaps since the caller cleared it

private:
    void swap(const ToMain& message) {
        ++swaps;
        for (const vx::LodPos chunk : message.hide) {
            const LevelChunk at =
                level_chunk_of(chunk.level, {.x = chunk.x, .y = chunk.y, .z = chunk.z});
            shown.erase(at);
            hidden.push_back(at);
        }
        for (const vx::LodPos chunk : message.show) {
            const LevelChunk at =
                level_chunk_of(chunk.level, {.x = chunk.x, .y = chunk.y, .z = chunk.z});
            const auto mesh = held.find(at);
            shown_bare += mesh == held.end() || mesh->second.empty ? 1 : 0;
            shown.insert(at);
        }
    }

    void take(const vx::app::MeshUpdate& update) {
        Held& slot = held[level_chunk_of(update.level, update.pos)];
        if (update.generation < slot.generation) {
            return;
        }
        slot.generation = update.generation;
        slot.empty =
            std::ranges::all_of(update.counts, [](std::uint32_t count) { return count == 0; });
        slot.bytes = mesh_hash(update.counts, update.quads.data());
        slot.counts = update.counts;
        slot.border = update.border;
        slot.open = update.open;
        std::size_t total = 0;
        for (const std::uint32_t count : update.counts) {
            total += count;
        }
        for (const std::uint32_t count : update.open) {
            total += count;
        }
        slot.quads.resize(total);
        if (total > 0) {
            std::memcpy(slot.quads.data(), update.quads.data(), total * sizeof(vx::Quad));
        }
    }
};

std::set<LevelChunk> keys(const std::map<LevelChunk, LevelMeshes::Held>& held) {
    std::set<LevelChunk> out;
    for (const auto& [chunk, mesh] : held) {
        out.insert(chunk);
    }
    return out;
}

} // namespace

TEST_CASE("with RFC 0011's rings each level streams its own disc, and every chunk drawn sends "
          "its mesh once, with its level") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
    REQUIRE(loop.levels() == vx::lod_levels);
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    std::set<LevelChunk> meshed;
    for (const ToMain& message : messages) {
        REQUIRE(message.kind == ToMain::Kind::mesh);
        CHECK(meshed.insert(level_chunk_of(message.mesh.level, message.mesh.pos)).second);
    }
    CHECK(meshed == shown_on_levels(loop));
    const vx::LodRings rings = rings_for({}, small);
    for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
        INFO("level " << int{level});
        CHECK(loop.streamer(level).limits().view_distance == rings.stream_distance(level));
        CHECK(std::ranges::count_if(meshed, [level](const LevelChunk& chunk) {
                  return std::get<0>(chunk) == level;
              }) > 0);
    }
    CHECK(loop.meshes_in_flight() == 0);
}

TEST_CASE("coarse levels are meshed without corner shadows, level 0 with them") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    LevelMeshes main;
    main.receive(messages);
    Mesher mesher;
    std::array<std::size_t, vx::lod_levels> compared{};
    std::size_t shadowed = 0; // coarse meshes their shadows would change
    for (const auto& [chunk, mesh] : main.held) {
        const auto& [level, x, y, z] = chunk;
        if (mesh.empty) {
            continue;
        }
        const auto at = static_cast<std::uint8_t>(level);
        const ChunkPos pos{.x = x, .y = y, .z = z};
        const std::uint64_t made =
            mesher.hash(loop, pos, table, {.merge = true, .occlusion = level == 0}, at);
        CHECK(mesh.bytes == made);
        ++compared.at(at);
        if (level > 0 &&
            mesher.hash(loop, pos, table, {.merge = true, .occlusion = true}, at) != made) {
            ++shadowed;
        }
    }
    CHECK(compared[0] > 10);
    CHECK(compared[1] > 0);
    // The check can tell: on some coarse meshes, corner shadows would show.
    CHECK(shadowed > 0);
}

TEST_CASE("a far move unloads every level's meshes with their level, and each level ends holding "
          "what it shows") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
    LevelMeshes main;
    std::vector<ToMain> messages;
    for (const ChunkPos camera : {above_hills, ChunkPos{.x = 3 + 4'000, .y = 2, .z = 3}}) {
        move_camera(loop, camera);
        loop.settle();
        loop.take(messages);
        main.receive(messages);
        CHECK(keys(main.held) == shown_on_levels(loop));
    }
    // 4,000 chunks is 125 level-5 chunks: every level's disc moved on.
    CHECK(main.unloads == std::set<int>{0, 1, 2, 3, 4, 5});
}

TEST_CASE("the render distance is the terrain disc of all levels, and each level streams as far "
          "as it needs within it") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
    move_camera(loop, above_hills);
    loop.settle();
    ToWorld message;
    message.kind = ToWorld::Kind::view_distance;
    message.view_distance = 8;
    loop.to_world().push(std::move(message));
    loop.settle();
    vx::RegionSettings farther = small;
    farther.view_distance = 8;
    const vx::LodRings rings = rings_for({}, farther);
    for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
        INFO("level " << int{level});
        CHECK(loop.streamer(level).limits() ==
              vx::RegionLimits{.view_distance = rings.stream_distance(level),
                               .hysteresis = small.hysteresis});
    }
    // 8, 6, 4, 3, 2 and 2 of their own chunks.
    CHECK(loop.streamer(1).limits().view_distance == 6);
}

TEST_CASE("a sample counts every level's chunks, and its checks find every level whole") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // A small window's rings: level 0 streams 7 chunks of the 8 in view.
    vx::LodRings window;
    window.framebuffer_height = 480;
    window.pixels_per_cell = 8.0;
    vx::RegionSettings region = small;
    region.view_distance = 8;
    REQUIRE(rings_for(window, region).stream_distance(0) == 7);
    // No upload is acknowledged here, so no backpressure: over 512 meshes go.
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = region, .lod = window, .unuploaded_limit = 1'000'000});
    MainThread main;
    move_camera(loop, above_hills);
    loop.pass();
    const vx::app::WorldSample early = ask_sample(loop, main);
    CHECK_FALSE(early.complete);
    loop.settle();
    ToWorld message;
    message.kind = ToWorld::Kind::sample;
    message.sample_kind = SampleKind::checked;
    loop.to_world().push(std::move(message));
    loop.pass();
    std::vector<ToMain> messages;
    loop.take(messages);
    main.receive(messages);
    REQUIRE_FALSE(main.samples.empty());
    const vx::app::WorldSample done = main.samples.back();
    CHECK(done.complete);
    CHECK(done.broken == 0);
    CHECK(done.holes == 0);
    // The terrain disc's distance, not level 0's own, which the rings with
    // the region's near cylinder give.
    CHECK(done.view_distance == 8);
    CHECK(loop.streamer().limits().view_distance == 7);
    // Every level's chunks counted.
    std::uint64_t generated = 0;
    std::size_t bytes = 0;
    for (std::uint8_t level = 0; level < loop.levels(); ++level) {
        generated += loop.streamer(level).stats().generated;
        bytes += loop.streamer(level).chunk_bytes();
    }
    CHECK(done.generated == generated);
    CHECK(done.chunk_bytes == bytes);
    CHECK(loop.streamer(5).stats().generated > 0);
}

TEST_CASE("a pass's room is shared between the levels in turn, so none starves") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // Room for 3 jobs a pass, for 6 levels.
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = small, .lod = vx::LodRings{}, .queue_limit = 3});
    move_camera(loop, above_hills);
    std::array<std::size_t, vx::lod_levels> passes_with_a_job{};
    for (int pass = 0; pass < 6; ++pass) {
        jobs.run_until_idle();
        loop.pass();
        // Every earlier job ran and was taken in: those in flight started now.
        std::size_t started = 0;
        for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
            const std::size_t in_flight = loop.streamer(level).stats().jobs_in_flight;
            started += in_flight;
            passes_with_a_job.at(level) += in_flight > 0 ? 1 : 0;
        }
        CHECK(started + loop.meshes_in_flight() == 3);
    }
    // Each level had a job in half the passes: the turn moved on each pass.
    for (std::uint8_t level = 0; level < vx::lod_levels; ++level) {
        INFO("level " << int{level});
        CHECK(passes_with_a_job.at(level) == 3);
    }
}

TEST_CASE("coarse levels stream no near cylinder: high over the hills, only level 0 keeps chunks "
          "around the camera") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
    // 2,560 blocks up: on levels 1 to 3, rows 40, 20 and 10.
    const ChunkPos high{.x = 3, .y = 80, .z = 3};
    move_camera(loop, high);
    loop.settle();
    const auto top_row = [&loop](std::uint8_t level) {
        std::int32_t top = std::numeric_limits<std::int32_t>::min();
        const vx::ChunkRecords& records = loop.streamer(level).records();
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index)) {
                top = std::max(top, records[index].pos.y);
            }
        }
        return top;
    };
    CHECK(top_row(0) >= high.y);
    for (std::uint8_t level = 1; level <= 3; ++level) {
        INFO("level " << int{level});
        // The terrain and the rows planned around it, far under the camera.
        CHECK(top_row(level) < (high.y >> level) - 2);
    }
}

TEST_CASE("room a level leaves goes to the others, the finest first") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = small, .lod = vx::LodRings{}, .queue_limit = 6});
    move_camera(loop, {.x = 2, .y = 2, .z = 3});
    loop.settle();
    // A step east within the same level-1 chunk: only level 0 has new work,
    // so it takes the coarse levels' shares too.
    move_camera(loop, {.x = 3, .y = 2, .z = 3});
    loop.pass();
    for (std::uint8_t level = 1; level < vx::lod_levels; ++level) {
        CHECK(loop.streamer(level).stats().jobs_in_flight == 0);
    }
    CHECK(loop.streamer().stats().jobs_in_flight + loop.meshes_in_flight() == 6);
}

TEST_CASE("a coarse chunk's sight takes its own box, 32 x 2^level blocks a side") {
    // Looking north from the middle of chunk (0, 0, 0).
    vx::Viewer viewer = vx::viewer_at({.x = 0, .y = 0, .z = 0});
    viewer.view = vx::Frustum({.x = 0.0, .y = 0.0, .z = -1.0}, {.x = 1.0, .y = 0.0, .z = 0.0},
                              {.x = 0.0, .y = 1.0, .z = 0.0}, 0.7002075382097097, 16.0 / 9.0, 0.05);
    // Level-2 chunk (0, 0, -3) spans 256 to 384 blocks north of the origin:
    // ahead, its nearest point 272 blocks from the eye. Its level-0
    // namesake would be 80 away.
    const ChunkPos ahead{.x = 0, .y = 0, .z = -3};
    const vx::app::ChunkSight coarse = vx::app::chunk_sight(viewer, ahead, 256.0, 2);
    CHECK(coarse.in_view);
    CHECK_FALSE(coarse.near);
    CHECK(vx::app::chunk_sight(viewer, ahead, 280.0, 2).near);
    CHECK(vx::app::chunk_sight(viewer, ahead, 256.0).near);
    // Three behind: out of view.
    CHECK_FALSE(vx::app::chunk_sight(viewer, {.x = 0, .y = 0, .z = 3}, 256.0, 2).in_view);
}

TEST_CASE("the loop goes with every level's meshes queued, or done and not taken in, and every "
          "pin back") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // In debug builds each streamer's destructor asserts that every job
    // record came back to it.
    for (const bool run : {false, true}) {
        WorldLoop loop(generator, table, jobs, pool, {.region = small, .lod = vx::LodRings{}});
        move_camera(loop, above_hills);
        loop.settle();
        // Merging off: every level's meshes again.
        ToWorld message;
        message.kind = ToWorld::Kind::meshing;
        message.meshing = {.merge = false, .occlusion = true};
        loop.to_world().push(std::move(message));
        loop.pass();
        REQUIRE(loop.meshes_in_flight() > 0);
        if (run) {
            jobs.run_until_idle(); // done, their results not taken in
        }
    }
}

namespace {

// Whether a chunk around `chunk` is drawn too.
bool under_shown(const std::set<LevelChunk>& shown, LevelChunk chunk) {
    auto [level, x, y, z] = chunk;
    while (level < vx::max_lod_level) {
        x >>= 1;
        y >>= 1;
        z >>= 1;
        ++level;
        if (shown.contains({level, x, y, z})) {
            return true;
        }
    }
    return false;
}

// What a chunk offers the swaps, worked out apart from the loop: a chunk its
// level draws with a mesh held, empty or with faces; none yet; or nothing to
// draw, outside its level's drawn region once its column is known.
vx::LodReady ready_now(const WorldLoop& loop, const LevelMeshes& main, LevelChunk chunk) {
    const auto [level, x, y, z] = chunk;
    const vx::WorldStreamer& streamer = loop.streamer(static_cast<std::uint8_t>(level));
    const vx::ChunkRecord* record = streamer.find({.x = x, .y = y, .z = z});
    if (record != nullptr && record->drawn) {
        const auto mesh = main.held.find(chunk);
        if (mesh == main.held.end()) {
            return vx::LodReady::waiting;
        }
        return mesh->second.empty ? vx::LodReady::empty : vx::LodReady::mesh;
    }
    return streamer.band({.x = x, .z = z}).has_value() ? vx::LodReady::empty
                                                       : vx::LodReady::waiting;
}

// Whether a chunk in view is drawn: itself, a chunk around it, or each of its
// children in view, where one with nothing to draw counts. A chunk around it
// with nothing to draw at its level, where its place calls for that level,
// draws it too: there that level shows nothing. With `any_level`, so does
// one around it with nothing to draw at any level: what finer levels hold
// there comes once all of it can be drawn.
bool covered_now(const WorldLoop& loop, const LevelMeshes& main, const vx::LodRings& rings,
                 const vx::Vec3d& eye, LevelChunk chunk, bool any_level = false) {
    if (main.shown.contains(chunk) || under_shown(main.shown, chunk)) {
        return true;
    }
    const auto [level, x, y, z] = chunk;
    const vx::LodPos pos{.x = x, .y = y, .z = z, .level = static_cast<std::uint8_t>(level)};
    for (vx::LodPos around = pos; around.level < vx::max_lod_level;) {
        around = vx::parent(around);
        const LevelChunk at =
            level_chunk_of(around.level, {.x = around.x, .y = around.y, .z = around.z});
        if ((any_level || rings.drawn(eye, around)) &&
            ready_now(loop, main, at) == vx::LodReady::empty) {
            return true;
        }
    }
    if (level == 0) {
        return false;
    }
    return std::ranges::all_of(vx::children(pos), [&](vx::LodPos child) {
        const LevelChunk at =
            level_chunk_of(child.level, {.x = child.x, .y = child.y, .z = child.z});
        return !rings.in_view(eye, child) || ready_now(loop, main, at) == vx::LodReady::empty ||
               covered_now(loop, main, rings, eye, at, any_level);
    });
}

// The chunks the view wants that wait for their mesh while a chunk around
// them is ready with faces, and yet nothing draws them: after a teleport the
// view fills coarse first, never with holes the coarse levels could fill.
std::size_t unfilled(const WorldLoop& loop, const LevelMeshes& main, const vx::LodRings& rings,
                     const vx::Vec3d& eye) {
    std::size_t count = 0;
    for (const vx::LodPos target : vx::lod_cover(rings, eye, -8, 40)) {
        const LevelChunk at =
            level_chunk_of(target.level, {.x = target.x, .y = target.y, .z = target.z});
        if (ready_now(loop, main, at) != vx::LodReady::waiting) {
            continue;
        }
        bool coarser_ready = false;
        for (vx::LodPos up = target; up.level < vx::max_lod_level;) {
            up = vx::parent(up);
            coarser_ready =
                coarser_ready ||
                ready_now(loop, main,
                          level_chunk_of(up.level, {.x = up.x, .y = up.y, .z = up.z})) ==
                    vx::LodReady::mesh;
        }
        count += coarser_ready && !covered_now(loop, main, rings, eye, at, true) ? 1 : 0;
    }
    return count;
}

// A small window's rings over a region 8 chunks out: levels 0, 1 and 2 drawn
// near the hills.
vx::LodRings small_window() {
    vx::LodRings rings;
    rings.framebuffer_height = 480;
    rings.pixels_per_cell = 8.0;
    return rings;
}

constexpr vx::RegionSettings eight{
    .view_distance = 8, .near_distance = 1, .near_height = 1, .hysteresis = 2};

} // namespace

TEST_CASE("with RFC 0011's rings, once settled the swaps sent draw lod_cover's chunks that have "
          "faces, on several levels") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = eight, .lod = small_window(), .unuploaded_limit = 1'000'000});
    LevelMeshes main;
    std::vector<ToMain> messages;
    // Above the hills, far east, and 8 chunks from a spire 640 blocks tall,
    // past level 0's 7: only coarser levels draw it, up to row 19.
    for (const ChunkPos camera :
         {above_hills, ChunkPos{.x = 3 + 300, .y = 2, .z = 3}, ChunkPos{.x = 8, .y = 2, .z = 16}}) {
        move_camera(loop, camera);
        loop.settle();
        // The next frame's camera, after the last mesh: the selection runs.
        move_camera(loop, camera);
        loop.pass();
        loop.take(messages);
        main.receive(messages);
        const vx::Vec3d eye = vx::viewer_at(camera).eye;
        std::set<LevelChunk> expected;
        for (const vx::LodPos chunk :
             vx::lod_cover(rings_for(small_window(), eight), eye, -8, 40)) {
            const LevelChunk at =
                level_chunk_of(chunk.level, {.x = chunk.x, .y = chunk.y, .z = chunk.z});
            const auto mesh = main.held.find(at);
            if (mesh != main.held.end() && !mesh->second.empty) {
                expected.insert(at);
            }
        }
        CHECK(main.shown == expected);
        std::set<int> levels;
        for (const LevelChunk& chunk : main.shown) {
            levels.insert(std::get<0>(chunk));
        }
        CHECK(levels.size() >= 2);
        // The loop's own record of what it sent agrees.
        CHECK(loop.drawn().size() == main.shown.size());
    }
    CHECK(main.shown_bare == 0);
}

TEST_CASE("while the world streams in and the camera walks, a swap shows only chunks whose mesh "
          "with faces came first, and never a chunk with a chunk around it") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    // A frame's worth of jobs: 64 at a time.
    WorldLoop loop(
        generator, table, jobs, pool,
        {.region = eight, .lod = small_window(), .queue_limit = 64, .unuploaded_limit = 1'000'000});
    LevelMeshes main;
    std::vector<ToMain> messages;
    const vx::LodRings rings = rings_for(small_window(), eight);
    std::size_t overlaps = 0;
    std::size_t hidden = 0;     // hidden while still in view and streamed
    std::size_t uncovered = 0;  // of them, left with nothing drawing them
    std::size_t teleported = 0; // frames after the teleport with a chunk to fill
    std::size_t holes = 0;      // chunks a coarser one could have drawn
    std::size_t walked = 0;     // chunks drawn at the walk's end
    for (int frame = 0; frame < 140; ++frame) {
        // A walk east, a chunk every 3 frames; a jump 6 chunks on, and 6
        // back; then a teleport 60 chunks on, where the view fills anew.
        std::int32_t east = 3 + (std::min(frame, 72) / 3);
        if (frame >= 72) {
            east += frame < 92 ? 6 : 0;
        }
        east += frame >= 112 ? 60 : 0;
        const ChunkPos camera{.x = east, .y = 2, .z = 3};
        move_camera(loop, camera);
        loop.pass();
        loop.take(messages);
        main.receive(messages);
        for (const LevelChunk& chunk : main.shown) {
            overlaps += under_shown(main.shown, chunk) ? 1 : 0;
        }
        // Never neither: what a swap hid, still in view and streamed, is
        // drawn by something else, or has nothing left to draw.
        const vx::Vec3d eye = vx::viewer_at(camera).eye;
        for (const LevelChunk& chunk : main.hidden) {
            const auto [level, x, y, z] = chunk;
            const vx::LodPos pos{.x = x, .y = y, .z = z, .level = static_cast<std::uint8_t>(level)};
            if (!rings.in_view(eye, pos) ||
                loop.streamer(static_cast<std::uint8_t>(level)).find({.x = x, .y = y, .z = z}) ==
                    nullptr) {
                continue;
            }
            ++hidden;
            uncovered += covered_now(loop, main, rings, eye, chunk) ? 0 : 1;
        }
        main.hidden.clear();
        if (frame == 71) {
            walked = main.shown.size();
        }
        if (frame >= 112) {
            const std::size_t open = unfilled(loop, main, rings, eye);
            holes += open;
            teleported += 1;
        }
        jobs.run_until_idle();
    }
    CHECK(teleported > 0);
    CHECK(holes == 0);
    CHECK(main.swaps > 20);
    CHECK(walked > 20);
    CHECK(main.shown_bare == 0);
    CHECK(overlaps == 0);
    CHECK(hidden > 10);
    CHECK(uncovered == 0);
}

TEST_CASE("with levels off the loop sends no swaps") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool, {.region = small});
    MainThread main;
    move_camera(loop, above_hills);
    loop.settle();
    move_camera(loop, above_hills);
    loop.pass();
    std::vector<ToMain> messages;
    loop.take(messages);
    main.receive(messages);
    CHECK(main.swaps == 0);
    CHECK(loop.drawn().empty());
}

namespace {

// RFC 0011's open border set of a level's chunk toward `face`, worked out
// apart from the loop: its padded blocks from its level's streamer, the
// border that way all air, its outermost slice meshed; with the main mesh's
// border counts that way.
struct OpenSet {
    std::array<std::vector<vx::Quad>, 2> quads; // by pass
    std::array<std::uint32_t, 2> main_border{};
};

OpenSet open_set(const WorldLoop& loop, LevelChunk chunk, vx::Face face,
                 std::span<const vx::BlockVisual> visuals) {
    const auto [level, x, y, z] = chunk;
    const auto at = static_cast<std::uint8_t>(level);
    std::array<vx::ChunkStorage, 27> uniform;
    const auto chunk_at = [&](int dx, int dy, int dz) -> const vx::ChunkStorage* {
        const vx::ChunkRecord* record =
            loop.streamer(at).find({.x = x + dx, .y = y + dy, .z = z + dz});
        REQUIRE(record != nullptr);
        if (record->blocks != nullptr) {
            return &record->blocks->value();
        }
        vx::ChunkStorage& made =
            uniform.at(static_cast<std::size_t>((dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1))));
        made = vx::ChunkStorage::uniform(record->uniform);
        return &made;
    };
    vx::ChunkNeighborhood neighborhood(*chunk_at(0, 0, 0));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    neighborhood.set(dx, dy, dz, chunk_at(dx, dy, dz));
                }
            }
        }
    }
    auto padded = std::make_unique<vx::PaddedChunk>();
    vx::assemble_padded(neighborhood, *padded);
    vx::MeshScratch scratch;
    const vx::MeshOptions options{.merge = true, .occlusion = level == 0};
    OpenSet set;
    const vx::ChunkMesh main = vx::mesh_chunk(*padded, visuals, scratch, options);
    for (std::size_t pass = 0; pass < 2; ++pass) {
        set.main_border.at(pass) = main.border.at(pass).at(static_cast<std::size_t>(face));
    }
    vx::open_border(face, *padded);
    const vx::ChunkMesh border = vx::mesh_border(*padded, visuals, scratch, face, options);
    for (std::size_t pass = 0; pass < 2; ++pass) {
        const std::span<const vx::Quad> quads = border.range(static_cast<vx::Pass>(pass), face);
        set.quads.at(pass).assign(quads.begin(), quads.end());
    }
    return set;
}

} // namespace

TEST_CASE("above level 0 a mesh with faces carries an open border set toward each side: its "
          "outermost slice meshed as if nothing were drawn across") {
    const vx::PlaceholderGenerator generator(0x5EED, blocks);
    const std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs({});
    vx::MeshResultPool pool;
    WorldLoop loop(generator, table, jobs, pool,
                   {.region = eight, .lod = small_window(), .unuploaded_limit = 1'000'000});
    move_camera(loop, above_hills);
    loop.settle();
    std::vector<ToMain> messages;
    loop.take(messages);
    LevelMeshes main;
    main.receive(messages);
    std::size_t compared = 0;
    std::size_t faces = 0;
    for (const auto& [chunk, mesh] : main.held) {
        const auto level = std::get<0>(chunk);
        // Level 0 has no finer level to be drawn beside it; a mesh with no
        // faces is not drawn.
        if (level == 0 || mesh.empty) {
            CHECK(std::ranges::all_of(mesh.open, [](std::uint32_t count) { return count == 0; }));
            continue;
        }
        std::size_t start = 0;
        for (const std::uint32_t count : mesh.counts) {
            start += count;
        }
        for (std::size_t face = 0; face < 6; ++face) {
            INFO("level " << level << " face " << face);
            const OpenSet expected = open_set(loop, chunk, static_cast<vx::Face>(face), table);
            for (std::size_t pass = 0; pass < 2; ++pass) {
                CHECK(mesh.border.at((pass * 6) + face) == expected.main_border.at(pass));
                const std::uint32_t count = mesh.open.at((face * 2) + pass);
                REQUIRE(start + count <= mesh.quads.size());
                const std::vector<vx::Quad> sent(
                    mesh.quads.begin() + static_cast<std::ptrdiff_t>(start),
                    mesh.quads.begin() + static_cast<std::ptrdiff_t>(start + count));
                CHECK(sent == expected.quads.at(pass));
                start += count;
                faces += count;
            }
            ++compared;
        }
        CHECK(start == mesh.quads.size());
    }
    CHECK(compared > 100);
    CHECK(faces > 500);
}
