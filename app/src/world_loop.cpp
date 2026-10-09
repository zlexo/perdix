#include "world_loop.hpp"

#include <vx/core/inbox.hpp>
#include <vx/core/job_system.hpp>
#include <vx/core/mesh_result_pool.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/padded_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/movement.hpp>
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

ChunkSight chunk_sight(const Viewer& viewer, ChunkPos pos, double near) noexcept {
    // The chunk's box relative to the eye.
    const BlockPos low = block_at(pos, {});
    const Vec3d min{.x = static_cast<double>(low.x) - viewer.eye.x,
                    .y = static_cast<double>(low.y) - viewer.eye.y,
                    .z = static_cast<double>(low.z) - viewer.eye.z};
    constexpr Vec3d side{.x = chunk_size, .y = chunk_size, .z = chunk_size};
    const Vec3d max = min + side;
    if (!viewer.view.may_contain(min, max)) {
        return {};
    }
    // The box's point nearest the eye, at 0.
    const Vec3d to{.x = std::clamp(0.0, min.x, max.x),
                   .y = std::clamp(0.0, min.y, max.y),
                   .z = std::clamp(0.0, min.z, max.z)};
    return {.in_view = true, .near = (to.x * to.x) + (to.y * to.y) + (to.z * to.z) <= near * near};
}

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
    const ChunkMesh mesh = mesh_chunk(scratch.padded, context->visuals, scratch.mesh, options);
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
      to_main_(1'024, main_wake_), boxes_(streamer_, settings.physics) {
    context_.scratch = scratch_.data();
    if (settings.clock != nullptr) {
        streamer_.set_clock(*settings.clock);
    }
    if (settings.player) {
        walking_.emplace(settings.player->spawn, settings.player->lowest);
    }
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
    const std::chrono::steady_clock::time_point start = time();
    answering_ = std::chrono::nanoseconds{0};
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
    behind_ = streamer_.integrate();
    follow_events();
    mesh_results_.drain([this](MeshResult&& result) { integrate_mesh(result); });
    send_ready_batches();
    feed();
    // RFC 0006's flythrough reports the longest pass with the longest step:
    // a step is budgeted, and the rest of a pass is not.
    longest_pass_ = std::max(longest_pass_, time() - start - answering_);
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
        tick(next_tick_);
        next_tick_ += tick_length;
    }
    // Results left from a step: the next pass comes now, not at the tick.
    return behind_ ? now : next_tick_;
}

void WorldLoop::tick(std::chrono::steady_clock::time_point due) {
    FrameMarkStart(tick_frame);
    {
        ZoneScopedN("Tick");
        run_tick(due);
    }
    FrameMarkEnd(tick_frame);
}

void WorldLoop::run_tick(std::chrono::steady_clock::time_point due) {
    ++ticks_;
    last_tick_ = due;
    // The player moves with the keys held now, against the streamed chunks,
    // unless its move could touch one not ready yet: then it waits where it
    // is, keeping its speed, until that chunk is (RFC 0010's open question 7).
    if (walking_) {
        if (boxes_.ready(tick_reach(walking_->player()))) {
            walking_->tick(input_, boxes_);
        } else {
            walking_->hold();
        }
        send_player(due);
    }
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
        camera_ = message.camera.chunk;
        viewer_ = message.camera;
        if (streamer_.set_viewer(message.camera)) {
            rescore_meshes();
        }
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
    case ToWorld::Kind::input:
        input_ = message.input;
        break;
    case ToWorld::Kind::land:
        land(message.feet);
        break;
    case ToWorld::Kind::meshing:
        remesh(message.meshing);
        break;
    case ToWorld::Kind::view_distance:
        // The checks recompute the region from the loop's copy of the
        // settings: it follows too.
        settings_.region.view_distance = message.view_distance;
        streamer_.set_view_distance(message.view_distance);
        follow_events();
        break;
    case ToWorld::Kind::sample: {
        // What the loop would do later in this pass, done first: the halves
        // agree once the streamer's events are followed.
        follow_events();
        const std::chrono::steady_clock::time_point start = time();
        ToMain answer;
        answer.kind = ToMain::Kind::sample;
        answer.sample = sample(message.sample_kind);
        answer.sample.id = message.sample_id;
        to_main_.push(std::move(answer));
        answering_ += time() - start;
        break;
    }
    case ToWorld::Kind::stop:
        stopping_ = true;
        break;
    }
}

