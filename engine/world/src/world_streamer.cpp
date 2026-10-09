#include <vx/world/world_streamer.hpp>

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/region.hpp>
#include <vx/world/versions.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace vx {
namespace {

// The column map keys a column by its chunk at y = 0.
ChunkPos column_key(ColumnPos column) noexcept {
    return {.x = column.x, .y = 0, .z = column.z};
}

// Surfaces are wanted 2 columns beyond the grown region's disc: the planned
// region reaches that far, and a band reads its column's four sides.
constexpr std::int32_t surface_reach = 2;

struct SurfaceJob {
    const Generator* generator;
    GenScratch* scratch;
    Inbox<StreamResult>* results;
    ColumnPos column;
    std::uint32_t load_id;

    void operator()(JobContext& context) const noexcept {
        StreamResult result;
        result.kind = StreamResult::Kind::surface;
        result.pos = column_key(column);
        result.load_id = load_id;
        result.surface = generator->surface_range(column, scratch[context.worker()]);
        // NOLINTNEXTLINE(performance-move-const-arg): trivially copyable until builds bring blocks
        results->push(std::move(result));
    }
};

struct PlanJob {
    const Generator* generator;
    GenScratch* scratch;
    Inbox<StreamResult>* results;
    ChunkPos pos;
    std::uint32_t load_id;

    void operator()(JobContext& context) const noexcept {
        StreamResult result;
        result.kind = StreamResult::Kind::plan;
        result.pos = pos;
        result.load_id = load_id;
        result.plan = generator->plan(pos, scratch[context.worker()]);
        // NOLINTNEXTLINE(performance-move-const-arg): trivially copyable until builds bring blocks
        results->push(std::move(result));
    }
};

// Calls `visit` with each chunk within one of `pos`, itself included, that
// the world holds.
template <class F> void around(ChunkPos pos, F&& visit) {
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const std::int64_t x = std::int64_t{pos.x} + dx;
                const std::int64_t y = std::int64_t{pos.y} + dy;
                const std::int64_t z = std::int64_t{pos.z} + dz;
                if (x >= lowest_chunk && x <= highest_chunk && y >= lowest_chunk &&
                    y <= highest_chunk && z >= lowest_chunk && z <= highest_chunk) {
                    visit(ChunkPos{.x = static_cast<std::int32_t>(x),
                                   .y = static_cast<std::int32_t>(y),
                                   .z = static_cast<std::int32_t>(z)});
                }
            }
        }
    }
}

} // namespace

template <class F> void WorldStreamer::start_waiting(std::vector<Waiting>& waiting, F&& try_start) {
    std::size_t kept = 0;
    bool full = false;
    for (const Waiting entry : waiting) {
        const Start started = full ? Start::full : try_start(entry);
        if (started != Start::done) {
            full = started == Start::full;
            waiting[kept++] = entry;
        }
    }
    waiting.resize(kept);
}

WorldStreamer::WorldStreamer(const Generator& generator, JobSystem& jobs, WakeEvent& results_ready,
                             RegionSettings settings)
    : generator_(&generator), jobs_(&jobs), settings_(settings),
      scratch_(std::max<std::uint32_t>(1, jobs.worker_count())),
      results_(std::size_t{2} * jobs.slot_count(), results_ready) {}

WorldStreamer::~WorldStreamer() {
    // What is still queued never runs; what ran has sent its result.
    for (std::uint32_t index = 0; index < chunks_.end_index(); ++index) {
        if (chunks_.live(index) && chunks_[index].job) {
            jobs_->cancel(chunks_[index].job);
        }
    }
    for (const Column& column : columns_) {
        if (column.job) {
            jobs_->cancel(column.job);
        }
    }
    results_.drain([](StreamResult&& /*result*/) {});
}

