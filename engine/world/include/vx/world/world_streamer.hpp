#pragma once

#include <vx/core/frustum.hpp>
#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/backlog.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/region.hpp>
#include <vx/world/versions.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace vx {

// What the world tells the mesh half about a chunk (RFC 0006).
struct ChunkEvent {
    enum class Kind : std::uint8_t {
        shown,   // complete and in the drawn region: it can be meshed
        changed, // its blocks changed, or a neighbor's next to its border
        hidden,  // no longer complete, or no longer in the drawn region
        freed,   // its record is gone, and the index will be reused
    };
    Kind kind = Kind::shown;
    ChunkPos pos;
    // The mesh half keeps its records at the same index.
    std::uint32_t record = 0;
};

// A job's result, on its way from a worker to the world thread (RFC 0005).
struct StreamResult {
    enum class Kind : std::uint8_t { surface, plan, build };
    Kind kind = Kind::surface;
    // A chunk, or for a surface the column at x and z.
    ChunkPos pos;
    // The load id of the chunk or column the job was for.
    std::uint32_t load_id = 0;
    // The generator's version the job was submitted for: a result made for
    // an older one is dropped.
    std::uint32_t version = 0;
    // A build's pins, given back when the result is integrated.
    JobRecord* record = nullptr;
    ChunkRange surface;
    ChunkPlan plan;
    ChunkStorage blocks;
};

// Counts for tests and the overlay.
struct StreamingStats {
    std::array<std::size_t, 8> chunks{}; // by ChunkState
    std::size_t columns = 0;
    std::size_t surfaces_known = 0;
    std::size_t jobs_in_flight = 0;
    // Entries in the backlogs, stale ones included, and plans parked: the
    // overlay's backlog length.
    std::size_t waiting = 0;
    // Chunks still to regenerate after the generator's version changed.
    std::size_t regenerating = 0;
    // Versions alive: current ones and frozen ones jobs still read.
    std::size_t block_versions = 0;
    std::size_t plan_versions = 0;
    // Since the streamer started: chunks built by a job, and chunks found
    // uniform without one (RFC 0006's flythrough counts both per second).
    std::uint64_t generated = 0;
    std::uint64_t uniform = 0;
};

// Where streaming looks from (RFC 0006): the camera's chunk, around which
// the region lies, and the eye and view that rank what waits for a job.
struct Viewer {
    ChunkPos chunk;
    // In blocks, 64-bit (RFC 0001).
    Vec3d eye;
    // The unit vector the eye looks along: a turn of more than 15 degrees
    // ranks the waiting jobs again.
    Vec3d forward{.x = 0.0, .y = 0.0, .z = -1.0};
    // By default it holds everything, so only distance ranks.
    Frustum view;
};

// A viewer at the chunk's middle that sees everything: for tests, and for
// a camera with no view yet.
[[nodiscard]] Viewer viewer_at(ChunkPos chunk) noexcept;

// The data half of RFC 0006's streaming, on RFC 0005's world thread: which
// chunks exist around the camera, and the jobs that make them. Every column
// near the region gets its surface range first. Once a column's terrain band
// is known, its chunks in the drawn region are marked drawn, and kept drawn
// while inside the region grown for hysteresis; every chunk within one of a
// drawn chunk is loaded, and every chunk within one of a loaded one has a
// record and a plan, as RFC 0006's region grows, counted record by record so
// it holds whatever order columns arrive in. A chunk out of the generator's
// reach, or above its column's surface, is uniform: it is ready at once, with
// no job. Every other chunk is planned, then built once it is loaded and it
// and its 26 neighbors have plans, then complete once they are all ready.
// Events tell the mesh half when a chunk can be meshed. Columns, plans and
// builds wait in backlogs, nearest and in view first, and go to the job
// queue in that order with their score as the job's key. When the
// generator's version changes, every column asks for its surface again and
// every unedited chunk regenerates, nearest first: chunks still on their
// way start over, and ready ones keep their blocks, so the mesh half keeps
// their meshes, until a plan of the new version and then, with their 26
// neighbors', new blocks replace them. An edited chunk keeps its blocks and
// the plan they came from.
class WorldStreamer {
public:
    // Results come back through an inbox that signals `results_ready`.
    WorldStreamer(const Generator& generator, JobSystem& jobs, WakeEvent& results_ready,
                  RegionSettings settings);
    WorldStreamer(const WorldStreamer&) = delete;
    WorldStreamer& operator=(const WorldStreamer&) = delete;
    WorldStreamer(WorldStreamer&&) = delete;
    WorldStreamer& operator=(WorldStreamer&&) = delete;
    // Cancels what is queued; every running job must have reported back.
    ~WorldStreamer();

