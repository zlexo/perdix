#include <vx/core/job_system.hpp>
#include <vx/world/block_data_pool.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/job_record.hpp>
#include <vx/world/versions.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

// RFC 0005's pins under random loads, edits, unloads, jobs, cancels and
// completions, against a model: every version lives exactly while it is
// current or pinned, and every job reads the blocks it was handed.

namespace {

using vx::BlockState;
using vx::ChunkPlan;
using vx::ChunkStorage;
using vx::JobRecord;
using vx::JobRecords;
using vx::Version;
using vx::Versions;

std::size_t block_data_in_use() {
    std::size_t total = 0;
    for (const vx::BlockDataClass& sizes : vx::block_data::stats()) {
        total += sizes.in_use;
    }
    return total;
}

// A chunk of stone whose block 0 is `mark`, so a job can tell versions apart.
ChunkStorage marked(BlockState mark) {
    ChunkStorage chunk = ChunkStorage::uniform(static_cast<BlockState>(1));
    chunk.set(0, mark);
    return chunk;
}

using Marks = std::array<BlockState, 27>;

struct Seen {
    const JobRecord* record = nullptr;
    Marks marks{};
};

// Reads block 0 of each of the 27 chunks it was handed, and reports them.
struct MarkJob {
    const JobRecord* record;
    std::vector<Seen>* seen;

    void operator()(vx::JobContext& /*context*/) const noexcept {
        Marks marks{};
        for (std::size_t at = 0; at < marks.size(); ++at) {
            const Version<ChunkStorage>* blocks = record->chunks[at];
            marks[at] = blocks != nullptr ? blocks->value().get(0) : record->uniform[at];
        }
        seen->push_back({.record = record, .marks = marks});
    }
};

// The model's view of one version.
struct ModelVersion {
    Version<ChunkStorage>* address = nullptr;
    std::uint32_t pins = 0;
    bool current = true;
    bool freed = false;
    BlockState mark = BlockState::air;
};

enum class JobState : std::uint8_t { queued, ran, done };

struct ModelJob {
    JobRecord* record = nullptr;
    vx::JobHandle handle{};
    std::vector<std::size_t> pinned{}; // model version ids
    Marks expected{};
    Marks seen{};
    JobState state = JobState::queued;
};

class Model {
public:
    std::size_t make(Version<ChunkStorage>& address, BlockState mark) {
        versions_.push_back(
            {.address = &address, .pins = 0, .current = true, .freed = false, .mark = mark});
        return versions_.size() - 1;
    }
    void pin(std::size_t id) { ++versions_[id].pins; }
    void unpin(std::size_t id) {
        --versions_[id].pins;
        settle(id);
    }
    void retire(std::size_t id) {
        versions_[id].current = false;
        settle(id);
    }
    void write_in_place(std::size_t id, BlockState mark) { versions_[id].mark = mark; }
    [[nodiscard]] const ModelVersion& operator[](std::size_t id) const { return versions_[id]; }

    // The store's counts and every live version's pins and currency agree.
    void check(const Versions<ChunkStorage>& chunks) const {
        std::size_t live = 0;
        std::size_t frozen = 0;
        std::uint64_t pins = 0;
        for (const ModelVersion& version : versions_) {
            if (version.freed) {
                continue;
            }
            ++live;
            if (!version.current) {
                ++frozen;
            }
            pins += version.pins;
            REQUIRE(version.address->pins() == version.pins);
            REQUIRE(version.address->current() == version.current);
            REQUIRE(version.address->value().get(0) == version.mark);
        }
        REQUIRE(chunks.live() == live);
        REQUIRE(chunks.frozen() == frozen);
        REQUIRE(chunks.pins() == pins);
    }

private:
    void settle(std::size_t id) {
        ModelVersion& version = versions_[id];
        if (version.pins == 0 && !version.current) {
            version.freed = true;
        }
    }

    std::vector<ModelVersion> versions_;
};

} // namespace