void WorldLoop::remesh(MeshOptions options) {
    if (options.merge == meshing_.merge) {
        return;
    }
    meshing_ = options;
    // An empty mesh is empty with any options.
    for (std::uint32_t index = 0; index < meshes_.size(); ++index) {
        if (meshes_[index].state != MeshRecord::State::none && !known_empty(index)) {
            queue_mesh(index, false);
        }
    }
}

void WorldLoop::land(Vec3d feet) {
    // Where a standing player fits, inside no block and no chunk still
    // loading: the noclip camera may not land the player in a wall.
    bool landed = false;
    if (walking_) {
        const Box body = body_box(feet, Pose::standing);
        std::vector<Box> inside;
        boxes_.collect(body, inside);
        if (!overlaps_any(body, inside)) {
            walking_->place(feet);
            landed = true;
            // At once, so the camera never shows where the player was.
            send_player(last_tick_);
        }
    }
    ToMain message;
    message.kind = ToMain::Kind::landed;
    message.landed = landed;
    to_main_.push(std::move(message));
}

void WorldLoop::send_player(std::chrono::steady_clock::time_point at) {
    assert(walking_.has_value());
    ToMain message;
    message.kind = ToMain::Kind::player;
    message.player = walking_->frame();
    message.tick_at = at;
    to_main_.push(std::move(message));
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
        if (urgent) {
            waiting_urgent_.push_back(index);
        } else {
            waiting_meshes_.push({.key = streamer_.score(streamer_.records()[index].pos),
                                  .index = index,
                                  .load_id = record.load_id});
        }
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
    ++meshes_made_;
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
        // RFC 0006's fast path: a mesh known to be empty takes no job, and
        // no room in the queue. It goes at once, as a job's result would.
        if (known_empty(index)) {
            skip_mesh(index);
            return true;
        }
        if (urgent_room == 0 || !start_mesh(index, 0)) {
            return false;
        }
        --urgent_room;
        return true;
    });
    // A mesh known to be empty goes even while the jobs wait for room or
    // for the main thread: it takes neither.
    std::size_t normal_room = room(JobLevel::normal, "Queued normal");
    const bool held = unuploaded() >= settings_.unuploaded_limit;
    while (!waiting_meshes_.empty()) {
        const Backlog::Entry entry = waiting_meshes_.top();
        const MeshRecord& record = mesh_record(entry.index);
        if (record.state != MeshRecord::State::queued || record.urgent ||
            record.load_id != entry.load_id) {
            waiting_meshes_.pop(); // started, dropped, or an edit's now
            continue;
        }
        if (known_empty(entry.index)) {
            skip_mesh(entry.index);
            waiting_meshes_.pop();
            continue;
        }
        if (held || normal_room == 0 || !start_mesh(entry.index, entry.key)) {
            break; // no room, or no job slot or record: it stays first in line
        }
        waiting_meshes_.pop();
        --normal_room;
    }
    streamer_.feed(normal_room);
}

void WorldLoop::rescore_meshes() {
    waiting_meshes_.rescore([this](const Backlog::Entry& entry) -> std::optional<std::uint32_t> {
        const MeshRecord& record = mesh_record(entry.index);
        if (record.state != MeshRecord::State::queued || record.load_id != entry.load_id) {
            return std::nullopt;
        }
        return streamer_.score(streamer_.records()[entry.index].pos);
    });
}

WorldSample WorldLoop::sample(SampleKind kind) {
    const StreamingStats stats = streamer_.stats();
    const bool idle = stats.jobs_in_flight == 0 && stats.waiting == 0 && meshes_in_flight_ == 0;
    WorldSample out{.id = 0,
                    .kind = kind,
                    .at = time(),
                    .generated = stats.generated,
                    .uniform = stats.uniform,
                    .meshed = meshes_made_,
                    .skipped = meshes_skipped_};
    const ChunkRecords& records = streamer_.records();
    if (kind == SampleKind::completion) {
        bool complete = idle;
        for (std::uint32_t index = 0; complete && index < records.end_index(); ++index) {
            complete = !missing(index);
        }
        if (!complete) {
            return out;
        }
        out.kind = SampleKind::full;
    }
    out.longest_step = streamer_.take_longest_step();
    out.longest_pass = std::exchange(longest_pass_, std::chrono::nanoseconds{0});
    out.chunk_bytes = streamer_.chunk_bytes();
    if (out.kind == SampleKind::checked) {
        out.broken = broken_invariants(stats);
    }
    bool any_missing = false;
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!missing(index)) {
            continue;
        }
        any_missing = true;
        if (!viewer_) {
            continue;
        }
        const ChunkSight sight = chunk_sight(*viewer_, records[index].pos, near_holes_distance);
        if (sight.in_view) {
            ++out.holes;
        }
        if (sight.near) {
            ++out.near_holes;
        }
    }
    out.complete = idle && !any_missing;
    return out;
}

