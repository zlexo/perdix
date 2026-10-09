#include <vx/world/world_streamer.hpp>

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/region.hpp>
#include <vx/world/versions.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
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

// Where a build's plans and a mesh job's chunks sit in their records: x
// fastest, then z, then y, so 13 is the middle.
constexpr std::size_t around_index(std::int32_t dx, std::int32_t dy, std::int32_t dz) noexcept {
    const std::int32_t at = (dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1));
    return static_cast<std::size_t>(at);
}

struct SurfaceJob {
    [[maybe_unused]] static constexpr const char* profile_name = "Surface";
    const Generator* generator;
    GenScratch* scratch;
    Inbox<StreamResult>* results;
    ColumnPos column;
    std::uint32_t load_id;
    std::uint32_t version;

    void operator()(JobContext& context) const noexcept {
        context.describe(column.x, column.z);
        StreamResult result;
        result.kind = StreamResult::Kind::surface;
        result.pos = column_key(column);
        result.load_id = load_id;
        result.version = version;
        result.surface = generator->surface_range(column, scratch[context.worker()]);
        results->push(std::move(result));
    }
};

struct PlanJob {
    [[maybe_unused]] static constexpr const char* profile_name = "Plan";
    const Generator* generator;
    GenScratch* scratch;
    Inbox<StreamResult>* results;
    ChunkPos pos;
    std::uint32_t load_id;
    std::uint32_t version;

    void operator()(JobContext& context) const noexcept {
        context.describe(pos.x, pos.y, pos.z);
        StreamResult result;
        result.kind = StreamResult::Kind::plan;
        result.pos = pos;
        result.load_id = load_id;
        result.version = version;
        result.plan = generator->plan(pos, scratch[context.worker()]);
        results->push(std::move(result));
    }
};

// Reads the 27 plans its record pinned. "Generate" in RFC 0005's Tracy.
struct BuildJob {
    [[maybe_unused]] static constexpr const char* profile_name = "Generate";
    const Generator* generator;
    GenScratch* scratch;
    Inbox<StreamResult>* results;
    JobRecord* record;
    ChunkPos pos;
    std::uint32_t version;

