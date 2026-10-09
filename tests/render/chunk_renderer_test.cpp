// Needs a Vulkan loader and at least one driver: labeled "vulkan" in CTest.
// RFC 0007's chunk renderer on a real driver: a frame's changes go into the
// arena, and its draw lists into the frame slot's buffer.

#include "one_shot.hpp"

#include <vx/core/frustum.hpp>
#include <vx/core/result.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/context.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/world/coordinates.hpp>

#include <doctest/doctest.h>
#include <glm/ext/vector_int3_sized.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <volk.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

// Uploads every update it holds as one batch.
class Batch final : public vx::render::ChunkChanges {
public:
    std::vector<vx::render::ChunkMeshUpdate> updates;

    vx::Result<void> apply(vx::render::ChunkUploader& uploader) override {
        const vx::Result<vx::render::Uploaded> done = uploader.upload_batch(updates);
        if (!done.has_value()) {
            return done.error();
        }
        return {};
    }
};

// Chunks along +x: each one opaque quad facing -x, and on every other chunk
// one cutout quad facing +y, both toward a camera at the row's west end.
struct Row {
    std::vector<vx::Quad> quads;
    Batch changes;

    explicit Row(std::int32_t chunks) {
        quads.resize(static_cast<std::size_t>(chunks) * 2);
        for (std::int32_t chunk = 0; chunk < chunks; ++chunk) {
            const std::span<const vx::Quad> two =
                std::span<const vx::Quad>(quads).subspan(static_cast<std::size_t>(chunk) * 2, 2);
            vx::ChunkMesh mesh{};
            std::size_t at = 0;
            for (std::size_t pass = 0; pass < 2; ++pass) {
                for (std::size_t face = 0; face < 6; ++face) {
                    // RFC 0003's order: -x is face 1, +y face 2.
                    const bool one =
                        (pass == 0 && face == 1) || (pass == 1 && face == 2 && chunk % 2 == 1);
                    const std::size_t size = one ? 1U : 0U;
                    mesh.ranges[pass][face] = two.subspan(at, size);
                    at += size;
                }
            }
            changes.updates.push_back({.pos = {.x = chunk}, .generation = 1, .mesh = mesh});
        }
    }
};

template <class T> T read(std::span<const std::byte> bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.subspan(offset, sizeof(T)).data(), sizeof(T));
    return value;
}

} // namespace

TEST_CASE("a frame's draws land in its slot's buffer: the origins, then each pass's commands") {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    const vx::render::Device& device = context.value().device();
    for (const vx::render::BufferMemory memory :
         {vx::render::BufferMemory::upload_target, vx::render::BufferMemory::device_local}) {
        INFO("staged: ", memory == vx::render::BufferMemory::device_local);
        vx::Result<vx::render::ChunkRenderer> created =
            vx::render::ChunkRenderer::create(device, 1U << 16U, 2, memory);
        REQUIRE(created.has_value());
        vx::render::ChunkRenderer& chunks = created.value();
        CHECK(chunks.draw_limit() > 0);

        // Five chunks seen from the block (-3, 1, 2): everything is in view.
        Row row(5);
        chunks.begin_frame(device, 0);
        {
            vx::test::OneShot shot(device);
            REQUIRE(chunks.apply(row.changes, shot.commands()).has_value());
            shot.submit_and_wait();
        }
        const glm::i64vec3 camera(-3, 1, 2);
        const vx::Result<vx::render::ChunkFrame> frame =
            chunks.prepare(device, camera, glm::vec3(0.5F), vx::Frustum());
        REQUIRE(frame.has_value());
        CHECK(frame.value().opaque == 5);
        CHECK(frame.value().cutout == 2);
        CHECK(frame.value().opaque_offset == 5 * sizeof(glm::vec4));
        CHECK(frame.value().cutout_offset ==
              frame.value().opaque_offset + (5 * sizeof(VkDrawIndexedIndirectCommand)));
        const std::span<const std::byte> bytes = chunks.frame_bytes();
        std::uint32_t cutout = 0;
        // Origins in the records' order, each the chunk's lowest corner from
        // the camera's block.
        for (std::uint32_t slot = 0; slot < 5; ++slot) {
            const auto x = static_cast<std::int32_t>(slot);
            CHECK(read<glm::vec4>(bytes, slot * sizeof(glm::vec4)) ==
                  glm::vec4(static_cast<float>((x * vx::chunk_size) + 3), -1.0F, -2.0F, 0.0F));
            const vx::render::ChunkMeshRecord* record = chunks.uploads().find({.x = x});
            REQUIRE(record != nullptr);
            const auto opaque = read<VkDrawIndexedIndirectCommand>(
                bytes, frame.value().opaque_offset + (slot * sizeof(VkDrawIndexedIndirectCommand)));
            CHECK(opaque.indexCount == 6);
            CHECK(opaque.instanceCount == 1);
            CHECK(opaque.firstIndex == 0);
            CHECK(opaque.vertexOffset == static_cast<std::int32_t>(4 * record->range.first_quad));
            CHECK(opaque.firstInstance == slot);
            if (x % 2 == 1) {
                const auto run = read<VkDrawIndexedIndirectCommand>(
                    bytes,
                    frame.value().cutout_offset + (cutout * sizeof(VkDrawIndexedIndirectCommand)));
                CHECK(run.vertexOffset ==
                      static_cast<std::int32_t>(4 * (record->range.first_quad + 1)));
                CHECK(run.firstInstance == slot);
                ++cutout;
            }
        }
        CHECK(cutout == 2);

        // 3,000 chunks outgrow the other slot's first buffer, which doubles.
        Row many(3'000);
        chunks.begin_frame(device, 1);
        {
            vx::test::OneShot shot(device);
            REQUIRE(chunks.apply(many.changes, shot.commands()).has_value());
            shot.submit_and_wait();
        }
        const vx::Result<vx::render::ChunkFrame> grown =
            chunks.prepare(device, camera, glm::vec3(0.5F), vx::Frustum());
        REQUIRE(grown.has_value());
        CHECK(grown.value().opaque == 3'000);
        CHECK(grown.value().cutout == 1'500);
        CHECK(chunks.frame_bytes().size() >=
              grown.value().cutout_offset + (1'500 * sizeof(VkDrawIndexedIndirectCommand)));
        CHECK(read<glm::vec4>(chunks.frame_bytes(), 2'999 * sizeof(glm::vec4)) ==
              glm::vec4(static_cast<float>((2'999 * vx::chunk_size) + 3), -1.0F, -2.0F, 0.0F));
        CHECK(chunks.uploads().records().size() == 3'000);
    }
    CHECK(context.value().instance().validation_counts().errors == 0);
    CHECK(context.value().instance().validation_counts().warnings == 0);
}
