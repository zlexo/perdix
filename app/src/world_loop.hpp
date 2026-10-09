#pragma once

#include "walking.hpp"

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/world/backlog.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/movement.hpp>
#include <vx/world/region.hpp>
#include <vx/world/streamed_boxes.hpp>
#include <vx/world/world_streamer.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vx::app {

// A chunk's mesh on its way to the main thread (RFC 0005), for RFC 0007's
// upload: its quads in a buffer from the mesh result pool, and how many of
// them lie in each of RFC 0003's twelve ranges, back to back in ChunkMesh's
// order. An empty mesh has no buffer.
struct MeshUpdate {
    ChunkPos pos;
    std::uint32_t load_id = 0;
    // Rises with every remesh of the chunk, so an older mesh is dropped.
    std::uint32_t generation = 0;
    std::array<std::uint32_t, 12> counts{}; // by pass, then face
    MeshBuffer quads;
};

// What a sample request asks for (RFC 0006's flythrough): whether the region
// is complete, as a phase waiting for it asks each frame, answered as a full
// sample once it is, so the phase ends on every number; every number, a few
// times a second; or every number and the invariants checked.
enum class SampleKind : std::uint8_t { completion, full, checked };

// Within this many blocks of the eye, a hole counts as near (RFC 0006).
inline constexpr double near_holes_distance = 256.0;

// How a chunk looks from a viewer: whether the view holds it, and whether
// its nearest point lies within `near` blocks of the eye.
struct ChunkSight {
    bool in_view = false;
    bool near = false;
};
[[nodiscard]] ChunkSight chunk_sight(const Viewer& viewer, ChunkPos pos, double near) noexcept;

// What the world thread reports when the main thread asks (RFC 0006's
// flythrough). A completion sample, the answer to a completion request
// while the region is not complete, holds only the totals, which cost
// nothing to read, and its time.
struct WorldSample {
    std::uint32_t id = 0; // the request's
    SampleKind kind = SampleKind::full;
    // When the world thread took it, on the loop's clock. Its {} lets
    // designated initializers leave it out (Linux Clang's
    // -Wmissing-designated-field-initializers).
    std::chrono::steady_clock::time_point at{}; // NOLINT(readability-redundant-member-init)
    // So far: chunks built by a job, chunks found uniform without one, meshes
    // made by a job, and meshes known empty, sent without one.
    std::uint64_t generated = 0;
    std::uint64_t uniform = 0;
    std::uint64_t meshed = 0;
    std::uint64_t skipped = 0;
    // Since the last full sample: the longest streaming step, and the longest
    // pass of the loop, less the time it spent answering samples.
    std::chrono::nanoseconds longest_step{0};
    std::chrono::nanoseconds longest_pass{0};
    // Chunks of the drawn region in view with no mesh sent and not known
    // empty, and of them those near the eye. The meshes sent that wait on
    // the main thread are its to count.
    std::uint32_t holes = 0;
    std::uint32_t near_holes = 0;
    // The chunk data held, in bytes.
    std::size_t chunk_bytes = 0;
    // Streaming is idle, and every chunk of the drawn region has its mesh
    // sent or is known empty.
    bool complete = false;
    // RFC 0006's invariants found broken, when the request asked for the
    // checks: records whose counts or state break the lifecycle's rules,
    // state counts that do not add up, a mesh half that holds a chunk the
    // world half does not show or the other way round, and records left
    // outside the region grown by the hysteresis.
    std::uint32_t broken = 0;
};

// What the world thread tells the main thread, in one ordered stream: a mesh
// never arrives after its chunk's unload.
struct ToMain {
    enum class Kind : std::uint8_t {
        mesh,   // a mesh, uploaded within the frame's budget
        batch,  // an edit's remeshes, uploaded whole in one frame
        unload, // the chunk's mesh goes
        player, // the player after a tick, or after landing
        landed, // whether the noclip camera's landing worked
        sample, // what the main thread asked for with ToWorld::Kind::sample
    };
    Kind kind = Kind::mesh;
    ChunkPos pos; // the unloaded chunk
    MeshUpdate mesh;
    std::vector<MeshUpdate> batch;
    WalkingFrame player{};
    // When the tick was due: the camera draws between the frame's two ticks
    // by the time since. Its {} lets designated initializers leave it out
    // (Linux Clang's -Wmissing-designated-field-initializers).
    std::chrono::steady_clock::time_point tick_at{}; // NOLINT(readability-redundant-member-init)
    bool landed = false;
    WorldSample sample{};
};

