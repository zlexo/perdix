#pragma once

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/versions.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vx {

// What one job in flight reads (RFC 0005). The world thread fills the record
// and pins what it names before submitting the job, which captures a pointer
// to it. When the job's completion arrives, or its cancel succeeds, exactly
// the versions the record names are unpinned, whatever is current by then.
struct JobRecord {
    // With the chunk's position, these key the job's result: a load id is
    // never reused, so the result of a chunk that left and came back is told
    // from the new one's, and the sequence orders one chunk's results.
    std::uint32_t load_id = 0;
    std::uint32_t sequence = 0;
    // A mesh job's chunk and its 26 neighbors, in the order of a build's
    // plans: x fastest, then z, then y, so [13] is the job's own. Null for a
    // uniform chunk, which has no blocks to pin: its state is in `uniform`.
    std::array<Version<ChunkStorage>*, 27> chunks{};
    std::array<BlockState, 27> uniform{};
    // A build job's 27 plans, in the same order; null where out of reach.
    // RFC 0008's parameter snapshot joins them when it exists.
    std::array<Version<ChunkPlan>*, 27> plans{};
};

// The records of the jobs in flight and the pins they hold (RFC 0005): a pool
// sized to the job slots, at stable addresses. World thread only.
class JobRecords {
public:
    JobRecords(std::uint32_t capacity, Versions<ChunkStorage>& chunks, Versions<ChunkPlan>& plans);
    JobRecords(const JobRecords&) = delete;
    JobRecords& operator=(const JobRecords&) = delete;
    JobRecords(JobRecords&&) = delete;
    JobRecords& operator=(JobRecords&&) = delete;
    ~JobRecords() = default;

    // An empty record for a job about to be submitted, or null when every one
    // is taken: the world thread then leaves the chunk in its backlog, as
    // when submit finds no free slot.
    [[nodiscard]] JobRecord* acquire(std::uint32_t load_id, std::uint32_t sequence) noexcept;

    // The chunk at `at` for the record's job: pins `blocks`, its current
    // version, or keeps `uniform` for a uniform chunk, which has none.
    void read_chunk(JobRecord& record, std::size_t at, Version<ChunkStorage>* blocks,
                    BlockState uniform) noexcept;
    // The plan at `at` for the record's job, pinned; null where out of reach.
    void read_plan(JobRecord& record, std::size_t at, Version<ChunkPlan>* plan) noexcept;

    // The job reported back, its cancel succeeded or its submit failed:
    // unpins exactly what the record names, and the record is free again.
    void release(JobRecord& record) noexcept;

    [[nodiscard]] std::uint32_t capacity() const noexcept {
        return static_cast<std::uint32_t>(records_.size());
    }
    [[nodiscard]] std::uint32_t in_use() const noexcept {
        return capacity() - static_cast<std::uint32_t>(free_.size());
    }

private:
    std::vector<JobRecord> records_;  // never resized: jobs hold addresses
    std::vector<std::uint32_t> free_; // a stack of indices into records_
    std::vector<std::uint8_t> taken_; // 1 while a record's job is in flight
    Versions<ChunkStorage>* chunks_;
    Versions<ChunkPlan>* plans_;
};

} // namespace vx