    // Where the camera is and looks. When its chunk changes, chunks join
    // the region and those outside the region grown for hysteresis leave
    // it; when its chunk changes or it turns more than 15 degrees, every
    // waiting job is ranked again (RFC 0006), and true says so, for the
    // mesh half's.
    bool set_viewer(const Viewer& viewer);
    // set_viewer(viewer_at(camera)), keeping the view's direction.
    void set_camera(ChunkPos camera);
    // The terrain disc's radius from now on, in chunks: RFC 0006's Render
    // Distance option. The region is made again around the camera, and
    // chunks join it and leave it as they do on a move, hysteresis included.
    void set_view_distance(std::int32_t chunks);
    // The view distance and the hysteresis from now on, as RFC 0006's memory
    // budget lowers and raises them: as set_view_distance, and a smaller
    // hysteresis lets go of the chunks only the larger one kept.
    void set_limits(RegionLimits limits);
    [[nodiscard]] RegionLimits limits() const noexcept {
        return {.view_distance = settings_.view_distance, .hysteresis = settings_.hysteresis};
    }
    // Submits the jobs that can start, at most `room` of them: surfaces
    // first, then plans and builds, each lowest score first.
    void feed(std::size_t room = std::numeric_limits<std::size_t>::max());
    // Takes in the results the jobs sent back, one step at a time (RFC
    // 0006): it stops once a step's length has passed after a result, a
    // surface's including the records its column brings, so a burst such
    // as a teleport's never holds the world thread long. True while results
    // are left for the next call.
    bool integrate();
    static constexpr std::chrono::microseconds step_length{500};
    // The clock steps are measured on, instead of the steady clock: a
    // test's. It must outlive the streamer.
    void set_clock(const JobClock& clock) noexcept { clock_ = &clock; }
    // The events since the last call, moved into `out`.
    void take_events(std::vector<ChunkEvent>& out);

    // Writes a block of a ready or complete chunk (M5): bumps its version,
    // marks it edited, so it never leaves, and reports it and each neighbor
    // whose padded border holds the block as changed. False, changing
    // nothing, when the chunk is not ready.
    bool set_block(BlockPos pos, BlockState state);

    // For a mesh job (RFC 0005): a record pinning the complete chunk at
    // `pos` and its 26 neighbors, each its blocks or, for a uniform chunk,
    // its state, with the chunk's load id and `sequence`. Null when no
    // record is free or the chunk is not complete.
    [[nodiscard]] JobRecord* pin_neighborhood(ChunkPos pos, std::uint32_t sequence) noexcept;
    // The job reported back, or its cancel succeeded: unpins exactly what
    // the record names.
    void release(JobRecord& record) noexcept { job_records_.release(record); }
    // Shutdown's first step (RFC 0005): every job still queued is canceled
    // and gives back what it pinned. Nothing is fed or integrated after it.
    void cancel_queued() noexcept;

    // Pins held on chunks and plans, and copies made because an edit wrote
    // a chunk a job read: for Tracy's plots.
    [[nodiscard]] std::uint64_t pins() const noexcept { return blocks_.pins() + plans_.pins(); }
    [[nodiscard]] std::uint64_t copies() const noexcept { return blocks_.copies(); }
    // Results the jobs sent that integrate has not taken yet.
    [[nodiscard]] std::size_t results_waiting() const {
        return results_.size() + (pending_.size() - pending_at_);
    }

