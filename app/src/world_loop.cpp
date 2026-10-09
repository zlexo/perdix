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

constexpr std::size_t around_index(int dx, int dy, int dz) noexcept {
    const int at = (dx + 1) + (3 * (dz + 1)) + (9 * (dy + 1));
    return static_cast<std::size_t>(at);
}

} // namespace

void WorldLoop::MeshJob::operator()(JobContext& job) const noexcept {
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
    from_main_.drain([this](ToWorld&& message) { handle(message); });
    if (stopping_) {
        return false;
    }
    streamer_.integrate();
    follow_events();
    mesh_results_.drain([this](MeshResult&& result) { integrate_mesh(result); });
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

void WorldLoop::handle(ToWorld& message) {
    switch (message.kind) {
    case ToWorld::Kind::camera:
        streamer_.set_camera(message.camera);
        follow_events();
        break;
    case ToWorld::Kind::stop:
        stopping_ = true;
        break;
    }
}

void WorldLoop::follow_events() {
    streamer_.take_events(events_);
    for (const ChunkEvent& event : events_) {
        switch (event.kind) {
        case ChunkEvent::Kind::shown:
            mesh_record(event.record).load_id = streamer_.records()[event.record].load_id;
            queue_mesh(event.record);
            break;
        case ChunkEvent::Kind::changed:
            if (mesh_record(event.record).state != MeshRecord::State::none) {
                queue_mesh(event.record);
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

void WorldLoop::queue_mesh(std::uint32_t index) {
    MeshRecord& record = mesh_record(index);
    switch (record.state) {
    case MeshRecord::State::none:
    case MeshRecord::State::sent:
        record.state = MeshRecord::State::queued;
        waiting_meshes_.push_back(index);
        break;
    case MeshRecord::State::queued:
        break;
    case MeshRecord::State::meshing:
        // Meshed again when this job reports: it read the blocks before the
        // change.
        record.dirty = true;
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
        to_main_.push({.kind = ToMain::Kind::unload, .pos = pos, .mesh = {}});
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
            queue_mesh(index);
        } else {
            record.state = MeshRecord::State::sent;
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
    to_main_.push({.kind = ToMain::Kind::mesh, .pos = {}, .mesh = std::move(update)});
}

void WorldLoop::feed() {
    // Streaming meshes first, then the streamer's jobs, in the room the
    // normal level has left.
    const auto room = [this](JobLevel level) {
        const std::uint32_t queued = jobs_->queued(level);
        return queued >= settings_.queue_limit ? 0U : settings_.queue_limit - queued;
    };
    std::size_t normal_room = room(JobLevel::normal);
    std::erase_if(waiting_meshes_, [&](std::uint32_t index) {
        const MeshRecord& record = mesh_record(index);
        if (record.state != MeshRecord::State::queued) {
            return true;
        }
        if (normal_room == 0 || !start_mesh(index)) {
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
    const JobPriority priority{.level = JobLevel::normal, .key = 0};
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