TEST_CASE("under random loads, edits, unloads and jobs, versions live exactly while current or "
          "pinned, and jobs read what they were handed") {
    const std::size_t data_before = block_data_in_use();
    Versions<ChunkStorage> chunks;
    Versions<ChunkPlan> plans;
    JobRecords records(24, chunks, plans);
    vx::JobSystem jobs({.workers = 0});
    std::vector<Seen> seen;
    seen.reserve(64);
    Model model;
    constexpr std::size_t slots = 6;
    std::vector<std::ptrdiff_t> current(slots, -1); // model ids; -1 unloaded
    std::vector<ModelJob> in_flight;
    std::mt19937 random(0x5EED);
    std::uint16_t next_mark = 2;
    std::size_t completed = 0;
    std::size_t canceled = 0;
    std::size_t copies = 0;
    const auto fresh_mark = [&next_mark] {
        next_mark =
            next_mark == 60'000 ? std::uint16_t{2} : static_cast<std::uint16_t>(next_mark + 1);
        return static_cast<BlockState>(next_mark);
    };
    for (int step = 0; step < 4'000; ++step) {
        const std::size_t slot = random() % slots;
        const std::ptrdiff_t id = current[slot];
        switch (random() % 7) {
        case 0: // load
            if (id < 0) {
                const BlockState mark = fresh_mark();
                current[slot] =
                    static_cast<std::ptrdiff_t>(model.make(chunks.make(marked(mark)), mark));
            }
            break;
        case 1: // unload
            if (id >= 0) {
                chunks.retire(*model[static_cast<std::size_t>(id)].address);
                model.retire(static_cast<std::size_t>(id));
                current[slot] = -1;
            }
            break;
        case 2: { // edit
            if (id < 0) {
                break;
            }
            Version<ChunkStorage>* address = model[static_cast<std::size_t>(id)].address;
            const bool pinned = address->pins() > 0;
            const BlockState mark = fresh_mark();
            chunks.write(address).set(0, mark);
            if (pinned) {
                // A copy, now current; the pinned version stays for its jobs.
                ++copies;
                model.retire(static_cast<std::size_t>(id));
                current[slot] = static_cast<std::ptrdiff_t>(model.make(*address, mark));
            } else {
                model.write_in_place(static_cast<std::size_t>(id), mark);
            }
            break;
        }
        case 3:
        case 4: { // a job meshing the slot, with every loaded slot as a neighbor
            if (id < 0) {
                break;
            }
            JobRecord* record =
                records.acquire(static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(step));
            if (record == nullptr) {
                break;
            }
            ModelJob job{.record = record};
            for (std::size_t at = 0; at < 27; ++at) {
                const std::size_t from = at == 13 ? slot : at % slots;
                const std::ptrdiff_t neighbor = current[from];
                // Some neighbors uniform: their state comes in the record.
                if (neighbor < 0 || (at != 13 && at % 3 == 0)) {
                    records.read_chunk(*record, at, nullptr, static_cast<BlockState>(1));
                    job.expected[at] = static_cast<BlockState>(1);
                    continue;
                }
                const auto neighbor_id = static_cast<std::size_t>(neighbor);
                records.read_chunk(*record, at, model[neighbor_id].address, BlockState::air);
                model.pin(neighbor_id);
                job.pinned.push_back(neighbor_id);
                job.expected[at] = model[neighbor_id].mark;
            }
            job.handle = jobs.submit({}, MarkJob{record, &seen});
            REQUIRE(job.handle);
            in_flight.push_back(job);
            break;
        }
        case 5: // the workers catch up
            jobs.run_until_idle();
            for (const Seen& report : seen) {
                for (ModelJob& job : in_flight) {
                    if (job.state == JobState::queued && job.record == report.record) {
                        job.state = JobState::ran;
                        job.seen = report.marks;
                    }
                }
            }
            seen.clear();
            break;
        default: { // a completion arrives, or a cancel succeeds
            if (in_flight.empty()) {
                break;
            }
            const std::size_t pick = random() % in_flight.size();
            ModelJob job = in_flight[pick];
            if (job.state == JobState::queued) {
                REQUIRE(jobs.cancel(job.handle));
                ++canceled;
            } else {
                CHECK(job.seen == job.expected);
                ++completed;
            }
            records.release(*job.record);
            for (const std::size_t pinned : job.pinned) {
                model.unpin(pinned);
            }
            in_flight.erase(in_flight.begin() + static_cast<std::ptrdiff_t>(pick));
            break;
        }
        }
        model.check(chunks);
    }
    // Everything reports back and leaves.
    jobs.run_until_idle();
    seen.clear();
    for (const ModelJob& job : in_flight) {
        records.release(*job.record);
        for (const std::size_t pinned : job.pinned) {
            model.unpin(pinned);
        }
    }
    for (const std::ptrdiff_t id : current) {
        if (id >= 0) {
            chunks.retire(*model[static_cast<std::size_t>(id)].address);
            model.retire(static_cast<std::size_t>(id));
        }
    }
    model.check(chunks);
    CHECK(chunks.live() == 0);
    CHECK(block_data_in_use() == data_before);
    CHECK(records.in_use() == 0);
    CHECK(chunks.copies() == copies);
    // Every path ran often.
    CHECK(completed > 200);
    CHECK(canceled > 50);
    CHECK(copies > 50);
}
