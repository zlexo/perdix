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
#include <vx/world/lod.hpp>
#include <vx/world/world_streamer.hpp>

#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <new>
#include <optional>
#include <span>
#include <tuple>
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
    return {
        .pos = update.pos, .generation = update.generation, .mesh = mesh, .level = update.level};
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
    take(messages, {});
}

void StreamedMeshes::receive(std::vector<ToMain>& messages,
                             const std::function<void(const WorldSample&)>& sampled) {
    take(messages, sampled);
}

void StreamedMeshes::take(std::vector<ToMain>& messages,
                          const std::function<void(const WorldSample&)>& sampled) {
    std::size_t kept = 0;
    for (std::size_t at = 0; at < messages.size(); ++at) {
        ToMain& message = messages[at];
        if (message.kind == ToMain::Kind::sample && sampled) {
            sampled(message.sample);
            continue;
        }
        switch (message.kind) {
        case ToMain::Kind::mesh:
            meshes_.push_back({.order = next_order_, .update = std::move(message.mesh)});
            break;
        case ToMain::Kind::batch:
            batches_.push_back(std::move(message.batch));
            break;
        case ToMain::Kind::unload:
            take_unload({.pos = message.pos, .level = message.level});
            break;
        case ToMain::Kind::player:
        case ToMain::Kind::perdix:
        case ToMain::Kind::landed:
        case ToMain::Kind::sample:
        case ToMain::Kind::report:
        case ToMain::Kind::swap: // RFC 0011's, for the caller until levels are drawn
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

void StreamedMeshes::take_unload(Unload unload) {
    unloads_.push_back(unload);
    for (std::vector<MeshUpdate>& batch : batches_) {
        std::erase_if(batch, [unload](const MeshUpdate& update) {
            return update.pos == unload.pos && update.level == unload.level;
        });
    }
    std::erase_if(batches_, [](const std::vector<MeshUpdate>& batch) { return batch.empty(); });
    if (meshes_.empty()) {
        return; // none came before it
    }
    ChunkMap& last = last_unloads_.at(unload.level);
    if (const std::optional<std::uint32_t> at = last.find(unload.pos)) {
        unload_orders_[*at] = next_order_;
    } else {
        last.insert(unload.pos, static_cast<std::uint32_t>(unload_orders_.size()));
        unload_orders_.push_back(next_order_);
    }
}

bool StreamedMeshes::in_waiting_batch(ChunkPos pos, std::uint8_t level) const noexcept {
    return std::ranges::any_of(batches_, [pos, level](const std::vector<MeshUpdate>& batch) {
        return std::ranges::any_of(batch, [pos, level](const MeshUpdate& update) {
            return update.pos == pos && update.level == level;
        });
    });
}

Holes StreamedMeshes::holes(const render::ChunkUploads& uploads, const Viewer& viewer,
                            double near) const {
    // Each chunk's meshes waiting, the newest last: the uploads keep the
    // newest, and an empty one leaves nothing to draw.
    struct Waiting {
        ChunkPos pos;
        std::uint8_t level = 0;
        std::uint32_t generation = 0;
        bool quads = false;
    };
    std::vector<Waiting> waiting;
    const auto add = [&waiting](const MeshUpdate& update) {
        waiting.push_back({.pos = update.pos,
                           .level = update.level,
                           .generation = update.generation,
                           .quads = !update.quads.empty()});
    };
    for (const Mesh& mesh : meshes_) {
        if (!unloaded(mesh)) {
            add(mesh.update);
        }
    }
    for (const std::vector<MeshUpdate>& batch : batches_) {
        std::ranges::for_each(batch, add);
    }
    std::ranges::sort(waiting, {}, [](const Waiting& mesh) {
        return std::tuple(mesh.level, mesh.pos.x, mesh.pos.y, mesh.pos.z, mesh.generation);
    });
    Holes holes;
    for (std::size_t at = 0; at < waiting.size(); ++at) {
        const Waiting& newest = waiting[at];
        const bool newer_waits = at + 1 < waiting.size() && waiting[at + 1].pos == newest.pos &&
                                 waiting[at + 1].level == newest.level;
        if (newer_waits || !newest.quads || uploads.find(newest.pos, newest.level) != nullptr) {
            continue;
        }
        const ChunkSight sight = chunk_sight(viewer, newest.pos, near, newest.level);
        if (sight.in_view) {
            ++holes.all;
        }
        if (sight.near) {
            ++holes.near;
        }
    }
    return holes;
}

bool StreamedMeshes::unloaded(const Mesh& mesh) const noexcept {
    const std::optional<std::uint32_t> at =
        last_unloads_.at(mesh.update.level).find(mesh.update.pos);
    return at.has_value() && mesh.order < unload_orders_[*at];
}

Result<void> StreamedMeshes::apply(render::ChunkUploader& uploader) {
    // The unloads first: the room they free is what a mesh that does not
    // fit waits for (RFC 0007).
    for (const Unload& unload : unloads_) {
        uploader.remove(unload.pos, unload.level);
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
            if (in_waiting_batch(mesh.update.pos, mesh.update.level)) {
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
    forget_unloads();
    return {};
}

void StreamedMeshes::forget_unloads() noexcept {
    if (unload_orders_.empty()) {
        return;
    }
    for (ChunkMap& last : last_unloads_) {
        if (!last.empty()) {
            last.clear();
        }
    }
    unload_orders_.clear();
}

} // namespace vx::app
