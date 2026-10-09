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

    void operator()(JobContext& context) const noexcept {
        context.describe(column.x, column.z);
        StreamResult result;
        result.kind = StreamResult::Kind::surface;
        result.pos = column_key(column);
        result.load_id = load_id;
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

    void operator()(JobContext& context) const noexcept {
        context.describe(pos.x, pos.y, pos.z);
        StreamResult result;
        result.kind = StreamResult::Kind::plan;
        result.pos = pos;
        result.load_id = load_id;
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
        result.record = record;
        result.blocks = generator->build(plans, scratch[context.worker()]);
        results->push(std::move(result));
    }
};

// Whether the mesh half has been told to mesh it: complete and drawn.
bool shown(const ChunkRecord& record) noexcept {
    return record.state == ChunkState::complete && record.drawn;
}

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
      results_(std::size_t{2} * jobs.slot_count(), results_ready) {}

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
        if (record.loaded && record.planned_neighbors == 26) {
            next = ChunkState::build_queued;
            builds_backlog_.push(
                {.key = score(record.pos), .index = index, .load_id = record.load_id});
        }
        break;
    case ChunkState::build_queued:
        if (!record.loaded || record.planned_neighbors < 26) {
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
}

void WorldStreamer::feed(std::size_t room) {
    feed_surfaces(room);
    unpark();
    feed_chunks(room);
}

void WorldStreamer::feed_surfaces(std::size_t& room) {
    while (room > 0 && !surfaces_backlog_.empty()) {
        const Backlog::Entry entry = surfaces_backlog_.top();
        Column& column = columns_[entry.index];
        if (column.load_id != entry.load_id || column.known || column.job) {
            surfaces_backlog_.pop();
            continue;
        }
        column.job = jobs_->submit({.level = JobLevel::normal, .key = entry.key},
                                   SurfaceJob{.generator = generator_,
                                              .scratch = scratch_.data(),
                                              .results = &results_,
                                              .column = column.pos,
                                              .load_id = column.load_id});
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
    std::erase_if(parked_, [this](const Backlog::Entry& entry) {
        if (!chunks_.live(entry.index) || chunks_[entry.index].load_id != entry.load_id) {
            return true;
        }
        const ChunkPos pos = chunks_[entry.index].pos;
        const Column* own = column({.x = pos.x, .z = pos.z});
        if (own == nullptr || !own->known) {
            return false;
        }
        plans_backlog_.push({.key = score(pos), .index = entry.index, .load_id = entry.load_id});
        return true;
    });
}

void WorldStreamer::feed_chunks(std::size_t& room) {
    while (room > 0) {
        const bool plans = !plans_backlog_.empty();
        const bool builds = !builds_backlog_.empty();
        if (!plans && !builds) {
            return;
        }
        // The lower score next; a plan first on a tie, since builds wait for
        // plans.
        const bool plan =
            plans && (!builds || plans_backlog_.top().key <= builds_backlog_.top().key);
        Backlog& backlog = plan ? plans_backlog_ : builds_backlog_;
        const Backlog::Entry entry = backlog.top();
        backlog.pop();
        switch (plan ? start_plan(entry) : start_build(entry)) {
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
    if (own == nullptr || !own->known) {
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
                                       .load_id = record.load_id});
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
                                        .pos = record.pos});
    if (!record.job) {
        job_records_.release(*pins);
        return Start::full;
    }
    ++in_flight_;
    record.job_record = pins;
    set_state(entry.index, ChunkState::building);
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
    if (pending_at_ == pending_.size()) {
        pending_.clear();
        pending_at_ = 0;
    }
    // A new surface completes its own column's band, or a side's.
    for (const ColumnPos pos : dirty_) {
        refresh(pos);
        refresh({.x = pos.x + 1, .z = pos.z});
        refresh({.x = pos.x - 1, .z = pos.z});
        refresh({.x = pos.x, .z = pos.z + 1});
        refresh({.x = pos.x, .z = pos.z - 1});
    }
    dirty_.clear();
    return pending_at_ < pending_.size();
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
    surfaces_arrived_ = true;
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
    if (result.blocks.is_uniform()) {
        record.uniform = result.blocks.get(0);
    } else {
        record.blocks = &blocks_.make(std::move(result.blocks));
    }
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
    stats.block_versions = blocks_.live();
    stats.plan_versions = plans_.live();
    return stats;
}

} // namespace vx
