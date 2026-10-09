#include <vx/world/job_record.hpp>

#include <vx/world/block_state.hpp>
#include <vx/world/chunk_storage.hpp>
#include <vx/world/generator.hpp>
#include <vx/world/versions.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>

namespace vx {

JobRecords::JobRecords(std::uint32_t capacity, Versions<ChunkStorage>& chunks,
                       Versions<ChunkPlan>& plans)
    : records_(capacity), taken_(capacity, 0), chunks_(&chunks), plans_(&plans) {
    free_.reserve(capacity);
    // The first record on top.
    for (std::uint32_t at = capacity; at > 0; --at) {
        free_.push_back(at - 1);
    }
}

JobRecord* JobRecords::acquire(std::uint32_t load_id, std::uint32_t sequence) noexcept {
    if (free_.empty()) {
        return nullptr;
    }
    const std::uint32_t at = free_.back();
    free_.pop_back();
    taken_[at] = 1;
    JobRecord& record = records_[at];
    record.load_id = load_id;
    record.sequence = sequence;
    return &record;
}

void JobRecords::read_chunk(JobRecord& record, std::size_t at, Version<ChunkStorage>* blocks,
                            BlockState uniform) noexcept {
    assert(at < record.chunks.size() && record.chunks[at] == nullptr);
    if (blocks != nullptr) {
        chunks_->pin(*blocks);
    }
    record.chunks[at] = blocks;
    record.uniform[at] = uniform;
}

void JobRecords::read_plan(JobRecord& record, std::size_t at, Version<ChunkPlan>* plan) noexcept {
    assert(at < record.plans.size() && record.plans[at] == nullptr);
    if (plan != nullptr) {
        plans_->pin(*plan);
    }
    record.plans[at] = plan;
}

void JobRecords::release(JobRecord& record) noexcept {
    const auto at = static_cast<std::size_t>(&record - records_.data());
    assert(at < records_.size() && taken_[at] != 0 && "released twice, or not this pool's");
    for (Version<ChunkStorage>*& blocks : record.chunks) {
        if (blocks != nullptr) {
            chunks_->unpin(*blocks);
            blocks = nullptr;
        }
    }
    for (Version<ChunkPlan>*& plan : record.plans) {
        if (plan != nullptr) {
            plans_->unpin(*plan);
            plan = nullptr;
        }
    }
    record.uniform.fill(BlockState::air);
    taken_[at] = 0;
    free_.push_back(static_cast<std::uint32_t>(at));
}

} // namespace vx