    void operator()(JobContext& context) const noexcept {
        context.describe(pos.x, pos.y, pos.z);
        std::array<const ChunkPlan*, 27> plans{};
        for (std::size_t at = 0; at < plans.size(); ++at) {
            const Version<ChunkPlan>* plan = record->plans[at];
            plans[at] = plan != nullptr ? &plan->value() : nullptr;
        }
        StreamResult result;
        result.kind = StreamResult::Kind::build;
        result.pos = pos;
        result.load_id = record->load_id;
        result.version = version;
        result.record = record;
        result.blocks = generator->build(plans, scratch[context.worker()]);
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

WorldStreamer::WorldStreamer(const Generator& generator, JobSystem& jobs, WakeEvent& results_ready,
                             RegionSettings settings)
    : generator_(&generator), jobs_(&jobs), settings_(settings),
      job_records_(jobs.slot_count(), blocks_, plans_),
      scratch_(std::max<std::uint32_t>(1, jobs.worker_count())),
      results_(std::size_t{2} * jobs.slot_count(), results_ready), version_(generator.version()) {}

void WorldStreamer::cancel_queued() noexcept {
    // A job canceled already, or done, cancels no more: this may run twice.
    for (std::uint32_t index = 0; index < chunks_.end_index(); ++index) {
        if (!chunks_.live(index) || !chunks_[index].job) {
            continue;
        }
        const ChunkRecord& record = chunks_[index];
        if (jobs_->cancel(record.job) && record.job_record != nullptr) {
            job_records_.release(*record.job_record);
        }
    }
    for (const Column& column : columns_) {
        if (column.job) {
            jobs_->cancel(column.job);
        }
    }
}

WorldStreamer::~WorldStreamer() {
    // What is still queued never runs; what ran has sent its result.
    cancel_queued();
    results_.drain([this](StreamResult&& result) { pending_.push_back(std::move(result)); });
    for (std::size_t at = pending_at_; at < pending_.size(); ++at) {
        if (pending_[at].record != nullptr) {
            job_records_.release(*pending_[at].record);
        }
    }
    assert(job_records_.in_use() == 0 && "a job still runs: the world loop drains them first");
}

Viewer viewer_at(ChunkPos chunk) noexcept {
    const auto middle = [](std::int32_t at) {
        return (static_cast<double>(at) * chunk_size) + (chunk_size / 2.0);
    };
    return {.chunk = chunk,
            .eye = {.x = middle(chunk.x), .y = middle(chunk.y), .z = middle(chunk.z)},
            .forward = {.x = 0.0, .y = 0.0, .z = -1.0},
            .view = {}};
}

bool WorldStreamer::set_viewer(const Viewer& viewer) {
    const bool moved = !region_ || region_->camera() != viewer.chunk;
    // More than 15 degrees from the view the scores were made for.
    constexpr double cos_15_degrees = 0.9659258262890683;
    const Vec3d& was = scored_forward_;
    const bool turned =
        (viewer.forward.x * was.x) + (viewer.forward.y * was.y) + (viewer.forward.z * was.z) <
        cos_15_degrees;
    viewer_ = viewer;
    if (moved) {
        move_region(viewer.chunk);
    }
    if (!moved && !turned) {
        return false;
    }
    scored_forward_ = viewer.forward;
    rescore();
    return true;
}

void WorldStreamer::set_view_distance(std::int32_t chunks) {
    if (chunks == settings_.view_distance) {
        return;
    }
    settings_.view_distance = chunks;
    if (region_) {
        move_region(region_->camera());
        // Plans parked until their column's surface came are tried again:
        // a nearer distance may bring no surface, and those of the chunks it
        // let go would wait for one for good.
        surfaces_arrived_ = true;
    }
}

void WorldStreamer::set_camera(ChunkPos camera) {
    Viewer viewer = viewer_at(camera);
    viewer.forward = viewer_.forward;
    set_viewer(viewer);
}

void WorldStreamer::move_region(ChunkPos camera) {
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
    surfaces_backlog_.push({.key = column_score(pos.x, pos.z, viewer_.chunk),
                            .index = index,
                            .load_id = column.load_id});
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
        plans_backlog_.push({.key = score(pos), .index = index, .load_id = chunks_[index].load_id});
    }
    return index;
}

std::optional<BlockState> WorldStreamer::uniform_state(ChunkPos pos) const noexcept {
    if (const std::optional<BlockState> uniform = generator_->out_of_reach(pos)) {
        return uniform;
    }
    const Column* own = column({.x = pos.x, .z = pos.z});
    if (own != nullptr && own->known && own->current && pos.y > own->surface.max_y) {
        return BlockState::air;
    }
    return std::nullopt;
}

bool WorldStreamer::make_uniform(std::uint32_t index) {
    const std::optional<BlockState> uniform = uniform_state(chunks_[index].pos);
    if (!uniform) {
        return false;
    }
    chunks_[index].uniform = *uniform;
    ++classified_uniform_;
    set_state(index, ChunkState::ready);
    advance(index);
    plan_current(index);
    return true;
}

void WorldStreamer::set_drawn(ChunkPos pos, bool drawn) {
    const std::optional<std::uint32_t> found = chunks_.find(pos);
    if ((found && chunks_[*found].drawn) == drawn) {
        return;
    }
    if (drawn) {
        // Shown now if complete already; if the loads below complete it,
        // advance reports it then, once.
        const std::uint32_t index = ensure(pos);
        chunks_[index].drawn = true;
        if (shown(chunks_[index])) {
            report(ChunkEvent::Kind::shown, index);
        }
        around(pos, [this](ChunkPos near) { add_drawn_near(near, 1); });
        return;
    }
    // Before the counts drop: the record may go with them.
    if (shown(chunks_[*found])) {
        report(ChunkEvent::Kind::hidden, *found);
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
    if (!loaded && chunks_.live(index) && chunks_[index].pos == pos) {
        check_rebuild(index);
    }
    // Gone, when it was the last loaded chunk near itself.
    if (chunks_.live(index) && chunks_[index].pos == pos) {
        advance(index);
    }
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
    // Edited chunks stay until saving exists (M8), outside every region.
    if (record.edited || record.state == ChunkState::unloading) {
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
        if (record.job_record != nullptr) {
            job_records_.release(*record.job_record);
        }
    }
    free_chunk(index);
}

void WorldStreamer::free_chunk(std::uint32_t index) {
    set_regeneration(index, Regeneration::none);
    const ChunkRecord& record = chunks_[index];
    if (shown(record)) {
        report(ChunkEvent::Kind::hidden, index);
    }
    if (record.blocks != nullptr) {
        blocks_.retire(*record.blocks);
    }
    if (record.plan != nullptr) {
        plans_.retire(*record.plan);
    }
    const ChunkPos pos = record.pos;
    chunks_.remove(index, [this](std::uint32_t neighbor) { to_advance_.push_back(neighbor); });
    events_.push_back({.kind = ChunkEvent::Kind::freed, .pos = pos, .record = index});
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
    const bool was_shown = shown(record);
    std::optional<ChunkState> next;
    switch (record.state) {
    case ChunkState::planned:
        if (record.loaded && record.planned_neighbors == 26 &&
            (regenerating_ == 0 || neighborhood_current(record.pos))) {
            next = ChunkState::build_queued;
            builds_backlog_.push(
                {.key = score(record.pos), .index = index, .load_id = record.load_id});
        }
        break;
    case ChunkState::build_queued:
        if (!record.loaded || record.planned_neighbors < 26 ||
            (regenerating_ > 0 && !neighborhood_current(record.pos))) {
            next = ChunkState::planned;
        }
        break;
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
        // Planned and build-queued count the same for the neighbors, and so
        // do ready and complete: no neighbor's count moves.
        chunks_.set_state(index, *next, [](std::uint32_t /*neighbor*/) {});
    }
    if (shown(record) != was_shown) {
        report(was_shown ? ChunkEvent::Kind::hidden : ChunkEvent::Kind::shown, index);
    }
}

void WorldStreamer::report(ChunkEvent::Kind kind, std::uint32_t index) {
    events_.push_back({.kind = kind, .pos = chunks_[index].pos, .record = index});
}

void WorldStreamer::report_changed_around(std::uint32_t index) {
    around(chunks_[index].pos, [this](ChunkPos near) {
        const std::optional<std::uint32_t> neighbor = chunks_.find(near);
        if (neighbor && counts_as_ready(chunks_[*neighbor].state)) {
            report(ChunkEvent::Kind::changed, *neighbor);
        }
    });
}

void WorldStreamer::begin_regeneration() {
    version_ = generator_->version();
    // Every column asks for its surface again: the old one serves the
    // region's bands until the new one comes.
    for (std::uint32_t index = 0; index < columns_.size(); ++index) {
        Column& column = columns_[index];
        if (column_map_.find(column_key(column.pos)) != index) {
            continue;
        }
        // One in line already, or whose job runs, stays so: a version
        // change at every pass, as a dragged slider makes, queues it once.
        const bool settled = column.known && column.current;
        column.current = false;
        const bool canceled = column.job && jobs_->cancel(column.job);
        if (canceled) {
            --in_flight_;
            column.job = {};
        }
        if (settled || canceled) {
            surfaces_backlog_.push({.key = column_score(column.pos.x, column.pos.z, viewer_.chunk),
                                    .index = index,
                                    .load_id = column.load_id});
        }
    }
    for (std::uint32_t index = 0; index < chunks_.end_index(); ++index) {
        if (chunks_.live(index)) {
            restart(index);
        }
    }
}

void WorldStreamer::restart(std::uint32_t index) {
    ChunkRecord& record = chunks_[index];
    // A queued job of the old version never runs; a running one's result
    // is dropped when it comes, and the chunk asks again then.
    const bool canceled = record.job && jobs_->cancel(record.job);
    if (canceled) {
        --in_flight_;
        record.job = {};
        if (record.job_record != nullptr) {
            job_records_.release(*record.job_record);
            record.job_record = nullptr;
        }
    }
    switch (record.state) {
    case ChunkState::queued:
        break;
    case ChunkState::unloading:
        // Its job runs. A ready one back in range before the job reports
        // holds the old version's plan and blocks: it replans then.
        if (counts_as_ready(record.before_unloading) && !record.edited) {
            set_regeneration(index, Regeneration::replan);
        }
        break;
    case ChunkState::planning:
    case ChunkState::building:
        if (record.job) {
            // Running: it starts over when its result comes. Till then a
            // build's plan is the old version's: no neighbor may read it.
            if (record.state == ChunkState::building) {
                set_regeneration(index, Regeneration::replan);
            }
            break;
        }
        [[fallthrough]];
    case ChunkState::planned:
    case ChunkState::build_queued:
        // Nothing of it is on screen yet: it starts over as a new chunk.
        if (record.plan != nullptr) {
            plans_.retire(*record.plan);
            record.plan = nullptr;
        }
        set_state(index, ChunkState::queued);
        plans_backlog_.push({.key = score(record.pos), .index = index, .load_id = record.load_id});
        break;
    case ChunkState::ready:
    case ChunkState::complete:
        // An edited chunk keeps the blocks it was made with, and so the plan
        // its neighbors' borders must match: it does not regenerate.
        if (record.edited) {
            break;
        }
        // Its blocks stay, and its mesh, until new ones are made. One
        // waiting for a plan already is in line already.
        if (!record.job && (record.regeneration != Regeneration::replan || canceled)) {
            replans_backlog_.push(
                {.key = score(record.pos), .index = index, .load_id = record.load_id});
        }
        set_regeneration(index, Regeneration::replan);
        break;
    }
}

void WorldStreamer::set_regeneration(std::uint32_t index, Regeneration regeneration) noexcept {
    Regeneration& now = chunks_[index].regeneration;
    if (now == Regeneration::none && regeneration != Regeneration::none) {
        ++regenerating_;
    } else if (now != Regeneration::none && regeneration == Regeneration::none) {
        --regenerating_;
    }
    now = regeneration;
}

bool WorldStreamer::neighborhood_current(ChunkPos pos) const noexcept {
    bool current = true;
    around(pos, [&](ChunkPos near) {
        const std::optional<std::uint32_t> index = chunks_.find(near);
        current = current && index && counts_as_planned(chunks_[*index].state) &&
                  chunks_[*index].regeneration != Regeneration::replan;
    });
    return current;
}

void WorldStreamer::plan_current(std::uint32_t index) {
    if (regenerating_ == 0) {
        return;
    }
    around(chunks_[index].pos, [this](ChunkPos near) {
        const std::optional<std::uint32_t> neighbor = chunks_.find(near);
        if (!neighbor) {
            return;
        }
        if (chunks_[*neighbor].regeneration == Regeneration::rebuild) {
            check_rebuild(*neighbor);
        } else if (chunks_[*neighbor].state == ChunkState::planned) {
            advance(*neighbor);
        }
    });
}

void WorldStreamer::check_rebuild(std::uint32_t index) {
    const ChunkRecord& record = chunks_[index];
    if (record.regeneration != Regeneration::rebuild || record.job) {
        return;
    }
    if (!record.loaded) {
        demote(index);
        return;
    }
    if (neighborhood_current(record.pos)) {
        rebuilds_backlog_.push(
            {.key = score(record.pos), .index = index, .load_id = record.load_id});
    }
}

void WorldStreamer::demote(std::uint32_t index) {
    ChunkRecord& record = chunks_[index];
    if (record.blocks != nullptr) {
        blocks_.retire(*record.blocks);
        record.blocks = nullptr;
    }
    set_regeneration(index, Regeneration::none);
    set_state(index, ChunkState::planned);
    advance(index);
}

void WorldStreamer::regenerate_uniform(std::uint32_t index, BlockState uniform) {
    ChunkRecord& record = chunks_[index];
    if (record.plan != nullptr) {
        plans_.retire(*record.plan);
        record.plan = nullptr;
    }
    if (record.blocks != nullptr || record.uniform != uniform) {
        if (record.blocks != nullptr) {
            blocks_.retire(*record.blocks);
            record.blocks = nullptr;
        }
        record.uniform = uniform;
        report_changed_around(index);
    }
    ++classified_uniform_;
    set_regeneration(index, Regeneration::none);
    plan_current(index);
}

void WorldStreamer::rescore() {
    surfaces_backlog_.rescore([this](const Backlog::Entry& entry) -> std::optional<std::uint32_t> {
        const Column& column = columns_[entry.index];
        if (column.load_id != entry.load_id) {
            return std::nullopt;
        }
        return column_score(column.pos.x, column.pos.z, viewer_.chunk);
    });
    const auto chunk = [this](const Backlog::Entry& entry) -> std::optional<std::uint32_t> {
        if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id) {
            return std::nullopt;
        }
        return score(chunks_[entry.index].pos);
    };
    plans_backlog_.rescore(chunk);
    builds_backlog_.rescore(chunk);
    replans_backlog_.rescore(chunk);
    rebuilds_backlog_.rescore(chunk);
}

void WorldStreamer::feed(std::size_t room) {
    if (generator_->version() != version_) {
        begin_regeneration();
    }
    feed_surfaces(room);
    unpark();
    feed_chunks(room);
}

void WorldStreamer::feed_surfaces(std::size_t& room) {
    while (room > 0 && !surfaces_backlog_.empty()) {
        const Backlog::Entry entry = surfaces_backlog_.top();
        Column& column = columns_[entry.index];
        if (column.load_id != entry.load_id || (column.known && column.current) || column.job) {
            surfaces_backlog_.pop();
            continue;
        }
        column.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                                   SurfaceJob{.generator = generator_,
                                              .scratch = scratch_.data(),
                                              .results = &results_,
                                              .column = column.pos,
                                              .load_id = column.load_id,
                                              .version = version_});
        if (!column.job) {
            return; // no job slot: it stays first in line
        }
        surfaces_backlog_.pop();
        ++in_flight_;
        --room;
    }
}

