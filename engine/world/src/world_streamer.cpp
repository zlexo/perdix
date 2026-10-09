#include <vx/world/world_streamer.hpp>

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/wake_event.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/region.hpp>

#include <algorithm>
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

void WorldStreamer::feed() {
    feed_surfaces();
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

void WorldStreamer::integrate() {
    results_.drain([this](StreamResult&& result) {
        --in_flight_;
        switch (result.kind) {
        case StreamResult::Kind::surface:
            integrate_surface(result);
            break;
        }
    });
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
    for (std::uint32_t index = 0; index < columns_.size(); ++index) {
        if (column_map_.find(column_key(columns_[index].pos)) == index) {
            ++stats.columns;
            if (columns_[index].known) {
                ++stats.surfaces_known;
            }
        }
    }
    stats.jobs_in_flight = in_flight_;
    return stats;
}

} // namespace vx
