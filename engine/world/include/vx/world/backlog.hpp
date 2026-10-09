#pragma once

#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vx {

// RFC 0006's score for a chunk waiting for a job: its distance from the
// camera's chunk in chunks, doubled when its box grown by a chunk is outside
// the view (so the neighbors a visible chunk needs rank as visible), and 0 for
// the 27 chunks around the camera. Squared, as a job key: lower runs first.
[[nodiscard]] std::uint32_t chunk_score(ChunkPos chunk, ChunkPos camera, Vec3d eye,
                                        const Frustum& view) noexcept;
// The same for a column's surface job, by its horizontal distance.
[[nodiscard]] std::uint32_t column_score(std::int32_t x, std::int32_t z, ChunkPos camera) noexcept;

// What waits for a job (RFC 0006), lowest key first: a binary heap of record
// indices, each with the load id it was queued with, so an entry whose
// record moved on is dropped when it comes up. rescore() keys every entry
// again in linear time, when the camera crosses a chunk border or turns.
class Backlog {
public:
    struct Entry {
        std::uint32_t key = 0;
        std::uint32_t index = 0;
        std::uint32_t load_id = 0;
    };

    void push(Entry entry) {
        entries_.push_back(entry);
        std::ranges::push_heap(entries_, later);
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    // The entry with the lowest key; ties go to the lower index, so the order
    // is the same on every run.
    [[nodiscard]] const Entry& top() const noexcept { return entries_.front(); }
    void pop() {
        std::ranges::pop_heap(entries_, later);
        entries_.pop_back();
    }
    // `key(entry)` gives each entry its new key, or none to drop it.
    template <class F> void rescore(F&& key) {
        std::erase_if(entries_, [&key](Entry& entry) {
            const std::optional<std::uint32_t> next = key(entry);
            if (!next) {
                return true;
            }
            entry.key = *next;
            return false;
        });
        std::ranges::make_heap(entries_, later);
    }

private:
    static bool later(const Entry& a, const Entry& b) noexcept {
        return a.key != b.key ? a.key > b.key : a.index > b.index;
    }

    std::vector<Entry> entries_;
};

} // namespace vx
