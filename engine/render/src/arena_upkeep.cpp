#include <vx/render/arena_upkeep.hpp>

#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>

#include <algorithm>
#include <cstdint>

namespace vx::render {

UpkeepStep ArenaUpkeep::step(const ArenaSpace& space, std::uint32_t wanted) {
    const UpkeepStep step = decide(space, wanted);
    if (stats_.full) {
        ++stats_.full_frames;
    }
    return step;
}

UpkeepStep ArenaUpkeep::decide(const ArenaSpace& space, std::uint32_t wanted) {
    stats_.moved = 0;
    if (state_ == State::settling && settle_frames_ > 0) {
        --settle_frames_;
    }
    if (wanted == 0) {
        stats_.full = false;
        if ((state_ == State::compacting && !forced_) ||
            (state_ == State::settling && settle_frames_ == 0)) {
            set_state(State::idle); // the room is made
        }
        return state_ == State::compacting ? UpkeepStep::compact : UpkeepStep::none;
    }
    if (space.capacity() - space.live() < wanted) {
        return grow(space, wanted); // packed, the meshes would still leave no room
    }
    switch (state_) {
    case State::idle:
        set_state(State::compacting);
        return UpkeepStep::compact;
    case State::compacting:
        return UpkeepStep::compact;
    case State::settling:
        // Compaction went as far as it can: once its ranges are back, only a
        // larger arena makes room.
        return settle_frames_ > 0 ? UpkeepStep::none : grow(space, wanted);
    }
    return UpkeepStep::none;
}

UpkeepStep ArenaUpkeep::grow(const ArenaSpace& space, std::uint32_t wanted) {
    const std::uint64_t live = space.live();
    const std::uint64_t current = space.capacity();
    const std::uint64_t needed = (live + wanted) * 5 / 4;
    const std::uint64_t grown = std::max(current + ((current + 1) / 2), needed);
    const auto capacity = static_cast<std::uint32_t>(std::min(grown, std::uint64_t{max_capacity_}));
    // A larger arena still helps the meshes after this one, even when this
    // one does not fit it.
    stats_.full = capacity < live + wanted;
    if (capacity <= current) {
        stats_.full = true;
        // Tried again once every slot has come round, and compaction not
        // before a frame goes by without a refusal.
        settle();
        return UpkeepStep::none;
    }
    repack_capacity_ = capacity;
    return UpkeepStep::repack;
}

void ArenaUpkeep::compacted(const Compacted& done) noexcept {
    stats_.moved = done.quads;
    stats_.moved_total += done.quads;
    if (done.stuck) {
        settle();
    }
}

void ArenaUpkeep::repacked(bool made, std::uint32_t current) noexcept {
    if (made) {
        ++stats_.repacks;
        set_state(State::idle); // the new arena is packed
    } else {
        max_capacity_ = current;
        stats_.full = true;
        settle();
    }
}

void ArenaUpkeep::settle() noexcept {
    // The ranges retired in this frame come back when its slot comes round,
    // `slots` frames on, and the uploads refused meanwhile try again in that
    // frame: the step after it sees whether they went in.
    set_state(State::settling);
    settle_frames_ = settings_.slots + 1;
}

void ArenaUpkeep::set_state(State state) noexcept {
    state_ = state;
    stats_.compacting = state == State::compacting;
    forced_ = false;
}

} // namespace vx::render
