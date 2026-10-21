#include <vx/render/chunk_uploads.hpp>

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/upload.hpp>
#include <vx/world/coordinates.hpp>

#include <volk.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace vx::render {
namespace {

constexpr std::array<Pass, 2> passes = {Pass::opaque, Pass::cutout};

// The update's quads: its twelve groups, then RFC 0011's open border sets.
std::uint32_t total_quads(const ChunkMeshUpdate& update) noexcept {
    std::size_t total = 0;
    for (const auto& faces : update.mesh.ranges) {
        for (const std::span<const Quad> range : faces) {
            total += range.size();
        }
    }
    for (const auto& faces : update.open) {
        for (const std::span<const Quad> set : faces) {
            total += set.size();
        }
    }
    return static_cast<std::uint32_t>(total);
}

// RFC 0011's open border sets, written from `first` on in the arena's order,
// their counts and the mesh's border counts kept in that order.
Result<void> write_open_sets(const ChunkMeshUpdate& update, std::uint32_t& first,
                             std::array<std::uint16_t, 12>& border,
                             std::array<std::uint16_t, 12>& open, QuadWriter& writer) {
    std::size_t set = 0;
    for (const Pass pass : passes) {
        for (const Face face : arena_faces) {
            const auto from = (static_cast<std::size_t>(pass) * 6) + static_cast<std::size_t>(face);
            const std::span<const Quad> quads =
                update.open.at(static_cast<std::size_t>(pass)).at(static_cast<std::size_t>(face));
            assert(quads.size() <= 32'768 && update.border.at(from) <= 32'768);
            border.at(set) = static_cast<std::uint16_t>(update.border.at(from));
            open.at(set) = static_cast<std::uint16_t>(quads.size());
            ++set;
            if (quads.empty()) {
                continue;
            }
            if (const Result<void> written = writer.write(first, quads); !written.has_value()) {
                return written.error();
            }
            first += static_cast<std::uint32_t>(quads.size());
        }
    }
    return {};
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
    stats_.wanted = 0;
}

Result<Uploaded> ChunkUploads::upload(const ChunkMeshUpdate& update, QuadWriter& writer,
                                      Clock::time_point now) {
    if (stale(update)) {
        ++stats_.dropped;
        return Uploaded::dropped;
    }
    // An empty mesh writes nothing: it costs none of the budget.
    const std::uint32_t quads = total_quads(update);
    if (quads == 0) {
        empty(update);
        return Uploaded::removed;
    }
    // The frame's first upload always goes, so a frame that spent its time
    // elsewhere still makes progress.
    if (stats_.uploads > 0 && now - frame_start_ >= budget_) {
        ++stats_.refused;
        return Uploaded::later;
    }
    const std::optional<ArenaRange> range = space_->allocate(quads);
    if (!range) {
        ++stats_.refused;
        stats_.wanted = std::max(stats_.wanted, quads);
        return Uploaded::full;
    }
    return place(update, *range, writer, frame_start_);
}

Result<Uploaded> ChunkUploads::upload_batch(std::span<const ChunkMeshUpdate> batch,
                                            QuadWriter& writer,
                                            std::optional<Clock::time_point> appearing) {
    // Every range first, the largest first, so a small mesh never takes the
    // one hole a large one needs: when one does not fit, nothing is
    // written, and the ranges taken go back with this frame's slot.
    std::vector<std::optional<ArenaRange>> ranges(batch.size());
    std::vector<std::uint32_t> quads(batch.size());
    std::vector<std::size_t> order(batch.size());
    for (std::size_t at = 0; at < batch.size(); ++at) {
        quads[at] = total_quads(batch[at]);
        order[at] = at;
    }
    std::ranges::stable_sort(order, std::ranges::greater{},
                             [&quads](std::size_t at) { return quads[at]; });
    // Ranges taken and never written go back at once: no frame reads them,
    // and held for a frame slot they would hide room the upkeep looks for.
    const auto give_back = [&](std::size_t from) {
        for (std::size_t at = from; at < ranges.size(); ++at) {
            if (ranges[at]) {
                space_->cancel(*ranges[at]);
            }
        }
    };
    for (const std::size_t at : order) {
        if (quads[at] == 0 || stale(batch[at])) {
            continue;
        }
        ranges[at] = space_->allocate(quads[at]);
        if (!ranges[at]) {
            give_back(0);
            ++stats_.refused;
            // The batch goes in whole, so it wants room for all of it.
            stats_.wanted = std::max(stats_.wanted, fresh_quads(batch, quads));
            return Uploaded::full;
        }
    }
    for (std::size_t at = 0; at < batch.size(); ++at) {
        const std::optional<ArenaRange>& range = ranges[at];
        if (stale(batch[at])) {
            // A newer update of its chunk came earlier in the batch: the
            // range taken for this one goes back, never written.
            if (range) {
                space_->cancel(*range);
            }
            ++stats_.dropped;
        } else if (!range) {
            empty(batch[at]);
        } else if (const Result<Uploaded> placed =
                       place(batch[at], *range, writer, appearing.value_or(frame_start_));
                   !placed.has_value()) {
            give_back(at + 1);
            return placed.error();
        }
    }
    return Uploaded::written;
}

Compacted ChunkUploads::compact(std::uint32_t max_meshes, std::uint32_t max_quads,
                                std::vector<ArenaMove>& moves) {
    // Only the highest meshes can move this frame: a partial sort finds them.
    gather_offsets();
    const std::size_t candidates = std::min<std::size_t>(max_meshes, by_offset_.size());
    const auto end = by_offset_.begin() + static_cast<std::ptrdiff_t>(candidates);
    std::ranges::partial_sort(by_offset_, end, std::ranges::greater{});
    // With no mesh it may move, compaction can go no further.
    Compacted done{.meshes = 0, .quads = 0, .stuck = candidates == 0};
    for (auto at = by_offset_.begin(); at != end; ++at) {
        ChunkMeshRecord& record = records_[at->record];
        // The frame's share is moved; its first move always goes, so a mesh
        // larger than the share still moves.
        if (done.meshes > 0 && done.quads + record.range.quads > max_quads) {
            break;
        }
        const std::optional<ArenaRange> lower = space_->allocate_lowest(record.range.quads);
        if (!lower || lower->first_quad > record.range.first_quad) {
            if (lower) {
                space_->cancel(*lower);
            }
            done.stuck = true;
            break;
        }
        moves.push_back(
            {.from = record.range.first_quad, .to = lower->first_quad, .quads = lower->quads});
        // Frames in flight may still draw the old range.
        space_->retire(record.range, slot_);
        record.range = *lower;
        ++done.meshes;
        done.quads += lower->quads;
    }
    return done;
}

bool ChunkUploads::repack(ArenaSpace& fresh, std::vector<ArenaMove>& moves) {
    gather_offsets();
    std::ranges::sort(by_offset_);
    placed_.assign(records_.size(), ArenaRange{});
    const std::size_t first_move = moves.size();
    for (const ByOffset& at : by_offset_) {
        const std::uint32_t quads = records_[at.record].range.quads;
        // Nothing was ever freed in the fresh space, so each range comes
        // straight after the one before.
        const std::optional<ArenaRange> range = fresh.allocate(quads);
        if (!range) {
            for (const ArenaRange& placed : placed_) {
                if (placed.allocation != VK_NULL_HANDLE) {
                    fresh.cancel(placed);
                }
            }
            moves.resize(first_move);
            return false;
        }
        placed_[at.record] = *range;
        moves.push_back({.from = at.first_quad, .to = range->first_quad, .quads = quads});
    }
    for (std::size_t at = 0; at < records_.size(); ++at) {
        records_[at].range = placed_[at];
    }
    return true;
}

void ChunkUploads::gather_offsets() {
    by_offset_.clear();
    for (std::uint32_t at = 0; at < records_.size(); ++at) {
        by_offset_.push_back({.first_quad = records_[at].range.first_quad, .record = at});
    }
}

std::uint32_t ChunkUploads::fresh_quads(std::span<const ChunkMeshUpdate> batch,
                                        std::span<const std::uint32_t> quads) const noexcept {
    std::uint32_t total = 0;
    for (std::size_t at = 0; at < batch.size(); ++at) {
        if (!stale(batch[at])) {
            total += quads[at];
        }
    }
    return total;
}

void ChunkUploads::remove(ChunkPos pos, std::uint8_t level) {
    if (const std::optional<std::uint32_t> at = index_[level].find(pos)) {
        erase(*at);
    }
    emptied_[level].erase(pos);
}

const ChunkMeshRecord* ChunkUploads::find(ChunkPos pos, std::uint8_t level) const noexcept {
    const std::optional<std::uint32_t> at = index_[level].find(pos);
    return at ? &records_[*at] : nullptr;
}

std::optional<std::uint32_t> ChunkUploads::newest(ChunkPos pos, std::uint8_t level) const noexcept {
    if (const std::optional<std::uint32_t> at = index_[level].find(pos)) {
        return records_[*at].generation;
    }
    return emptied_[level].find(pos);
}

bool ChunkUploads::stale(const ChunkMeshUpdate& update) const noexcept {
    const std::optional<std::uint32_t> known = newest(update.pos, update.level);
    return known.has_value() && update.generation <= *known;
}

Result<Uploaded> ChunkUploads::place(const ChunkMeshUpdate& update, const ArenaRange& range,
                                     QuadWriter& writer, Clock::time_point appearing) {
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
    // RFC 0011's open border sets, after the twelve groups.
    std::array<std::uint16_t, 12> border{};
    std::array<std::uint16_t, 12> open{};
    if (const Result<void> written = write_open_sets(update, first, border, open, writer);
        !written.has_value()) {
        space_->retire(range, slot_);
        return written.error();
    }
    if (const std::optional<std::uint32_t> at = index_[update.level].find(update.pos)) {
        ChunkMeshRecord& record = records_[*at];
        space_->retire(record.range, slot_);
        stats_.quads -= record.range.quads;
        record.generation = update.generation;
        record.range = range;
        record.count = count;
        record.border = border;
        record.open = open;
    } else {
        // A new record shown at once appeared now: it fades in (RFC 0013).
        // Not a chunk meshed empty since it loaded, as one of air is: its
        // first mesh with faces, after an edit, is a remesh, and a remesh
        // does not fade again (26.2).
        const bool shown = !swaps_ || shown_[update.level].find(update.pos).has_value();
        const bool remesh = emptied_[update.level].find(update.pos).has_value();
        const std::optional<Clock::time_point> appeared =
            shown && !remesh ? std::optional(appearing) : std::nullopt;
        index_[update.level].insert(update.pos, static_cast<std::uint32_t>(records_.size()));
        records_.push_back({.pos = update.pos,
                            .generation = update.generation,
                            .range = range,
                            .count = count,
                            .level = update.level,
                            .shown = shown,
                            .border = border,
                            .open = open,
                            .appeared = appeared});
        emptied_[update.level].erase(update.pos);
        ++stats_.meshes;
    }
    stats_.quads += range.quads;
    ++stats_.uploads;
    stats_.quads_uploaded += range.quads;
    return Uploaded::written;
}

void ChunkUploads::set_shown(ChunkPos pos, std::uint8_t level, bool shown, bool fade) {
    if (shown) {
        shown_[level].insert(pos, 0);
    } else {
        shown_[level].erase(pos);
    }
    if (const std::optional<std::uint32_t> at = index_[level].find(pos)) {
        records_[*at].shown = shown;
        records_[*at].appeared = shown && fade ? std::optional(frame_start_) : std::nullopt;
    }
}

void ChunkUploads::empty(const ChunkMeshUpdate& update) {
    if (const std::optional<std::uint32_t> at = index_[update.level].find(update.pos)) {
        erase(*at);
    }
    emptied_[update.level].erase(update.pos);
    emptied_[update.level].insert(update.pos, update.generation);
}

void ChunkUploads::erase(std::uint32_t index) {
    const ChunkMeshRecord record = records_[index];
    space_->retire(record.range, slot_);
    stats_.quads -= record.range.quads;
    --stats_.meshes;
    index_[record.level].erase(record.pos);
    // The last record fills the hole, so the draw lists walk no gaps.
    if (const std::size_t last = records_.size() - 1; index != last) {
        records_[index] = records_[last];
        index_[records_[index].level].erase(records_[index].pos);
        index_[records_[index].level].insert(records_[index].pos, index);
    }
    records_.pop_back();
}

} // namespace vx::render
