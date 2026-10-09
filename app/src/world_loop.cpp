#include "world_loop.hpp"

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/versions.hpp>
#include <vx/world/world_streamer.hpp>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace vx::app {
namespace {

// Tracy's frames for the world thread's ticks: a set of its own, between
// FrameMarkStart and FrameMarkEnd, since ticks are not frames.
[[maybe_unused]] constexpr const char* tick_frame = "Tick";

constexpr std::size_t around_index(int dx, int dy, int dz) noexcept {
    const int at = (dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1));
    return static_cast<std::size_t>(at);
}

} // namespace

void WorldLoop::MeshJob::operator()(JobContext& job) const noexcept {
    job.describe(pos.x, pos.y, pos.z);
    MeshWorkerScratch& scratch = context->scratch[job.worker()];
    // A uniform chunk has no blocks to pin: it is made here, from its state,
    // with no allocation.
    std::array<ChunkStorage, 27> uniform;
    const auto chunk_at = [&](std::size_t at) -> const ChunkStorage& {
        if (const Version<ChunkStorage>* blocks = pins->chunks[at]) {
            return blocks->value();
        }
        uniform[at] = ChunkStorage::uniform(pins->uniform[at]);
        return uniform[at];
    };
    ChunkNeighborhood neighborhood(chunk_at(around_index(0, 0, 0)));
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx != 0 || dy != 0 || dz != 0) {
                    neighborhood.set(dx, dy, dz, &chunk_at(around_index(dx, dy, dz)));
                }
            }
        }
    }
    assemble_padded(neighborhood, scratch.padded);
    const ChunkMesh mesh = mesh_chunk(scratch.padded, context->visuals, scratch.mesh);
    MeshResult result;
    result.pins = pins;
    result.update.pos = pos;
    result.update.load_id = pins->load_id;
    result.update.generation = generation;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face = 0; face < 6; ++face) {
            result.update.counts[(pass * 6) + face] =
                static_cast<std::uint32_t>(mesh.ranges[pass][face].size());
        }
    }
    const std::span<const Quad> quads = mesh.all();
    if (!quads.empty()) {
        result.update.quads = context->pool->allocate(quads.size_bytes());
        std::memcpy(result.update.quads.data(), quads.data(), quads.size_bytes());
    }
    context->results->push(std::move(result));
}

WorldLoop::WorldLoop(const Generator& generator, std::span<const BlockVisual> visuals,
                     JobSystem& jobs, MeshResultPool& pool, const WorldLoopSettings& settings)
    : streamer_(generator, jobs, wake_, settings.region), jobs_(&jobs), settings_(settings),
      from_main_(64, wake_), mesh_results_(std::size_t{2} * jobs.slot_count(), wake_),
      scratch_(std::max<std::uint32_t>(1, jobs.worker_count())),
      context_{.visuals = visuals, .pool = &pool, .results = &mesh_results_, .scratch = nullptr},
      to_main_(1'024, main_wake_) {
    context_.scratch = scratch_.data();
}

WorldLoop::~WorldLoop() {
    // RFC 0005's shutdown: what is queued never runs, what runs reports
    // back, and only then do the chunks go.
    for (const MeshRecord& record : meshes_) {
        if (record.job && jobs_->cancel(record.job) && record.pins != nullptr) {
            streamer_.release(*record.pins);
        }
    }
    streamer_.cancel_queued();
    jobs_->wait_until_idle();
    mesh_results_.drain([this](MeshResult&& result) { streamer_.release(*result.pins); });
}

bool WorldLoop::pass() {
    // RFC 0005's plots, as each pass finds the loop: what waits in each inbox
    // before the drains. A tick, right after a pass, would find them empty.
    TracyPlot("Inbox: from main", static_cast<std::int64_t>(from_main_.size()));
    TracyPlot("Inbox: meshes", static_cast<std::int64_t>(mesh_results_.size()));
    TracyPlot("Inbox: chunks", static_cast<std::int64_t>(streamer_.results_waiting()));
    TracyPlot("Pins", static_cast<std::int64_t>(streamer_.pins()));
    TracyPlot("Copies on write", static_cast<std::int64_t>(streamer_.copies()));
    TracyPlot("Meshes not uploaded", static_cast<std::int64_t>(unuploaded()));
    from_main_.drain([this](ToWorld&& message) { handle(message); });
    if (stopping_) {
        return false;
    }
    streamer_.integrate();
    follow_events();
    mesh_results_.drain([this](MeshResult&& result) { integrate_mesh(result); });
    send_ready_batches();
    feed();
    return true;
}

