#pragma once

#include <vx/core/frustum.hpp>
#include <vx/core/result.hpp>
#include <vx/render/arena_upkeep.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/device.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/upload.hpp>
#include <vx/world/coordinates.hpp>

#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <volk.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vx::render {

// A frame's uploads into the chunk arena (RFC 0007), which render() hands
// the app's ChunkChanges once the frame's slot is free, before it records.
class ChunkUploader {
public:
    ChunkUploader(ChunkUploads& uploads, QuadWriter& writer) noexcept
        : uploads_(&uploads), writer_(&writer) {}

    // Within the frame's budget, on the wall clock: later once it is spent,
    // full when no free range fits.
    [[nodiscard]] Result<Uploaded> upload(const ChunkMeshUpdate& update) {
        return uploads_->upload(update, *writer_, ChunkUploads::Clock::now());
    }
    // An edit's remeshes, all in this frame whatever the budget, or none.
    [[nodiscard]] Result<Uploaded> upload_batch(std::span<const ChunkMeshUpdate> batch) {
        return uploads_->upload_batch(batch, *writer_);
    }
    // The chunk unloaded: its mesh goes.
    void remove(ChunkPos pos) { uploads_->remove(pos); }

private:
    ChunkUploads* uploads_;
    QuadWriter* writer_;
};

// What a frame changes in the chunk arena (RFC 0007): the app hands over the
// world thread's meshes and unloads through it, in the order they came, and
// keeps whatever the budget leaves for the next frame.
class ChunkChanges {
public:
    ChunkChanges() = default;
    ChunkChanges(const ChunkChanges&) = delete;
    ChunkChanges& operator=(const ChunkChanges&) = delete;
    ChunkChanges(ChunkChanges&&) = delete;
    ChunkChanges& operator=(ChunkChanges&&) = delete;
    virtual ~ChunkChanges() = default;

    [[nodiscard]] virtual Result<void> apply(ChunkUploader& uploader) = 0;
};

// Where a frame's chunk passes read their commands (RFC 0007): the frame
// slot's buffer, the origins at its start, then each pass's commands; and
// the arena they draw from, which a repack replaces.
struct ChunkFrame {
    VkDeviceAddress quads = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress origins = 0;
    VkDeviceSize opaque_offset = 0;
    std::uint32_t opaque = 0; // commands
    VkDeviceSize cutout_offset = 0;
    std::uint32_t cutout = 0;
};

// RFC 0007's chunk rendering, owned by the renderer, main thread only: the
// arena, its uploads and upkeep, and each frame's draw lists, written with
// the chunks' origins into a host-visible buffer of the frame slot's, grown
// when a frame needs more.
class ChunkRenderer {
public:
    // The most quads an arena holds: a command's vertexOffset, four times its
    // first quad, is a signed 32-bit value.
    static constexpr std::uint32_t max_arena_quads = std::uint32_t{1} << 29U;

    // An arena of `capacity_quads` quads in `arena_memory`, an upload target
    // but in tests, which force staging with device-local memory alone. The
    // arena starts no larger than `arena_limit` quads and repacks grow it no
    // further: by default (0) a quarter of the device's memory budget.
    // `draw_call_limit` lowers the device's limit on commands a call, in
    // tests; 0 keeps it.
    [[nodiscard]] static Result<ChunkRenderer>
    create(const Device& device, std::uint32_t capacity_quads, std::uint32_t slots,
           BufferMemory arena_memory, std::uint32_t draw_call_limit = 0,
           std::uint32_t arena_limit = 0);

    // Frame slot `slot` is free and the frame is about to record: the ranges
    // retired in the slot return, the arenas and staging buffers its last
    // frame read go, and the upload budget starts over.
    void begin_frame(const Device& device, std::uint32_t slot);
    // RFC 0007's upkeep, before the frame's changes: compaction's moves, or a
    // repack into a larger arena, as the uploads the frame before refused
    // ask, recorded into `commands` with one barrier after them.
    [[nodiscard]] Result<void> keep_arena(const Device& device, VkCommandBuffer commands);
    // Compaction starts at the next frame and runs until it is stuck, with no
    // upload refused: for tests.
    void start_compaction() noexcept { upkeep_.start_compaction(); }
    // Makes the frame's changes: written in place where the arena is
    // host-visible, otherwise staged, with the copies recorded into
    // `commands` and one barrier after them.
    [[nodiscard]] Result<void> apply(ChunkChanges& changes, VkCommandBuffer commands);
    // Culls and builds the frame's draw lists for a camera at `camera_block`
    // plus `camera_fraction` that sees `view`, relative to it, and writes
    // them into the frame slot's buffer.
    [[nodiscard]] Result<ChunkFrame> prepare(const Device& device, const glm::i64vec3& camera_block,
                                             const glm::vec3& camera_fraction, const Frustum& view);

