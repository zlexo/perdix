#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/arena_upkeep.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <numbers>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

// RFC 0007's allocation trace of a 30-minute flight, replayed through the
// arena's space, the uploads and the upkeep, without a GPU: no upload is
// refused for long, and the arena never has to grow.
//
// The trace comes from a model of the flight, not a recording: the camera
// cruises, sprints, hovers, turns and twice teleports a million blocks, and
// each chunk border it crosses loads the columns RFC 0006's disc gains and
// unloads those its hysteresis lets go. Each column draws two to four chunks
// whose meshes are sized as RFC 0007 sizes rough terrain, 1,849 quads on
// average, with a long tail; a few chunks a second are remeshed, as edits
// and regeneration do. The meshes reach the uploads in order, at most 64 a
// frame, and the arena is sized as the app sizes it, for rough terrain plus
// a quarter.

namespace {

using vx::ChunkPos;
using vx::render::ArenaSpace;
using vx::render::ArenaUpkeep;
using vx::render::ChunkMeshUpdate;
using vx::render::ChunkUploads;
using vx::render::UpkeepStep;
using vx::render::Uploaded;
using Time = ChunkUploads::Clock::time_point;

constexpr int frames_per_second = 60;
constexpr std::int32_t view_distance = 32;
constexpr std::int32_t hysteresis = 2;
constexpr std::size_t uploads_per_frame = 64;

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 33U;
    value *= 0xFF51'AFD7'ED55'8CCDULL;
    value ^= value >> 33U;
    value *= 0xC4CE'B9FE'1A85'EC53ULL;
    return value ^ (value >> 33U);
}

std::uint64_t column_key(std::int32_t x, std::int32_t z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
           static_cast<std::uint32_t>(z);
}

// How many chunks a column draws: its surface band, two to four.
std::int32_t band(std::int32_t x, std::int32_t z) {
    return 2 + static_cast<std::int32_t>(mix(column_key(x, z)) % 3);
}

