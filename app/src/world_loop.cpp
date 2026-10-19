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
#include <vx/world/body_motion.hpp>
#include <vx/world/chunk_records.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/collision.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/life.hpp>
#include <vx/world/lod.hpp>
#include <vx/world/movement.hpp>
#include <vx/world/versions.hpp>
#include <vx/world/world_streamer.hpp>

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <tuple>
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

// The level-0 chunk `chunk`'s place on RFC 0011's `level`: the chunk there
// that holds it.
ChunkPos on_level(ChunkPos chunk, std::uint8_t level) noexcept {
    return {.x = chunk.x >> level, .y = chunk.y >> level, .z = chunk.z >> level};
}

// RFC 0011's open border sets of a mesh job's chunk: the main mesh's border
// counts toward each face, then each face's outermost slice meshed with the
// border that way all air, appended to the scratch's quads. `mesh` points
// into the scratch, so it is read first.
void add_open_sets(const ChunkMesh& mesh, std::span<const BlockVisual> visuals, MeshOptions options,
                   MeshWorkerScratch& scratch, MeshUpdate& update) {
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face = 0; face < 6; ++face) {
            update.border.at((pass * 6) + face) = mesh.border.at(pass).at(face);
        }
    }
    for (std::size_t face = 0; face < 6; ++face) {
        const auto side = static_cast<Face>(face);
        open_border(side, scratch.padded);
        const ChunkMesh set = mesh_border(scratch.padded, visuals, scratch.mesh, side, options);
        for (std::size_t pass = 0; pass < 2; ++pass) {
            const std::span<const Quad> quads = set.range(static_cast<Pass>(pass), side);
            scratch.joined.insert(scratch.joined.end(), quads.begin(), quads.end());
            update.open.at((face * 2) + pass) = static_cast<std::uint32_t>(quads.size());
        }
    }
}

} // namespace

ChunkSight chunk_sight(const Viewer& viewer, ChunkPos pos, double near,
                       std::uint8_t level) noexcept {
    // The chunk's box relative to the eye, in doubles: a coarse chunk's
    // corner can be 2^31 blocks out.
    const double size = std::ldexp(static_cast<double>(chunk_size), level);
    const Vec3d min{.x = (static_cast<double>(pos.x) * size) - viewer.eye.x,
                    .y = (static_cast<double>(pos.y) * size) - viewer.eye.y,
                    .z = (static_cast<double>(pos.z) * size) - viewer.eye.z};
    const Vec3d side{.x = size, .y = size, .z = size};
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
    result.update.level = level;
    result.update.load_id = pins->load_id;
    result.update.generation = generation;
    for (std::size_t pass = 0; pass < 2; ++pass) {
        for (std::size_t face = 0; face < 6; ++face) {
            result.update.counts[(pass * 6) + face] =
                static_cast<std::uint32_t>(mesh.ranges[pass][face].size());
        }
    }
    // The main mesh's quads first: the open border sets, above level 0 and
    // only for a mesh with faces, reuse the scratch.
    std::vector<Quad>& joined = scratch.joined;
    const std::span<const Quad> quads = mesh.all();
    joined.assign(quads.begin(), quads.end());
    if (open && !quads.empty()) {
        add_open_sets(mesh, context->visuals, options, scratch, result.update);
    }
    if (!joined.empty()) {
        const std::size_t bytes = joined.size() * sizeof(Quad);
        result.update.quads = context->pool->allocate(bytes);
        std::memcpy(result.update.quads.data(), joined.data(), bytes);
    }
    context->results->push(std::move(result));
}