void WorldStreamer::unpark() {
    if (!surfaces_arrived_) {
        return;
    }
    surfaces_arrived_ = false;
    const auto back_in_line = [this](Backlog& backlog) {
        return [this, &backlog](const Backlog::Entry& entry) {
            if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id) {
                return true;
            }
            const ChunkPos pos = chunks_[entry.index].pos;
            const Column* own = column({.x = pos.x, .z = pos.z});
            if (own == nullptr || !own->known || !own->current) {
                return false;
            }
            backlog.push({.key = score(pos), .index = entry.index, .load_id = entry.load_id});
            return true;
        };
    };
    std::erase_if(parked_, back_in_line(plans_backlog_));
    std::erase_if(parked_replans_, back_in_line(replans_backlog_));
}

void WorldStreamer::feed_chunks(std::size_t& room) {
    // While regenerating, replans and rebuilds wait too. The lowest score
    // next; on a tie plans first, then replans, since builds wait for both.
    const std::array<Backlog*, 4> backlogs = {&plans_backlog_, &replans_backlog_, &builds_backlog_,
                                              &rebuilds_backlog_};
    while (room > 0) {
        std::size_t kind = backlogs.size();
        for (std::size_t at = 0; at < backlogs.size(); ++at) {
            if (!backlogs[at]->empty() &&
                (kind == backlogs.size() || backlogs[at]->top().key < backlogs[kind]->top().key)) {
                kind = at;
            }
        }
        if (kind == backlogs.size()) {
            return;
        }
        Backlog& backlog = *backlogs[kind];
        const Backlog::Entry entry = backlog.top();
        backlog.pop();
        const std::array<Start (WorldStreamer::*)(const Backlog::Entry&), 4> start = {
            &WorldStreamer::start_plan, &WorldStreamer::start_replan, &WorldStreamer::start_build,
            &WorldStreamer::start_rebuild};
        switch ((this->*start[kind])(entry)) {
        case Start::started:
            --room;
            break;
        case Start::dropped:
        case Start::parked:
            break;
        case Start::full:
            backlog.push(entry);
            return;
        }
    }
}

