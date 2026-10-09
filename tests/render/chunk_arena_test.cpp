#include "one_shot.hpp"

#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/chunk_arena.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/context.hpp>
#include <vx/render/upload.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// RFC 0007's arena on a real driver: one buffer with a device address,
// meshes uploaded into it in place or through staging, moved down it by
// compaction and into larger ones by repacks.

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

// A chunk's mesh: all its quads in one group, each naming the chunk, its
// generation and its place.
struct Mesh {
    std::vector<vx::Quad> quads;

    Mesh(std::int32_t x, std::uint32_t generation, std::uint32_t count) {
        for (std::uint32_t at = 0; at < count; ++at) {
            quads.push_back(
                {.geometry = (static_cast<std::uint32_t>(x) << 16U) | generation, .material = at});
        }
    }

    [[nodiscard]] vx::render::ChunkMeshUpdate update(std::int32_t x,
                                                     std::uint32_t generation) const {
        vx::ChunkMesh mesh{};
        mesh.ranges[0][0] = quads;
        return {.pos = {.x = x}, .generation = generation, .mesh = mesh};
    }
};

// The world thread's side, for one test: meshes offered in order every frame
// until one is refused, and removals.
class Script final : public vx::render::ChunkChanges {
public:
    std::vector<vx::render::ChunkMeshUpdate> offers;
    std::vector<vx::ChunkPos> removals;

    vx::Result<void> apply(vx::render::ChunkUploader& uploader) override {
        for (const vx::ChunkPos pos : removals) {
            uploader.remove(pos);
        }
        removals.clear();
        std::size_t taken = 0;
        for (; taken < offers.size(); ++taken) {
            const vx::Result<vx::render::Uploaded> uploaded = uploader.upload(offers[taken]);
            if (!uploaded.has_value()) {
                return uploaded.error();
            }
            if (uploaded.value() == vx::render::Uploaded::later ||
                uploaded.value() == vx::render::Uploaded::full) {
                break;
            }
        }
        offers.erase(offers.begin(), offers.begin() + static_cast<std::ptrdiff_t>(taken));
        return {};
    }
};

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

TEST_CASE("compaction and repacks keep every mesh's quads, in place and staged") {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    const vx::render::Device& device = context.value().device();
    for (const vx::render::BufferMemory memory :
         {vx::render::BufferMemory::upload_target, vx::render::BufferMemory::device_local}) {
        const bool staged = memory == vx::render::BufferMemory::device_local;
        INFO("staged: ", staged);
        // 1,000 quads to start with, room to grow to 4,000.
        vx::Result<vx::render::ChunkRenderer> created =
            vx::render::ChunkRenderer::create(device, 1'000, 2, memory, 0, 4'000);
        INFO((created.has_value() ? std::string() : created.error().message));
        REQUIRE(created.has_value());
        vx::render::ChunkRenderer& chunks = created.value();
        chunks.uploads().set_budget(std::chrono::hours(1));
        std::map<std::int32_t, std::unique_ptr<Mesh>> meshes; // each chunk's newest
        std::uint32_t generation = 0;
        Script script;
        const auto offer = [&](std::int32_t x, std::uint32_t quads) {
            ++generation;
            meshes[x] = std::make_unique<Mesh>(x, generation, quads);
            script.offers.push_back(meshes[x]->update(x, generation));
        };
        const auto remove = [&](std::int32_t x) {
            meshes.erase(x);
            script.removals.push_back({.x = x});
        };
        std::uint32_t slot = 0;
        int frames = 0;
        // A frame as the renderer records one, submitted and waited for;
        // then every mesh in the arena is read back and checked.
        const auto frame = [&] {
            vx::test::OneShot shot(device);
            chunks.begin_frame(device, slot);
            const vx::Result<void> kept = chunks.keep_arena(device, shot.commands());
            INFO((kept.has_value() ? std::string() : kept.error().message));
            REQUIRE(kept.has_value());
            REQUIRE(chunks.apply(script, shot.commands()).has_value());
            const vx::render::Buffer& arena = chunks.arena().buffer();
            vx::Result<vx::render::Buffer> readback =
                vx::render::Buffer::create(device, arena.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           vx::render::BufferMemory::readback, "arena readback");
            REQUIRE(readback.has_value());
            record_readback(shot.commands(), arena, readback.value());
            shot.submit_and_wait();
            vmaInvalidateAllocation(device.allocator(), readback.value().allocation(), 0,
                                    VK_WHOLE_SIZE);
            for (const vx::render::ChunkMeshRecord& record : chunks.uploads().records()) {
                const Mesh& mesh = *meshes.at(record.pos.x);
                REQUIRE(record.range.quads == mesh.quads.size());
                for (std::uint32_t at = 0; at < record.range.quads; ++at) {
                    vx::Quad held;
                    std::memcpy(&held,
                                readback.value().mapping().data() +
                                    ((std::size_t{record.range.first_quad} + at) * 8),
                                8);
                    REQUIRE(held == mesh.quads[at]);
                }
            }
            slot ^= 1U;
            ++frames;
        };
        const auto until_offered = [&] {
            for (int round = 0; round < 60 && !script.offers.empty(); ++round) {
                frame();
            }
            REQUIRE(script.offers.empty());
        };

        // Ten meshes of 90 quads, then every other one goes: holes of 90.
        for (std::int32_t x = 0; x < 10; ++x) {
            offer(x, 90);
        }
        until_offered();
        for (std::int32_t x = 0; x < 10; x += 2) {
            remove(x);
        }
        frame();
        frame();
        // 400 quads fit no hole, but the meshes packed leave 550: compaction
        // makes room, without a repack.
        offer(100, 400);
        until_offered();
        CHECK(chunks.upkeep().moved_total > 0);
        CHECK(chunks.upkeep().repacks == 0);
        CHECK(chunks.arena().space().stats().capacity == 1'000);
        // 2,000 more quads than the arena holds: repacks, 1.5 times larger.
        for (std::int32_t x = 200; x < 220; ++x) {
            offer(x, 100);
        }
        until_offered();
        CHECK(chunks.upkeep().repacks > 0);
        CHECK(chunks.arena().space().stats().capacity > 2'000);
        CHECK(chunks.arena().space().stats().capacity <= 4'000);
        // Past the limit: the arena says it is full, and the meshes in it
        // stay as they are.
        for (std::int32_t x = 300; x < 340; ++x) {
            offer(x, 100);
        }
        bool full = false;
        for (int round = 0; round < 20; ++round) {
            frame();
            full = full || chunks.upkeep().full;
        }
        CHECK(full);
        CHECK(chunks.arena().space().stats().capacity == 4'000);
        CHECK(frames > 10);
    }
    CHECK(context.value().instance().validation_counts().errors == 0);
    CHECK(context.value().instance().validation_counts().warnings == 0);
}