WorldLoop::WorldLoop(const Generator& generator, std::span<const BlockVisual> visuals,
                     JobSystem& jobs, MeshResultPool& pool, const WorldLoopSettings& settings)
    : levels_(make_levels(generator, jobs, wake_, settings)), jobs_(&jobs), settings_(settings),
      from_main_(64, wake_), mesh_results_(std::size_t{2} * jobs.slot_count(), wake_),
      scratch_(std::max<std::uint32_t>(1, jobs.worker_count())),
      context_{.visuals = visuals, .pool = &pool, .results = &mesh_results_, .scratch = nullptr},
      to_main_(1'024, main_wake_), boxes_(levels_.front()->streamer, settings.physics),
      budget_(settings.memory_budget,
              {.view_distance = settings.region.view_distance,
               .hysteresis = settings.region.hysteresis},
              settings.region.near_distance),
      chosen_hysteresis_(settings.region.hysteresis) {
    context_.scratch = scratch_.data();
    if (settings.clock != nullptr) {
        for (const std::unique_ptr<Level>& level : levels_) {
            level->streamer.set_clock(*settings.clock);
        }
    }
    if (settings.player) {
        walking_.emplace(settings.player->spawn, settings.player->lowest);
        if (settings.perdix) {
            perdix_.emplace(
                Perdix{.walking = Walking(settings.perdix->spawn, settings.player->lowest),
                       .life = Life(settings.perdix->seed, settings.perdix->facing),
                       .scratch = {}});
            perdix_->walking.set_may_fly(false);
            perdix_->walking.face(settings.perdix->facing, 0.0F);
            perdix_->life.set_mode(settings.perdix->mode);
            perdix_->scratch.reserve(256);
        }
    }
}

std::vector<std::unique_ptr<WorldLoop::Level>>
WorldLoop::make_levels(const Generator& generator, JobSystem& jobs, WakeEvent& wake,
                       const WorldLoopSettings& settings) {
    const RegionLimits limits{.view_distance = settings.region.view_distance,
                              .hysteresis = settings.region.hysteresis};
    const std::uint8_t count = settings.lod ? lod_levels : 1;
    std::vector<std::unique_ptr<Level>> levels;
    levels.reserve(count);
    for (std::uint8_t level = 0; level < count; ++level) {
        levels.push_back(
            std::make_unique<Level>(generator, jobs, wake, level_region(settings, level, limits)));
    }
    return levels;
}

LodRings WorldLoop::lod_rings(const WorldLoopSettings& settings, RegionLimits limits) noexcept {
    // The rings' terrain disc is the region's, and their near cylinder too,
    // so the levels drawn agree with what level 0 streams.
    LodRings rings = settings.lod.value_or(LodRings{});
    rings.view_distance = limits.view_distance;
    rings.near_distance = settings.region.near_distance;
    rings.near_height = settings.region.near_height;
    return rings;
}

RegionSettings WorldLoop::level_region(const WorldLoopSettings& settings, std::uint8_t level,
                                       RegionLimits limits) noexcept {
    RegionSettings region = settings.region;
    region.view_distance = limits.view_distance;
    region.hysteresis = limits.hysteresis;
    if (!settings.lod) {
        return region;
    }
    region.view_distance = lod_rings(settings, limits).stream_distance(level);
    region.level = level;
    if (level > 0) {
        region.near_distance = -1;
        region.near_height = 0;
    }
    return region;
}

WorldLoop::MeshRecord& WorldLoop::Level::record(std::uint32_t index) {
    if (index >= meshes.size()) {
        meshes.resize(std::size_t{index} + 1);
    }
    return meshes[index];
}

WorldLoop::~WorldLoop() {
    // RFC 0005's shutdown: what is queued never runs, what runs reports
    // back, and only then do the chunks go.
    for (const std::unique_ptr<Level>& level : levels_) {
        for (const MeshRecord& record : level->meshes) {
            if (record.job && jobs_->cancel(record.job) && record.pins != nullptr) {
                level->streamer.release(*record.pins);
            }
        }
        level->streamer.cancel_queued();
    }
    jobs_->wait_until_idle();
    mesh_results_.drain([this](MeshResult&& result) {
        levels_.at(result.update.level)->streamer.release(*result.pins);
    });
}

bool WorldLoop::pass() {
    const std::chrono::steady_clock::time_point start = time();
    answering_ = std::chrono::nanoseconds{0};
    // RFC 0005's plots, as each pass finds the loop: what waits in each inbox
    // before the drains. A tick, right after a pass, would find them empty.
    // RFC 0011's levels together.
    [[maybe_unused]] std::size_t results_waiting = 0;
    [[maybe_unused]] std::uint64_t pins = 0;
    [[maybe_unused]] std::uint64_t copies = 0;
    for (const std::unique_ptr<Level>& level : levels_) {
        results_waiting += level->streamer.results_waiting();
        pins += level->streamer.pins();
        copies += level->streamer.copies();
    }
    TracyPlot("Inbox: from main", static_cast<std::int64_t>(from_main_.size()));
    TracyPlot("Inbox: meshes", static_cast<std::int64_t>(mesh_results_.size()));
    TracyPlot("Inbox: chunks", static_cast<std::int64_t>(results_waiting));
    TracyPlot("Pins", static_cast<std::int64_t>(pins));
    TracyPlot("Copies on write", static_cast<std::int64_t>(copies));
    TracyPlot("Meshes not uploaded", static_cast<std::int64_t>(unuploaded()));
    from_main_.drain([this](ToWorld&& message) { handle(message); });
    if (stopping_) {
        return false;
    }
    behind_ = false;
    for (const std::unique_ptr<Level>& level : levels_) {
        behind_ = level->streamer.integrate() || behind_;
    }
    follow_events();
    mesh_results_.drain([this](MeshResult&& result) { integrate_mesh(result); });
    send_ready_batches();
    if (select_due_) {
        select();
    }
    feed();
    keep_memory(start);
    // RFC 0006's flythrough reports the longest pass with the longest step:
    // a step is budgeted, and the rest of a pass is not.
    longest_pass_ = std::max(longest_pass_, time() - start - answering_);
    return true;
}

void WorldLoop::settle() {
    // Once a stop arrived, results are no longer taken in, so nothing would
    // ever settle: the destructor waits for the jobs instead.
    while (pass()) {
        const bool idle = std::ranges::all_of(levels_, [](const std::unique_ptr<Level>& level) {
            return level->streamer.stats().jobs_in_flight == 0;
        });
        if (idle && meshes_in_flight_ == 0) {
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
        if (perdix_) {
            tick_perdix(due);
        }
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
    Level& ground = *levels_.front();
    for (std::int32_t dz = -near; dz <= near; ++dz) {
        for (std::int32_t dx = -near; dx <= near; ++dx) {
            if ((dx * dx) + (dz * dz) > near * near) {
                continue;
            }
            for (std::int32_t dy = -region.near_height; dy <= region.near_height; ++dy) {
                const ChunkPos pos{.x = camera.x + dx, .y = camera.y + dy, .z = camera.z + dz};
                const std::optional<std::uint32_t> index = ground.streamer.records().find(pos);
                if (!index || ground.record(*index).forwarded == 0) {
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
        for (const std::unique_ptr<Level>& level : levels_) {
            // Each level's streamer looks from the chunk on its grid.
            Viewer viewer = message.camera;
            viewer.chunk = on_level(viewer.chunk, level->level);
            if (level->streamer.set_viewer(viewer)) {
                level->rescore();
            }
        }
        follow_events();
        // Once a frame: the main thread sends the camera each frame.
        select_due_ = settings_.lod.has_value();
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
    case ToWorld::Kind::perdix_mode:
        if (perdix_) {
            perdix_->life.set_mode(message.perdix_mode);
        }
        break;
    case ToWorld::Kind::bring_perdix:
        bring_perdix(message.feet, message.input.yaw);
        break;
    case ToWorld::Kind::meshing:
        remesh(message.meshing);
        break;
    case ToWorld::Kind::view_distance:
        budget_.choose({.view_distance = message.view_distance, .hysteresis = chosen_hysteresis_});
        apply_limits();
        break;
    case ToWorld::Kind::arena:
        arena_full_frames_ = std::max(arena_full_frames_, message.arena_full_frames);
        arena_full_ = message.arena_full;
        arena_share_ = message.arena_share;
        break;
    case ToWorld::Kind::report: {
        // Like a sample's answer, left out of the longest pass.
        const std::chrono::steady_clock::time_point start = time();
        ToMain answer;
        answer.kind = ToMain::Kind::report;
        answer.report = report();
        to_main_.push(std::move(answer));
        answering_ += time() - start;
        break;
    }
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

void WorldLoop::keep_memory(std::chrono::steady_clock::time_point now) {
    if (next_memory_check_ && now < *next_memory_check_) {
        return;
    }
    next_memory_check_ = now + memory_check_interval;
    chunk_bytes_ = 0;
    for (const std::unique_ptr<Level>& level : levels_) {
        chunk_bytes_ += level->streamer.chunk_bytes();
    }
    TracyPlot("Chunk data (MiB)", static_cast<double>(chunk_bytes_) / (1024.0 * 1024.0));
    // Full in a frame since the last check: a count, so a flag left from
    // frames no longer drawn, such as a minimized window's, adds nothing; and
    // still full, as last reported: the frames after a lowering go on ending
    // full until its unloads apply, which is the same fill, not a new one.
    const bool arena_full = arena_full_ && arena_full_frames_ > checked_full_frames_;
    checked_full_frames_ = arena_full_frames_;
    if (budget_.check(
            {.chunk_bytes = chunk_bytes_, .arena_full = arena_full, .arena_share = arena_share_},
            now)) {
        apply_limits();
    }
}

void WorldLoop::apply_limits() {
    // The budget limits the terrain disc of all levels together: each
    // streams as far as its level needs within it.
    for (const std::unique_ptr<Level>& level : levels_) {
        const RegionSettings region = region_settings(*level);
        level->streamer.set_limits(
            {.view_distance = region.view_distance, .hysteresis = region.hysteresis});
    }
    follow_events();
}

RegionSettings WorldLoop::region_settings(const Level& level) const noexcept {
    return level_region(settings_, level.level, budget_.limits());
}

WorldReport WorldLoop::report() const {
    StreamingStats stats;
    for (const std::unique_ptr<Level>& level : levels_) {
        const StreamingStats one = level->streamer.stats();
        for (std::size_t state = 0; state < stats.chunks.size(); ++state) {
            stats.chunks.at(state) += one.chunks.at(state);
        }
        stats.waiting += one.waiting;
    }
    return {.chunks = stats.chunks,
            .waiting = stats.waiting,
            .chunk_bytes = chunk_bytes_,
            .budget = budget_.bytes(),
            .limits = budget_.limits(),
            .chosen = budget_.chosen(),
            .limited_by = budget_.limited_by()};
}

void WorldLoop::remesh(MeshOptions options) {
    if (options.merge == meshing_.merge) {
        return;
    }
    meshing_ = options;
    // An empty mesh is empty with any options.
    for (const std::unique_ptr<Level>& level : levels_) {
        for (std::uint32_t index = 0; index < level->meshes.size(); ++index) {
            if (level->meshes[index].state != MeshRecord::State::none &&
                !known_empty(*level, index)) {
                queue_mesh(*level, index, false);
            }
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

void WorldLoop::tick_perdix(std::chrono::steady_clock::time_point due) {
    assert(perdix_.has_value());
    assert(walking_.has_value());
    Perdix& perdix = *perdix_;
    if (!boxes_.ready(tick_reach(perdix.walking.player()))) {
        perdix.walking.hold();
        send_perdix(due);
        return;
    }
    const Player& you = walking_->player();
    const Onlooker onlooker{.feet = you.position,
                            .eye_height = eye_height(you.pose),
                            .sneaking = you.pose == Pose::crouching};
    const LifeTick choice =
        perdix.life.tick(perdix.walking.player(), perdix.walking.frame().motion.body_yaw, onlooker,
                         boxes_, perdix.scratch);
    const Vec3d before = perdix.walking.player().position;
    perdix.walking.tick(choice.input, boxes_, choice.look_yaw, choice.look_pitch);
    perdix.life.after_move(perdix.walking.player(), perdix.walking.player().position - before,
                           boxes_, perdix.scratch);
    send_perdix(due);
}

void WorldLoop::bring_perdix(Vec3d feet, float yaw) {
    if (!perdix_) {
        return;
    }
    // The first place from 4 above to 64 below, 5 blocks ahead, where a
    // standing body fits and stands on something: from a free camera high
    // over the ground, down to it.
    const auto heading = static_cast<double>(yaw);
    const Vec3d ahead{.x = feet.x - (5.0 * std::sin(heading)),
                      .y = std::floor(feet.y),
                      .z = feet.z - (5.0 * std::cos(heading))};
    std::vector<Box>& inside = perdix_->scratch;
    for (int rise = 4; rise >= -64; --rise) {
        const Vec3d spot = ahead + Vec3d{.x = 0.0, .y = static_cast<double>(rise), .z = 0.0};
        const Box body = body_box(spot, Pose::standing);
        // A sliver under his feet: something there holds him up.
        const Box below{.min = {.x = body.min.x, .y = spot.y - 0.05, .z = body.min.z},
                        .max = {.x = body.max.x, .y = spot.y, .z = body.max.z}};
        inside.clear();
        boxes_.collect(Box{.min = below.min, .max = body.max}, inside);
        if (!overlaps_any(body, inside) && overlaps_any(below, inside)) {
            const auto facing = static_cast<float>(wrap_turn(heading + std::numbers::pi));
            perdix_->walking.place(spot);
            perdix_->walking.face(facing, 0.0F);
            perdix_->life.start_over(facing);
            send_perdix(last_tick_);
            return;
        }
    }
}

void WorldLoop::send_perdix(std::chrono::steady_clock::time_point at) {
    assert(perdix_.has_value());
    ToMain message;
    message.kind = ToMain::Kind::perdix;
    message.player = perdix_->walking.frame();
    message.tick_at = at;
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
    Level& ground = *levels_.front();
    if (!ground.streamer.set_block(block, state)) {
        return;
    }
    ground.streamer.take_events(events_);
    // The chunks the edit changed, and those of any batch one of them is in
    // already: one batch, sent once all are remeshed.
    Batch batch{.id = next_batch_++, .members = {}};
    const auto join = [&](std::uint32_t index) {
        const bool known = std::ranges::any_of(
            batch.members, [index](const Batch::Member& member) { return member.record == index; });
        if (known || ground.record(index).state == MeshRecord::State::none) {
            return;
        }
        // Any job started from now on reads the edit.
        batch.members.push_back({.record = index,
                                 .load_id = ground.record(index).load_id,
                                 .wanted = next_generation_,
                                 .mesh = {},
                                 .done = false});
    };
    for (const ChunkEvent& event : events_) {
        if (event.kind != ChunkEvent::Kind::changed) {
            continue;
        }
        const std::uint32_t old_batch = ground.record(event.record).batch;
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
        ground.record(member.record).batch = batch.id;
        queue_mesh(ground, member.record, true);
    }
    batches_.push_back(std::move(batch));
}

void WorldLoop::follow_events() {
    for (const std::unique_ptr<Level>& level : levels_) {
        follow_events(*level);
    }
}

void WorldLoop::follow_events(Level& level) {
    level.streamer.take_events(events_);
    for (const ChunkEvent& event : events_) {
        switch (event.kind) {
        case ChunkEvent::Kind::shown:
            level.record(event.record).load_id = level.streamer.records()[event.record].load_id;
            queue_mesh(level, event.record, false);
            break;
        case ChunkEvent::Kind::changed:
            if (level.record(event.record).state != MeshRecord::State::none) {
                queue_mesh(level, event.record, false);
            }
            break;
        case ChunkEvent::Kind::hidden:
        case ChunkEvent::Kind::freed:
            drop_mesh(level, event.record, event.pos);
            break;
        }
    }
    events_.clear();
}

void WorldLoop::queue_mesh(Level& level, std::uint32_t index, bool urgent) {
    MeshRecord& record = level.record(index);
    switch (record.state) {
    case MeshRecord::State::none:
    case MeshRecord::State::sent:
        record.state = MeshRecord::State::queued;
        record.urgent = urgent;
        if (urgent) {
            waiting_urgent_.push_back(index);
        } else {
            level.waiting.push({.key = level.streamer.score(level.streamer.records()[index].pos),
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

void WorldLoop::drop_mesh(Level& level, std::uint32_t index, ChunkPos pos) {
    MeshRecord& record = level.record(index);
    if (record.state == MeshRecord::State::none) {
        return;
    }
    if (record.job && jobs_->cancel(record.job)) {
        level.streamer.release(*record.pins);
        --meshes_in_flight_;
    }
    if (record.forwarded != 0) {
        to_main_.push({.kind = ToMain::Kind::unload,
                       .pos = pos,
                       .level = level.level,
                       .mesh = {},
                       .batch = {}});
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
    Level& level = *levels_.at(result.update.level);
    level.streamer.release(*result.pins);
    --meshes_in_flight_;
    ++meshes_made_;
    const std::optional<std::uint32_t> found = level.streamer.records().find(result.update.pos);
    if (!found) {
        return; // freed: the buffer goes back to the pool
    }
    const std::uint32_t index = *found;
    MeshRecord& record = level.record(index);
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
            queue_mesh(level, index, record.urgent);
        } else {
            record.state = MeshRecord::State::sent;
            record.urgent = false;
        }
    }
    // Newer than the last one sent: on its way, even if a newer job runs, so
    // steady edits cannot starve the chunk (RFC 0006).
    if (result.update.generation > record.forwarded) {
        forward(level, index, std::move(result.update));
    }
}

void WorldLoop::forward(Level& level, std::uint32_t index, MeshUpdate&& update) {
    MeshRecord& record = level.record(index);
    record.forwarded = update.generation;
    record.faces =
        std::ranges::any_of(update.counts, [](std::uint32_t count) { return count > 0; });
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
            MeshRecord& record = levels_.front()->record(member.record);
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
    // An edit's remeshes first, at the urgent level; then each level's
    // streaming meshes, unless too many sent wait to be uploaded, and its
    // streamer's jobs. Each job level's count is plotted where it is read
    // (RFC 0005): how far the workers ran it down since the last pass,
    // without another lock.
    const auto room = [this](JobLevel level, [[maybe_unused]] const char* plot) {
        const std::uint32_t queued = jobs_->queued(level);
        TracyPlot(plot, static_cast<std::int64_t>(queued));
        return queued >= settings_.queue_limit ? 0U : settings_.queue_limit - queued;
    };
    Level& ground = *levels_.front();
    std::size_t urgent_room = room(JobLevel::urgent, "Queued urgent");
    std::erase_if(waiting_urgent_, [&](std::uint32_t index) {
        const MeshRecord& record = ground.record(index);
        if (record.state != MeshRecord::State::queued || !record.urgent) {
            return true; // started, or dropped
        }
        // RFC 0006's fast path: a mesh known to be empty takes no job, and
        // no room in the queue. It goes at once, as a job's result would.
        if (known_empty(ground, index)) {
            skip_mesh(ground, index);
            return true;
        }
        if (urgent_room == 0 || !start_mesh(ground, index, 0)) {
            return false;
        }
        --urgent_room;
        return true;
    });
    // The normal job level's room, shared between RFC 0011's levels so none
    // starves: an equal share each, and what is left over a job each to the
    // levels next in turn, a turn that moves on with each pass; then what a
    // level leaves goes to the others, the finest first. The job queue runs
    // the jobs by their keys, each level's size on screen.
    const std::size_t normal_room = room(JobLevel::normal, "Queued normal");
    const std::size_t count = levels_.size();
    std::size_t over = normal_room % count;
    std::size_t left = 0;
    for (std::size_t step = 0; step < count; ++step) {
        std::size_t share = normal_room / count;
        if (over > 0) {
            ++share;
            --over;
        }
        feed(*levels_[(feed_turn_ + step) % count], share);
        left += share;
    }
    feed_turn_ = (feed_turn_ + 1) % count;
    for (std::size_t level = 0; count > 1 && left > 0 && level < count; ++level) {
        feed(*levels_[level], left);
    }
}

void WorldLoop::feed(Level& level, std::size_t& room) {
    // A mesh known to be empty goes even while the jobs wait for room or
    // for the main thread: it takes neither.
    const bool held = unuploaded() >= settings_.unuploaded_limit;
    while (!level.waiting.empty()) {
        const Backlog::Entry entry = level.waiting.top();
        const MeshRecord& record = level.record(entry.index);
        if (record.state != MeshRecord::State::queued || record.urgent ||
            record.load_id != entry.load_id) {
            level.waiting.pop(); // started, dropped, or an edit's now
            continue;
        }
        if (known_empty(level, entry.index)) {
            skip_mesh(level, entry.index);
            level.waiting.pop();
            continue;
        }
        if (held || room == 0 || !start_mesh(level, entry.index, entry.key)) {
            break; // no room, or no job slot or record: it stays first in line
        }
        level.waiting.pop();
        --room;
    }
    room = level.streamer.feed(room);
}

void WorldLoop::Level::rescore() {
    waiting.rescore([this](const Backlog::Entry& entry) -> std::optional<std::uint32_t> {
        const MeshRecord& mesh = record(entry.index);
        if (mesh.state != MeshRecord::State::queued || mesh.load_id != entry.load_id) {
            return std::nullopt;
        }
        return streamer.score(streamer.records()[entry.index].pos);
    });
}

WorldSample WorldLoop::sample(SampleKind kind) {
    // RFC 0011's levels together.
    WorldSample out{.id = 0,
                    .kind = kind,
                    .at = time(),
                    .generated = 0,
                    .uniform = 0,
                    .meshed = meshes_made_,
                    .skipped = meshes_skipped_};
    bool idle = meshes_in_flight_ == 0;
    for (const std::unique_ptr<Level>& level : levels_) {
        const StreamingStats stats = level->streamer.stats();
        idle = idle && stats.jobs_in_flight == 0 && stats.waiting == 0;
        out.generated += stats.generated;
        out.uniform += stats.uniform;
    }
    if (kind == SampleKind::completion) {
        bool complete = idle;
        for (const std::unique_ptr<Level>& level : levels_) {
            const ChunkRecords& records = level->streamer.records();
            for (std::uint32_t index = 0; complete && index < records.end_index(); ++index) {
                complete = !missing(*level, index);
            }
        }
        if (!complete) {
            return out;
        }
        out.kind = SampleKind::full;
    }
    out.longest_pass = std::exchange(longest_pass_, std::chrono::nanoseconds{0});
    // The terrain disc's, all levels together.
    out.view_distance = budget_.limits().view_distance;
    bool any_missing = false;
    for (const std::unique_ptr<Level>& level : levels_) {
        out.longest_step = std::max(out.longest_step, level->streamer.take_longest_step());
        out.chunk_bytes += level->streamer.chunk_bytes();
        if (out.kind == SampleKind::checked) {
            out.broken += broken_invariants(*level, level->streamer.stats());
        }
        any_missing = count_holes(*level, out) || any_missing;
    }
    out.complete = idle && !any_missing;
    return out;
}

bool WorldLoop::count_holes(Level& level, WorldSample& sample) {
    bool any_missing = false;
    const ChunkRecords& records = level.streamer.records();
    for (std::uint32_t index = 0; index < records.end_index(); ++index) {
        if (!missing(level, index)) {
            continue;
        }
        any_missing = true;
        if (!viewer_) {
            continue;
        }
        const ChunkSight sight =
            chunk_sight(*viewer_, records[index].pos, near_holes_distance, level.level);
        if (sight.in_view) {
            ++sample.holes;
        }
        if (sight.near) {
            ++sample.near_holes;
        }
    }
    return any_missing;
}

namespace {

// RFC 0011's chunks in the order drawn_ keeps.
bool before(const LodPos& a, const LodPos& b) noexcept {
    return std::tuple(a.level, a.x, a.y, a.z) < std::tuple(b.level, b.x, b.y, b.z);
}

} // namespace

void WorldLoop::select() {
    select_due_ = false;
    if (!settings_.lod || !viewer_) {
        return;
    }
    const LodRings rings = lod_rings(settings_, budget_.limits());
    // The rows any level's drawn chunks reach, in level-0 chunks, and the
    // near cylinder's.
    const Vec3d& eye = viewer_->eye;
    const auto eye_row = static_cast<std::int64_t>(std::floor(eye.y)) >> chunk_bits;
    std::int64_t low = eye_row - rings.near_height;
    std::int64_t high = eye_row + rings.near_height;
    for (const std::unique_ptr<Level>& level : levels_) {
        const ChunkRecords& records = level->streamer.records();
        for (std::uint32_t index = 0; index < records.end_index(); ++index) {
            if (records.live(index) && records[index].drawn) {
                const std::int64_t row = records[index].pos.y;
                low = std::min(low, row << level->level);
                high = std::max(high, ((row + 1) << level->level) - 1);
            }
        }
    }
    const auto clamp_row = [](std::int64_t row) {
        return static_cast<std::int32_t>(
            std::clamp<std::int64_t>(row, lowest_chunk, highest_chunk));
    };
    std::vector<LodPos> chosen = lod_select(
        rings, eye, clamp_row(low), clamp_row(high), [this](LodPos pos) { return readiness(pos); },
        drawn_);
    std::ranges::sort(chosen, before);
    ToMain swap;
    swap.kind = ToMain::Kind::swap;
    std::ranges::set_difference(chosen, drawn_, std::back_inserter(swap.show), before);
    std::ranges::set_difference(drawn_, chosen, std::back_inserter(swap.hide), before);
    if (!swap.show.empty() || !swap.hide.empty()) {
        to_main_.push(std::move(swap));
    }
    drawn_ = std::move(chosen);
}

LodReady WorldLoop::readiness(LodPos pos) {
    Level& level = *levels_.at(pos.level);
    const ChunkPos chunk{.x = pos.x, .y = pos.y, .z = pos.z};
    const ChunkRecords& records = level.streamer.records();
    const std::optional<std::uint32_t> index = records.find(chunk);
    if (index && records[*index].drawn) {
        const MeshRecord& mesh = level.record(*index);
        if (mesh.forwarded == 0) {
            return LodReady::waiting;
        }
        return mesh.faces ? LodReady::mesh : LodReady::empty;
    }
    // Not drawn: out of its level's terrain once its column's band is known.
    // Until then it may be drawn yet, though its record can already be
    // there, loaded for a neighbor. Never `outside`, which would end the
    // descent: a coarse level's terrain is its cells', and the full-detail
    // terrain can rise past it.
    return level.streamer.band({.x = chunk.x, .z = chunk.z}).has_value() ? LodReady::empty
                                                                         : LodReady::waiting;
}

bool WorldLoop::missing(Level& level, std::uint32_t index) {
    const ChunkRecords& records = level.streamer.records();
    if (!records.live(index)) {
        return false;
    }
    const ChunkRecord& chunk = records[index];
    return chunk.drawn && level.record(index).forwarded == 0 &&
           !(counts_as_ready(chunk.state) && known_empty(level, index));
}

std::uint32_t WorldLoop::broken_invariants(Level& level, const StreamingStats& stats) {
    const ChunkRecords& records = level.streamer.records();
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
    broken += records.disagreements([&level](std::uint32_t index) {
        return level.record(index).state != MeshRecord::State::none;
    });
    // The region grown by the hysteresis, recomputed from the camera rather
    // than taken from the streamer: no record stays outside it.
    if (camera_) {
        const RegionSettings region = region_settings(level);
        broken += records.outside(
            Region(on_level(*camera_, level.level), region).grown(region.hysteresis));
    }
    return broken;
}

bool WorldLoop::known_empty(const Level& level, std::uint32_t index) const noexcept {
    const ChunkRecord& chunk = level.streamer.records()[index];
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
        const ChunkRecord* neighbor = level.streamer.find(
            {.x = chunk.pos.x + side[0], .y = chunk.pos.y + side[1], .z = chunk.pos.z + side[2]});
        return neighbor != nullptr && neighbor->blocks == nullptr && opaque(neighbor->uniform);
    });
}

void WorldLoop::skip_mesh(Level& level, std::uint32_t index) {
    MeshRecord& record = level.record(index);
    const ChunkPos pos = level.streamer.records()[index].pos;
    record.generation = next_generation_++;
    record.state = MeshRecord::State::sent;
    record.urgent = false;
    ++meshes_skipped_;
    forward(level, index,
            MeshUpdate{.pos = pos,
                       .level = level.level,
                       .load_id = record.load_id,
                       .generation = record.generation,
                       .counts = {},
                       .quads = {}});
}

bool WorldLoop::start_mesh(Level& level, std::uint32_t index, std::uint32_t key) {
    MeshRecord& record = level.record(index);
    const ChunkPos pos = level.streamer.records()[index].pos;
    const std::uint32_t generation = next_generation_;
    JobRecord* pins = level.streamer.pin_neighborhood(pos, generation);
    if (pins == nullptr) {
        return false;
    }
    const JobPriority priority{.level = record.urgent ? JobLevel::urgent : JobLevel::normal,
                               .key = key};
    // Corner shadows at level 0 only: a coarse cell's corners are too far
    // apart for them to read as contact shadows (RFC 0011).
    MeshOptions options = meshing_;
    options.occlusion = options.occlusion && level.level == 0;
    record.job = jobs_->submit(priority, MeshJob{.context = &context_,
                                                 .pins = pins,
                                                 .pos = pos,
                                                 .level = level.level,
                                                 .generation = generation,
                                                 .options = options,
                                                 .open = level.level > 0});
    if (!record.job) {
        level.streamer.release(*pins);
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
