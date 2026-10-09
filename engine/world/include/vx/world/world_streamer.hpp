#pragma once

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/region.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// A job's result, on its way from a worker to the world thread (RFC 0005).
struct StreamResult {
    enum class Kind : std::uint8_t { surface };
    Kind kind = Kind::surface;
    // For a surface, the column at x and z.
    ChunkPos pos;
    // The load id of the column the job was for.
    std::uint32_t load_id = 0;
    ChunkRange surface;
};

// Counts for tests and the overlay.
struct StreamingStats {
    std::array<std::size_t, 8> chunks{}; // by ChunkState
    std::size_t columns = 0;
    std::size_t surfaces_known = 0;
    std::size_t jobs_in_flight = 0;
};

// The data half of RFC 0006's streaming, on RFC 0005's world thread: which
// chunks exist around the camera. Every column near the region gets its
// surface range first. Once a column's terrain band is known, its chunks in
// the drawn region are marked drawn, and kept drawn while inside the region
// grown for hysteresis; every chunk within one of a drawn chunk is loaded,
// and every chunk within one of a loaded one has a record, as RFC 0006's
// region grows, counted record by record so it holds whatever order columns
// arrive in. Plans and blocks come next.
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

    // The camera's chunk. When it changes, chunks join the region and those
    // outside the region grown for hysteresis leave it.
    void set_camera(ChunkPos camera);
    // Submits every job that can start.
    void feed();
    // Takes in every result the jobs have sent back.
    void integrate();

    [[nodiscard]] const ChunkRecord* find(ChunkPos pos) const noexcept;
    // Every record: for the mesh half, the overlay and tests.
    [[nodiscard]] const ChunkRecords& records() const noexcept { return chunks_; }
    // A column's surface range, once its job has reported: for tests and
    // the overlay.
    [[nodiscard]] std::optional<ChunkRange> surface(ColumnPos pos) const noexcept;
    [[nodiscard]] StreamingStats stats() const noexcept;

private:
    struct Column {
        ColumnPos pos;
        ChunkRange surface;
        bool known = false;
        JobHandle job;
        std::uint32_t load_id = 0;
        // Where its drawn chunks may be: its drawn runs in the grown region
        // at its last refresh.
        ColumnRuns drawn_kept;
    };

    // A chunk or column waiting for its job, as it was when it started
    // waiting: stale once its load id or state moves on.
    struct Waiting {
        std::uint32_t index = 0;
        std::uint32_t load_id = 0;
    };

    void update_columns();
    void add_column(ColumnPos pos);
    void remove_column(std::uint32_t index);
    [[nodiscard]] const Column* column(ColumnPos pos) const noexcept;
    // The column's terrain band, if the surfaces it needs are known.
    [[nodiscard]] std::optional<ChunkRange> band(ColumnPos pos) const noexcept;
    // Marks which of the column's chunks are drawn, once its band is known.
    void refresh(ColumnPos pos);

    // What start_waiting's callback did with an entry.
    enum class Start : std::uint8_t {
        done,  // started, or stale: the entry goes
        later, // not yet: the entry stays, and the next ones are tried
        full,  // no job slot or record: this entry and the rest stay
    };
    template <class F> static void start_waiting(std::vector<Waiting>& waiting, F&& try_start);

    // The chunk's record, made if it has none.
    std::uint32_t ensure(ChunkPos pos);
    std::uint32_t add_chunk(ChunkPos pos);
    void set_drawn(ChunkPos pos, bool drawn);
    // A chunk within one of `pos`, or `pos` itself, became drawn or loaded
    // (delta 1), or stopped (delta -1).
    void add_drawn_near(ChunkPos pos, int delta);
    void add_loaded_near(ChunkPos pos, int delta);
    void set_loaded(std::uint32_t index, bool loaded);
    void free_chunk(std::uint32_t index);

    void feed_surfaces();

    void integrate_surface(StreamResult& result);

    const Generator* generator_;
    JobSystem* jobs_;
    RegionSettings settings_;
    std::optional<Region> region_;
    std::optional<Region> kept_;

    ChunkRecords chunks_;
    ChunkMap column_map_; // a column at y = 0
    std::vector<Column> columns_;
    std::vector<std::uint32_t> free_columns_;

    std::vector<GenScratch> scratch_; // one for each worker
    Inbox<StreamResult> results_;

    std::vector<Waiting> waiting_surfaces_;
    std::vector<ColumnPos> dirty_; // columns whose surface arrived
    std::uint32_t next_load_id_ = 1;
    std::size_t in_flight_ = 0;
};

} // namespace vx