void WorldStreamer::set_camera(ChunkPos camera) {
    if (region_ && region_->camera() == camera) {
        return;
    }
    region_.emplace(camera, settings_);
    kept_.emplace(region_->grown(settings_.hysteresis));
    // Every column with a known band marks its drawn chunks afresh, then
    // the columns follow the region; new ones mark theirs once their
    // surfaces arrive.
    for (std::uint32_t index = 0; index < columns_.size(); ++index) {
        if (column_map_.find(column_key(columns_[index].pos)) == index) {
            refresh(columns_[index].pos);
        }
    }
    update_columns();
}

void WorldStreamer::update_columns() {
    if (!region_) {
        return;
    }
    const ColumnPos center{.x = region_->camera().x, .z = region_->camera().z};
    // The grown region's drawn columns reach as far as its disc or its near
    // cylinder, whichever is wider: at short view distances, the cylinder.
    const std::int32_t radius =
        std::max(settings_.view_distance, settings_.near_distance) + settings_.hysteresis;
    // Within reach of those: the nearest column of the square of half side
    // surface_reach around it is within their radius.
    const auto in_ring = [&](ColumnPos pos) {
        const std::int64_t nearest_x = std::clamp<std::int64_t>(
            center.x, std::int64_t{pos.x} - surface_reach, std::int64_t{pos.x} + surface_reach);
        const std::int64_t nearest_z = std::clamp<std::int64_t>(
            center.z, std::int64_t{pos.z} - surface_reach, std::int64_t{pos.z} + surface_reach);
        const std::int64_t dx = nearest_x - center.x;
        const std::int64_t dz = nearest_z - center.z;
        return (dx * dx) + (dz * dz) <= std::int64_t{radius} * radius && pos.x >= lowest_chunk &&
               pos.x <= highest_chunk && pos.z >= lowest_chunk && pos.z <= highest_chunk;
    };
    for (std::uint32_t index = 0; index < columns_.size(); ++index) {
        const ColumnPos pos = columns_[index].pos;
        if (column_map_.find(column_key(pos)) == index && !in_ring(pos)) {
            remove_column(index);
        }
    }
    const std::int32_t reach = radius + surface_reach;
    for (std::int32_t z = center.z - reach; z <= center.z + reach; ++z) {
        for (std::int32_t x = center.x - reach; x <= center.x + reach; ++x) {
            const ColumnPos pos{.x = x, .z = z};
            if (in_ring(pos) && column(pos) == nullptr) {
                add_column(pos);
            }
        }
    }
}

void WorldStreamer::add_column(ColumnPos pos) {
    std::uint32_t index = 0;
    if (free_columns_.empty()) {
        index = static_cast<std::uint32_t>(columns_.size());
        columns_.emplace_back();
    } else {
        index = free_columns_.back();
        free_columns_.pop_back();
        columns_[index] = Column{};
    }
    Column& column = columns_[index];
    column.pos = pos;
    column.load_id = next_load_id_++;
    column_map_.insert(column_key(pos), index);
    waiting_surfaces_.push_back({.index = index, .load_id = column.load_id});
}

void WorldStreamer::remove_column(std::uint32_t index) {
    Column& column = columns_[index];
    // Its drawn chunks were cleared by its last refresh: nothing else would.
    assert(column.drawn_kept.empty() && "a column left with drawn chunks");
    if (column.job && jobs_->cancel(column.job)) {
        --in_flight_;
    }
    column_map_.erase(column_key(column.pos));
    column = Column{};
    free_columns_.push_back(index);
}

const WorldStreamer::Column* WorldStreamer::column(ColumnPos pos) const noexcept {
    const std::optional<std::uint32_t> index = column_map_.find(column_key(pos));
    return index ? &columns_[*index] : nullptr;
}

