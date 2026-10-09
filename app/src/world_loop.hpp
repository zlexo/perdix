#pragma once

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/region.hpp>
#include <vx/world/world_streamer.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
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

// What the world thread tells the main thread, in one ordered stream: a mesh
// never arrives after its chunk's unload.
struct ToMain {
    enum class Kind : std::uint8_t {
        mesh,   // a mesh, uploaded within the frame's budget
        unload, // the chunk's mesh goes
    };
    Kind kind = Kind::mesh;
    ChunkPos pos; // the unloaded chunk
    MeshUpdate mesh;
};

// What the main thread tells the world thread.
struct ToWorld {
    enum class Kind : std::uint8_t {
        camera, // the camera's chunk
        stop,
    };
    Kind kind = Kind::camera;
    ChunkPos camera;
};

struct WorldLoopSettings {
    RegionSettings region;
    // Each pass tops every job level up to this many queued jobs (RFC 0005).
    std::uint32_t queue_limit = 256;
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
// goes to the main thread.
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
    // Mesh jobs queued or running.
    [[nodiscard]] std::size_t meshes_in_flight() const noexcept { return meshes_in_flight_; }

private:
    struct MeshRecord {
        enum class State : std::uint8_t { none, queued, meshing, sent };
        State state = State::none;
        bool dirty = false; // changed while meshing: meshed again when it reports
        std::uint32_t load_id = 0;
        std::uint32_t generation = 0; // the last mesh job's: from one counter, never reused
        std::uint32_t forwarded = 0;  // the last generation sent, 0 for none
        JobHandle job;
        JobRecord* pins = nullptr;
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
        const MeshContext* context;
        JobRecord* pins; // read only, until the world thread releases it
        ChunkPos pos;
        std::uint32_t generation;
        void operator()(JobContext& job) const noexcept;
    };

    void handle(ToWorld& message);
    void follow_events();
    MeshRecord& mesh_record(std::uint32_t index);
    void queue_mesh(std::uint32_t index);
    void drop_mesh(std::uint32_t index, ChunkPos pos);
    void integrate_mesh(MeshResult& result);
    void forward(std::uint32_t index, MeshUpdate&& update);
    void feed();
    bool start_mesh(std::uint32_t index);

    WakeEvent wake_; // before the inboxes that signal it
    WorldStreamer streamer_;
    JobSystem* jobs_;
    WorldLoopSettings settings_;
    Inbox<ToWorld> from_main_;
    Inbox<MeshResult> mesh_results_;
    std::vector<MeshWorkerScratch> scratch_;
    MeshContext context_;

    std::vector<MeshRecord> meshes_; // by the streamer's record index
    std::vector<std::uint32_t> waiting_meshes_;
    std::uint32_t next_generation_ = 1;
    std::vector<ChunkEvent> events_;
    WakeEvent main_wake_; // the main thread polls each frame and never waits on it
    Inbox<ToMain> to_main_;
    std::size_t meshes_in_flight_ = 0;
    bool stopping_ = false;
};

} // namespace vx::app