// What the main thread tells the world thread.
struct ToWorld {
    enum class Kind : std::uint8_t {
        camera,   // where the camera is and looks
        edit,     // a block to set; M5's actions will bring the view ray
        uploaded, // how far the main thread is with the meshes sent alone
        input,    // the keys held and the yaw, for the player's next ticks
        land,     // the noclip camera lands the player at `feet`
        meshing,  // RFC 0003's merge toggle: every chunk shown meshed again
        sample,   // a WorldSample, for the flythrough
        stop,
    };
    Kind kind = Kind::camera;
    Viewer camera;
    BlockPos block;
    BlockState state = BlockState::air;
    TickInput input{};
    Vec3d feet{};
    MeshOptions meshing{};
    std::uint32_t sample_id = 0; // echoed in the WorldSample
    SampleKind sample_kind = SampleKind::full;
    // Of the meshes sent alone (ToMain::Kind::mesh, never a batch's), how
    // many the main thread is done with so far: uploaded, or dropped without
    // an upload (empty, older than the one it holds, or its chunk's unload
    // came first). One it drops but never counts holds streaming back for
    // good.
    std::uint64_t uploaded = 0;
};

struct WorldLoopSettings {
    RegionSettings region;
    // Each pass tops every job level up to this many queued jobs (RFC 0005).
    std::uint32_t queue_limit = 256;
    // Streaming meshes sent but not yet uploaded, above which new streaming
    // mesh jobs wait (RFC 0005); an edit's remeshes never wait.
    std::uint32_t unuploaded_limit = 512;
    // What the streamer's steps are measured on; null for the steady clock.
    const JobClock* clock = nullptr;
    // The player (RFC 0010), on the loop's ticks: where it starts, and the
    // world's lowest block, 64 below which a fall returns it there. None
    // without one, as most tests run.
    struct PlayerStart {
        Vec3d spawn;
        double lowest = 0.0;
    };
    std::optional<PlayerStart> player = std::nullopt;
    // The block registry's physics (RFC 0010), indexed by state, which the
    // player collides with. Its {} lets designated initializers leave it out
    // (Linux Clang's -Wmissing-designated-field-initializers).
    std::span<const BlockPhysics> physics{}; // NOLINT(readability-redundant-member-init)
};

// A worker's memory for meshing (RFC 0005): RFC 0003's padded chunk and the
// mesher's scratch, about 1.7 MiB, made once, never shared.
struct MeshWorkerScratch {
    PaddedChunk padded;
    MeshScratch mesh;
};

// RFC 0005's world loop, which owns every chunk: it takes the main thread's
// messages, integrates the workers' results, and feeds the job queue; the
// world thread runs it, and tests step it. With RFC 0006's streamer it holds
// the mesh half of the lifecycle: each chunk shown is meshed by a job that
// pins it and its 26 neighbors, and each mesh newer than the last one sent
// goes to the main thread, the remeshes of one edit together.
class WorldLoop {
public:
    WorldLoop(const Generator& generator, std::span<const BlockVisual> visuals, JobSystem& jobs,
              MeshResultPool& pool, const WorldLoopSettings& settings);
    WorldLoop(const WorldLoop&) = delete;
    WorldLoop& operator=(const WorldLoop&) = delete;
    WorldLoop(WorldLoop&&) = delete;
    WorldLoop& operator=(WorldLoop&&) = delete;
    // Every job has reported back: the loop drains them before it goes.
    ~WorldLoop();

    // The main thread's way in, and the event the world thread sleeps on.
    [[nodiscard]] Inbox<ToWorld>& to_world() noexcept { return from_main_; }
    [[nodiscard]] WakeEvent& wake() noexcept { return wake_; }