std::optional<ChunkRange> WorldStreamer::band(ColumnPos pos) const noexcept {
    const Column* own = column(pos);
    if (own == nullptr || !own->known) {
        return std::nullopt;
    }
    std::array<ChunkRange, 4> sides{};
    const std::array<ColumnPos, 4> next = {
        ColumnPos{.x = pos.x + 1, .z = pos.z}, ColumnPos{.x = pos.x - 1, .z = pos.z},
        ColumnPos{.x = pos.x, .z = pos.z + 1}, ColumnPos{.x = pos.x, .z = pos.z - 1}};
    for (std::size_t at = 0; at < sides.size(); ++at) {
        // Past the world's edge nothing rises: the side stays empty.
        const bool outside = next[at].x < lowest_chunk || next[at].x > highest_chunk ||
                             next[at].z < lowest_chunk || next[at].z > highest_chunk;
        if (outside) {
            continue;
        }
        const Column* side = column(next[at]);
        if (side == nullptr || !side->known) {
            return std::nullopt;
        }
        sides[at] = side->surface;
    }
    return terrain_band(own->surface, sides);
}

void WorldStreamer::refresh(ColumnPos pos) {
    const std::optional<std::uint32_t> at = column_map_.find(column_key(pos));
    if (!at || !region_ || !kept_) {
        return;
    }
    ColumnRuns now;
    ColumnRuns kept;
    if (kept_->column_drawn(pos)) {
        const std::optional<ChunkRange> own_band = band(pos);
        if (!own_band) {
            return; // until its surfaces and its sides' arrive
        }
        now = region_->drawn(pos, *own_band);
        kept = kept_->drawn(pos, *own_band);
    }
    const ColumnRuns before = columns_[*at].drawn_kept;
    // Drawn inside the region, and still drawn while inside the grown one.
    const auto visit = [&](std::int32_t y) {
        const ChunkPos chunk{.x = pos.x, .y = y, .z = pos.z};
        const ChunkRecord* record = find(chunk);
        set_drawn(chunk,
                  now.contains(y) || (record != nullptr && record->drawn && kept.contains(y)));
    };
    for (const ChunkRange& run : before.runs()) {
        for (std::int32_t y = run.min_y; y <= run.max_y; ++y) {
            visit(y);
        }
    }
    for (const ChunkRange& run : kept.runs()) {
        for (std::int32_t y = run.min_y; y <= run.max_y; ++y) {
            if (!before.contains(y)) {
                visit(y);
            }
        }
    }
    columns_[*at].drawn_kept = kept;
}

std::uint32_t WorldStreamer::ensure(ChunkPos pos) {
    const std::optional<std::uint32_t> index = chunks_.find(pos);
    return index ? *index : add_chunk(pos);
}

std::uint32_t WorldStreamer::add_chunk(ChunkPos pos) {
    const std::uint32_t index = chunks_.add(pos, next_load_id_++);
    if (!make_uniform(index)) {
        waiting_plans_.push_back({.index = index, .load_id = chunks_[index].load_id});
    }
    return index;
}

bool WorldStreamer::make_uniform(std::uint32_t index) {
    const ChunkPos pos = chunks_[index].pos;
    std::optional<BlockState> uniform = generator_->out_of_reach(pos);
    const Column* own = column({.x = pos.x, .z = pos.z});
    if (!uniform && own != nullptr && own->known && pos.y > own->surface.max_y) {
        uniform = BlockState::air;
    }
    if (!uniform) {
        return false;
    }
    chunks_[index].uniform = *uniform;
    set_state(index, ChunkState::ready);
    advance(index);
    return true;
}

void WorldStreamer::set_drawn(ChunkPos pos, bool drawn) {
    const std::optional<std::uint32_t> found = chunks_.find(pos);
    if ((found && chunks_[*found].drawn) == drawn) {
        return;
    }
    if (drawn) {
        const std::uint32_t index = ensure(pos);
        chunks_[index].drawn = true;
        around(pos, [this](ChunkPos near) { add_drawn_near(near, 1); });
        return;
    }
    chunks_[*found].drawn = false;
    around(pos, [this](ChunkPos near) { add_drawn_near(near, -1); });
}

