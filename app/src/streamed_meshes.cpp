#include "streamed_meshes.hpp"

#include "world_loop.hpp"

#include <vx/core/result.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/camera.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/world/chunk_map.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/world_streamer.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace vx::app {

render::ChunkMeshUpdate chunk_update(const MeshUpdate& update) noexcept {
    ChunkMesh mesh{};
    // An empty mesh has no buffer, and no quads in any range.
    if (!update.quads.empty()) {
        // The worker copied the quads into the buffer's bytes, which made
        // Quads there (C++20's implicit object creation).
        const Quad* const quads =
            std::launder(reinterpret_cast<const Quad*>(update.quads.data())); // NOLINT
        std::size_t at = 0;
        for (std::size_t pass = 0; pass < 2; ++pass) {
            for (std::size_t face = 0; face < 6; ++face) {
                const std::uint32_t count = update.counts[(pass * 6) + face];
                mesh.ranges[pass][face] = std::span<const Quad>(quads + at, count);
                at += count;
            }
        }
    }
    return {.pos = update.pos, .generation = update.generation, .mesh = mesh};
}

Viewer camera_viewer(const render::Camera& camera, float aspect) {
    const Vec3d eye{.x = camera.position.x, .y = camera.position.y, .z = camera.position.z};
    const BlockPos block{.x = static_cast<std::int32_t>(std::floor(eye.x)),
                         .y = static_cast<std::int32_t>(std::floor(eye.y)),
                         .z = static_cast<std::int32_t>(std::floor(eye.z))};
    const glm::vec3 looking = render::forward(camera.yaw, camera.pitch);
    return {.chunk = chunk_of(block),
            .eye = eye,
            .forward = {.x = static_cast<double>(looking.x),
                        .y = static_cast<double>(looking.y),
                        .z = static_cast<double>(looking.z)},
            .view = render::view_frustum(camera, aspect)};
}

void StreamedMeshes::receive(std::vector<ToMain>& messages) {
    std::size_t kept = 0;
    for (std::size_t at = 0; at < messages.size(); ++at) {
        ToMain& message = messages[at];
        switch (message.kind) {
        case ToMain::Kind::mesh:
            meshes_.push_back({.order = next_order_, .update = std::move(message.mesh)});
            break;
        case ToMain::Kind::batch:
            batches_.push_back(std::move(message.batch));
            break;
        case ToMain::Kind::unload:
            take_unload(message.pos);
            break;
        case ToMain::Kind::player:
        case ToMain::Kind::landed:
        case ToMain::Kind::sample:
            if (kept != at) {
                messages[kept] = std::move(message);
            }
            ++kept;
            continue;
        }
        ++next_order_;
    }
    messages.erase(messages.begin() + static_cast<std::ptrdiff_t>(kept), messages.end());
}

void StreamedMeshes::take_unload(ChunkPos pos) {
    unloads_.push_back(pos);
    for (std::vector<MeshUpdate>& batch : batches_) {
        std::erase_if(batch, [pos](const MeshUpdate& update) { return update.pos == pos; });
    }
    std::erase_if(batches_, [](const std::vector<MeshUpdate>& batch) { return batch.empty(); });
    if (meshes_.empty()) {
        return; // none came before it
    }
    if (const std::optional<std::uint32_t> at = last_unloads_.find(pos)) {
        unload_orders_[*at] = next_order_;
    } else {
        last_unloads_.insert(pos, static_cast<std::uint32_t>(unload_orders_.size()));
        unload_orders_.push_back(next_order_);
    }
}

bool StreamedMeshes::in_waiting_batch(ChunkPos pos) const noexcept {
    return std::ranges::any_of(batches_, [pos](const std::vector<MeshUpdate>& batch) {
        return std::ranges::any_of(batch,
                                   [pos](const MeshUpdate& update) { return update.pos == pos; });
    });
}

bool StreamedMeshes::unloaded(const Mesh& mesh) const noexcept {
    const std::optional<std::uint32_t> at = last_unloads_.find(mesh.update.pos);
    return at.has_value() && mesh.order < unload_orders_[*at];
}

Result<void> StreamedMeshes::apply(render::ChunkUploader& uploader) {
    // The unloads first: the room they free is what a mesh that does not
    // fit waits for (RFC 0007).
    for (const ChunkPos pos : unloads_) {
        uploader.remove(pos);
    }
    unloads_.clear();
    // An edit's batches go ahead of the streaming meshes, each whole and
    // outside the budget (RFC 0005); one that does not fit waits, and the
    // edits after it with it.
    while (!batches_.empty()) {
        batch_.clear();
        for (const MeshUpdate& update : batches_.front()) {
            batch_.push_back(chunk_update(update));
        }
        const Result<render::Uploaded> uploaded = uploader.upload_batch(batch_);
        if (!uploaded.has_value()) {
            return uploaded.error();
        }
        if (uploaded.value() == render::Uploaded::full) {
            break;
        }
        batches_.pop_front();
    }
    // Then the streaming meshes in order, until the budget says later, one
    // does not fit, or one is of a chunk a waiting batch holds, which goes
    // after the edit so that no neighbor shows the edit without it. A mesh
    // its chunk's unload came after is dropped.
    while (!meshes_.empty()) {
        const Mesh& mesh = meshes_.front();
        if (!unloaded(mesh)) {
            if (in_waiting_batch(mesh.update.pos)) {
                return {};
            }
            const Result<render::Uploaded> uploaded = uploader.upload(chunk_update(mesh.update));
            if (!uploaded.has_value()) {
                return uploaded.error();
            }
            if (uploaded.value() == render::Uploaded::later ||
                uploaded.value() == render::Uploaded::full) {
                return {};
            }
        }
        ++done_; // written, removed or dropped
        // Its buffer goes back to the pool with it.
        meshes_.pop_front();
    }
    // No mesh waits that came before an unload.
    if (!last_unloads_.empty()) {
        last_unloads_.clear();
        unload_orders_.clear();
    }
    return {};
}

} // namespace vx::app