WorldStreamer::Start WorldStreamer::start_plan(const Backlog::Entry& entry) {
    if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id ||
        chunks_[entry.index].state != ChunkState::queued) {
        return Start::dropped;
    }
    // Its column's surface tells whether it is above every surface, so
    // uniform, or needs a plan.
    const ChunkPos pos = chunks_[entry.index].pos;
    const Column* own = column({.x = pos.x, .z = pos.z});
    if (own == nullptr || !own->known || !own->current) {
        parked_.push_back(entry);
        return Start::parked;
    }
    if (make_uniform(entry.index)) {
        return Start::dropped;
    }
    ChunkRecord& record = chunks_[entry.index];
    record.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                               PlanJob{.generator = generator_,
                                       .scratch = scratch_.data(),
                                       .results = &results_,
                                       .pos = record.pos,
                                       .load_id = record.load_id,
                                       .version = version_});
    if (!record.job) {
        return Start::full;
    }
    ++in_flight_;
    set_state(entry.index, ChunkState::planning);
    return Start::started;
}

WorldStreamer::Start WorldStreamer::start_build(const Backlog::Entry& entry) {
    if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id ||
        chunks_[entry.index].state != ChunkState::build_queued) {
        return Start::dropped;
    }
    ChunkRecord& record = chunks_[entry.index];
    JobRecord* pins = job_records_.acquire(record.load_id, 0);
    if (pins == nullptr) {
        return Start::full;
    }
    pin_plans(*pins, record.pos);
    record.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                               BuildJob{.generator = generator_,
                                        .scratch = scratch_.data(),
                                        .results = &results_,
                                        .record = pins,
                                        .pos = record.pos,
                                        .version = version_});
    if (!record.job) {
        job_records_.release(*pins);
        return Start::full;
    }
    ++in_flight_;
    record.job_record = pins;
    set_state(entry.index, ChunkState::building);
    return Start::started;
}