// A mesh's quads, rough terrain's: 1,849 on average, most between 600 and
// 3,100, one in twenty up to 8,000 and one in a thousand up to 32,000.
std::uint32_t mesh_quads(std::uint64_t seed) {
    const std::uint64_t roll = mix(seed);
    const std::uint64_t kind = roll % 1'000;
    const std::uint64_t size = (roll >> 10U) % 1'000'000;
    if (kind == 0) {
        return static_cast<std::uint32_t>(8'000 + (size % 24'000));
    }
    if (kind < 50) {
        return static_cast<std::uint32_t>(3'000 + (size % 5'000));
    }
    return static_cast<std::uint32_t>(600 + (size % 2'300));
}

// What the replay saw.
struct Report {
    std::uint64_t unexpected = 0; // uploads neither written nor refused for room
    std::uint64_t uploads = 0;
    std::uint64_t removals = 0;
    std::uint64_t refusal_frames = 0; // frames with a refusal for room
    int longest_refusal = 0;          // frames in a row
    std::uint64_t peak_live = 0;      // quads
    std::uint64_t moved = 0;          // quads compaction moved
    std::uint32_t repacks = 0;
    std::uint32_t capacity = 0; // at the end
    std::uint64_t compaction_frames = 0;
};

class Flight {
public:
    // An arena of `capacity` quads that repacks may grow to `max_capacity`.
    Flight(std::uint32_t capacity, std::uint32_t max_capacity)
        : space_(make_space(capacity)), uploads_(space_),
          upkeep_({.moves = 8,
                   .move_quads = std::uint32_t{1} << 20U,
                   .max_capacity = max_capacity,
                   .slots = 2}),
          quads_(32'768) {
        uploads_.set_budget(std::chrono::hours(1));
    }

    Report fly(int minutes) {
        teleport(0.0, 0.0);
        double heading = 0.0;
        const int frames = minutes * 60 * frames_per_second;
        for (int frame = 0; frame < frames; ++frame) {
            const double seconds = static_cast<double>(frame) / frames_per_second;
            // Every ten minutes a teleport, a million blocks east.
            if (frame > 0 && frame % (10 * 60 * frames_per_second) == 0) {
                teleport(x_ + 1'000'000.0, z_);
            }
            // A two-minute cycle: cruise, sprint, hover, S-curves.
            const double in_cycle = std::fmod(seconds, 120.0);
            double speed = 64.0;
            if (in_cycle >= 60.0 && in_cycle < 75.0) {
                speed = 512.0;
            } else if (in_cycle >= 75.0 && in_cycle < 85.0) {
                speed = 0.0;
            } else if (in_cycle >= 85.0) {
                heading += 0.5 * std::sin(in_cycle * std::numbers::pi / 7.5) / frames_per_second;
            } else {
                heading += 0.05 / frames_per_second;
            }
            x_ += std::cos(heading) * speed / frames_per_second;
            z_ += std::sin(heading) * speed / frames_per_second;
            move_to(column_of(x_), column_of(z_));
            // A few remeshes a second, of chunks drawn at random.
            if (!drawn_.empty() && mix(static_cast<std::uint64_t>(frame) * 7 + 1) % 20 == 0) {
                remesh(static_cast<std::size_t>(mix(static_cast<std::uint64_t>(frame)) %
                                                drawn_.size()));
            }
            run_frame(static_cast<std::uint32_t>(frame));
        }
        report_.capacity = space_.capacity();
        report_.repacks = upkeep_.stats().repacks;
        report_.moved = upkeep_.stats().moved_total;
        return report_;
    }

private:
    struct Chunk {
        ChunkPos pos;
        std::uint32_t generation = 0;
    };

    static ArenaSpace make_space(std::uint32_t capacity) {
        vx::Result<ArenaSpace> space = ArenaSpace::create(capacity, 2);
        REQUIRE(space.has_value());
        return std::move(space).value();
    }

    static std::int32_t column_of(double block) {
        return static_cast<std::int32_t>(std::floor(block / vx::chunk_size));
    }

    // One frame: the slot comes round, the upkeep runs, then the unloads
    // and the meshes in order, until one is refused.
    void run_frame(std::uint32_t frame) {
        const std::uint32_t slot = frame % 2;
        const std::uint32_t wanted = uploads_.stats().wanted;
        uploads_.begin_frame(slot, Time{});
        switch (upkeep_.step(space_, wanted)) {
        case UpkeepStep::none:
            break;
        case UpkeepStep::compact: {
            moves_.clear();
            const vx::render::UpkeepSettings& settings = upkeep_.settings();
            upkeep_.compacted(uploads_.compact(settings.moves, settings.move_quads, moves_));
            ++report_.compaction_frames;
            break;
        }
        case UpkeepStep::repack: {
            vx::Result<ArenaSpace> fresh = ArenaSpace::create(upkeep_.repack_capacity(), 2);
            REQUIRE(fresh.has_value());
            moves_.clear();
            const std::uint32_t current = space_.capacity();
            REQUIRE(uploads_.repack(fresh.value(), moves_));
            space_ = std::move(fresh).value();
            upkeep_.repacked(true, current);
            break;
        }
        }
        for (const ChunkPos pos : unloads_) {
            uploads_.remove(pos);
            ++report_.removals;
        }
        unloads_.clear();
        std::size_t offered = 0;
        bool refused = false;
        while (!queue_.empty() && offered < uploads_per_frame) {
            const Chunk chunk = queue_.front();
            const auto drawn = index_.find(key(chunk.pos));
            if (drawn == index_.end() || drawn_[drawn->second].generation != chunk.generation) {
                queue_.pop_front(); // unloaded or remeshed since: dropped
                continue;
            }
            const vx::Result<Uploaded> uploaded = uploads_.upload(update(chunk), writer_, Time{});
            // Counted, not asserted: a doctest assertion a step would be most
            // of the replay's time.
            if (uploaded.has_value() && uploaded.value() == Uploaded::full) {
                refused = true;
                break;
            }
            if (!uploaded.has_value() || uploaded.value() != Uploaded::written) {
                ++report_.unexpected;
            }
            ++report_.uploads;
            ++offered;
            queue_.pop_front();
        }
        refusals_ = refused ? refusals_ + 1 : 0;
        report_.refusal_frames += refused ? 1 : 0;
        report_.longest_refusal = std::max(report_.longest_refusal, refusals_);
        report_.peak_live = std::max<std::uint64_t>(report_.peak_live, space_.live());
    }

    ChunkMeshUpdate update(const Chunk& chunk) {
        const std::uint32_t quads = mesh_quads(key(chunk.pos) * 31 + chunk.generation);
        vx::ChunkMesh mesh{};
        mesh.ranges[0][0] = std::span<const vx::Quad>(quads_).first(quads);
        return {.pos = chunk.pos, .generation = chunk.generation, .mesh = mesh};
    }

    static std::uint64_t key(ChunkPos pos) {
        return mix(column_key(pos.x, pos.z)) ^ static_cast<std::uint64_t>(pos.y);
    }

    void load_column(std::int32_t x, std::int32_t z) {
        if (!columns_.emplace(column_key(x, z), 0).second) {
            return;
        }
        for (std::int32_t y = 0; y < band(x, z); ++y) {
            const ChunkPos pos{.x = x, .y = y, .z = z};
            index_[key(pos)] = drawn_.size();
            drawn_.push_back({.pos = pos, .generation = ++generation_});
            queue_.push_back(drawn_.back());
        }
    }

    void unload_column(std::int32_t x, std::int32_t z) {
        if (columns_.erase(column_key(x, z)) == 0) {
            return;
        }
        for (std::int32_t y = 0; y < band(x, z); ++y) {
            const ChunkPos pos{.x = x, .y = y, .z = z};
            const auto found = index_.find(key(pos));
            const std::size_t at = found->second;
            index_.erase(found);
            if (at != drawn_.size() - 1) {
                drawn_[at] = drawn_.back();
                index_[key(drawn_[at].pos)] = at;
            }
            drawn_.pop_back();
            unloads_.push_back(pos);
        }
    }

    void remesh(std::size_t at) {
        drawn_[at].generation = ++generation_;
        queue_.push_back(drawn_[at]);
    }

    static bool within(std::int32_t dx, std::int32_t dz, std::int32_t distance) {
        return (std::int64_t{dx} * dx) + (std::int64_t{dz} * dz) <=
               std::int64_t{distance} * distance;
    }

    // The camera's column changed: the disc's new columns load, nearest
    // first, and those past the hysteresis unload.
    void move_to(std::int32_t cx, std::int32_t cz) {
        if (cx == cx_ && cz == cz_) {
            return;
        }
        const std::int32_t reach = view_distance + hysteresis;
        for (std::int32_t dz = -reach; dz <= reach; ++dz) {
            for (std::int32_t dx = -reach; dx <= reach; ++dx) {
                const std::int32_t x = cx_ + dx;
                const std::int32_t z = cz_ + dz;
                if (within(dx, dz, reach) && !within(x - cx, z - cz, reach)) {
                    unload_column(x, z);
                }
            }
        }
        std::vector<std::pair<std::int64_t, std::pair<std::int32_t, std::int32_t>>> entering;
        for (std::int32_t dz = -view_distance; dz <= view_distance; ++dz) {
            for (std::int32_t dx = -view_distance; dx <= view_distance; ++dx) {
                if (within(dx, dz, view_distance) &&
                    !within(cx + dx - cx_, cz + dz - cz_, view_distance)) {
                    entering.push_back(
                        {(std::int64_t{dx} * dx) + (std::int64_t{dz} * dz), {cx + dx, cz + dz}});
                }
            }
        }
        std::ranges::sort(entering);
        for (const auto& [distance, column] : entering) {
            load_column(column.first, column.second);
        }
        cx_ = cx;
        cz_ = cz;
    }

    void teleport(double x, double z) {
        std::vector<std::uint64_t> loaded;
        for (const auto& [column, unused] : columns_) {
            loaded.push_back(column);
        }
        for (const std::uint64_t column : loaded) {
            unload_column(static_cast<std::int32_t>(column >> 32U),
                          static_cast<std::int32_t>(column & 0xFFFF'FFFFU));
        }
        x_ = x;
        z_ = z;
        // Far from every loaded column, so the whole disc loads.
        cx_ = column_of(x) + 1'000;
        cz_ = column_of(z) + 1'000;
        move_to(column_of(x), column_of(z));
    }

    class NoWrites final : public vx::render::QuadWriter {
    public:
        vx::Result<void> write(std::uint32_t /*first*/,
                               std::span<const vx::Quad> /*quads*/) override {
            return {};
        }
    };

    ArenaSpace space_;
    ChunkUploads uploads_;
    ArenaUpkeep upkeep_;
    std::vector<vx::Quad> quads_; // every mesh's quads point here: nothing reads them
    NoWrites writer_;
    std::vector<vx::render::ArenaMove> moves_;
    std::unordered_map<std::uint64_t, int> columns_; // loaded
    std::vector<Chunk> drawn_;                       // the chunks drawn, in no order
    std::unordered_map<std::uint64_t, std::size_t> index_;
    std::deque<Chunk> queue_; // meshes on their way, oldest first
    std::vector<ChunkPos> unloads_;
    std::uint32_t generation_ = 0;
    double x_ = 0.0;
    double z_ = 0.0;
    std::int32_t cx_ = 0;
    std::int32_t cz_ = 0;
    int refusals_ = 0;
    Report report_;
};

// The app's arena for a view distance (app/src/main.cpp): RFC 0006's drawn
// meshes, 9,400 at 32 chunks, as large as rough terrain's, plus a quarter.
std::uint32_t app_capacity() {
    return static_cast<std::uint32_t>(9'400.0 * 1'843.0 * 1.25);
}

} // namespace

TEST_CASE("a 30-minute flight at the app's arena size needs no repack, and no upload is refused "
          "room") {
    Flight flight(app_capacity(), app_capacity() * 2);
    const Report report = flight.fly(30);
    MESSAGE("uploads ", report.uploads, ", removals ", report.removals, ", peak live ",
            report.peak_live, " of ", app_capacity(), " quads");
    CHECK(report.unexpected == 0);
    CHECK(report.uploads > 1'000'000);
    CHECK(report.peak_live > app_capacity() * 9 / 10);
    CHECK(report.refusal_frames == 0);
    CHECK(report.repacks == 0);
}

TEST_CASE("the same flight in an arena under its peak repacks, and no upload waits long") {
    const std::uint32_t small = app_capacity() * 9 / 10;
    Flight flight(small, app_capacity() * 2);
    const Report report = flight.fly(30);
    MESSAGE("refusal frames ", report.refusal_frames, ", longest ", report.longest_refusal,
            ", moved ", report.moved, " in ", report.compaction_frames, " frames, ", report.repacks,
            " repacks, ", report.capacity, " quads at the end");
    CHECK(report.unexpected == 0);
    CHECK(report.repacks > 0);
    CHECK(report.capacity > small);
    CHECK(report.longest_refusal <= 6);
}
