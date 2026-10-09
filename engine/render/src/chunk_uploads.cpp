#include <vx/render/chunk_uploads.hpp>

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/upload.hpp>
#include <vx/world/coordinates.hpp>

#include <volk.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vx::render {
namespace {

constexpr std::array<Pass, 2> passes = {Pass::opaque, Pass::cutout};

std::uint32_t total_quads(const ChunkMesh& mesh) noexcept {
    std::size_t total = 0;
    for (const auto& faces : mesh.ranges) {
        for (const std::span<const Quad> range : faces) {
            total += range.size();
        }
    }
    return static_cast<std::uint32_t>(total);
}

} // namespace

Result<void> ArenaWriter::write(std::uint32_t first, std::span<const Quad> quads) {
    return upload(*context_, commands_, arena_->buffer(),
                  VkDeviceSize{first} * ChunkArena::quad_bytes, std::as_bytes(quads));
}

void ChunkUploads::begin_frame(std::uint32_t slot, Clock::time_point now) noexcept {
    space_->reclaim(slot);
    slot_ = slot;
    frame_start_ = now;
    stats_.uploads = 0;
    stats_.quads_uploaded = 0;
    stats_.dropped = 0;
    stats_.refused = 0;
}

Result<Uploaded> ChunkUploads::upload(const ChunkMeshUpdate& update, QuadWriter& writer,
                                      Clock::time_point now) {
    if (stale(update)) {
        ++stats_.dropped;
        return Uploaded::dropped;
    }
    // An empty mesh writes nothing: it costs none of the budget.
    const std::uint32_t quads = total_quads(update.mesh);
    if (quads == 0) {
        empty(update);
        return Uploaded::removed;
    }
    if (now - frame_start_ >= budget_) {
        ++stats_.refused;
        return Uploaded::later;
    }
    const std::optional<ArenaRange> range = space_->allocate(quads);
    if (!range) {
        ++stats_.refused;
        return Uploaded::full;
    }
    return place(update, *range, writer);
}

Result<Uploaded> ChunkUploads::upload_batch(std::span<const ChunkMeshUpdate> batch,
                                            QuadWriter& writer) {
    // Every range first: when one does not fit, nothing is written, and the
    // ranges taken go back with this frame's slot.
    std::vector<std::optional<ArenaRange>> ranges(batch.size());
    const auto give_back = [&](std::size_t from) {
        for (std::size_t at = from; at < ranges.size(); ++at) {
            if (ranges[at]) {
                space_->retire(*ranges[at], slot_);
            }
        }
    };
    for (std::size_t at = 0; at < batch.size(); ++at) {
        const std::uint32_t quads = total_quads(batch[at].mesh);
        if (quads == 0 || stale(batch[at])) {
            continue;
        }
        ranges[at] = space_->allocate(quads);
        if (!ranges[at]) {
            give_back(0);
            ++stats_.refused;
            return Uploaded::full;
        }
    }
    for (std::size_t at = 0; at < batch.size(); ++at) {
        const std::optional<ArenaRange>& range = ranges[at];
        if (stale(batch[at])) {
            ++stats_.dropped;
        } else if (!range) {
            empty(batch[at]);
        } else if (const Result<Uploaded> placed = place(batch[at], *range, writer);
                   !placed.has_value()) {
            give_back(at + 1);
            return placed.error();
        }
    }
    return Uploaded::written;
}

void ChunkUploads::remove(ChunkPos pos) {
    if (const std::optional<std::uint32_t> at = index_.find(pos)) {
        erase(*at);
    }
    emptied_.erase(pos);
}

const ChunkMeshRecord* ChunkUploads::find(ChunkPos pos) const noexcept {
    const std::optional<std::uint32_t> at = index_.find(pos);
    return at ? &records_[*at] : nullptr;
}

std::optional<std::uint32_t> ChunkUploads::newest(ChunkPos pos) const noexcept {
    if (const std::optional<std::uint32_t> at = index_.find(pos)) {
        return records_[*at].generation;
    }
    return emptied_.find(pos);
}

bool ChunkUploads::stale(const ChunkMeshUpdate& update) const noexcept {
    const std::optional<std::uint32_t> known = newest(update.pos);
    return known.has_value() && update.generation <= *known;
}

Result<Uploaded> ChunkUploads::place(const ChunkMeshUpdate& update, const ArenaRange& range,
                                     QuadWriter& writer) {
    std::array<std::uint16_t, 12> count{};
    std::uint32_t first = range.first_quad;
    std::size_t group = 0;
    for (const Pass pass : passes) {
        for (const Face face : arena_faces) {
            const std::span<const Quad> quads = update.mesh.range(pass, face);
            assert(quads.size() <= 32'768);
            count[group++] = static_cast<std::uint16_t>(quads.size());
            if (quads.empty()) {
                continue;
            }
            if (const Result<void> written = writer.write(first, quads); !written.has_value()) {
                space_->retire(range, slot_);
                return written.error();
            }
            first += static_cast<std::uint32_t>(quads.size());
        }
    }
    if (const std::optional<std::uint32_t> at = index_.find(update.pos)) {
        ChunkMeshRecord& record = records_[*at];
        space_->retire(record.range, slot_);
        stats_.quads -= record.range.quads;
        record.generation = update.generation;
        record.range = range;
        record.count = count;
    } else {
        index_.insert(update.pos, static_cast<std::uint32_t>(records_.size()));
        records_.push_back(
            {.pos = update.pos, .generation = update.generation, .range = range, .count = count});
        emptied_.erase(update.pos);
        ++stats_.meshes;
    }
    stats_.quads += range.quads;
    ++stats_.uploads;
    stats_.quads_uploaded += range.quads;
    return Uploaded::written;
}

void ChunkUploads::empty(const ChunkMeshUpdate& update) {
    if (const std::optional<std::uint32_t> at = index_.find(update.pos)) {
        erase(*at);
    }
    emptied_.erase(update.pos);
    emptied_.insert(update.pos, update.generation);
}

void ChunkUploads::erase(std::uint32_t index) {
    const ChunkMeshRecord record = records_[index];
    space_->retire(record.range, slot_);
    stats_.quads -= record.range.quads;
    --stats_.meshes;
    index_.erase(record.pos);
    // The last record fills the hole, so the draw lists walk no gaps.
    if (const std::size_t last = records_.size() - 1; index != last) {
        records_[index] = records_[last];
        index_.erase(records_[index].pos);
        index_.insert(records_[index].pos, index);
    }
    records_.pop_back();
}

} // namespace vx::render
