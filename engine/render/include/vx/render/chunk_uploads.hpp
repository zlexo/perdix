#pragma once

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/upload.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/lod.hpp>

#include <volk.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vx::render {

// A finished mesh on its way into the arena (RFC 0007).
struct ChunkMeshUpdate {
    ChunkPos pos;
    // Rises with every remesh of the chunk: an update no newer than the mesh
    // the arena holds is dropped.
    std::uint32_t generation = 0;
    // RFC 0003's twelve ranges, all empty for a chunk with nothing to draw.
    ChunkMesh mesh;
    // RFC 0011's level: `pos` is on that level's grid, and the mesh's quads
    // are in cells 2^level blocks wide.
    std::uint8_t level = 0;
    // RFC 0011's open border sets, by pass then face (RFC 0003's order, as
    // the mesh's ranges): how many of each range lie in the mesh's outermost
    // slice that way, and that slice meshed as if nothing were drawn across.
    std::array<std::uint32_t, 12> border{};
    std::array<std::array<std::span<const Quad>, 6>, 2> open{};
};

// The order of a pass's six face groups in the arena: +x, +y, +z, -x, -y,
// -z. A camera outside a chunk on every axis faces three of them, which in
// this order form 1.75 runs on average over the eight octants, against 2.5
// in RFC 0003's order (RFC 0007).
inline constexpr std::array<Face, 6> arena_faces = {Face::pos_x, Face::pos_y, Face::pos_z,
                                                    Face::neg_x, Face::neg_y, Face::neg_z};

// One chunk's mesh in the arena: its twelve face groups back to back, the
// opaque ones in arena_faces' order, then the cutout ones in the same order.
// The draw lists are built from these every frame, so moving a mesh changes
// only its range.
struct ChunkMeshRecord {
    ChunkPos pos;
    std::uint32_t generation = 0;
    ArenaRange range;
    // Quads in each group, in arena order. A group holds at most 32,768: one
    // direction of a checkerboard of two cutout blocks that do not hide each
    // other.
    std::array<std::uint16_t, 12> count{};
    std::uint8_t level = 0; // RFC 0011's, as the update's
    // Drawn: always, unless the uploads take RFC 0011's swaps, then while a
    // swap shows the chunk.
    bool shown = true;
    // RFC 0011's open border sets, in arena order, by pass: each group's
    // quads in the mesh's outermost slice that way, at the end of a positive
    // face's group and the start of a negative one's; and each open set's,
    // back to back after the twelve groups.
    std::array<std::uint16_t, 12> border{};
    std::array<std::uint16_t, 12> open{};
    // RFC 0013's fade: the frame it appeared where nothing was drawn, from
    // which it fades in from the fog color; none for a chunk shown at once,
    // as a level swap shows one, or for one meshed empty before.
    std::optional<std::chrono::steady_clock::time_point> appeared = std::nullopt;
};

// Where uploads put quads: the arena's buffer, or a stand-in in tests.
class QuadWriter {
public:
    QuadWriter() = default;
    QuadWriter(const QuadWriter&) = delete;
    QuadWriter& operator=(const QuadWriter&) = delete;
    QuadWriter(QuadWriter&&) = delete;
    QuadWriter& operator=(QuadWriter&&) = delete;
    virtual ~QuadWriter() = default;

    // Writes `quads` into the arena from quad `first` on.
    [[nodiscard]] virtual Result<void> write(std::uint32_t first, std::span<const Quad> quads) = 0;
};

// Writes quads into the arena's buffer through RFC 0001's upload: in place
// where the arena is host-visible, as on unified memory; elsewhere into a
// staging buffer, with a copy recorded into `commands` that the context's
// finish() makes visible before the chunk passes.
class ArenaWriter final : public QuadWriter {
public:
    ArenaWriter(UploadContext& context, VkCommandBuffer commands, const ChunkArena& arena) noexcept
        : context_(&context), commands_(commands), arena_(&arena) {}

    [[nodiscard]] Result<void> write(std::uint32_t first, std::span<const Quad> quads) override;

private:
    UploadContext* context_;
    VkCommandBuffer commands_;
    const ChunkArena* arena_;
};