    // One pass of the loop: the main thread's messages, the jobs' results,
    // then feeding. False once a stop arrived.
    bool pass();
    // A pass, then each tick that falls due by `now`, 20 a second: more than
    // a second behind, the rest are skipped. Returns when the next tick is
    // due, the deadline the world thread sleeps until, or `now` while the
    // streamer has results left from its step. After a stop, no tick runs.
    std::chrono::steady_clock::time_point step(std::chrono::steady_clock::time_point now);
    // Waits for no job, then integrates their results, until nothing runs or
    // waits, or a stop arrives: for tests. The destructor does shutdown's
    // waiting.
    void settle();
    // What went to the main thread since the last call, moved into `out`:
    // the main thread's, one caller at a time, while the world thread runs.
    void take(std::vector<ToMain>& out);

    // The rest is the world thread's: read it there, or once it has stopped.
    [[nodiscard]] bool stopping() const noexcept { return stopping_; }
    [[nodiscard]] const WorldStreamer& streamer() const noexcept { return streamer_; }
    // Streaming meshes sent but not yet uploaded.
    [[nodiscard]] std::uint64_t unuploaded() const noexcept { return sent_ - uploaded_; }
    // Mesh jobs queued or running.
    [[nodiscard]] std::size_t meshes_in_flight() const noexcept { return meshes_in_flight_; }
    // Meshes known to be empty without meshing, sent without a job.
    [[nodiscard]] std::uint64_t meshes_skipped() const noexcept { return meshes_skipped_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] std::uint64_t skipped_ticks() const noexcept { return skipped_; }
    // RFC 0006's catching up: a chunk of the near cylinder, all of it drawn,
    // has no mesh sent yet. Workers then run at user-initiated (RFC 0005).
    [[nodiscard]] bool catching_up() const noexcept { return catching_up_; }
    // The player, when the loop has one.
    [[nodiscard]] const Walking* walking() const noexcept {
        return walking_.has_value() ? &*walking_ : nullptr;
    }

    static constexpr std::chrono::milliseconds tick_length{50};

private:
    struct MeshRecord {
        enum class State : std::uint8_t { none, queued, meshing, sent };
        State state = State::none;
        bool urgent = false; // an edit's remesh
        bool dirty = false;  // changed while meshing: meshed again when it reports
        std::uint32_t load_id = 0;
        std::uint32_t generation = 0; // the last mesh job's: from one counter, never reused
        std::uint32_t forwarded = 0;  // the last generation sent, 0 for none
        std::uint32_t batch = 0;      // the pending edit batch it is in, 0 for none
        JobHandle job;
        JobRecord* pins = nullptr;
    };

    // An edit's remeshes, sent together once every member has reported.
    struct Batch {
        std::uint32_t id = 0;
        struct Member {
            std::uint32_t record = 0;
            std::uint32_t load_id = 0;
            std::uint32_t wanted = 0; // the first generation started after the edit
            MeshUpdate mesh;          // its newest mesh, once one arrived
            bool done = false;
        };
        std::vector<Member> members;
    };

    // A mesh job's result, back on the world thread.
    struct MeshResult {
        MeshUpdate update;
        JobRecord* pins = nullptr;
    };

    struct MeshContext {
        std::span<const BlockVisual> visuals;
        MeshResultPool* pool = nullptr;
        Inbox<MeshResult>* results = nullptr;
        MeshWorkerScratch* scratch = nullptr; // one for each worker
    };

    struct MeshJob {
        static constexpr const char* profile_name = "Mesh";
        const MeshContext* context;
        JobRecord* pins; // read only, until the world thread releases it
        ChunkPos pos;
        std::uint32_t generation;
        MeshOptions options; // the job's own: the loop's may change while it runs
        void operator()(JobContext& job) const noexcept;
    };

