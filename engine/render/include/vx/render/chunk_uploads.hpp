#pragma once

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>

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
    // retires its old one; unless the budget was spent by `now`, or no free
    // range fits. An error is the writer's.
    [[nodiscard]] Result<Uploaded> upload(const ChunkMeshUpdate& update, QuadWriter& writer,
                                          Clock::time_point now);
    // An edit's remeshes (RFC 0005), all in this frame whatever the budget,
    // so no frame shows some new and some old; or none of them, `full`, when
    // a range does not fit for one. A writer's error may leave it partly
    // written.
    [[nodiscard]] Result<Uploaded> upload_batch(std::span<const ChunkMeshUpdate> batch,
                                                QuadWriter& writer);
    // The chunk unloaded: its mesh goes.
    void remove(ChunkPos pos);
    // Raised while a loading screen shows (RFC 0007).
    void set_budget(std::chrono::nanoseconds budget) noexcept { budget_ = budget; }

    [[nodiscard]] const ChunkMeshRecord* find(ChunkPos pos) const noexcept;
    [[nodiscard]] std::span<const ChunkMeshRecord> records() const noexcept { return records_; }
    [[nodiscard]] const UploadStats& stats() const noexcept { return stats_; }

private:
    // The newest generation known for `pos`: its mesh's, or its empty one's.
    [[nodiscard]] std::optional<std::uint32_t> newest(ChunkPos pos) const noexcept;
    [[nodiscard]] bool stale(const ChunkMeshUpdate& update) const noexcept;
    // Writes the mesh into `range`, which the update now holds.
    [[nodiscard]] Result<Uploaded> place(const ChunkMeshUpdate& update, const ArenaRange& range,
                                         QuadWriter& writer);
    void empty(const ChunkMeshUpdate& update);
    void erase(std::uint32_t index);

    ArenaSpace* space_;
    std::chrono::nanoseconds budget_ = default_budget;
    std::uint32_t slot_ = 0;
    Clock::time_point frame_start_;
    std::vector<ChunkMeshRecord> records_;
    ChunkMap index_;   // a chunk's record
    ChunkMap emptied_; // the generation of a chunk's empty mesh, until it unloads
    UploadStats stats_;
};

} // namespace vx::render