void WorldStreamer::add_drawn_near(ChunkPos pos, int delta) {
    // A chunk losing a drawn neighbor has a record: that neighbor loaded it.
    const std::optional<std::uint32_t> found =
        delta > 0 ? std::optional<std::uint32_t>(ensure(pos)) : chunks_.find(pos);
    if (!found) {
        return;
    }
    const std::uint32_t index = *found;
    ChunkRecord& record = chunks_[index];
    const bool was = record.drawn_near > 0;
    record.drawn_near = static_cast<std::uint8_t>(record.drawn_near + delta);
    assert(record.drawn_near <= 27);
    if ((record.drawn_near > 0) != was) {
        set_loaded(index, !was);
    }
}

void WorldStreamer::set_loaded(std::uint32_t index, bool loaded) {
    const ChunkPos pos = chunks_[index].pos;
    chunks_[index].loaded = loaded;
    around(pos, [this, loaded](ChunkPos near) { add_loaded_near(near, loaded ? 1 : -1); });
}

void WorldStreamer::add_loaded_near(ChunkPos pos, int delta) {
    // A chunk losing a loaded neighbor has a record: that neighbor planned it.
    const std::optional<std::uint32_t> found =
        delta > 0 ? std::optional<std::uint32_t>(ensure(pos)) : chunks_.find(pos);
    if (!found) {
        return;
    }
    const std::uint32_t index = *found;
    ChunkRecord& record = chunks_[index];
    const bool was = record.loaded_near > 0;
    record.loaded_near = static_cast<std::uint8_t>(record.loaded_near + delta);
    assert(record.loaded_near <= 27);
    if (!was && record.loaded_near > 0 && record.state == ChunkState::unloading) {
        // Back before its job reported: as it was.
        set_state(index, record.before_unloading);
        advance(index);
    } else if (was && record.loaded_near == 0) {
        leave(index);
    }
}

void WorldStreamer::leave(std::uint32_t index) {
    ChunkRecord& record = chunks_[index];
    if (record.state == ChunkState::unloading) {
        return;
    }
    if (record.job) {
        if (!jobs_->cancel(record.job)) {
            // It runs: the record goes when the job reports back.
            record.before_unloading = record.state;
            set_state(index, ChunkState::unloading);
            return;
        }
        --in_flight_;
    }
    free_chunk(index);
}

void WorldStreamer::free_chunk(std::uint32_t index) {
    const ChunkRecord& record = chunks_[index];
    if (record.plan != nullptr) {
        plans_.retire(*record.plan);
    }
    chunks_.remove(index, [this](std::uint32_t neighbor) { to_advance_.push_back(neighbor); });
    advance_neighbors();
}

void WorldStreamer::set_state(std::uint32_t index, ChunkState state) {
    chunks_.set_state(index, state,
                      [this](std::uint32_t neighbor) { to_advance_.push_back(neighbor); });
    advance_neighbors();
}

void WorldStreamer::advance_neighbors() {
    while (!to_advance_.empty()) {
        const std::uint32_t neighbor = to_advance_.back();
        to_advance_.pop_back();
        advance(neighbor);
    }
}

void WorldStreamer::advance(std::uint32_t index) {
    const ChunkRecord& record = chunks_[index];
    std::optional<ChunkState> next;
    switch (record.state) {
    case ChunkState::ready:
        if (record.ready_neighbors == 26) {
            next = ChunkState::complete;
        }
        break;
    case ChunkState::complete:
        if (record.ready_neighbors < 26) {
            next = ChunkState::ready;
        }
        break;
    default:
        break;
    }
    if (next) {
        // Ready and complete count the same for the neighbors: no neighbor's
        // count moves.
        chunks_.set_state(index, *next, [](std::uint32_t /*neighbor*/) {});
    }
}

void WorldStreamer::feed() {
    feed_surfaces();
    feed_plans();
}

void WorldStreamer::feed_surfaces() {
    start_waiting(waiting_surfaces_, [this](Waiting entry) {
        Column& column = columns_[entry.index];
        if (column.load_id != entry.load_id || column.known || column.job) {
            return Start::done;
        }
        column.job = jobs_->submit({}, SurfaceJob{.generator = generator_,
                                                  .scratch = scratch_.data(),
                                                  .results = &results_,
                                                  .column = column.pos,
                                                  .load_id = column.load_id});
        if (!column.job) {
            return Start::full;
        }
        ++in_flight_;
        return Start::done;
    });
}