void WorldLoop::settle() {
    // Once a stop arrived, results are no longer taken in, so nothing would
    // ever settle: the destructor waits for the jobs instead.
    while (pass()) {
        if (streamer_.stats().jobs_in_flight == 0 && meshes_in_flight_ == 0) {
            return;
        }
        jobs_->wait_until_idle();
    }
}

void WorldLoop::take(std::vector<ToMain>& out) {
    out.clear();
    to_main_.drain([&out](ToMain&& message) { out.push_back(std::move(message)); });
}

std::chrono::steady_clock::time_point WorldLoop::step(std::chrono::steady_clock::time_point now) {
    if (!pass()) {
        return now; // stopping: no tick runs after a stop
    }
    if (!ticking_) {
        ticking_ = true;
        next_tick_ = now + tick_length;
    }
    // More than a second behind: the ticks missed are skipped, not run back
    // to back (RFC 0005).
    if (now - next_tick_ > std::chrono::seconds(1)) {
        skipped_ += static_cast<std::uint64_t>((now - next_tick_) / tick_length);
        next_tick_ = now;
    }
    while (now >= next_tick_) {
        tick();
        next_tick_ += tick_length;
    }
    return next_tick_;
}

void WorldLoop::tick() {
    FrameMarkStart(tick_frame);
    {
        ZoneScopedN("Tick");
        run_tick();
    }
    FrameMarkEnd(tick_frame);
}

void WorldLoop::run_tick() {
    ++ticks_;
    const bool catching_up = missing_near_camera();
    if (catching_up != catching_up_) {
        catching_up_ = catching_up;
        jobs_->set_catching_up(catching_up);
    }
    // Background jobs wait a second before they move up: a tick's sample of
    // their level is enough, and spares each pass a lock (RFC 0005's plots).
    TracyPlot("Queued background", static_cast<std::int64_t>(jobs_->queued(JobLevel::background)));
}