// What an upload did with an update.
enum class Uploaded : std::uint8_t {
    written, // its mesh is in the arena
    removed, // it was empty: the chunk has no mesh now
    dropped, // no newer than the mesh the arena holds: nothing to do
    later,   // this frame's budget is spent: offer it again next frame
    full,    // no free range fits it: offer it again later (RFC 0007's compaction)
};

// Counts for the overlay (RFC 0007).
struct UploadStats {
    std::uint32_t meshes = 0; // in the arena
    std::uint64_t quads = 0;  // in the arena
    // This frame's:
    std::uint32_t uploads = 0;
    std::uint64_t quads_uploaded = 0;
    std::uint32_t dropped = 0;
    std::uint32_t refused = 0; // later, or full
    // The room the largest upload refused as full needed, in quads: one
    // mesh's, or an edit's whole batch's. What RFC 0007's compaction or
    // repack must make.
    std::uint32_t wanted = 0;
};

// A mesh moved by compaction, or into a new arena by a repack (RFC 0007):
// its quads go from `from` to `to`.
struct ArenaMove {
    std::uint32_t from = 0;
    std::uint32_t to = 0;
    std::uint32_t quads = 0;
};

// What a frame's compaction did (RFC 0007).
struct Compacted {
    std::uint32_t meshes = 0;
    std::uint32_t quads = 0;
    // The highest mesh left found no lower range that fits it: compaction
    // can go no further.
    bool stuck = false;
};

// RFC 0007's uploads, on the main thread, which alone owns the arena: each
// chunk's newest mesh in a range of its own, the old range retired until
// its frame slot comes round again. Uploads spend a time budget each frame,
// 1 ms by default, counted from begin_frame on the wall clock: the first
// write to a page costs several times a later one, so bytes would be the
// wrong unit while the arena is fresh. An edit's batch goes in whole,
// outside the budget.
//
// The caller holds the updates that wait, in the order they came (RFC 0005
// sends a chunk's meshes oldest first): an update offered again after a
// newer one went in is dropped, and so is one older than an empty mesh that
// removed the chunk's. After remove(), the chunk's waiting updates are the
// caller's to drop.
class ChunkUploads {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds default_budget{1};

    explicit ChunkUploads(ArenaSpace& space) noexcept : space_(&space) {}
    // One owner: a copy would retire its ranges a second time.
    ChunkUploads(const ChunkUploads&) = delete;
    ChunkUploads& operator=(const ChunkUploads&) = delete;
    ChunkUploads(ChunkUploads&&) = default;
    ChunkUploads& operator=(ChunkUploads&&) = default;
    ~ChunkUploads() = default;