    // A chunk's rank for its jobs (RFC 0006), lowest first: also the mesh
    // half's.
    [[nodiscard]] std::uint32_t score(ChunkPos pos) const noexcept {
        return chunk_score(pos, viewer_.chunk, viewer_.eye, viewer_.view, settings_.level);
    }

    [[nodiscard]] const ChunkRecord* find(ChunkPos pos) const noexcept;
    // Every record: for the mesh half, the overlay and tests.
    [[nodiscard]] const ChunkRecords& records() const noexcept { return chunks_; }
    // A column's surface range, once its job has reported: for tests and
    // the overlay.
    [[nodiscard]] std::optional<ChunkRange> surface(ColumnPos pos) const noexcept;
    [[nodiscard]] StreamingStats stats() const noexcept;
    // The chunk data held, in bytes: the blocks of every chunk loaded, and
    // the records, columns and maps that keep them (RFC 0006's flythrough
    // reports it). Walks every record: a few times a second, not each frame.
    [[nodiscard]] std::size_t chunk_bytes() const noexcept;
    // The longest step integrate() took since the last call, which starts
    // the next count: RFC 0006's longest streaming step.
    [[nodiscard]] std::chrono::nanoseconds take_longest_step() noexcept;
    // The block at `pos`, if its chunk is ready or complete.
    [[nodiscard]] std::optional<BlockState> block(BlockPos pos) const noexcept;

private:
    struct Column {
        ColumnPos pos;
        ChunkRange surface;
        bool known = false;
        JobHandle job;
        std::uint32_t load_id = 0;
        // Its surface is the generator's current version's; until a new one
        // comes, an old one still serves the region's bands.
        bool current = true;
        // Where its drawn chunks may be: its drawn runs in the grown region
        // at its last refresh.
        ColumnRuns drawn_kept;
    };

    void update_columns();
    void add_column(ColumnPos pos);
    void remove_column(std::uint32_t index);
    [[nodiscard]] const Column* column(ColumnPos pos) const noexcept;
    // The column's terrain band, if the surfaces it needs are known.
    [[nodiscard]] std::optional<ChunkRange> band(ColumnPos pos) const noexcept;
    // Marks which of the column's chunks are drawn, once its band is known.
    void refresh(ColumnPos pos);

    // The region around `camera`, with the columns and records it brings.
    void move_region(ChunkPos camera);
    // Ranks every waiting entry for the viewer as it is now.
    void rescore();

    // What became of a backlog's entry.
    enum class Start : std::uint8_t {
        started, // a job was submitted
        dropped, // stale, or done without a job
        parked,  // its column's surface is not known yet
        full,    // no job slot or record: back in the backlog
    };

    // The chunk's record, made if it has none.
    std::uint32_t ensure(ChunkPos pos);
    std::uint32_t add_chunk(ChunkPos pos);
    // The state filling a chunk out of the generator's reach, or air above
    // its column's surface; none when it needs a plan, or when its column's
    // current surface is not known yet.
    [[nodiscard]] std::optional<BlockState> uniform_state(ChunkPos pos) const noexcept;
    // Ready at once when uniform. False when not, or when the surface is
    // unknown.
    bool make_uniform(std::uint32_t index);
    void set_drawn(ChunkPos pos, bool drawn);
    // A chunk within one of `pos`, or `pos` itself, became drawn or loaded
    // (delta 1), or stopped (delta -1).
    void add_drawn_near(ChunkPos pos, int delta);
    void add_loaded_near(ChunkPos pos, int delta);
    void set_loaded(std::uint32_t index, bool loaded);
    void leave(std::uint32_t index);
    void free_chunk(std::uint32_t index);
    // Moves the chunk to `state`, then moves on each neighbor whose count
    // that changed.
    void set_state(std::uint32_t index, ChunkState state);
    void advance_neighbors();
    // Moves a chunk on when a count or a flag lets it: to build_queued,
    // complete, or back. Never changes a neighbor's count.
    void advance(std::uint32_t index);
    void report(ChunkEvent::Kind kind, std::uint32_t index);
    // Its blocks changed: it, and each neighbor whose border reads them.
    void report_changed_around(std::uint32_t index);

