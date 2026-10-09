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
#include <vx/world/placeholder_generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/world_streamer.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
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
            case ToMain::Kind::landed:
                landings.push_back(message.landed);
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
    std::vector<std::chrono::steady_clock::time_point> tick_times;
    std::vector<bool> landings;

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
// main thread should hold for a chunk once the loop has settled.
class Mesher {
public:
    std::uint64_t hash(const WorldLoop& loop, ChunkPos pos,
                       std::span<const vx::BlockVisual> visuals, vx::MeshOptions options = {}) {
        std::array<vx::ChunkStorage, 27> uniform;
        const auto chunk_at = [&](int dx, int dy, int dz) -> const vx::ChunkStorage* {
            const vx::ChunkRecord* record =
                loop.streamer().find({.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz});
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
}

namespace {

// A loop with a player standing over block column (100, 100), streamed in
// around it unless `streamed` is false, and the main thread's view of what it
// sends.
struct Walker {
    vx::PlaceholderGenerator generator{0x5EED, blocks};
    std::vector<vx::BlockVisual> table = visuals();
    vx::JobSystem jobs{{}};
    vx::MeshResultPool pool;
    std::int32_t ground = generator.height(100, 100);
    std::optional<WorldLoop> loop;
    MainThread main;
    std::chrono::steady_clock::time_point now{};

    explicit Walker(double above, bool streamed = true) {
        vx::app::WorldLoopSettings settings{.region = small};
        settings.player = vx::app::WorldLoopSettings::PlayerStart{
            .spawn = {.x = 100.5, .y = ground + above, .z = 100.5}, .lowest = -64.0};
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
        loop->to_world().push(std::move(message));
        loop->pass();
        std::vector<ToMain> messages;
        loop->take(messages);
        main.receive(messages);
    }
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