WorldStreamer::Start WorldStreamer::start_replan(const Backlog::Entry& entry) {
    if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id ||
        chunks_[entry.index].regeneration != Regeneration::replan || chunks_[entry.index].job) {
        return Start::dropped;
    }
    const ChunkPos pos = chunks_[entry.index].pos;
    const Column* own = column({.x = pos.x, .z = pos.z});
    if (own == nullptr || !own->known || !own->current) {
        parked_replans_.push_back(entry);
        return Start::parked;
    }
    if (const std::optional<BlockState> uniform = uniform_state(pos)) {
        regenerate_uniform(entry.index, *uniform);
        return Start::dropped;
    }
    ChunkRecord& record = chunks_[entry.index];
    record.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                               PlanJob{.generator = generator_,
                                       .scratch = scratch_.data(),
                                       .results = &results_,
                                       .pos = record.pos,
                                       .load_id = record.load_id,
                                       .version = version_});
    if (!record.job) {
        return Start::full;
    }
    ++in_flight_;
    return Start::started;
}

WorldStreamer::Start WorldStreamer::start_rebuild(const Backlog::Entry& entry) {
    if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id ||
        chunks_[entry.index].regeneration != Regeneration::rebuild || chunks_[entry.index].job) {
        return Start::dropped;
    }
    // A plan made since may have changed what it waits for: the next one
    // brings it back.
    ChunkRecord& record = chunks_[entry.index];
    if (!record.loaded || !neighborhood_current(record.pos)) {
        check_rebuild(entry.index);
        return Start::dropped;
    }
    JobRecord* pins = job_records_.acquire(record.load_id, 0);
    if (pins == nullptr) {
        return Start::full;
    }
    pin_plans(*pins, record.pos);
    record.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                               BuildJob{.generator = generator_,
                                        .scratch = scratch_.data(),
                                        .results = &results_,
                                        .record = pins,
                                        .pos = record.pos,
                                        .version = version_});
    if (!record.job) {
        job_records_.release(*pins);
        return Start::full;
    }
    ++in_flight_;
    record.job_record = pins;
    return Start::started;
}