    // The generator's version changed: every column and chunk starts again.
    void begin_regeneration();
    void restart(std::uint32_t index);
    void set_regeneration(std::uint32_t index, Regeneration regeneration) noexcept;
    // The chunk's plan, or that it needs none, is the current version's:
    // chunks around it waiting for that may move on.
    void plan_current(std::uint32_t index);
    // A ready chunk whose plan is current gets new blocks once its 26
    // neighbors' plans are current too, if it is loaded; one not loaded
    // goes back to planned, as a fresh load would leave it.
    void check_rebuild(std::uint32_t index);
    void demote(std::uint32_t index);
    void regenerate_uniform(std::uint32_t index, BlockState uniform);
    // It and its 26 neighbors have plans of the current version.
    [[nodiscard]] bool neighborhood_current(ChunkPos pos) const noexcept;

    void feed_surfaces(std::size_t& room);
    // Plans and builds, the lower score of the two first.
    void feed_chunks(std::size_t& room);
    [[nodiscard]] Start start_plan(const Backlog::Entry& entry);
    [[nodiscard]] Start start_build(const Backlog::Entry& entry);
    [[nodiscard]] Start start_replan(const Backlog::Entry& entry);
    [[nodiscard]] Start start_rebuild(const Backlog::Entry& entry);
    // Plans parked until their column's surface came, back in line.
    void unpark();
    // Pins the 27 plans around a chunk for its build; a uniform neighbor
    // has none.
    void pin_plans(JobRecord& pins, ChunkPos pos) noexcept;

    [[nodiscard]] std::chrono::steady_clock::time_point time() const noexcept {
        return clock_ != nullptr ? clock_->now() : std::chrono::steady_clock::now();
    }
    void integrate_surface(StreamResult& result);
    void integrate_plan(StreamResult& result);
    void integrate_build(StreamResult& result);

    const Generator* generator_;
    JobSystem* jobs_;
    RegionSettings settings_;
    std::optional<Region> region_;
    std::optional<Region> kept_;

    ChunkRecords chunks_;
    ChunkMap column_map_; // a column at y = 0
    std::vector<Column> columns_;
    std::vector<std::uint32_t> free_columns_;

    Versions<ChunkStorage> blocks_;
    Versions<ChunkPlan> plans_;
    std::uint64_t generated_ = 0;
    std::uint64_t classified_uniform_ = 0;
    std::chrono::nanoseconds longest_step_{0};
    JobRecords job_records_;
    std::vector<GenScratch> scratch_; // one for each worker
    Inbox<StreamResult> results_;
    // Results drained from the inbox and not yet taken in, from pending_at_.
    std::vector<StreamResult> pending_;
    std::size_t pending_at_ = 0;
    const JobClock* clock_ = nullptr; // null for the steady clock

    Viewer viewer_;
    Vec3d scored_forward_; // the view the backlogs were ranked for
    Backlog surfaces_backlog_;
    Backlog plans_backlog_;
    Backlog builds_backlog_;
    Backlog replans_backlog_;
    Backlog rebuilds_backlog_;
    std::vector<Backlog::Entry> parked_;
    std::vector<Backlog::Entry> parked_replans_;
    std::uint32_t version_ = 0;     // the generator's, as last seen
    std::size_t regenerating_ = 0;  // records whose regeneration is not none
    bool surfaces_arrived_ = false; // since the parked plans were last tried
    std::vector<ChunkEvent> events_;
    std::vector<std::uint32_t> to_advance_; // neighbors whose counts changed
    std::uint32_t next_load_id_ = 1;
    std::uint32_t next_version_ = 1;
    std::size_t in_flight_ = 0;
};

} // namespace vx