    // The overlay's switches (RFC 0007), from the next frame.
    void set_switches(const DrawSwitches& switches) noexcept { switches_ = switches; }
    [[nodiscard]] const DrawSwitches& switches() const noexcept { return switches_; }
    // Freeze culling, the overlay's last switch (RFC 0007): from the next
    // frame on, chunks and faces are culled as that frame's camera saw them
    // while the camera moves on, to look at what was culled. Off culls with
    // each frame's camera again.
    void set_frozen(bool frozen) noexcept {
        frozen_ = frozen;
        if (!frozen) {
            frozen_view_.reset();
        }
    }
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    // For uploads outside any frame, as RFC 0003's one chunk makes them.
    [[nodiscard]] ChunkUploads& uploads() noexcept { return uploads_; }
    [[nodiscard]] const ChunkUploads& uploads() const noexcept { return uploads_; }
    [[nodiscard]] const ChunkArena& arena() const noexcept { return *arena_; }
    // Compaction and repacks, for the overlay; whether the arena is full,
    // for RFC 0006.
    [[nodiscard]] const UpkeepStats& upkeep() const noexcept { return upkeep_.stats(); }
    // The most quads repacks grow the arena to: less after a repack the
    // device could not make, whose error the overlay shows.
    [[nodiscard]] std::uint32_t arena_limit() const noexcept { return upkeep_.max_capacity(); }
    [[nodiscard]] const std::string& repack_error() const noexcept { return repack_error_; }
    // The last frame's culling, for the overlay.
    [[nodiscard]] const DrawStats& draw_stats() const noexcept { return lists_.stats; }
    // The last frame's draw lists: its commands, for the overlay's counts.
    [[nodiscard]] const DrawLists& draw_lists() const noexcept { return lists_; }
    // The most commands one indirect call may draw: the device's
    // maxDrawIndirectCount.
    [[nodiscard]] std::uint32_t draw_limit() const noexcept { return draw_limit_; }
    // The frame slot's origins and commands as prepare() last wrote them.
    [[nodiscard]] std::span<const std::byte> frame_bytes() const noexcept {
        return frames_[slot_].mapping();
    }

private:
    ChunkRenderer(std::unique_ptr<ChunkArena> arena, BufferMemory arena_memory,
                  std::uint32_t arena_limit, std::uint32_t slots,
                  std::vector<UploadContext> staging, std::vector<Buffer> frames,
                  std::uint32_t draw_limit) noexcept;

    // A repack into a new arena of `capacity` quads: every mesh is copied
    // over, and the old arena goes once the frames that read it are done.
    [[nodiscard]] Result<void> repack(const Device& device, VkCommandBuffer commands,
                                      std::uint32_t capacity);
    // The GPU's copies of moves_, from `from` into `to`, and the barrier
    // after them.
    void record_moves(VkCommandBuffer commands, const Buffer& from, const Buffer& to);

    // On the heap, so the uploads' pointer to its space survives a move. A
    // repack moves a new arena into it.
    std::unique_ptr<ChunkArena> arena_;
    BufferMemory arena_memory_;
    ChunkUploads uploads_;
    ArenaUpkeep upkeep_;
    // What the frame before could not upload, for the upkeep.
    std::uint32_t wanted_ = 0;
    std::string repack_error_; // the last repack the device could not make
    // Arenas a repack replaced, by the frame slot that last read them.
    std::vector<std::vector<ChunkArena>> replaced_;
    // Scratch, kept for its capacity: the upkeep's moves and their copies.
    std::vector<ArenaMove> moves_;
    std::vector<VkBufferCopy2> regions_;
    // Each frame slot's staging, made afresh when the slot comes round.
    std::vector<UploadContext> staging_;
    std::vector<Buffer> frames_; // each frame slot's origins and commands
    // Where a frame culls from: the camera's block and fraction, and what it
    // sees relative to them.
    struct CullingView {
        glm::i64vec3 camera_block;
        glm::vec3 camera_fraction;
        Frustum frustum;
    };

    DrawLists lists_;
    DrawSwitches switches_;
    bool frozen_ = false;
    std::optional<CullingView> frozen_view_; // the first frozen frame's
    std::uint32_t slot_ = 0;
    std::uint32_t draw_limit_ = 0;
};

} // namespace vx::render