void WorldStreamer::pin_plans(JobRecord& pins, ChunkPos pos) noexcept {
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const ChunkPos near{.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz};
                const std::optional<std::uint32_t> neighbor = chunks_.find(near);
                job_records_.read_plan(pins, around_index(dx, dy, dz),
                                       neighbor ? chunks_[*neighbor].plan : nullptr);
            }
        }
    }
}

bool WorldStreamer::integrate() {
    results_.drain([this](StreamResult&& result) { pending_.push_back(std::move(result)); });
    const std::chrono::steady_clock::time_point start = time();
    while (pending_at_ < pending_.size()) {
        StreamResult& result = pending_[pending_at_++];
        --in_flight_;
        switch (result.kind) {
        case StreamResult::Kind::surface:
            integrate_surface(result);
            break;
        case StreamResult::Kind::plan:
            integrate_plan(result);
            break;
        case StreamResult::Kind::build:
            integrate_build(result);
            break;
        }
        if (time() - start >= step_length) {
            break;
        }
    }
    longest_step_ = std::max(longest_step_,
                             std::chrono::duration_cast<std::chrono::nanoseconds>(time() - start));
    if (pending_at_ == pending_.size()) {
        pending_.clear();
        pending_at_ = 0;
    }
    return pending_at_ < pending_.size();
}

void WorldStreamer::integrate_surface(StreamResult& result) {
    const std::optional<std::uint32_t> index = column_map_.find(result.pos);
    if (!index || columns_[*index].load_id != result.load_id) {
        return; // the column left
    }
    Column& column = columns_[*index];
    column.job = {};
    if (result.version != version_) {
        surfaces_backlog_.push({.key = column_score(column.pos.x, column.pos.z, viewer_.chunk),
                                .index = *index,
                                .load_id = column.load_id});
        return;
    }
    column.surface = result.surface;
    column.known = true;
    column.current = true;
    surfaces_arrived_ = true;
    // It completes its own column's band, or a side's: the records that
    // brings are made now, within the step, as they are most of its cost.
    const ColumnPos pos = column.pos;
    refresh(pos);
    refresh({.x = pos.x + 1, .z = pos.z});
    refresh({.x = pos.x - 1, .z = pos.z});
    refresh({.x = pos.x, .z = pos.z + 1});
    refresh({.x = pos.x, .z = pos.z - 1});
}

