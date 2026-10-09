#pragma once

#include "world_loop.hpp"

#include <vx/core/result.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/world_streamer.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace vx::app {

// A mesh from the world thread as RFC 0007's uploads take it: its twelve
// ranges over the quads in its pool buffer, which must outlive the result.
[[nodiscard]] render::ChunkMeshUpdate chunk_update(const MeshUpdate& update) noexcept;

// Where the camera is and looks, for what streams in first (RFC 0006): its
// chunk, eye and heading, and its view `aspect` wide.
[[nodiscard]] Viewer camera_viewer(const render::Camera& camera, float aspect);

// The world thread's meshes and unloads on the main thread (RFC 0005),
// handed to RFC 0007's uploads frame by frame. Each frame the unloads go
// first, then an edit's batches, each whole and outside the budget, then
// the streaming meshes in the order they came, until the budget says
// later, one does not fit, or one is of a chunk a batch waiting for room
// holds. A chunk's meshes that came before its unload are
// dropped, so a chunk's own order holds, and a mesh that waits for room in
// the arena never holds back the unloads that make it. What waits is the
// overlay's backlog; what is done with the meshes sent alone tells the
// world thread how far behind the main thread is.
class StreamedMeshes final : public render::ChunkChanges {
public:
    // Takes the meshes, batches and unloads among `messages`, in order; the
    // rest stay for the caller, in order.
    void receive(std::vector<ToMain>& messages);

    [[nodiscard]] Result<void> apply(render::ChunkUploader& uploader) override;

    // Of the meshes sent alone, those uploaded or dropped so far: what
    // ToWorld::uploaded reports.
    [[nodiscard]] std::uint64_t done() const noexcept { return done_; }
    // Meshes, batches and unloads waiting for the next frames.
    [[nodiscard]] std::size_t waiting() const noexcept {
        return meshes_.size() + batches_.size() + unloads_.size();
    }

private:
    struct Mesh {
        std::uint64_t order = 0; // when it came, among the stream's messages
        MeshUpdate update;
    };

    // The chunk's place in the batches waiting goes, and the meshes of it
    // waiting are dropped when they come up: they came before it.
    void take_unload(ChunkPos pos);
    // Whether `mesh` came before its chunk's last unload.
    [[nodiscard]] bool unloaded(const Mesh& mesh) const noexcept;
    // Whether an edit's batch waiting for room holds the chunk at `pos`.
    [[nodiscard]] bool in_waiting_batch(ChunkPos pos) const noexcept;

    std::deque<Mesh> meshes_;
    std::deque<std::vector<MeshUpdate>> batches_;
    std::vector<ChunkPos> unloads_; // since the last frame
    // Each chunk's last unload, while a mesh that came before it may wait:
    // the chunk's index into unload_orders_, the unload's order there.
    ChunkMap last_unloads_;
    std::vector<std::uint64_t> unload_orders_;
    std::uint64_t next_order_ = 0;
    std::vector<render::ChunkMeshUpdate> batch_; // reused for each batch
    std::uint64_t done_ = 0;
};

} // namespace vx::app
