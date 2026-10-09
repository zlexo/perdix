#pragma once

#include <vx/core/job_system.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/versions.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// A chunk's place in RFC 0006's lifecycle, the world half.
enum class ChunkState : std::uint8_t {
    queued,       // in the region, waiting for its plan job
    planning,     // its plan job runs
    planned,      // its plan exists: waiting for its neighbors', or in the plan ring
    build_queued, // it and its 26 neighbors have plans: waiting for its build job
    building,     // its build job runs
    ready,        // its blocks exist, or it is uniform
    complete,     // it and its 26 neighbors are ready: it can be meshed
    unloading,    // it left the region while its job ran, and waits for it
};

// What a chunk's neighbors count it as: planned from its plan on, or when
// uniform (a build reads a null plan for it); ready once its blocks exist.
[[nodiscard]] constexpr bool counts_as_planned(ChunkState state) noexcept {
    return state >= ChunkState::planned && state <= ChunkState::complete;
}
[[nodiscard]] constexpr bool counts_as_ready(ChunkState state) noexcept {
    return state == ChunkState::ready || state == ChunkState::complete;
}

// One loaded chunk's bookkeeping (RFC 0006), on the world thread.
struct ChunkRecord {
    ChunkPos pos;
    ChunkState state = ChunkState::queued;
    // Where it goes back to when it returns to the region while unloading.
    ChunkState before_unloading = ChunkState::queued;
    std::uint8_t planned_neighbors = 0; // 0 to 26
    std::uint8_t ready_neighbors = 0;   // 0 to 26
    // Drawn chunks, and loaded ones, within one chunk of it, itself
    // included: 0 to 27. RFC 0006's loaded region is the drawn one grown by
    // a chunk, and the planned one the loaded one grown by a chunk, so a
    // chunk is loaded while drawn_near is above 0, and has a record while
    // loaded_near is.
    std::uint8_t drawn_near = 0;
    std::uint8_t loaded_near = 0;
    // In the drawn region, in the loaded region (it gets blocks), edited.
    bool drawn = false;
    bool loaded = false;
    bool edited = false;
    // The whole chunk, while it has no blocks.
    BlockState uniform = BlockState::air;
    // Bumped by every edit, from one world-wide counter.
    std::uint32_t version = 0;
    // Never reused: a reloaded chunk's results are told from the old one's.
    std::uint32_t load_id = 0;
    // Its current blocks, null when uniform; its plan, null until it has
    // one or when uniform.
    Version<ChunkStorage>* blocks = nullptr;
    Version<ChunkPlan>* plan = nullptr;
    // The job making its plan or blocks, while one is queued or running;
    // a build's record of the plans it pinned.
    JobHandle job;
    JobRecord* job_record = nullptr;
};

// The world's chunk records (RFC 0006): a pool at stable indices behind the
// chunk map, with each record's counts of planned and ready neighbors kept
// up to date as states change, 26 lookups a change. World thread only.
class ChunkRecords {
public:
    // A record at `pos`, which has none, in state queued, counting the
    // records already around it, and as planned and ready the chunks past
    // the world's edge.
    std::uint32_t add(ChunkPos pos, std::uint32_t load_id);

    // Moves the record to `state`. Each neighbor whose count changes is
    // passed to `changed`, so the caller can move it on in turn.
    template <class F> void set_state(std::uint32_t index, ChunkState state, F&& changed) {
        ChunkRecord& record = records_[index];
        const int planned =
            counted(counts_as_planned(state)) - counted(counts_as_planned(record.state));
        const int ready = counted(counts_as_ready(state)) - counted(counts_as_ready(record.state));
        record.state = state;
        if (planned != 0 || ready != 0) {
            for_neighbors(record.pos, [&](std::uint32_t neighbor) {
                ChunkRecord& other = records_[neighbor];
                other.planned_neighbors =
                    static_cast<std::uint8_t>(other.planned_neighbors + planned);
                other.ready_neighbors = static_cast<std::uint8_t>(other.ready_neighbors + ready);
                assert(other.planned_neighbors <= 26 && other.ready_neighbors <= 26);
                changed(neighbor);
            });
        }
    }

    // The record goes, and its neighbors stop counting it: each whose count
    // changes is passed to `changed`.
    template <class F> void remove(std::uint32_t index, F&& changed) {
        set_state(index, ChunkState::queued, changed);
        [[maybe_unused]] const bool erased = map_.erase(records_[index].pos);
        assert(erased);
        records_[index] = ChunkRecord{};
        free_.push_back(index);
    }

    [[nodiscard]] ChunkRecord& operator[](std::uint32_t index) noexcept { return records_[index]; }
    [[nodiscard]] const ChunkRecord& operator[](std::uint32_t index) const noexcept {
        return records_[index];
    }
    [[nodiscard]] std::optional<std::uint32_t> find(ChunkPos pos) const noexcept {
        return map_.find(pos);
    }
    [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }
    // One past the highest index in use: live records are those whose
    // position maps to their index.
    [[nodiscard]] std::uint32_t end_index() const noexcept {
        return static_cast<std::uint32_t>(records_.size());
    }
    [[nodiscard]] bool live(std::uint32_t index) const noexcept {
        return index < records_.size() && map_.find(records_[index].pos) == index;
    }

    // Calls `visit` with the index of each of the 26 records around `pos`
    // that exist.
    template <class F> void for_neighbors(ChunkPos pos, F&& visit) const {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;
                    }
                    const ChunkPos next{.x = pos.x + dx, .y = pos.y + dy, .z = pos.z + dz};
                    if (const std::optional<std::uint32_t> neighbor = map_.find(next)) {
                        visit(*neighbor);
                    }
                }
            }
        }
    }

private:
    static constexpr int counted(bool counts) noexcept { return counts ? 1 : 0; }

    ChunkMap map_;
    std::vector<ChunkRecord> records_;
    std::vector<std::uint32_t> free_; // a stack of indices
};

} // namespace vx