void WorldStreamer::feed_plans() {
    start_waiting(waiting_plans_, [this](Waiting entry) {
        if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id ||
            chunks_[entry.index].state != ChunkState::queued) {
            return Start::done;
        }
        // Its column's surface tells whether it is above every surface, so
        // uniform, or needs a plan.
        const ChunkPos pos = chunks_[entry.index].pos;
        const Column* own = column({.x = pos.x, .z = pos.z});
        if (own == nullptr || !own->known) {
            return Start::later;
        }
        if (make_uniform(entry.index)) {
            return Start::done;
        }
        ChunkRecord& record = chunks_[entry.index];
        record.job = jobs_->submit({}, PlanJob{.generator = generator_,
                                               .scratch = scratch_.data(),
                                               .results = &results_,
                                               .pos = record.pos,
                                               .load_id = record.load_id});
        if (!record.job) {
            return Start::full;
        }
        ++in_flight_;
        set_state(entry.index, ChunkState::planning);
        return Start::done;
    });
}

void WorldStreamer::integrate() {
    results_.drain([this](StreamResult&& result) {
        --in_flight_;
        switch (result.kind) {
        case StreamResult::Kind::surface:
            integrate_surface(result);
            break;
        case StreamResult::Kind::plan:
            integrate_plan(result);
            break;
        }
    });
    // A new surface completes its own column's band, or a side's.
    for (const ColumnPos pos : dirty_) {
        refresh(pos);
        refresh({.x = pos.x + 1, .z = pos.z});
        refresh({.x = pos.x - 1, .z = pos.z});
        refresh({.x = pos.x, .z = pos.z + 1});
        refresh({.x = pos.x, .z = pos.z - 1});
    }
    dirty_.clear();
}

void WorldStreamer::integrate_surface(StreamResult& result) {
    const std::optional<std::uint32_t> index = column_map_.find(result.pos);
    if (!index || columns_[*index].load_id != result.load_id) {
        return; // the column left
    }
    Column& column = columns_[*index];
    column.surface = result.surface;
    column.known = true;
    column.job = {};
    dirty_.push_back(column.pos);
}

void WorldStreamer::integrate_plan(StreamResult& result) {
    const std::optional<std::uint32_t> index = chunks_.find(result.pos);
    if (!index || chunks_[*index].load_id != result.load_id) {
        return;
    }
    ChunkRecord& record = chunks_[*index];
    record.job = {};
    record.plan = &plans_.make(result.plan);
    if (record.state == ChunkState::unloading) {
        free_chunk(*index);
        return;
    }
    set_state(*index, ChunkState::planned);
    advance(*index);
}

const ChunkRecord* WorldStreamer::find(ChunkPos pos) const noexcept {
    const std::optional<std::uint32_t> index = chunks_.find(pos);
    return index ? &chunks_[*index] : nullptr;
}

std::optional<ChunkRange> WorldStreamer::surface(ColumnPos pos) const noexcept {
    const Column* found = column(pos);
    if (found == nullptr || !found->known) {
        return std::nullopt;
    }
    return found->surface;
}

StreamingStats WorldStreamer::stats() const noexcept {
    StreamingStats stats;
    for (std::uint32_t index = 0; index < chunks_.end_index(); ++index) {
        if (chunks_.live(index)) {
            ++stats.chunks[static_cast<std::size_t>(chunks_[index].state)];
        }
    }
    for (std::uint32_t index = 0; index < columns_.size(); ++index) {
        if (column_map_.find(column_key(columns_[index].pos)) == index) {
            ++stats.columns;
            if (columns_[index].known) {
                ++stats.surfaces_known;
            }
        }
    }
    stats.jobs_in_flight = in_flight_;
    stats.plan_versions = plans_.live();
    return stats;
}

} // namespace vx