void WorldStreamer::integrate_plan(StreamResult& result) {
    const std::optional<std::uint32_t> index = chunks_.find(result.pos);
    if (!index || chunks_[*index].load_id != result.load_id) {
        return;
    }
    ChunkRecord& record = chunks_[*index];
    record.job = {};
    if (record.state == ChunkState::unloading) {
        free_chunk(*index);
        return;
    }
    if (result.version != version_) {
        // Made for an older version: asked for again.
        if (record.state == ChunkState::planning) {
            set_state(*index, ChunkState::queued);
            plans_backlog_.push(
                {.key = score(record.pos), .index = *index, .load_id = record.load_id});
        } else if (record.regeneration == Regeneration::replan) {
            replans_backlog_.push(
                {.key = score(record.pos), .index = *index, .load_id = record.load_id});
        }
        return;
    }
    if (record.state == ChunkState::planning) {
        record.plan = &plans_.make(result.plan);
        set_state(*index, ChunkState::planned);
        advance(*index);
        plan_current(*index);
        return;
    }
    // A ready chunk's new plan, unless an edit stopped its regeneration
    // meanwhile: new blocks follow once its neighbors' plans are current,
    // if it is loaded.
    if (record.regeneration != Regeneration::replan) {
        return;
    }
    if (record.plan != nullptr) {
        plans_.retire(*record.plan);
    }
    record.plan = &plans_.make(result.plan);
    set_regeneration(*index, Regeneration::rebuild);
    check_rebuild(*index);
    plan_current(*index);
}

void WorldStreamer::integrate_build(StreamResult& result) {
    job_records_.release(*result.record);
    const std::optional<std::uint32_t> index = chunks_.find(result.pos);
    if (!index || chunks_[*index].load_id != result.load_id) {
        return;
    }
    ChunkRecord& record = chunks_[*index];
    record.job = {};
    record.job_record = nullptr;
    if (record.state == ChunkState::unloading) {
        free_chunk(*index);
        return;
    }
    if (result.version != version_) {
        // Built from an older version's plans: it starts over (a ready
        // chunk was set to replan when the version changed).
        if (record.state == ChunkState::building) {
            if (record.plan != nullptr) {
                plans_.retire(*record.plan);
                record.plan = nullptr;
            }
            set_regeneration(*index, Regeneration::none);
            set_state(*index, ChunkState::queued);
            plans_backlog_.push(
                {.key = score(record.pos), .index = *index, .load_id = record.load_id});
        } else if (record.regeneration == Regeneration::replan) {
            replans_backlog_.push(
                {.key = score(record.pos), .index = *index, .load_id = record.load_id});
        }
        return;
    }
    if (record.state != ChunkState::building) {
        // New blocks for a ready chunk, unless an edit stopped its
        // regeneration meanwhile.
        if (record.regeneration == Regeneration::rebuild) {
            if (record.blocks != nullptr) {
                blocks_.retire(*record.blocks);
                record.blocks = nullptr;
            }
            if (result.blocks.is_uniform()) {
                record.uniform = result.blocks.get(0);
            } else {
                record.blocks = &blocks_.make(std::move(result.blocks));
            }
            ++generated_;
            report_changed_around(*index);
        }
        set_regeneration(*index, Regeneration::none);
        return;
    }
    if (result.blocks.is_uniform()) {
        record.uniform = result.blocks.get(0);
    } else {
        record.blocks = &blocks_.make(std::move(result.blocks));
    }
    ++generated_;
    set_state(*index, ChunkState::ready);
    advance(*index);
}

void WorldStreamer::take_events(std::vector<ChunkEvent>& out) {
    out.clear();
    std::swap(out, events_);
}