bool WorldLoop::missing_near_camera() {
    if (!camera_) {
        return false;
    }
    const ChunkPos camera = *camera_;
    const RegionSettings& region = settings_.region;
    const std::int32_t near = region.near_distance;
    for (std::int32_t dz = -near; dz <= near; ++dz) {
        for (std::int32_t dx = -near; dx <= near; ++dx) {
            if ((dx * dx) + (dz * dz) > near * near) {
                continue;
            }
            for (std::int32_t dy = -region.near_height; dy <= region.near_height; ++dy) {
                const ChunkPos pos{.x = camera.x + dx, .y = camera.y + dy, .z = camera.z + dz};
                const std::optional<std::uint32_t> index = streamer_.records().find(pos);
                if (!index || mesh_record(*index).forwarded == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

void WorldLoop::handle(ToWorld& message) {
    switch (message.kind) {
    case ToWorld::Kind::camera:
        camera_ = message.camera;
        streamer_.set_camera(message.camera);
        follow_events();
        break;
    case ToWorld::Kind::edit:
        edit(message.block, message.state);
        break;
    case ToWorld::Kind::uploaded:
        // Never past what was sent, as when batches are counted too: the
        // difference would wrap and hold streaming back for good.
        uploaded_ = std::min(std::max(uploaded_, message.uploaded), sent_);
        break;
    case ToWorld::Kind::stop:
        stopping_ = true;
        break;
    }
}

void WorldLoop::edit(BlockPos block, BlockState state) {
    follow_events();
    if (!streamer_.set_block(block, state)) {
        return;
    }
    streamer_.take_events(events_);
    // The chunks the edit changed, and those of any batch one of them is in
    // already: one batch, sent once all are remeshed.
    Batch batch{.id = next_batch_++, .members = {}};
    const auto join = [&](std::uint32_t index) {
        const bool known = std::ranges::any_of(
            batch.members, [index](const Batch::Member& member) { return member.record == index; });
        if (known || mesh_record(index).state == MeshRecord::State::none) {
            return;
        }
        // Any job started from now on reads the edit.
        batch.members.push_back({.record = index,
                                 .load_id = mesh_record(index).load_id,
                                 .wanted = next_generation_,
                                 .mesh = {},
                                 .done = false});
    };
    for (const ChunkEvent& event : events_) {
        if (event.kind != ChunkEvent::Kind::changed) {
            continue;
        }
        const std::uint32_t old_batch = mesh_record(event.record).batch;
        join(event.record);
        if (old_batch != 0) {
            const auto merged = std::ranges::find(batches_, old_batch, &Batch::id);
            if (merged != batches_.end()) {
                for (const Batch::Member& member : merged->members) {
                    join(member.record);
                }
                batches_.erase(merged);
            }
        }
    }
    events_.clear();
    if (batch.members.empty()) {
        return;
    }
    for (const Batch::Member& member : batch.members) {
        mesh_record(member.record).batch = batch.id;
        queue_mesh(member.record, true);
    }
    batches_.push_back(std::move(batch));
}

void WorldLoop::follow_events() {
    streamer_.take_events(events_);
    for (const ChunkEvent& event : events_) {
        switch (event.kind) {
        case ChunkEvent::Kind::shown:
            mesh_record(event.record).load_id = streamer_.records()[event.record].load_id;
            queue_mesh(event.record, false);
            break;
        case ChunkEvent::Kind::changed:
            if (mesh_record(event.record).state != MeshRecord::State::none) {
                queue_mesh(event.record, false);
            }
            break;
        case ChunkEvent::Kind::hidden:
        case ChunkEvent::Kind::freed:
            drop_mesh(event.record, event.pos);
            break;
        }
    }
    events_.clear();
}

WorldLoop::MeshRecord& WorldLoop::mesh_record(std::uint32_t index) {
    if (index >= meshes_.size()) {
        meshes_.resize(std::size_t{index} + 1);
    }
    return meshes_[index];
}

void WorldLoop::queue_mesh(std::uint32_t index, bool urgent) {
    MeshRecord& record = mesh_record(index);
    switch (record.state) {
    case MeshRecord::State::none:
    case MeshRecord::State::sent:
        record.state = MeshRecord::State::queued;
        record.urgent = urgent;
        (urgent ? waiting_urgent_ : waiting_meshes_).push_back(index);
        break;
    case MeshRecord::State::queued:
        if (urgent && !record.urgent) {
            record.urgent = true;
            waiting_urgent_.push_back(index);
        }
        break;
    case MeshRecord::State::meshing:
        // Meshed again when this job reports: it read the blocks before the
        // change.
        record.dirty = true;
        record.urgent = record.urgent || urgent;
        break;
    }
}

void WorldLoop::drop_mesh(std::uint32_t index, ChunkPos pos) {
    MeshRecord& record = mesh_record(index);
    if (record.state == MeshRecord::State::none) {
        return;
    }
    if (record.job && jobs_->cancel(record.job)) {
        streamer_.release(*record.pins);
        --meshes_in_flight_;
    }
    if (record.forwarded != 0) {
        to_main_.push({.kind = ToMain::Kind::unload, .pos = pos, .mesh = {}, .batch = {}});
    }
    // It leaves its batch, which goes out without it.
    if (record.batch != 0) {
        const auto batch = std::ranges::find(batches_, record.batch, &Batch::id);
        if (batch != batches_.end()) {
            std::erase_if(batch->members,
                          [index](const Batch::Member& member) { return member.record == index; });
        }
    }
    record = MeshRecord{};
}

void WorldLoop::integrate_mesh(MeshResult& result) {
    streamer_.release(*result.pins);
    --meshes_in_flight_;
    const std::optional<std::uint32_t> found = streamer_.records().find(result.update.pos);
    if (!found) {
        return; // freed: the buffer goes back to the pool
    }
    const std::uint32_t index = *found;
    MeshRecord& record = mesh_record(index);
    if (record.state == MeshRecord::State::none || record.load_id != result.update.load_id) {
        return;
    }
    if (record.state == MeshRecord::State::meshing &&
        record.generation == result.update.generation) {
        record.job = {};
        record.pins = nullptr;
        if (record.dirty) {
            record.dirty = false;
            record.state = MeshRecord::State::sent; // so queue_mesh queues it
            queue_mesh(index, record.urgent);
        } else {
            record.state = MeshRecord::State::sent;
            record.urgent = false;
        }
    }
    // Newer than the last one sent: on its way, even if a newer job runs, so
    // steady edits cannot starve the chunk (RFC 0006).
    if (result.update.generation > record.forwarded) {
        forward(index, std::move(result.update));
    }
}

void WorldLoop::forward(std::uint32_t index, MeshUpdate&& update) {
    MeshRecord& record = mesh_record(index);
    record.forwarded = update.generation;
    if (record.batch != 0) {
        const auto batch = std::ranges::find(batches_, record.batch, &Batch::id);
        if (batch != batches_.end()) {
            for (Batch::Member& member : batch->members) {
                if (member.record == index) {
                    member.done = member.done || update.generation >= member.wanted;
                    member.mesh = std::move(update);
                    return;
                }
            }
        }
    }
    ++sent_;
    to_main_.push({.kind = ToMain::Kind::mesh, .pos = {}, .mesh = std::move(update), .batch = {}});
}

void WorldLoop::send_ready_batches() {
    std::erase_if(batches_, [this](Batch& batch) {
        const bool ready = std::ranges::all_of(
            batch.members, [](const Batch::Member& member) { return member.done; });
        if (!ready) {
            return false;
        }
        ToMain message{.kind = ToMain::Kind::batch, .pos = {}, .mesh = {}, .batch = {}};
        for (Batch::Member& member : batch.members) {
            MeshRecord& record = mesh_record(member.record);
            record.batch = 0;
            // Still the chunk the edit changed.
            if (record.load_id == member.load_id && member.mesh.load_id == member.load_id) {
                message.batch.push_back(std::move(member.mesh));
            }
        }
        if (!message.batch.empty()) {
            to_main_.push(std::move(message));
        }
        return true;
    });
}

void WorldLoop::feed() {
    // An edit's remeshes first, at the urgent level; then streaming meshes,
    // unless too many sent wait to be uploaded; then the streamer's jobs.
    // Each level's count is plotted where it is read (RFC 0005): how far the
    // workers ran it down since the last pass, without another lock.
    const auto room = [this](JobLevel level, [[maybe_unused]] const char* plot) {
        const std::uint32_t queued = jobs_->queued(level);
        TracyPlot(plot, static_cast<std::int64_t>(queued));
        return queued >= settings_.queue_limit ? 0U : settings_.queue_limit - queued;
    };
    std::size_t urgent_room = room(JobLevel::urgent, "Queued urgent");
    std::erase_if(waiting_urgent_, [&](std::uint32_t index) {
        const MeshRecord& record = mesh_record(index);
        if (record.state != MeshRecord::State::queued || !record.urgent) {
            return true; // started, or dropped
        }
        if (urgent_room == 0 || !start_mesh(index)) {
            return false;
        }
        --urgent_room;
        return true;
    });
    std::size_t normal_room = room(JobLevel::normal, "Queued normal");
    const bool held = unuploaded() >= settings_.unuploaded_limit;
    std::erase_if(waiting_meshes_, [&](std::uint32_t index) {
        const MeshRecord& record = mesh_record(index);
        if (record.state != MeshRecord::State::queued || record.urgent) {
            return true;
        }
        if (held || normal_room == 0 || !start_mesh(index)) {
            return false;
        }
        --normal_room;
        return true;
    });
    streamer_.feed(normal_room);
}

bool WorldLoop::start_mesh(std::uint32_t index) {
    MeshRecord& record = mesh_record(index);
    const ChunkPos pos = streamer_.records()[index].pos;
    const std::uint32_t generation = next_generation_;
    JobRecord* pins = streamer_.pin_neighborhood(pos, generation);
    if (pins == nullptr) {
        return false;
    }
    const JobPriority priority{.level = record.urgent ? JobLevel::urgent : JobLevel::normal,
                               .key = 0};
    record.job = jobs_->submit(
        priority,
        MeshJob{.context = &context_, .pins = pins, .pos = pos, .generation = generation});
    if (!record.job) {
        streamer_.release(*pins);
        return false;
    }
    ++next_generation_;
    record.pins = pins;
    record.generation = generation;
    record.state = MeshRecord::State::meshing;
    ++meshes_in_flight_;
    return true;
}

} // namespace vx::app
