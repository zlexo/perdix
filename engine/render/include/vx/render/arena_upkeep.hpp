#pragma once

#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>

#include <cstdint>

namespace vx::render {

// How RFC 0007 keeps the arena usable.
struct UpkeepSettings {
    // Meshes compaction moves in a frame: VMA finds each one's lowest free
    // range by walking every range of the block.
    std::uint32_t moves = 8;
    // Quads compaction moves in a frame: 8 MiB, though a frame's first move
    // goes whatever its size.
    std::uint32_t move_quads = std::uint32_t{1} << 20U;
    // The most quads a repack grows the arena to.
    std::uint32_t max_capacity = 0;
    // Frame slots: a range retired in a frame is free again this many
    // frames later.
    std::uint32_t slots = 2;
};

// What the upkeep does in a frame.
enum class UpkeepStep : std::uint8_t {
    none,
    compact, // move meshes down, within the frame's limits
    repack,  // into a new arena of repack_capacity() quads
};

// The upkeep's state, for the overlay (RFC 0007) and for RFC 0006, which
// answers a full arena by lowering the view distance.
struct UpkeepStats {
    bool compacting = false;
    // A mesh found no room, and the arena can neither make it nor grow.
    bool full = false;
    std::uint32_t moved = 0;       // quads compaction moved this frame
    std::uint64_t moved_total = 0; // quads compaction moved so far
    std::uint32_t repacks = 0;
    // Frames that ended with the arena full, so far: RFC 0006's signal. A
    // frame not drawn runs no upkeep and adds nothing, so a full flag left
    // from before a minimized window does not count again and again.
    std::uint64_t full_frames = 0;
};

// RFC 0007's answers to a full arena, decided once a frame, after the frame
// slot's ranges came back and before anything is uploaded:
//
// - An upload refused for want of room starts compaction when the meshes,
//   packed, would leave room for it. When they would not, the arena is
//   repacked into one 1.5 times larger, or larger still when the meshes and
//   the refused upload need more.
// - Compaction moves the highest meshes down while uploads are refused,
//   until the highest finds no lower range that fits it. Then it settles:
//   the ranges it moved meshes out of come back once every frame slot has
//   come round, and the refused uploads try again. A refusal still there
//   after that repacks.
// - Repacks stop at the maximum capacity. Then the arena is full, and says
//   so until a frame goes by without a refusal.
//
// RFC 0007 also started compaction when the fragmentation passed 0.5 with
// under a quarter of the arena free. Replayed 30-minute flights showed that
// moving meshes on every such frame, up to a millisecond of the main
// thread's time, without one refusal it prevented: VMA's allocator places
// streaming meshes well on its own. Compaction waits for a refusal instead.
class ArenaUpkeep {
public:
    explicit ArenaUpkeep(UpkeepSettings settings) noexcept
        : settings_(settings), max_capacity_(settings.max_capacity) {}

    // The frame's step, from the arena's space and what the frame before
    // could not upload: `wanted` quads, 0 when nothing was refused.
    [[nodiscard]] UpkeepStep step(const ArenaSpace& space, std::uint32_t wanted);
    // The size the repack step asks for, in quads.
    [[nodiscard]] std::uint32_t repack_capacity() const noexcept { return repack_capacity_; }
    // What the compaction step did.
    void compacted(const Compacted& done) noexcept;
    // Whether the repack step made the new arena. When it could not, the
    // arena stays at `current` quads and grows no further.
    void repacked(bool made, std::uint32_t current) noexcept;
    // Compaction starts at the next step and runs until it is stuck, refused
    // uploads or not: for tests.
    void start_compaction() noexcept {
        set_state(State::compacting);
        forced_ = true;
    }

    [[nodiscard]] const UpkeepSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const UpkeepStats& stats() const noexcept { return stats_; }
    // The most quads a repack grows the arena to now: the settings', or less
    // once the device could not make a repack's arena.
    [[nodiscard]] std::uint32_t max_capacity() const noexcept { return max_capacity_; }

private:
    enum class State : std::uint8_t { idle, compacting, settling };

    [[nodiscard]] UpkeepStep decide(const ArenaSpace& space, std::uint32_t wanted);
    [[nodiscard]] UpkeepStep grow(const ArenaSpace& space, std::uint32_t wanted);
    void settle() noexcept;
    void set_state(State state) noexcept;

    UpkeepSettings settings_;
    std::uint32_t max_capacity_;
    UpkeepStats stats_;
    State state_ = State::idle;
    std::uint32_t settle_frames_ = 0; // left until compaction's ranges are back
    bool forced_ = false;             // compacting without a refusal
    std::uint32_t repack_capacity_ = 0;
};

} // namespace vx::render