    // A frame begins in frame slot `slot`, whose last frame the GPU has
    // finished: the ranges retired in it are free again, and the budget
    // starts over at `now`.
    void begin_frame(std::uint32_t slot, Clock::time_point now) noexcept;
    // Writes the chunk's mesh through `writer` into a new range, and
    // retires its old one; unless the budget was spent by `now` (never for
    // the frame's first upload), or no free range fits. An error is the
    // writer's.
    [[nodiscard]] Result<Uploaded> upload(const ChunkMeshUpdate& update, QuadWriter& writer,
                                          Clock::time_point now);
    // An edit's remeshes (RFC 0005), all in this frame whatever the budget,
    // so no frame shows some new and some old; or none of them, `full`, when
    // a range does not fit for one. A writer's error may leave it partly
    // written. A chunk shown for the first time appears at `appearing`
    // (RFC 0013), or when the frame began: a batch set outside any frame
    // gives its own time.
    [[nodiscard]] Result<Uploaded>
    upload_batch(std::span<const ChunkMeshUpdate> batch, QuadWriter& writer,
                 std::optional<Clock::time_point> appearing = std::nullopt);
    // The chunk at `pos` on `level`'s grid unloaded: its mesh goes.
    void remove(ChunkPos pos, std::uint8_t level = 0);
    // RFC 0007's compaction: the highest meshes move down, each into the
    // lowest free range that fits it while that lies lower, up to
    // `max_meshes` meshes and `max_quads` quads, though the first move goes
    // whatever its size; each old range is retired with the frame. `moves`
    // gets each move, which the GPU must copy before anything draws this
    // frame.
    [[nodiscard]] Compacted compact(std::uint32_t max_meshes, std::uint32_t max_quads,
                                    std::vector<ArenaMove>& moves);
    // RFC 0007's repack: every mesh into `fresh`, a new arena's space, in
    // the order the meshes lie in this one, so they pack from its start.
    // `moves` gets each mesh's old and new place, for the copy into the new
    // buffer. The ranges in the old space stay: it goes whole, with its
    // buffer, once the frames that read it are done, and `fresh` takes its
    // place before anything else is asked of the uploads. False, with
    // nothing changed, when the meshes do not fit.
    [[nodiscard]] bool repack(ArenaSpace& fresh, std::vector<ArenaMove>& moves);
    // Raised while a loading screen shows (RFC 0007).
    void set_budget(std::chrono::nanoseconds budget) noexcept { budget_ = budget; }
    // RFC 0011's swaps: from now on a chunk's mesh is drawn only while a
    // swap shows the chunk, whose levels' meshes all stream in.
    void use_swaps() noexcept { swaps_ = true; }
    // A swap shows or hides the chunk at `pos` on `level`: its mesh now, and
    // every mesh it gets while shown, through an empty one and an unload.
    // Shown with `fade`, it fades in from this frame (RFC 0013), as a chunk
    // that appears where nothing was drawn; without, at once.
    void set_shown(ChunkPos pos, std::uint8_t level, bool shown, bool fade = false);
    // When this frame began.
    [[nodiscard]] Clock::time_point frame_start() const noexcept { return frame_start_; }

    [[nodiscard]] const ChunkMeshRecord* find(ChunkPos pos, std::uint8_t level = 0) const noexcept;
    [[nodiscard]] std::span<const ChunkMeshRecord> records() const noexcept { return records_; }
    [[nodiscard]] const UploadStats& stats() const noexcept { return stats_; }

private:
    // The newest generation known for `pos` on `level`: its mesh's, or its
    // empty one's.
    [[nodiscard]] std::optional<std::uint32_t> newest(ChunkPos pos,
                                                      std::uint8_t level) const noexcept;
    [[nodiscard]] bool stale(const ChunkMeshUpdate& update) const noexcept;
    // The quads of the batch's updates that are not stale; `quads` holds each
    // update's.
    [[nodiscard]] std::uint32_t fresh_quads(std::span<const ChunkMeshUpdate> batch,
                                            std::span<const std::uint32_t> quads) const noexcept;
    // Writes the mesh into `range`, which the update now holds; a chunk
    // shown for the first time appears at `appearing`.
    [[nodiscard]] Result<Uploaded> place(const ChunkMeshUpdate& update, const ArenaRange& range,
                                         QuadWriter& writer, Clock::time_point appearing);
    void empty(const ChunkMeshUpdate& update);
    void erase(std::uint32_t index);
    // Where each record's range starts, unsorted, into by_offset_.
    void gather_offsets();

    ArenaSpace* space_;
    std::chrono::nanoseconds budget_ = default_budget;
    std::uint32_t slot_ = 0;
    Clock::time_point frame_start_;
    std::vector<ChunkMeshRecord> records_;
    // By RFC 0011's level: a chunk's record, and the generation of a chunk's
    // empty mesh until it unloads.
    std::array<ChunkMap, lod_levels> index_;
    std::array<ChunkMap, lod_levels> emptied_;
    // With RFC 0011's swaps, the chunks shown, by level: a set.
    bool swaps_ = false;
    std::array<ChunkMap, lod_levels> shown_;
    UploadStats stats_;
    // Scratch, kept for its capacity: records by first quad, and a repack's
    // new ranges by record.
    struct ByOffset {
        std::uint32_t first_quad = 0;
        std::uint32_t record = 0;

        friend constexpr auto operator<=>(const ByOffset&, const ByOffset&) = default;
    };
    std::vector<ByOffset> by_offset_;
    std::vector<ArenaRange> placed_;
};

} // namespace vx::render