bool WorldStreamer::set_block(BlockPos pos, BlockState state) {
    const ChunkPos chunk = chunk_of(pos);
    const std::optional<std::uint32_t> index = chunks_.find(chunk);
    if (!index || !counts_as_ready(chunks_[*index].state)) {
        return false;
    }
    ChunkRecord& record = chunks_[*index];
    const LocalPos local = local_of(pos);
    if (record.blocks == nullptr) {
        record.blocks = &blocks_.make(ChunkStorage::uniform(record.uniform));
    }
    blocks_.write(record.blocks).set(local_index(local.x, local.y, local.z), state);
    record.version = next_version_++;
    record.edited = true;
    // Edited, it keeps these blocks and the plan they came from: whatever
    // regeneration it waited for would undo the edit.
    if (record.regeneration != Regeneration::none) {
        set_regeneration(*index, Regeneration::none);
        plan_current(*index);
    }
    // It, and each neighbor whose padded border holds the block.
    const auto reach = [](std::int32_t at) {
        return std::pair{at == 0 ? -1 : 0, at == chunk_size - 1 ? 1 : 0};
    };
    const auto [low_x, high_x] = reach(local.x);
    const auto [low_y, high_y] = reach(local.y);
    const auto [low_z, high_z] = reach(local.z);
    for (std::int32_t dy = low_y; dy <= high_y; ++dy) {
        for (std::int32_t dz = low_z; dz <= high_z; ++dz) {
            for (std::int32_t dx = low_x; dx <= high_x; ++dx) {
                const ChunkPos near{.x = chunk.x + dx, .y = chunk.y + dy, .z = chunk.z + dz};
                const std::optional<std::uint32_t> neighbor = chunks_.find(near);
                if (neighbor && counts_as_ready(chunks_[*neighbor].state)) {
                    report(ChunkEvent::Kind::changed, *neighbor);
                }
            }
        }
    }
    return true;
}

JobRecord* WorldStreamer::pin_neighborhood(ChunkPos pos, std::uint32_t sequence) noexcept {
    const std::optional<std::uint32_t> own = chunks_.find(pos);
    if (!own || chunks_[*own].state != ChunkState::complete) {
        return nullptr;
    }
    JobRecord* pins = job_records_.acquire(chunks_[*own].load_id, sequence);
    if (pins == nullptr) {
        return nullptr;
    }
    // Complete: all 26 neighbors are ready, each with blocks or a state.
    around(pos, [&](ChunkPos near) {
        const std::optional<std::uint32_t> neighbor = chunks_.find(near);
        const ChunkRecord* record = neighbor ? &chunks_[*neighbor] : nullptr;
        job_records_.read_chunk(*pins, around_index(near.x - pos.x, near.y - pos.y, near.z - pos.z),
                                record != nullptr ? record->blocks : nullptr,
                                record != nullptr ? record->uniform : BlockState::air);
    });
    return pins;
}

const ChunkRecord* WorldStreamer::find(ChunkPos pos) const noexcept {
    const std::optional<std::uint32_t> index = chunks_.find(pos);
    return index ? &chunks_[*index] : nullptr;
}

std::optional<BlockState> WorldStreamer::block(BlockPos pos) const noexcept {
    const ChunkRecord* record = find(chunk_of(pos));
    if (record == nullptr || !counts_as_ready(record->state)) {
        return std::nullopt;
    }
    if (record->blocks == nullptr) {
        return record->uniform;
    }
    const LocalPos local = local_of(pos);
    return record->blocks->value().get(local_index(local.x, local.y, local.z));
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
    stats.waiting = surfaces_backlog_.size() + plans_backlog_.size() + builds_backlog_.size() +
                    replans_backlog_.size() + rebuilds_backlog_.size() + parked_.size() +
                    parked_replans_.size();
    stats.regenerating = regenerating_;
    stats.block_versions = blocks_.live();
    stats.plan_versions = plans_.live();
    stats.generated = generated_;
    stats.uniform = classified_uniform_;
    return stats;
}

std::size_t WorldStreamer::chunk_bytes() const noexcept {
    std::size_t bytes = chunks_.memory_bytes() + column_map_.memory_bytes() +
                        (columns_.capacity() * sizeof(Column));
    // A free record holds no blocks.
    for (std::uint32_t index = 0; index < chunks_.end_index(); ++index) {
        if (const Version<ChunkStorage>* blocks = chunks_[index].blocks) {
            bytes += sizeof(ChunkStorage) + blocks->value().memory_bytes();
        }
    }
    return bytes;
}

std::chrono::nanoseconds WorldStreamer::take_longest_step() noexcept {
    return std::exchange(longest_step_, std::chrono::nanoseconds{0});
}

} // namespace vx