    void handle(ToWorld& message);
    // New mesh options: every chunk that has a mesh, or waits for one,
    // meshed again with them.
    void remesh(MeshOptions options);
    void edit(BlockPos block, BlockState state);
    void follow_events();
    MeshRecord& mesh_record(std::uint32_t index);
    void queue_mesh(std::uint32_t index, bool urgent);
    // The streaming meshes waiting, ranked as the streamer ranks its jobs.
    void rescore_meshes();
    void drop_mesh(std::uint32_t index, ChunkPos pos);
    void integrate_mesh(MeshResult& result);
    void forward(std::uint32_t index, MeshUpdate&& update);
    void send_ready_batches();
    void feed();
    bool start_mesh(std::uint32_t index, std::uint32_t key);
    // What a sample request is answered with, now, once the streamer's
    // events are followed. A full one walks every record and starts the next
    // longest step and pass: a few times a second, or each frame of a check.
    // A completion one walks them only once streaming is idle, and is full
    // once the region is complete.
    [[nodiscard]] WorldSample sample(SampleKind kind);
    // RFC 0006's invariants the flythrough's checks count, as
    // WorldSample::broken says.
    [[nodiscard]] std::uint32_t broken_invariants(const StreamingStats& stats);
    // A chunk of the drawn region with no mesh sent and not known empty.
    [[nodiscard]] bool missing(std::uint32_t index);
    [[nodiscard]] std::chrono::steady_clock::time_point time() const noexcept {
        return settings_.clock != nullptr ? settings_.clock->now()
                                          : std::chrono::steady_clock::now();
    }
    // Uniform air, or an opaque state among six uniform opaque neighbors:
    // no face of it can show.
    [[nodiscard]] bool known_empty(std::uint32_t index) const noexcept;
    // Sends a known empty mesh, as a job's result would go.
    void skip_mesh(std::uint32_t index);
    // A tick in Tracy: a frame of its own set, and a zone. `due` is when it
    // fell due on the loop's clock.
    void tick(std::chrono::steady_clock::time_point due);
    void run_tick(std::chrono::steady_clock::time_point due);
    // Puts the player at `feet` if it stands free there, and tells the main
    // thread whether it did.
    void land(Vec3d feet);
    void send_player(std::chrono::steady_clock::time_point at);
    [[nodiscard]] bool missing_near_camera();

    WakeEvent wake_; // before the inboxes that signal it
    WorldStreamer streamer_;
    JobSystem* jobs_;
    WorldLoopSettings settings_;
    Inbox<ToWorld> from_main_;
    Inbox<MeshResult> mesh_results_;
    std::vector<MeshWorkerScratch> scratch_;
    MeshContext context_;

    std::vector<MeshRecord> meshes_; // by the streamer's record index
    std::vector<std::uint32_t> waiting_urgent_;
    Backlog waiting_meshes_;
    std::vector<Batch> batches_;
    std::uint32_t next_batch_ = 1;
    std::uint32_t next_generation_ = 1;
    std::vector<ChunkEvent> events_;
    WakeEvent main_wake_; // the main thread polls each frame and never waits on it
    Inbox<ToMain> to_main_;
    std::optional<ChunkPos> camera_;
    std::optional<Viewer> viewer_; // where the camera looks, for the holes in view
    StreamedBoxes boxes_;          // the player's world: the streamer's chunks
    std::optional<Walking> walking_;
    TickInput input_;
    MeshOptions meshing_;
    std::chrono::steady_clock::time_point last_tick_;
    std::chrono::steady_clock::time_point next_tick_;
    bool ticking_ = false;
    std::uint64_t ticks_ = 0;
    std::uint64_t skipped_ = 0;
    bool catching_up_ = false;
    std::uint64_t sent_ = 0;     // streaming meshes sent
    std::uint64_t uploaded_ = 0; // of them, uploaded, as the main thread last said
    std::size_t meshes_in_flight_ = 0;
    std::uint64_t meshes_skipped_ = 0;
    std::uint64_t meshes_made_ = 0; // job results back
    // The longest pass since the last full sample, and this pass's time
    // spent answering samples, which it leaves out.
    std::chrono::nanoseconds longest_pass_{0};
    std::chrono::nanoseconds answering_{0};
    bool behind_ = false; // the streamer has results left from its last step
    bool stopping_ = false;
};

} // namespace vx::app