bool WorldLoop::missing(std::uint32_t index) {
    const ChunkRecords& records = streamer_.records();
    if (!records.live(index)) {
        return false;
    }
    const ChunkRecord& chunk = records[index];
    return chunk.drawn && mesh_record(index).forwarded == 0 &&
           !(counts_as_ready(chunk.state) && known_empty(index));
}

std::uint32_t WorldLoop::broken_invariants(const StreamingStats& stats) {
    const ChunkRecords& records = streamer_.records();
    // Each record's counts and state, against its neighbors.
    std::uint32_t broken = records.broken();
    // Each record counted once, in its state.
    std::size_t counted = 0;
    for (const std::size_t count : stats.chunks) {
        counted += count;
    }
    if (counted != records.size()) {
        ++broken;
    }
    // The mesh half holds a chunk exactly while the world half shows it:
    // complete, so with its 26 neighbors ready, and in the drawn region.
    broken += records.disagreements([this](std::uint32_t index) {
        return mesh_record(index).state != MeshRecord::State::none;
    });
    // The region grown by the hysteresis, recomputed from the camera rather
    // than taken from the streamer: no record stays outside it.
    if (camera_) {
        broken +=
            records.outside(Region(*camera_, settings_.region).grown(settings_.region.hysteresis));
    }
    return broken;
}

bool WorldLoop::known_empty(std::uint32_t index) const noexcept {
    const ChunkRecord& chunk = streamer_.records()[index];
    if (chunk.blocks != nullptr) {
        return false;
    }
    if (chunk.uniform == BlockState::air) {
        return true; // air draws no faces
    }
    const auto opaque = [this](BlockState state) {
        const auto at = static_cast<std::size_t>(state);
        return at < context_.visuals.size() && context_.visuals[at].opacity == Opacity::opaque;
    };
    if (!opaque(chunk.uniform)) {
        return false;
    }
    // Every face of an opaque block meets an opaque block, so none shows.
    constexpr std::array<std::array<std::int32_t, 3>, 6> sides = {
        {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    return std::ranges::all_of(sides, [&](const std::array<std::int32_t, 3>& side) {
        const ChunkRecord* neighbor = streamer_.find(
            {.x = chunk.pos.x + side[0], .y = chunk.pos.y + side[1], .z = chunk.pos.z + side[2]});
        return neighbor != nullptr && neighbor->blocks == nullptr && opaque(neighbor->uniform);
    });
}

void WorldLoop::skip_mesh(std::uint32_t index) {
    MeshRecord& record = mesh_record(index);
    const ChunkPos pos = streamer_.records()[index].pos;
    record.generation = next_generation_++;
    record.state = MeshRecord::State::sent;
    record.urgent = false;
    ++meshes_skipped_;
    forward(index, MeshUpdate{.pos = pos,
                              .load_id = record.load_id,
                              .generation = record.generation,
                              .counts = {},
                              .quads = {}});
}

bool WorldLoop::start_mesh(std::uint32_t index, std::uint32_t key) {
    MeshRecord& record = mesh_record(index);
    const ChunkPos pos = streamer_.records()[index].pos;
    const std::uint32_t generation = next_generation_;
    JobRecord* pins = streamer_.pin_neighborhood(pos, generation);
    if (pins == nullptr) {
        return false;
    }
    const JobPriority priority{.level = record.urgent ? JobLevel::urgent : JobLevel::normal,
                               .key = key};
    record.job = jobs_->submit(priority, MeshJob{.context = &context_,
                                                 .pins = pins,
                                                 .pos = pos,
                                                 .generation = generation,
                                                 .options = meshing_});
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
