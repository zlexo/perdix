#include "one_shot.hpp"

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/context.hpp>
#include <vx/render/upload.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

// RFC 0007's arena on a real driver: one buffer with a device address, and
// meshes uploaded into it in place or through staging.

namespace {

// Records a copy of all of `source` into `readback`, and the barrier that
// makes it visible to the host once the queue is idle.
void record_readback(VkCommandBuffer commands, const vx::render::Buffer& source,
                     const vx::render::Buffer& readback) {
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .pNext = nullptr,
        .srcOffset = 0,
        .dstOffset = 0,
        .size = source.size(),
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcBuffer = source.handle(),
        .dstBuffer = readback.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(commands, &copy);
    const VkMemoryBarrier2 to_host = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers = nullptr,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
}

} // namespace

TEST_CASE("the chunk arena is one buffer of quads with a device address") {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    vx::Result<vx::render::ChunkArena> arena =
        vx::render::ChunkArena::create(context.value().device(), 1'024, 3);
    INFO((arena.has_value() ? std::string() : arena.error().message));
    REQUIRE(arena.has_value());
    CHECK(arena.value().buffer().size() == 8 * 1'024);
    CHECK(arena.value().buffer().address() != 0);
    // Unified memory, as on Apple's GPUs and lavapipe: written in place.
    if (arena.value().buffer().mapping().empty()) {
        MESSAGE("the arena is not host-visible here: uploads stage into it");
    }
    const std::optional<vx::render::ArenaRange> range = arena.value().space().allocate(1'024);
    REQUIRE(range.has_value());
    CHECK_FALSE(arena.value().space().allocate(1).has_value());
    CHECK(context.value().instance().validation_counts().errors == 0);
    CHECK(context.value().instance().validation_counts().warnings == 0);
}

TEST_CASE("uploads write each mesh's groups into the arena's buffer, in place or staged") {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    const vx::render::Device& device = context.value().device();
    // Each quad names its mesh and its place in RFC 0003's order.
    std::vector<vx::Quad> quads;
    std::array<vx::ChunkMesh, 3> meshes{};
    for (std::uint32_t mesh = 0; mesh < meshes.size(); ++mesh) {
        for (std::uint32_t group = 0; group < 12; ++group) {
            for (std::uint32_t at = 0; at < 1 + ((mesh + group) % 4); ++at) {
                quads.push_back({.geometry = mesh, .material = (group << 16U) | at});
            }
        }
    }
    std::size_t next = 0;
    for (std::uint32_t mesh = 0; mesh < meshes.size(); ++mesh) {
        for (std::uint32_t group = 0; group < 12; ++group) {
            const std::size_t size = 1 + ((mesh + group) % 4);
            meshes[mesh].ranges[group / 6][group % 6] =
                std::span<const vx::Quad>(quads).subspan(next, size);
            next += size;
        }
    }
    for (const vx::render::BufferMemory memory :
         {vx::render::BufferMemory::upload_target, vx::render::BufferMemory::device_local}) {
        const bool staged = memory == vx::render::BufferMemory::device_local;
        INFO("staged: ", staged);
        vx::Result<vx::render::ChunkArena> arena =
            vx::render::ChunkArena::create(device, 1'024, 3, memory);
        REQUIRE(arena.has_value());
        vx::Result<vx::render::Buffer> readback = vx::render::Buffer::create(
            device, arena.value().buffer().size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            vx::render::BufferMemory::readback, "arena readback");
        REQUIRE(readback.has_value());
        vx::render::ChunkUploads uploads(arena.value().space());
        vx::render::UploadContext staging(device);
        vx::test::OneShot shot(device);
        vx::render::ArenaWriter writer(staging, shot.commands(), arena.value());
        uploads.begin_frame(0, vx::render::ChunkUploads::Clock::time_point{});
        for (std::int32_t mesh = 0; mesh < 3; ++mesh) {
            const vx::Result<vx::render::Uploaded> done =
                uploads.upload({.pos = {.x = mesh},
                                .generation = 1,
                                .mesh = meshes[static_cast<std::size_t>(mesh)]},
                               writer, vx::render::ChunkUploads::Clock::time_point{});
            INFO((done.has_value() ? std::string() : done.error().message));
            REQUIRE(done.has_value());
            CHECK(done.value() == vx::render::Uploaded::written);
        }
        staging.finish(shot.commands());
        record_readback(shot.commands(), arena.value().buffer(), readback.value());
        shot.submit_and_wait();
        vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0,
                                VK_WHOLE_SIZE);
        // Staged only where the arena has no mapping: here, where it was forced.
        CHECK((staging.staged_bytes() > 0) == arena.value().buffer().mapping().empty());
        // Each mesh's range holds its groups in the arena's order.
        for (std::int32_t mesh = 0; mesh < 3; ++mesh) {
            const vx::render::ChunkMeshRecord* record = uploads.find({.x = mesh});
            REQUIRE(record != nullptr);
            std::size_t at = record->range.first_quad;
            for (const vx::Pass pass : {vx::Pass::opaque, vx::Pass::cutout}) {
                for (const vx::Face face : vx::render::arena_faces) {
                    for (const vx::Quad expected :
                         meshes[static_cast<std::size_t>(mesh)].range(pass, face)) {
                        vx::Quad held;
                        std::memcpy(&held, readback.value().mapping().data() + (at * 8), 8);
                        CHECK(held == expected);
                        ++at;
                    }
                }
            }
        }
        staging.reset();
    }
    CHECK(context.value().instance().validation_counts().errors == 0);
    CHECK(context.value().instance().validation_counts().warnings == 0);
}
