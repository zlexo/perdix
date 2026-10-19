#include <vx/render/renderer.hpp>

#include "debug_name.hpp"
#include "transition.hpp"
#include "vk_result.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/mesh/mesh_chunk.hpp>
#include <vx/mesh/name_tag.hpp>
#include <vx/mesh/player_model.hpp>
#include <vx/mesh/quad.hpp>
#include <vx/render/block_texture_array.hpp>
#include <vx/render/block_textures.hpp>
#include <vx/render/buffer.hpp>
#include <vx/render/camera_relative.hpp>
#include <vx/render/chunk_renderer.hpp>
#include <vx/render/chunk_uploads.hpp>
#include <vx/render/draw_lists.hpp>
#include <vx/render/gpu_timer.hpp>
#include <vx/render/texture_table.hpp>
#include <vx/render/upload.hpp>
#include <vx/shaders/body.hpp>
#include <vx/shaders/chunk.hpp>
#include <vx/shaders/cube.hpp>
#include <vx/shaders/text.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/font.hpp>
#include <vx/world/skin.hpp>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <tracy/Tracy.hpp>
#include <tracy/TracyVulkan.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::render {
namespace {

constexpr std::uint64_t forever = std::numeric_limits<std::uint64_t>::max();

// RGBA byte order, so a captured frame reads without swizzling.
constexpr VkFormat offscreen_format = VK_FORMAT_R8G8B8A8_SRGB;

// Headless rendering's color target, a copy source for capture().
Result<Image> make_offscreen_target(const Device& device, VkExtent2D extent) {
    return Image::create(device, extent, offscreen_format,
                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, "offscreen target");
}

// RFC 0001: depth is a 32-bit float, so reversed-Z keeps its precision far
// away. Vulkan promises a 24-bit or a 32-bit depth format, not both, so a
// device without this one is told why here.
constexpr VkFormat depth_format = VK_FORMAT_D32_SFLOAT;

Result<Image> make_depth_target(const Device& device, VkExtent2D extent) {
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(device.physical_device(), depth_format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
        return Error{"the device cannot draw depth into 32-bit floats, which RFC 0001's "
                     "reversed-Z needs"};
    }
    return Image::create(device, extent, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT, "depth target");
}

enum class Target : std::uint8_t { swapchain, offscreen };

// What a frame draws into: a color image, a swapchain's or the offscreen
// target, and the depth attachment beside it.
struct FrameTarget {
    Target kind;
    VkImage image;
    VkImageView view;
    VkExtent2D extent;
    VkImage depth_image;
    VkImageView depth_view;
};

// Cube vertices as cube.slang reads them: a corner within the block and
// the face's linear color.
struct CubeVertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
    std::array<float, 2> uv;
    std::array<float, 2> padding;
};
static_assert(sizeof(CubeVertex) == 48, "cube.slang's vertex stride");

// A face: its corners counterclockwise seen from outside, so it is a front
// face from there, and its color, which tints the texture.
struct Face {
    std::array<std::array<float, 3>, 4> corners;
    std::array<float, 4> color;
};

constexpr std::array<Face, 6> cube_faces = {{
    {.corners = {{{1, 0, 1}, {1, 0, 0}, {1, 1, 0}, {1, 1, 1}}}, .color = {1, 0, 0, 1}}, // +x red
    {.corners = {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}, .color = {0, 1, 1, 1}}, // -x cyan
    {.corners = {{{0, 1, 1}, {1, 1, 1}, {1, 1, 0}, {0, 1, 0}}}, .color = {0, 1, 0, 1}}, // +y green
    {.corners = {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
     .color = {1, 0, 1, 1}}, // -y magenta
    {.corners = {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}, .color = {0, 0, 1, 1}}, // +z blue
    {.corners = {{{1, 0, 0}, {0, 0, 0}, {0, 1, 0}, {1, 1, 0}}}, .color = {1, 1, 0, 1}}, // -z yellow
}};

// Two triangles a face, both counterclockwise: 36 vertices. Each face shows
// the whole texture, its first corner at the texture's.
constexpr std::array<CubeVertex, 36> make_cube_vertices() {
    constexpr std::array<std::array<float, 2>, 4> corner_uv = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
    std::array<CubeVertex, 36> vertices{};
    std::size_t next = 0;
    for (const Face& face : cube_faces) {
        for (const std::size_t corner : std::array<std::size_t, 6>{0, 1, 2, 0, 2, 3}) {
            const auto& [x, y, z] = face.corners.at(corner);
            vertices.at(next++) = {.position = {x, y, z, 1.0F},
                                   .color = face.color,
                                   .uv = corner_uv.at(corner),
                                   .padding = {0, 0}};
        }
    }
    return vertices;
}

constexpr std::array<CubeVertex, 36> cube_vertices = make_cube_vertices();

// RFC 0001's cube texture, until RFC 0004's block textures: 16 by 16
// texels, a checker of two grays, as sRGB bytes.
constexpr std::uint32_t checker_size = 16;
constexpr std::uint8_t checker_light = 230;
constexpr std::uint8_t checker_dark = 150;

std::array<std::byte, std::size_t{checker_size} * checker_size * 4> make_checker() {
    std::array<std::byte, std::size_t{checker_size} * checker_size * 4> texels{};
    for (std::uint32_t y = 0; y < checker_size; ++y) {
        for (std::uint32_t x = 0; x < checker_size; ++x) {
            const std::uint8_t gray = (x + y) % 2 == 0 ? checker_light : checker_dark;
            const std::size_t at = ((std::size_t{y} * checker_size) + x) * 4;
            texels.at(at) = std::byte{gray};
            texels.at(at + 1) = std::byte{gray};
            texels.at(at + 2) = std::byte{gray};
            texels.at(at + 3) = std::byte{255};
        }
    }
    return texels;
}

// The vertex shader reads the push constants' positions and addresses, the
// fragment shader its texture.
constexpr VkShaderStageFlags push_stages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// RFC 0001's push constants, as cube.slang reads them.
struct DrawPush {
    glm::mat4 view_projection; // offset 0: rotation and projection only
    VkDeviceAddress vertices;  // offset 64
    VkDeviceAddress origins;   // offset 72: one float4 per instance
    glm::vec3 camera_fraction; // offset 80: the camera's place within its block
    std::uint32_t texture;     // offset 92: the texture table's slot
};
static_assert(sizeof(DrawPush) == 96 && offsetof(DrawPush, texture) == 92);

// One frame's push constants for a target of `extent`.
DrawPush make_draw_push(const Camera& camera, const BlockAndFraction& split, VkExtent2D extent,
                        VkDeviceAddress vertices, VkDeviceAddress origins, std::uint32_t texture) {
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    return {.view_projection = view_projection(camera, aspect),
            .vertices = vertices,
            .origins = origins,
            .camera_fraction = split.fraction,
            .texture = texture};
}

// The bodies' push constants (body.slang): the cube's, the skin in place of
// the texture, and the frame's fog (RFC 0013).
struct BodyPush {
    DrawPush draw;         // offset 0
    VkDeviceAddress fog;   // offset 96
    std::uint64_t padding; // to 112, a multiple of 16 as the shader rounds it
};
static_assert(sizeof(BodyPush) == 112 && offsetof(BodyPush, fog) == 96);

// The body pipeline (RFC 0012) pulls vx::BodyVertex as two float4s.
static_assert(sizeof(BodyVertex) == 32 && offsetof(BodyVertex, texel) == 12 &&
              offsetof(BodyVertex, normal) == 20);

// Bodies are drawn from both sides, as the game draws them: through an outer
// layer's clear texels, the inside of the box behind shows.
Result<GraphicsPipeline> make_body_pipeline(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures) {
    return GraphicsPipeline::create(device,
                                    {.spirv = shaders::body,
                                     .vertex_entry = "vertex_main",
                                     .fragment_entry = "fragment_main",
                                     .color_format = color_format,
                                     .depth_format = depth_format,
                                     .cull_mode = VK_CULL_MODE_NONE,
                                     .set_layout = textures,
                                     .push_constant_size = sizeof(BodyPush),
                                     .push_constant_stages = push_stages},
                                    "body");
}

// The text pipeline (RFC 0012) pulls vx::TagVertex as two float4s.
static_assert(sizeof(TagVertex) == 32 && offsetof(TagVertex, texel) == 16);

// The text shader's push constants (text.slang): the cube's first 96 bytes,
// the font's slot in place of the texture, then the colors a pass draws a
// tag's box and letters in. All 128 bytes Vulkan 1.3 guarantees.
struct TagColors {
    std::array<float, 4> box; // alpha 0 draws no box
    std::array<float, 4> ink;
};
struct TagPush {
    DrawPush draw;    // offset 0
    TagColors colors; // offset 96
};
static_assert(sizeof(TagPush) == 128 && offsetof(TagPush, colors) == 96);

// The game's colors for a name tag (26.2): seen through walls, its box black
// at a quarter (63 of 255) and its letters white at half (128 of 255); where
// nothing is in front, the letters fully white over no box.
constexpr TagColors see_through_colors = {.box = {0.0F, 0.0F, 0.0F, 63.0F / 255.0F},
                                          .ink = {1.0F, 1.0F, 1.0F, 128.0F / 255.0F}};
constexpr TagColors normal_colors = {.box = {0.0F, 0.0F, 0.0F, 0.0F},
                                     .ink = {1.0F, 1.0F, 1.0F, 1.0F}};

// A text pipeline: both blend by alpha and cull nothing; the one for the
// pass seen through walls neither tests nor writes depth, the normal one
// does both, as the game's see-through and normal text do.
Result<GraphicsPipeline> make_text_pipeline(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures, bool see_through) {
    return GraphicsPipeline::create(device,
                                    {.spirv = shaders::text,
                                     .vertex_entry = "vertex_main",
                                     .fragment_entry = "fragment_main",
                                     .color_format = color_format,
                                     .depth_format = depth_format,
                                     .cull_mode = VK_CULL_MODE_NONE,
                                     .set_layout = textures,
                                     .blend = true,
                                     .depth_test = !see_through,
                                     .depth_write = !see_through,
                                     .push_constant_size = sizeof(TagPush),
                                     .push_constant_stages = push_stages},
                                    see_through ? "text, seen through walls" : "text");
}

// The skin of a body whose skin is missing: the missing texture's magenta
// and black, as layer 0 of the block textures shows them, in squares of 4
// texels, so each face of the head has two by two as a block's face has.
// Only the base layer is filled: the outer layer stays clear, as the game's
// own skins' hats are, so the body shows the model's own boxes.
std::array<std::byte, std::size_t{skin_size} * skin_size * 4> make_missing_skin() {
    std::array<std::byte, std::size_t{skin_size} * skin_size * 4> texels{};
    for (int y = 0; y < skin_size; ++y) {
        for (int x = 0; x < skin_size; ++x) {
            const bool base =
                (y < 16 && x < 32) || (y >= 16 && y < 32) || (y >= 48 && x >= 16 && x < 48);
            if (!base) {
                continue;
            }
            const bool magenta = ((x / 4) + (y / 4)) % 2 == 0;
            const auto at = static_cast<std::size_t>((y * skin_size) + x) * 4;
            texels.at(at) = std::byte{magenta ? std::uint8_t{255} : std::uint8_t{0}};
            texels.at(at + 2) = std::byte{magenta ? std::uint8_t{255} : std::uint8_t{0}};
            texels.at(at + 3) = std::byte{255};
        }
    }
    return texels;
}

Result<GraphicsPipeline> make_cube_pipeline(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures) {
    return GraphicsPipeline::create(device,
                                    {.spirv = shaders::cube,
                                     .vertex_entry = "vertex_main",
                                     .fragment_entry = "fragment_main",
                                     .color_format = color_format,
                                     .depth_format = depth_format,
                                     .cull_mode = VK_CULL_MODE_BACK_BIT,
                                     .set_layout = textures,
                                     .push_constant_size = sizeof(DrawPush),
                                     .push_constant_stages = push_stages},
                                    "cube");
}

// The vertex shader reads the chunk's push constants, and the fragment
// shader its filter.
constexpr VkShaderStageFlags chunk_push_stages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// The chunk shader's push constants (chunk.slang). Its float3 sits at a
// multiple of 16, as the shader lays it out.
struct ChunkPush {
    glm::mat4 view_projection; // offset 0: rotation and projection only
    VkDeviceAddress quads;     // offset 64: the arena
    VkDeviceAddress origins;   // offset 72: the frame's, one float4 a drawn chunk
    glm::vec3 camera_fraction; // offset 80
    float occlusion_strength;  // offset 92
    std::uint32_t filter;      // offset 96: a BlockFilter, as chunk.slang numbers them
    std::uint32_t padding;     // to 104, the address's alignment
    VkDeviceAddress fog;       // offset 104: the frame's fog (RFC 0013)
};
static_assert(sizeof(ChunkPush) == 112 && offsetof(ChunkPush, origins) == 72 &&
              offsetof(ChunkPush, camera_fraction) == 80 &&
              offsetof(ChunkPush, occlusion_strength) == 92 && offsetof(ChunkPush, filter) == 96 &&
              offsetof(ChunkPush, fog) == 104);

// One frame's push constants for the chunks, on a target of `extent`. Each
// chunk's origin was subtracted from the camera's block in 64-bit integers,
// as cubes are, so the GPU only sees small numbers.
ChunkPush make_chunk_push(const Camera& camera, const BlockAndFraction& split, VkExtent2D extent,
                          VkDeviceAddress quads, VkDeviceAddress origins, BlockFilter filter,
                          float occlusion_strength, VkDeviceAddress fog) {
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    return {.view_projection = view_projection(camera, aspect),
            .quads = quads,
            .origins = origins,
            .camera_fraction = split.fraction,
            .occlusion_strength = occlusion_strength,
            .filter = static_cast<std::uint32_t>(filter),
            .padding = 0,
            .fog = fog};
}

// RFC 0013's fog for a frame, as fog.slang reads it through its address.
struct FogBlock {
    glm::vec4 color; // sRGB-encoded, 0 to 1; w is 1 with the fog on, 0 off
    glm::vec4 ramps; // the haze's end, the band's start and end, in blocks
};
static_assert(sizeof(FogBlock) == 32);

// Writes a frame's fog, or none, into its slot's buffer.
Result<void> write_fog(VmaAllocator allocator, const Buffer& buffer,
                       const std::optional<Fog>& fog) {
    FogBlock block{.color = glm::vec4(0.0F), .ramps = glm::vec4(0.0F)};
    if (fog) {
        block.color = glm::vec4(static_cast<float>(fog->color.r) / 255.0F,
                                static_cast<float>(fog->color.g) / 255.0F,
                                static_cast<float>(fog->color.b) / 255.0F, 1.0F);
        block.ramps =
            glm::vec4(fog->ramps.haze_end, fog->ramps.band_start, fog->ramps.band_end, 0.0F);
    }
    std::memcpy(buffer.mapping().data(), &block, sizeof(block));
    if (const VkResult result =
            vmaFlushAllocation(allocator, buffer.allocation(), 0, VK_WHOLE_SIZE);
        result != VK_SUCCESS) {
        return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
    }
    return {};
}

// Writes each cube's block, as seen from the camera's block, into a frame
// slot's buffer of origins: subtracted in 64-bit integers, then small
// enough to be exact as floats.
Result<void> write_cube_origins(VmaAllocator allocator, const Buffer& origins,
                                std::span<const glm::i64vec3> cubes,
                                const glm::i64vec3& camera_block) {
    for (std::size_t cube = 0; cube < cubes.size(); ++cube) {
        const glm::vec4 origin(relative_origin(cubes[cube], camera_block), 0.0F);
        std::memcpy(origins.mapping().subspan(cube * sizeof(glm::vec4)).data(), &origin,
                    sizeof(origin));
    }
    if (const VkResult result =
            vmaFlushAllocation(allocator, origins.allocation(), 0, VK_WHOLE_SIZE);
        result != VK_SUCCESS) {
        return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
    }
    return {};
}

// Writes a frame's cube origins and its fog into its slot's buffers.
Result<void> write_cubes_and_fog(VmaAllocator allocator, const Buffer& origins, const Buffer& fog,
                                 const FrameInputs& inputs, const glm::i64vec3& camera_block) {
    if (const Result<void> written =
            write_cube_origins(allocator, origins, inputs.cubes, camera_block);
        !written.has_value()) {
        return written.error();
    }
    return write_fog(allocator, fog, inputs.fog);
}

// One of the chunk shader's two pipelines: its fragment shader is
// `fragment_entry`, the opaque one or the cutout one with its alpha test.
// Back faces are culled: the mesher only emits faces seen from outside.
Result<GraphicsPipeline> make_chunk_pipeline(const Device& device, VkFormat color_format,
                                             VkDescriptorSetLayout textures,
                                             const char* fragment_entry, VkPolygonMode polygon_mode,
                                             std::string_view name) {
    return GraphicsPipeline::create(device,
                                    {.spirv = shaders::chunk,
                                     .vertex_entry = "vertex_main",
                                     .fragment_entry = fragment_entry,
                                     .color_format = color_format,
                                     .depth_format = depth_format,
                                     .cull_mode = VK_CULL_MODE_BACK_BIT,
                                     .polygon_mode = polygon_mode,
                                     .set_layout = textures,
                                     .push_constant_size = sizeof(ChunkPush),
                                     .push_constant_stages = chunk_push_stages},
                                    name);
}

// The chunk shader's pipelines: opaque, cutout, and where the device offers
// fillModeNonSolid, the wireframe, the opaque one drawn as lines.
struct ChunkPipelines {
    GraphicsPipeline opaque;
    GraphicsPipeline cutout;
    std::optional<GraphicsPipeline> wireframe;
};

Result<ChunkPipelines> make_chunk_pipelines(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures) {
    Result<GraphicsPipeline> opaque = make_chunk_pipeline(
        device, color_format, textures, "fragment_opaque", VK_POLYGON_MODE_FILL, "chunk, opaque");
    if (!opaque.has_value()) {
        return opaque.error();
    }
    Result<GraphicsPipeline> cutout = make_chunk_pipeline(
        device, color_format, textures, "fragment_cutout", VK_POLYGON_MODE_FILL, "chunk, cutout");
    if (!cutout.has_value()) {
        return cutout.error();
    }
    std::optional<GraphicsPipeline> wireframe;
    if (device.optional().fill_mode_non_solid) {
        Result<GraphicsPipeline> lines =
            make_chunk_pipeline(device, color_format, textures, "fragment_opaque",
                                VK_POLYGON_MODE_LINE, "chunk, wireframe");
        if (!lines.has_value()) {
            return lines.error();
        }
        wireframe = std::move(lines).value();
    }
    return ChunkPipelines{.opaque = std::move(opaque).value(),
                          .cutout = std::move(cutout).value(),
                          .wireframe = std::move(wireframe)};
}

// The shared index buffer's contents: 0, 1, 2, 2, 3, 0 for the first quad,
// then the same 4 higher for each next one, as many quads as a chunk can
// have (RFC 0003). 32-bit, since 16-bit indices stop at 16,384 quads.
std::vector<std::uint32_t> quad_indices() {
    std::vector<std::uint32_t> indices;
    indices.reserve(max_chunk_quads * 6);
    for (std::uint32_t quad = 0; quad < max_chunk_quads; ++quad) {
        const std::uint32_t first = quad * 4;
        for (const std::uint32_t corner : {0U, 1U, 2U, 2U, 3U, 0U}) {
            indices.push_back(first + corner);
        }
    }
    return indices;
}

void begin_commands(VkCommandBuffer commands) {
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(commands, &begin);
}

// What a frame's passes draw, besides the target they draw into.
// The chunks to draw: the pipelines, the shared index buffer, the push
// constants, and the frame's commands for each pass, opaque first.
struct ChunkDraw {
    const GraphicsPipeline* opaque_pipeline;
    const GraphicsPipeline* cutout_pipeline;
    const GraphicsPipeline* wireframe_pipeline; // null to draw solid: both passes as lines when set
    VkBuffer indices;
    ChunkPush push;
    ChunkFrame frame;
    std::uint32_t draw_limit; // the device's maxDrawIndirectCount
};

// The bodies to draw: their pipeline, the push constants, how many, and
// each one's skin, a slot of the texture table.
struct BodyDraw {
    const GraphicsPipeline* pipeline;
    BodyPush push;
    std::uint32_t count;
    std::array<std::uint32_t, Renderer::max_players> skins;
};

// The name tags to draw: both pipelines, the push constants, how many, and
// each one's run of vertices, the farthest first.
struct TagDraw {
    const GraphicsPipeline* see_through;
    const GraphicsPipeline* normal;
    TagPush push;
    std::uint32_t count;
    std::array<TagRun, Renderer::max_tags> runs;
};

struct FrameDraw {
    Color clear;
    const GraphicsPipeline* pipeline;
    VkDescriptorSet textures; // set 0, the texture table
    DrawPush push;
    std::uint32_t cubes;
    ChunkDraw chunk;
    BodyDraw bodies;
    TagDraw tags;
    ImDrawData* overlay; // null: no overlay
};

// Draws the frame's name tags as the game draws them, the farthest first:
// every tag but a discreet one through walls, its box and its letters at
// half; then every tag where nothing is in front of it, its letters fully
// white, or a discreet one's box and letters at half as before. Each tag's
// quads start at its run's first vertex, and its block is its firstInstance.
void record_tags(VkCommandBuffer commands, VkDescriptorSet textures, const TagDraw& tags) {
    if (tags.count == 0) {
        return;
    }
    const auto pass = [&](const GraphicsPipeline& pipeline, bool see_through) {
        VkPipelineLayout layout = pipeline.layout();
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &textures,
                                0, nullptr);
        vkCmdPushConstants(commands, layout, push_stages, 0, sizeof(DrawPush), &tags.push.draw);
        for (std::uint32_t tag = 0; tag < tags.count; ++tag) {
            const TagRun& run = tags.runs.at(tag);
            if (see_through && run.discreet) {
                continue;
            }
            const TagColors& colors =
                see_through || run.discreet ? see_through_colors : normal_colors;
            vkCmdPushConstants(commands, layout, push_stages, offsetof(TagPush, colors),
                               sizeof(TagColors), &colors);
            vkCmdDraw(commands, run.count, 1, run.first, tag);
        }
    };
    pass(*tags.see_through, true);
    pass(*tags.normal, false);
}

// Draws each body in its own skin: its 432 vertices from its firstVertex on,
// its block through its firstInstance, and its skin's slot pushed before it.
void record_bodies(VkCommandBuffer commands, VkDescriptorSet textures, const BodyDraw& bodies) {
    if (bodies.count == 0) {
        return;
    }
    VkPipelineLayout layout = bodies.pipeline->layout();
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, bodies.pipeline->handle());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &textures, 0,
                            nullptr);
    vkCmdPushConstants(commands, layout, push_stages, 0, sizeof(bodies.push), &bodies.push);
    constexpr auto vertices = static_cast<std::uint32_t>(player_vertices);
    for (std::uint32_t body = 0; body < bodies.count; ++body) {
        vkCmdPushConstants(commands, layout, push_stages, offsetof(DrawPush, texture),
                           sizeof(std::uint32_t), &bodies.skins.at(body));
        vkCmdDraw(commands, vertices, 1, body * vertices, body);
    }
}

// Draws the chunks' runs through the shared index buffer (RFC 0007): every
// opaque run, then every cutout one, each pass one multi-draw indirect call,
// split only where it passes the device's limit. Each command's
// vertexOffset makes vertex_index >> 2 its quad's index in the arena, and
// its firstInstance picks its chunk's origin.
void record_chunks(VkCommandBuffer commands, VkDescriptorSet textures, const ChunkDraw& chunks) {
    vkCmdBindIndexBuffer(commands, chunks.indices, 0, VK_INDEX_TYPE_UINT32);
    const auto pass = [&](const GraphicsPipeline& pipeline, VkDeviceSize offset,
                          std::uint32_t count) {
        if (count == 0) {
            return;
        }
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.handle());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.layout(), 0, 1,
                                &textures, 0, nullptr);
        vkCmdPushConstants(commands, pipeline.layout(), chunk_push_stages, 0, sizeof(chunks.push),
                           &chunks.push);
        constexpr std::uint32_t stride = sizeof(VkDrawIndexedIndirectCommand);
        for_each_call(count, chunks.draw_limit, [&](std::uint32_t first, std::uint32_t calls) {
            vkCmdDrawIndexedIndirect(commands, chunks.frame.buffer,
                                     offset + (VkDeviceSize{first} * stride), calls, stride);
        });
    };
    const bool lines = chunks.wireframe_pipeline != nullptr;
    pass(lines ? *chunks.wireframe_pipeline : *chunks.opaque_pipeline, chunks.frame.opaque_offset,
         chunks.frame.opaque);
    pass(lines ? *chunks.wireframe_pipeline : *chunks.cutout_pipeline, chunks.frame.cutout_offset,
         chunks.frame.cutout);
}

// Records the frame's one pass, inside the GPU zone Main pass: it clears the
// target, draws the cubes, then the overlay over them when there is one. The
// zone's timestamps fall outside the pass: KosmicKrisp drops timestamps
// written within rendering.
//
// The overlay draws here, not in a pass of its own: a second pass loads and
// stores the whole target again, which took 0.57 ms of GPU time a frame on
// MoltenVK and 0.92 ms on KosmicKrisp at 2560 by 1440, where drawing it here
// takes 0.10 ms and 0.16 ms.
void record_main_pass(VkCommandBuffer commands, [[maybe_unused]] tracy::VkCtx* gpu_profiler,
                      const FrameTarget& target, const FrameDraw& draw) {
    TracyVkNamedZone(gpu_profiler, pass_zone, commands, "Main pass", gpu_profiler != nullptr);

    const VkRenderingAttachmentInfo attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = target.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {.float32 = {draw.clear.r, draw.clear.g, draw.clear.b,
                                             draw.clear.a}}},
    };
    // Reversed-Z: depth clears to 0, the far end. Nothing reads it after the
    // frame, so it is not stored.
    const VkRenderingAttachmentInfo depth_attachment = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .pNext = nullptr,
        .imageView = target.depth_view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .resolveMode = VK_RESOLVE_MODE_NONE,
        .resolveImageView = VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue = {.depthStencil = {.depth = 0.0F, .stencil = 0}},
    };
    const VkRenderingInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderArea = {.offset = {.x = 0, .y = 0}, .extent = target.extent},
        .layerCount = 1,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
        .pDepthAttachment = &depth_attachment,
        .pStencilAttachment = nullptr,
    };
    vkCmdBeginRendering(commands, &rendering);

    // The vertex shader pulls the vertices through their buffer's address.
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->handle());
    const VkViewport viewport = {
        .x = 0.0F,
        .y = 0.0F,
        .width = static_cast<float>(target.extent.width),
        .height = static_cast<float>(target.extent.height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F,
    };
    vkCmdSetViewport(commands, 0, 1, &viewport);
    const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = target.extent};
    vkCmdSetScissor(commands, 0, 1, &scissor);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline->layout(), 0,
                            1, &draw.textures, 0, nullptr);
    vkCmdPushConstants(commands, draw.pipeline->layout(), push_stages, 0, sizeof(draw.push),
                       &draw.push);
    vkCmdDraw(commands, static_cast<std::uint32_t>(cube_vertices.size()), draw.cubes, 0, 0);
    record_chunks(commands, draw.textures, draw.chunk);
    record_bodies(commands, draw.textures, draw.bodies);
    record_tags(commands, draw.textures, draw.tags);
    if (draw.overlay != nullptr) {
        // Last in the pass, so it draws over everything else. Its pipeline
        // neither tests nor writes depth.
        ImGui_ImplVulkan_RenderDrawData(draw.overlay, commands);
    }

    vkCmdEndRendering(commands);
}

// Records a frame's work into `commands`, inside the GPU zone Frame: the
// target becomes a color attachment, the main pass clears it and draws into
// it, and a swapchain image then becomes presentable. An offscreen image
// stays a color attachment, ready for the next frame or for capture.
void record_passes(VkCommandBuffer commands, [[maybe_unused]] tracy::VkCtx* gpu_profiler,
                   const FrameTarget& target, const FrameDraw& draw) {
    TracyVkNamedZone(gpu_profiler, frame_zone, commands, "Frame", gpu_profiler != nullptr);

    // For a swapchain image, the submission waits for the acquired image at
    // this stage, so starting the barrier there chains onto that wait. The
    // offscreen image has no such wait: the barrier waits for the previous
    // frame's writes to it instead. The old contents do not matter either way.
    transition(commands, target.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               target.kind == Target::swapchain ? VK_ACCESS_2_NONE
                                                : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Depth is cleared every frame, so its old contents never matter either;
    // with two frames in flight, the barrier waits for the previous frame's
    // depth writes.
    constexpr VkPipelineStageFlags2 depth_stages =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    transition(commands, target.depth_image, depth_stages,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, depth_stages,
               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT);

    record_main_pass(commands, gpu_profiler, target, draw);

    if (target.kind == Target::swapchain) {
        // Presentation waits on the semaphore the submission signals after all
        // of its commands, this layout transition included.
        transition(commands, target.image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
                   VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
}

// What a frame changes in the chunk arena and where it sees the chunks from.
struct ChunkView {
    const Device* device;
    ChunkChanges* changes; // null changes nothing
    glm::i64vec3 camera_block;
    glm::vec3 camera_fraction;
    Frustum frustum; // relative to the camera
};

// Records a frame: hands Tracy the GPU zones of earlier frames whose
// timestamps have landed, keeps the chunk arena up (compaction or a repack)
// and makes the frame's changes to it, whose copies run before anything
// reads it, builds the chunks' draw lists,
// then records the frame's passes, all timed in the frame slot's queries.
// The timer starts after Tracy's collect, so it times what Tracy's Frame zone
// does and none of Tracy's own work. Collecting and the timer's start reset
// queries for reuse, which must happen outside any rendering. After an error
// the commands are never submitted.
Result<void> record_frame(VkCommandBuffer commands, GpuTimer& timer, std::uint32_t slot,
                          tracy::VkCtx* gpu_profiler, const FrameTarget& target, FrameDraw draw,
                          ChunkRenderer& chunks, const ChunkView& view) {
    ZoneScopedN("Record");
    begin_commands(commands);
    if (gpu_profiler != nullptr) {
        TracyVkCollect(gpu_profiler, commands);
    }
    timer.begin(commands, slot);
    // Only now, after the wait for the image, does the upload budget start.
    chunks.begin_frame(*view.device, slot);
    {
        ZoneScopedN("Chunk arena upkeep");
        if (const Result<void> kept = chunks.keep_arena(*view.device, commands);
            !kept.has_value()) {
            vkEndCommandBuffer(commands);
            return kept.error();
        }
    }
    if (view.changes != nullptr) {
        ZoneScopedN("Chunk uploads");
        if (const Result<void> applied = chunks.apply(*view.changes, commands);
            !applied.has_value()) {
            vkEndCommandBuffer(commands);
            return applied.error();
        }
    }
    Result<ChunkFrame> frame =
        chunks.prepare(*view.device, view.camera_block, view.camera_fraction, view.frustum);
    if (!frame.has_value()) {
        vkEndCommandBuffer(commands);
        return frame.error();
    }
    draw.chunk.frame = frame.value();
    draw.chunk.push.quads = frame.value().quads;
    draw.chunk.push.origins = frame.value().origins;
    record_passes(commands, gpu_profiler, target, draw);
    timer.end(commands, slot);
    vkEndCommandBuffer(commands);
    return {};
}

// The overlay's pipeline, for the main pass: one color attachment of
// `format`, which must outlive the call it is passed to, and the pass's
// depth attachment, which the pipeline neither tests nor writes.
ImGui_ImplVulkan_PipelineInfo overlay_pipeline_info(const VkFormat* format) {
    ImGui_ImplVulkan_PipelineInfo info{};
    info.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext = nullptr,
        .viewMask = 0,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = format,
        .depthAttachmentFormat = depth_format,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };
    return info;
}

// Tracy's timing of the device's queue, for RFC 0001's GPU zones: null
// without VX_PROFILE, when the queue writes no timestamps, or when its
// command pool cannot be made. Creating it begins one command buffer several
// times, submitting and waiting each time, so it gets one of its own, from a
// pool that allows that, and the frames' pools need not.
tracy::VkCtx* make_gpu_profiler(const Device& device) {
#ifndef TRACY_ENABLE
    static_cast<void>(device);
    return nullptr;
#else
    if (timestamp_valid_bits(device) == 0) {
        return nullptr;
    }

    const VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = device.queue_family(),
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(device.handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
        return nullptr;
    }
    set_debug_name(device.handle(), pool, "Tracy's setup");
    const VkCommandBufferAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer commands = VK_NULL_HANDLE;
    tracy::VkCtx* profiler = nullptr;
    if (vkAllocateCommandBuffers(device.handle(), &allocate, &commands) == VK_SUCCESS) {
        set_debug_name(device.handle(), commands, "Tracy's setup");
        profiler =
            TracyVkContext(device.physical_device(), device.handle(), device.queue(), commands);
        constexpr std::string_view name = "Graphics queue";
        TracyVkContextName(profiler, name.data(), static_cast<std::uint16_t>(name.size()));
    }
    vkDestroyCommandPool(device.handle(), pool, nullptr); // frees its command buffer
    return profiler;
#endif
}

// Adds the time from its making to its end to a frame's wait: for the calls
// that wait for the GPU or the display.
class WaitTimer {
public:
    explicit WaitTimer(std::chrono::nanoseconds& waited) noexcept
        : waited_(&waited), start_(std::chrono::steady_clock::now()) {}
    WaitTimer(const WaitTimer&) = delete;
    WaitTimer& operator=(const WaitTimer&) = delete;
    WaitTimer(WaitTimer&&) = delete;
    WaitTimer& operator=(WaitTimer&&) = delete;
    ~WaitTimer() { *waited_ += std::chrono::steady_clock::now() - start_; }

private:
    std::chrono::nanoseconds* waited_;
    std::chrono::steady_clock::time_point start_;
};

Result<VkSemaphore> make_semaphore(VkDevice device, const void* type, std::string_view name) {
    const VkSemaphoreCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = type,
        .flags = 0,
    };
    VkSemaphore semaphore = VK_NULL_HANDLE;
    if (const VkResult result = vkCreateSemaphore(device, &info, nullptr, &semaphore);
        result != VK_SUCCESS) {
        return Error{"vkCreateSemaphore failed: " + vk_result_name(result)};
    }
    set_debug_name(device, semaphore, name);
    return semaphore;
}

VkSemaphoreSubmitInfo semaphore_submit(VkSemaphore semaphore, std::uint64_t value,
                                       VkPipelineStageFlags2 stage) {
    return {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext = nullptr,
        .semaphore = semaphore,
        .value = value,
        .stageMask = stage,
        .deviceIndex = 0,
    };
}

Result<void> submit(VkQueue queue, VkCommandBuffer commands,
                    std::span<const VkSemaphoreSubmitInfo> waits,
                    std::span<const VkSemaphoreSubmitInfo> signals) {
    ZoneScopedN("Submit");
    const VkCommandBufferSubmitInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext = nullptr,
        .commandBuffer = commands,
        .deviceMask = 0,
    };
    const VkSubmitInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext = nullptr,
        .flags = 0,
        .waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size()),
        .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &command_info,
        .signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size()),
        .pSignalSemaphoreInfos = signals.data(),
    };
    if (const VkResult result = vkQueueSubmit2(queue, 1, &info, VK_NULL_HANDLE);
        result != VK_SUCCESS) {
        return Error{"vkQueueSubmit2 failed: " + vk_result_name(result)};
    }
    return {};
}

// Submits a frame's commands. It counts as a wait: MoltenVK waits there for
// the drawable the frame presents.
Result<void> submit_frame(VkQueue queue, VkCommandBuffer commands,
                          std::span<const VkSemaphoreSubmitInfo> waits,
                          std::span<const VkSemaphoreSubmitInfo> signals,
                          std::chrono::nanoseconds& waited) {
    const WaitTimer timer(waited);
    return submit(queue, commands, waits, signals);
}

// Blocks until the timeline reaches `value`: the GPU is done with the frames
// that last used a slot.
Result<void> wait_for_slot(VkDevice device, VkSemaphore frames_done, std::uint64_t value,
                           std::chrono::nanoseconds& waited) {
    ZoneScopedN("Wait for frame slot");
    const WaitTimer timer(waited);
    const VkSemaphoreWaitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .pNext = nullptr,
        .flags = 0,
        .semaphoreCount = 1,
        .pSemaphores = &frames_done,
        .pValues = &value,
    };
    if (const VkResult result = vkWaitSemaphores(device, &wait, forever); result != VK_SUCCESS) {
        return Error{"vkWaitSemaphores failed: " + vk_result_name(result)};
    }
    return {};
}

// The swapchain's next image, its index in `index`; `acquired` signals once
// the presentation engine has let go of it.
VkResult acquire_image(VkDevice device, VkSwapchainKHR swapchain, VkSemaphore acquired,
                       std::uint32_t& index, std::chrono::nanoseconds& waited) {
    ZoneScopedN("Acquire");
    const WaitTimer timer(waited);
    return vkAcquireNextImageKHR(device, swapchain, forever, acquired, VK_NULL_HANDLE, &index);
}

// Queues image `index` for presentation once `rendered` signals.
VkResult present_image(VkQueue queue, VkSwapchainKHR swapchain, std::uint32_t index,
                       VkSemaphore rendered, std::chrono::nanoseconds& waited) {
    ZoneScopedN("Present");
    const WaitTimer timer(waited);
    const VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &rendered,
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &index,
        .pResults = nullptr,
    };
    return vkQueuePresentKHR(queue, &present);
}

VkExtent2D vk_extent(Extent extent) {
    return {.width = extent.width, .height = extent.height};
}

// The block texture array's layers: the pack's, or the checker alone. Bytes
// that are not whole layers are refused before anything is made.
Result<std::uint32_t> block_texture_layer_count(std::span<const std::byte> bytes) {
    if (bytes.size() % block_texture_layer_bytes != 0) {
        return Error{"the block textures are " + std::to_string(bytes.size()) +
                     " bytes, not whole layers of " + std::to_string(block_texture_layer_bytes)};
    }
    const std::size_t layers = bytes.empty() ? 1 : bytes.size() / block_texture_layer_bytes;
    if (layers > std::numeric_limits<std::uint32_t>::max()) {
        return Error{"the block textures hold " + std::to_string(layers) + " layers"};
    }
    return static_cast<std::uint32_t>(layers);
}

// The chunk shader's pipelines, and the index buffer every command shares.
struct ChunkResources {
    ChunkPipelines pipelines;
    Buffer indices;
};

Result<ChunkResources> make_chunk_resources(const Device& device, VkFormat color_format,
                                            VkDescriptorSetLayout textures) {
    Result<ChunkPipelines> pipelines = make_chunk_pipelines(device, color_format, textures);
    if (!pipelines.has_value()) {
        return pipelines.error();
    }
    Result<Buffer> indices = Buffer::create(device, max_chunk_quads * 6 * sizeof(std::uint32_t),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                            BufferMemory::upload_target, "chunk indices");
    if (!indices.has_value()) {
        return indices.error();
    }
    return ChunkResources{.pipelines = std::move(pipelines).value(),
                          .indices = std::move(indices).value()};
}

// What draws name tags: the two text pipelines, and the font's atlas when
// there is a font.
struct TextResources {
    GraphicsPipeline see_through;
    GraphicsPipeline normal;
    std::optional<Image> font;
};

Result<TextResources> make_text_resources(const Device& device, VkFormat color_format,
                                          VkDescriptorSetLayout textures, bool font) {
    Result<GraphicsPipeline> see_through = make_text_pipeline(device, color_format, textures, true);
    if (!see_through.has_value()) {
        return see_through.error();
    }
    Result<GraphicsPipeline> normal = make_text_pipeline(device, color_format, textures, false);
    if (!normal.has_value()) {
        return normal.error();
    }
    std::optional<Image> atlas;
    if (font) {
        Result<Image> image = Image::create(
            device, {.width = font_atlas_size, .height = font_atlas_size}, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
            "font");
        if (!image.has_value()) {
            return image.error();
        }
        atlas = std::move(image).value();
    }
    return TextResources{.see_through = std::move(see_through).value(),
                         .normal = std::move(normal).value(),
                         .font = std::move(atlas)};
}

// What draws bodies: their pipeline, and an image for each of `skins`
// skins and last one for the missing texture's.
struct BodyResources {
    GraphicsPipeline pipeline;
    std::vector<Image> skins;
};

Result<BodyResources> make_body_resources(const Device& device, VkFormat color_format,
                                          VkDescriptorSetLayout textures, std::size_t skins) {
    Result<GraphicsPipeline> pipeline = make_body_pipeline(device, color_format, textures);
    if (!pipeline.has_value()) {
        return pipeline.error();
    }
    std::vector<Image> images;
    for (std::size_t skin = 0; skin <= skins; ++skin) {
        Result<Image> image = Image::create(
            device, {.width = skin_size, .height = skin_size}, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
            skin < skins ? "skin " + std::to_string(skin) : std::string("missing skin"));
        if (!image.has_value()) {
            return image.error();
        }
        images.push_back(std::move(image).value());
    }
    return BodyResources{.pipeline = std::move(pipeline).value(), .skins = std::move(images)};
}

// What draws bodies and their name tags (RFC 0012), made together.
struct FigureResources {
    BodyResources bodies;
    TextResources text;
};

Result<FigureResources> make_figure_resources(const Device& device, VkFormat color_format,
                                              VkDescriptorSetLayout textures,
                                              const RendererDesc& desc) {
    Result<BodyResources> bodies =
        make_body_resources(device, color_format, textures, desc.skins.size());
    if (!bodies.has_value()) {
        return bodies.error();
    }
    Result<TextResources> text =
        make_text_resources(device, color_format, textures, desc.font != nullptr);
    if (!text.has_value()) {
        return text.error();
    }
    return FigureResources{.bodies = std::move(bodies).value(), .text = std::move(text).value()};
}

} // namespace

Result<Renderer> Renderer::create(const RendererDesc& desc) {
    ZoneScopedN("Create renderer");
    const Result<std::uint32_t> layers = block_texture_layer_count(desc.block_textures);
    if (!layers.has_value()) {
        return layers.error();
    }
    Result<Context> context =
        Context::create({.surface = desc.surface, .validation = desc.validation});
    if (!context.has_value()) {
        return context.error();
    }

    std::optional<Swapchain> swapchain;
    std::optional<Image> offscreen;
    if (desc.surface.has_value()) {
        Result<Swapchain> made =
            Swapchain::create(context.value().device(), context.value().surface(), desc.framebuffer,
                              desc.present_mode, VK_NULL_HANDLE);
        if (!made.has_value()) {
            return made.error();
        }
        swapchain = std::move(made).value();
    } else {
        Result<Image> made =
            make_offscreen_target(context.value().device(), vk_extent(desc.framebuffer));
        if (!made.has_value()) {
            return made.error();
        }
        offscreen = std::move(made).value();
    }

    const VkFormat color_format =
        swapchain.has_value() ? swapchain->choice().format.format : offscreen_format;
    Result<Image> depth = make_depth_target(context.value().device(),
                                            swapchain.has_value() ? swapchain->choice().extent
                                                                  : vk_extent(desc.framebuffer));
    if (!depth.has_value()) {
        return depth.error();
    }
    Result<TextureTable> textures = TextureTable::create(context.value().device());
    if (!textures.has_value()) {
        return textures.error();
    }
    Result<GraphicsPipeline> pipeline =
        make_cube_pipeline(context.value().device(), color_format, textures.value().layout());
    if (!pipeline.has_value()) {
        return pipeline.error();
    }
    Result<Image> checker = Image::create(
        context.value().device(), {.width = checker_size, .height = checker_size},
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, "cube checker");
    if (!checker.has_value()) {
        return checker.error();
    }
    Result<FigureResources> figures = make_figure_resources(context.value().device(), color_format,
                                                            textures.value().layout(), desc);
    if (!figures.has_value()) {
        return figures.error();
    }
    Result<Image> block_textures =
        create_block_texture_array(context.value().device(), layers.value());
    if (!block_textures.has_value()) {
        return block_textures.error();
    }
    Result<Buffer> vertices = Buffer::create(context.value().device(), sizeof(cube_vertices),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                             BufferMemory::upload_target, "cube vertices");
    if (!vertices.has_value()) {
        return vertices.error();
    }
    Result<ChunkResources> chunk_resources =
        make_chunk_resources(context.value().device(), color_format, textures.value().layout());
    if (!chunk_resources.has_value()) {
        return chunk_resources.error();
    }
    Result<ChunkRenderer> chunks = ChunkRenderer::create(
        context.value().device(), desc.chunk_arena_quads,
        static_cast<std::uint32_t>(frames_in_flight),
        desc.stage_chunk_uploads ? BufferMemory::device_local : BufferMemory::upload_target,
        desc.draw_call_limit, desc.chunk_arena_limit);
    if (!chunks.has_value()) {
        return chunks.error();
    }

    Renderer renderer(std::move(context).value(), std::move(swapchain), std::move(offscreen),
                      std::move(depth).value(), std::move(textures).value(),
                      std::move(block_textures).value(),
                      {.pipeline = std::move(pipeline).value(),
                       .vertices = std::move(vertices).value(),
                       .checker = std::move(checker).value()},
                      {.pipeline = std::move(figures.value().bodies.pipeline),
                       .skins = std::move(figures.value().bodies.skins),
                       .slots = {}},
                      {.see_through = std::move(figures.value().text.see_through),
                       .normal = std::move(figures.value().text.normal),
                       .font = std::move(figures.value().text.font),
                       .slot = 0,
                       .advances = {}},
                      {.opaque = std::move(chunk_resources.value().pipelines.opaque),
                       .cutout = std::move(chunk_resources.value().pipelines.cutout),
                       .wireframe = std::move(chunk_resources.value().pipelines.wireframe),
                       .indices = std::move(chunk_resources.value().indices)},
                      std::move(chunks).value(), desc.framebuffer, desc.present_mode);
    if (const Result<void> made = renderer.create_frame_objects(); !made.has_value()) {
        return made.error();
    }
    if (const Result<void> uploaded =
            renderer.upload_at_startup(desc.block_textures, desc.skins, desc.font);
        !uploaded.has_value()) {
        return uploaded.error();
    }
    if (desc.overlay != nullptr) {
        if (const Result<void> started = renderer.start_overlay(desc.overlay);
            !started.has_value()) {
            return started.error();
        }
    }
    renderer.gpu_profiler_ = make_gpu_profiler(renderer.context_.device());
    return renderer;
}

Renderer::Renderer(Context context, std::optional<Swapchain> swapchain,
                   std::optional<Image> offscreen, Image depth, TextureTable textures,
                   Image block_textures, Cube cube, BodyDrawing body_drawing,
                   TextDrawing text_drawing, ChunkDrawing chunk_drawing, ChunkRenderer chunks,
                   Extent framebuffer, PresentMode present_mode)
    : context_(std::move(context)), swapchain_(std::move(swapchain)),
      offscreen_(std::move(offscreen)), depth_(std::move(depth)), textures_(std::move(textures)),
      block_textures_(std::move(block_textures)), cube_(std::move(cube)),
      body_drawing_(std::move(body_drawing)), text_drawing_(std::move(text_drawing)),
      chunk_drawing_(std::move(chunk_drawing)), chunks_(std::move(chunks)),
      framebuffer_(framebuffer), target_framebuffer_(framebuffer), present_mode_(present_mode) {}

Renderer::Renderer(Renderer&& other) noexcept
    : context_(std::move(other.context_)), swapchain_(std::move(other.swapchain_)),
      offscreen_(std::move(other.offscreen_)), depth_(std::move(other.depth_)),
      textures_(std::move(other.textures_)), block_textures_(std::move(other.block_textures_)),
      cube_(std::move(other.cube_)), body_drawing_(std::move(other.body_drawing_)),
      text_drawing_(std::move(other.text_drawing_)),
      chunk_drawing_(std::move(other.chunk_drawing_)), chunks_(std::move(other.chunks_)),
      set_chunk_at_(other.set_chunk_at_), set_chunk_generation_(other.set_chunk_generation_),
      block_filter_(other.block_filter_), occlusion_strength_(other.occlusion_strength_),
      wireframe_(other.wireframe_), framebuffer_(other.framebuffer_),
      target_framebuffer_(other.target_framebuffer_), present_mode_(other.present_mode_),
      swapchain_stale_(other.swapchain_stale_), slots_(std::exchange(other.slots_, {})),
      origins_(std::move(other.origins_)), body_vertices_(std::move(other.body_vertices_)),
      body_origins_(std::move(other.body_origins_)), posed_(other.posed_),
      tag_vertices_(std::move(other.tag_vertices_)), tag_origins_(std::move(other.tag_origins_)),
      fog_(std::move(other.fog_)), laid_(other.laid_), tag_order_(other.tag_order_),
      image_rendered_(std::exchange(other.image_rendered_, {})),
      frames_done_(std::exchange(other.frames_done_, VK_NULL_HANDLE)), frame_(other.frame_),
      gpu_timer_(std::move(other.gpu_timer_)), times_(other.times_),
      gpu_profiler_(std::exchange(other.gpu_profiler_, nullptr)),
      overlay_(std::exchange(other.overlay_, nullptr)) {}

Renderer::~Renderer() {
    destroy();
}

Result<void> Renderer::create_frame_objects() {
    VkDevice device = context_.device().handle();

    for (std::size_t index = 0; index < frames_in_flight; ++index) {
        FrameSlot& slot = slots_.at(index);
        const std::string name = "frame slot " + std::to_string(index);
        const VkCommandPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = context_.device().queue_family(),
        };
        if (const VkResult result =
                vkCreateCommandPool(device, &pool_info, nullptr, &slot.command_pool);
            result != VK_SUCCESS) {
            return Error{"vkCreateCommandPool failed: " + vk_result_name(result)};
        }
        set_debug_name(device, slot.command_pool, name);
        const VkCommandBufferAllocateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = slot.command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (const VkResult result =
                vkAllocateCommandBuffers(device, &buffer_info, &slot.command_buffer);
            result != VK_SUCCESS) {
            return Error{"vkAllocateCommandBuffers failed: " + vk_result_name(result)};
        }
        set_debug_name(device, slot.command_buffer, name);
        Result<VkSemaphore> acquired = make_semaphore(device, nullptr, name + " image acquired");
        if (!acquired.has_value()) {
            return acquired.error();
        }
        slot.image_acquired = acquired.value();
    }

    const VkSemaphoreTypeCreateInfo timeline = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    Result<VkSemaphore> done = make_semaphore(device, &timeline, "frames done");
    if (!done.has_value()) {
        return done.error();
    }
    frames_done_ = done.value();

    for (std::size_t slot = 0; slot < frames_in_flight; ++slot) {
        Result<Buffer> origins = Buffer::create(
            context_.device(), max_cubes * sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " cube origins");
        if (!origins.has_value()) {
            return origins.error();
        }
        origins_.push_back(std::move(origins).value());

        Result<Buffer> body_vertices = Buffer::create(
            context_.device(), max_players * player_vertices * sizeof(BodyVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " body vertices");
        if (!body_vertices.has_value()) {
            return body_vertices.error();
        }
        body_vertices_.push_back(std::move(body_vertices).value());
        Result<Buffer> body_origins = Buffer::create(
            context_.device(), max_players * sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " body origins");
        if (!body_origins.has_value()) {
            return body_origins.error();
        }
        body_origins_.push_back(std::move(body_origins).value());

        Result<Buffer> tag_vertices = Buffer::create(
            context_.device(), max_tags * max_tag_vertices * sizeof(TagVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " tag vertices");
        if (!tag_vertices.has_value()) {
            return tag_vertices.error();
        }
        tag_vertices_.push_back(std::move(tag_vertices).value());
        Result<Buffer> tag_origins = Buffer::create(
            context_.device(), max_tags * sizeof(glm::vec4),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " tag origins");
        if (!tag_origins.has_value()) {
            return tag_origins.error();
        }
        tag_origins_.push_back(std::move(tag_origins).value());
        Result<Buffer> fog = Buffer::create(
            context_.device(), sizeof(FogBlock),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            BufferMemory::per_frame, "frame slot " + std::to_string(slot) + " fog");
        if (!fog.has_value()) {
            return fog.error();
        }
        fog_.push_back(std::move(fog).value());
    }

    Result<GpuTimer> timer = GpuTimer::create(context_.device(), frames_in_flight);
    if (!timer.has_value()) {
        return timer.error();
    }
    gpu_timer_ = std::move(timer).value();
    return add_rendered_semaphores();
}

// The presentation engine may hold an image's semaphore after the frame that
// signaled it is done, so none is destroyed before the renderer is; a new
// swapchain with more images only adds some.
Result<void> Renderer::add_rendered_semaphores() {
    VkDevice device = context_.device().handle();
    const std::size_t images = swapchain_.has_value() ? swapchain_->images().size() : 0;
    while (image_rendered_.size() < images) {
        Result<VkSemaphore> semaphore = make_semaphore(
            device, nullptr,
            "swapchain image " + std::to_string(image_rendered_.size()) + " rendered");
        if (!semaphore.has_value()) {
            return semaphore.error();
        }
        image_rendered_.push_back(semaphore.value());
    }
    return {};
}

// The cube's vertices and checker, the block texture array and the skins.
// On unified memory the vertices go through their buffer's mapping and
// nothing is recorded for them. The images always stage: the copies are
// submitted, and waited for before their staging buffers go away with
// `uploads`.
Result<void> Renderer::upload_at_startup(std::span<const std::byte> block_textures,
                                         std::span<const Skin> skins, const Font* font) {
    ZoneScopedN("Upload at startup");
    const Device& device = context_.device();
    UploadContext uploads(device);
    VkCommandBuffer commands = slots_[0].command_buffer;
    begin_commands(commands);
    Result<void> uploaded =
        upload(uploads, commands, cube_.vertices, 0, std::as_bytes(std::span(cube_vertices)));
    const std::vector<std::uint32_t> indices = quad_indices();
    if (uploaded.has_value()) {
        uploaded =
            upload(uploads, commands, chunk_drawing_.indices, 0, std::as_bytes(std::span(indices)));
    }
    if (uploaded.has_value()) {
        uploaded = upload_image(uploads, commands, cube_.checker, make_checker());
    }
    if (uploaded.has_value() && !block_textures.empty()) {
        uploaded = upload_image(uploads, commands, block_textures_, block_textures);
    } else if (uploaded.has_value()) {
        // Layer 0 alone, the checker: it reads no file.
        const std::array<TextureRef, 1> checker_only{};
        std::vector<Diagnostic> diagnostics;
        uploaded = upload_image(uploads, commands, block_textures_,
                                build_block_textures(checker_only, {}, diagnostics));
    }
    const auto missing_skin = make_missing_skin();
    for (std::size_t skin = 0; uploaded.has_value() && skin < body_drawing_.skins.size(); ++skin) {
        uploaded = upload_image(uploads, commands, body_drawing_.skins[skin],
                                skin < skins.size() ? std::span<const std::byte>(std::as_bytes(
                                                          std::span(skins[skin].rgba)))
                                                    : std::span<const std::byte>(missing_skin));
    }
    if (uploaded.has_value() && font != nullptr && text_drawing_.font.has_value()) {
        uploaded = upload_image(uploads, commands, *text_drawing_.font,
                                std::as_bytes(std::span(font->rgba)));
        text_drawing_.advances = font->advances;
    }
    uploads.finish(commands);
    vkEndCommandBuffer(commands);
    if (!uploaded.has_value()) {
        return uploaded.error();
    }
    if (const Result<void> submitted = submit(device.queue(), commands, {}, {});
        !submitted.has_value()) {
        return submitted.error();
    }
    vkQueueWaitIdle(device.queue());

    const Result<std::uint32_t> slot = textures_.add(cube_.checker.view());
    if (!slot.has_value()) {
        return slot.error();
    }
    cube_.checker_slot = slot.value();
    for (const Image& skin : body_drawing_.skins) {
        const Result<std::uint32_t> skin_slot = textures_.add(skin.view());
        if (!skin_slot.has_value()) {
            return skin_slot.error();
        }
        body_drawing_.slots.push_back(skin_slot.value());
    }
    if (text_drawing_.font.has_value()) {
        const Result<std::uint32_t> font_slot = textures_.add(text_drawing_.font->view());
        if (!font_slot.has_value()) {
            return font_slot.error();
        }
        text_drawing_.slot = font_slot.value();
    }
    textures_.set_block_textures(block_textures_.view());
    return {};
}

// Starts ImGui's Vulkan backend on the app's context: the overlay's pipeline
// for the target's format, and the font texture, which it uploads itself,
// through a command buffer of its own, when the overlay first draws.
Result<void> Renderer::start_overlay(ImGuiContext* overlay) {
    ZoneScopedN("Start overlay");
    ImGui::SetCurrentContext(overlay);
    const VkFormat format = cube_.pipeline.color_format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = context_.instance().handle();
    info.PhysicalDevice = context_.device().physical_device();
    info.Device = context_.device().handle();
    info.QueueFamily = context_.device().queue_family();
    info.Queue = context_.device().queue();
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    // ImGui reuses its vertex buffers after this many frames: the frames in
    // flight, each done before its slot comes round again.
    info.MinImageCount = frames_in_flight;
    info.ImageCount = frames_in_flight;
    info.UseDynamicRendering = true;
    info.PipelineInfoMain = overlay_pipeline_info(&format);
    if (!ImGui_ImplVulkan_Init(&info)) {
        return Error{"ImGui's Vulkan backend did not start"};
    }
    overlay_ = overlay;
    return {};
}

// Every pipeline again, for targets of `format`.
Result<void> Renderer::rebuild_pipelines(VkFormat format) {
    Result<GraphicsPipeline> cube =
        make_cube_pipeline(context_.device(), format, textures_.layout());
    if (!cube.has_value()) {
        return cube.error();
    }
    cube_.pipeline = std::move(cube).value();
    Result<BodyResources> bodies =
        make_body_resources(context_.device(), format, textures_.layout(), 0);
    if (!bodies.has_value()) {
        return bodies.error();
    }
    body_drawing_.pipeline = std::move(bodies.value().pipeline);
    Result<TextResources> text =
        make_text_resources(context_.device(), format, textures_.layout(), false);
    if (!text.has_value()) {
        return text.error();
    }
    text_drawing_.see_through = std::move(text.value().see_through);
    text_drawing_.normal = std::move(text.value().normal);
    Result<ChunkPipelines> chunk_pipelines =
        make_chunk_pipelines(context_.device(), format, textures_.layout());
    if (!chunk_pipelines.has_value()) {
        return chunk_pipelines.error();
    }
    chunk_drawing_.opaque = std::move(chunk_pipelines.value().opaque);
    chunk_drawing_.cutout = std::move(chunk_pipelines.value().cutout);
    chunk_drawing_.wireframe = std::move(chunk_pipelines.value().wireframe);
    if (overlay_ != nullptr) {
        ImGui::SetCurrentContext(overlay_);
        const ImGui_ImplVulkan_PipelineInfo overlay_pipeline = overlay_pipeline_info(&format);
        ImGui_ImplVulkan_CreateMainPipeline(&overlay_pipeline);
    }
    return {};
}

Result<void> Renderer::recreate_target() {
    ZoneScopedN("Recreate target");
    // The last frames, and their presentation, may still use the old target.
    vkDeviceWaitIdle(context_.device().handle());
    VkExtent2D extent = vk_extent(framebuffer_);
    if (swapchain_.has_value()) {
        Result<Swapchain> replacement =
            Swapchain::create(context_.device(), context_.surface(), framebuffer_, present_mode_,
                              swapchain_->handle());
        if (!replacement.has_value()) {
            return replacement.error();
        }
        swapchain_ = std::move(replacement).value(); // destroys the old one
        swapchain_stale_ = false;
        extent = swapchain_->choice().extent; // the surface may have its own
        // A surface may offer a different format after a change, such as a
        // move to another display.
        if (const VkFormat format = swapchain_->choice().format.format;
            format != cube_.pipeline.color_format()) {
            if (const Result<void> rebuilt = rebuild_pipelines(format); !rebuilt.has_value()) {
                return rebuilt.error();
            }
        }
    } else {
        Result<Image> replacement =
            make_offscreen_target(context_.device(), vk_extent(framebuffer_));
        if (!replacement.has_value()) {
            return replacement.error();
        }
        offscreen_ = std::move(replacement).value();
    }
    Result<Image> depth = make_depth_target(context_.device(), extent);
    if (!depth.has_value()) {
        return depth.error();
    }
    depth_ = std::move(depth).value();
    target_framebuffer_ = framebuffer_;
    return add_rendered_semaphores();
}

void Renderer::set_present_mode(PresentMode mode) noexcept {
    if (mode != present_mode_) {
        present_mode_ = mode;
        swapchain_stale_ = swapchain_.has_value(); // replaced on the next frame
    }
}

namespace {

// What a frame may ask for: at most max_cubes cubes and max_players bodies,
// and an overlay only of a renderer made with an overlay context.
Result<void> check_frame_inputs(const FrameInputs& inputs, bool has_overlay) {
    if (inputs.cubes.size() > Renderer::max_cubes) {
        return Error{"a frame draws at most " + std::to_string(Renderer::max_cubes) +
                     " cubes, not " + std::to_string(inputs.cubes.size())};
    }
    if (inputs.players.size() > Renderer::max_players) {
        return Error{"a frame draws at most " + std::to_string(Renderer::max_players) +
                     " bodies, not " + std::to_string(inputs.players.size())};
    }
    if (inputs.tags.size() > Renderer::max_tags) {
        return Error{"a frame draws at most " + std::to_string(Renderer::max_tags) +
                     " name tags, not " + std::to_string(inputs.tags.size())};
    }
    if (inputs.overlay != nullptr && !has_overlay) {
        return Error{"a frame's overlay needs a renderer made with an overlay context"};
    }
    return {};
}

} // namespace

// Each body is posed in doubles relative to the block its feet are in, then
// copied into the slot's vertices; that block, from the camera's block, goes
// into the slot's origins, subtracted in 64-bit integers as the cubes' are.
Result<std::uint32_t> Renderer::write_bodies(std::span<const PlayerPose> players,
                                             const glm::i64vec3& camera_block, std::size_t slot,
                                             std::span<std::uint32_t> skins) {
    const Buffer& vertices = body_vertices_[slot];
    const Buffer& origins = body_origins_[slot];
    constexpr std::size_t body_bytes = player_vertices * sizeof(BodyVertex);
    for (std::size_t body = 0; body < players.size(); ++body) {
        const PlayerPose& pose = players[body];
        const BlockPos block{.x = static_cast<std::int32_t>(std::floor(pose.feet.x)),
                             .y = static_cast<std::int32_t>(std::floor(pose.feet.y)),
                             .z = static_cast<std::int32_t>(std::floor(pose.feet.z))};
        pose_player(pose, block, posed_);
        std::memcpy(vertices.mapping().subspan(body * body_bytes).data(), posed_.data(),
                    body_bytes);
        const glm::vec4 origin(relative_origin({block.x, block.y, block.z}, camera_block), 0.0F);
        std::memcpy(origins.mapping().subspan(body * sizeof(glm::vec4)).data(), &origin,
                    sizeof(origin));
        // A skin the renderer was not given shows the missing texture, the last.
        skins[body] = body_drawing_.slots.at(
            std::min<std::size_t>(pose.skin, body_drawing_.slots.size() - 1));
    }
    VmaAllocator allocator = context_.device().allocator();
    for (const Buffer* buffer : {&vertices, &origins}) {
        if (const VkResult result =
                vmaFlushAllocation(allocator, buffer->allocation(), 0, VK_WHOLE_SIZE);
            result != VK_SUCCESS) {
            return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
        }
    }
    return static_cast<std::uint32_t>(players.size());
}

// Name tags show within 64 blocks of the camera, a sneaking player's within
// 32 (26.2's name tag distance), and are drawn the farthest first, so the
// nearest is drawn over the rest. Each is laid out relative to the block its
// anchor is in, then copied into the slot's vertices; that block, from the
// camera's block, goes into the slot's origins.
Result<std::uint32_t> Renderer::write_tags(std::span<const Tag> tags, const Camera& camera,
                                           const glm::i64vec3& camera_block, std::size_t slot,
                                           std::span<TagRun> runs) {
    if (!text_drawing_.font.has_value()) {
        return 0U;
    }
    std::array<double, max_tags> distances{};
    std::size_t shown = 0;
    for (std::size_t tag = 0; tag < tags.size(); ++tag) {
        const Vec3d offset =
            tags[tag].anchor -
            Vec3d{.x = camera.position.x, .y = camera.position.y, .z = camera.position.z};
        const double squared =
            (offset.x * offset.x) + (offset.y * offset.y) + (offset.z * offset.z);
        const double reach = tags[tag].discreet ? 32.0 : 64.0;
        if (squared <= reach * reach) {
            distances.at(tag) = squared;
            tag_order_.at(shown++) = tag;
        }
    }
    const auto order = std::span(tag_order_).first(shown);
    std::ranges::sort(order, [&](std::size_t a, std::size_t b) {
        return distances.at(a) != distances.at(b) ? distances.at(a) > distances.at(b) : a < b;
    });

    const Buffer& vertices = tag_vertices_[slot];
    const Buffer& origins = tag_origins_[slot];
    std::uint32_t first = 0;
    for (std::size_t at = 0; at < shown; ++at) {
        const Tag& tag = tags[order[at]];
        const BlockPos block{.x = static_cast<std::int32_t>(std::floor(tag.anchor.x)),
                             .y = static_cast<std::int32_t>(std::floor(tag.anchor.y)),
                             .z = static_cast<std::int32_t>(std::floor(tag.anchor.z))};
        const std::size_t count =
            layout_tag(tag, text_drawing_.advances, camera.yaw, camera.pitch, block, laid_);
        std::memcpy(vertices.mapping().subspan(std::size_t{first} * sizeof(TagVertex)).data(),
                    laid_.data(), count * sizeof(TagVertex));
        const glm::vec4 origin(relative_origin({block.x, block.y, block.z}, camera_block), 0.0F);
        std::memcpy(origins.mapping().subspan(at * sizeof(glm::vec4)).data(), &origin,
                    sizeof(origin));
        runs[at] = {
            .first = first, .count = static_cast<std::uint32_t>(count), .discreet = tag.discreet};
        first += static_cast<std::uint32_t>(count);
    }
    VmaAllocator allocator = context_.device().allocator();
    for (const Buffer* buffer : {&vertices, &origins}) {
        if (const VkResult result =
                vmaFlushAllocation(allocator, buffer->allocation(), 0, VK_WHOLE_SIZE);
            result != VK_SUCCESS) {
            return Error{"vmaFlushAllocation failed: " + vk_result_name(result)};
        }
    }
    return static_cast<std::uint32_t>(shown);
}

// The frame's bodies and their name tags, into frame slot `slot`'s buffers.
Result<FigureCounts> Renderer::write_figures(const FrameInputs& inputs,
                                             const glm::i64vec3& camera_block, std::size_t slot,
                                             std::span<std::uint32_t> skins,
                                             std::span<TagRun> runs) {
    const Result<std::uint32_t> bodies = write_bodies(inputs.players, camera_block, slot, skins);
    if (!bodies.has_value()) {
        return bodies.error();
    }
    const Result<std::uint32_t> tags =
        write_tags(inputs.tags, inputs.camera, camera_block, slot, runs);
    if (!tags.has_value()) {
        return tags.error();
    }
    return FigureCounts{.bodies = bodies.value(), .tags = tags.value()};
}

Result<void> Renderer::refresh_target() {
    if (!swapchain_stale_ && framebuffer_.width == target_framebuffer_.width &&
        framebuffer_.height == target_framebuffer_.height) {
        return {};
    }
    return recreate_target();
}

Result<Frame> Renderer::render(const FrameInputs& inputs) {
    ZoneScopedN("Render");
    if (const Result<void> valid = check_frame_inputs(inputs, overlay_ != nullptr);
        !valid.has_value()) {
        return valid.error();
    }
    if (framebuffer_.width == 0 || framebuffer_.height == 0) {
        return Frame::skipped;
    }
    if (overlay_ != nullptr) {
        ImGui::SetCurrentContext(overlay_); // ImGui's backend keeps its state there
    }
    if (const Result<void> refreshed = refresh_target(); !refreshed.has_value()) {
        return refreshed.error();
    }

    VkDevice device = context_.device().handle();
    VkQueue queue = context_.device().queue();
    const std::uint64_t frame = frame_ + 1;
    const auto slot_index = static_cast<std::uint32_t>(frame % frames_in_flight);
    const FrameSlot& slot = slots_[slot_index];
    std::chrono::nanoseconds waited{0};
    // The slot is free once the last frame that used it is done. The timeline
    // starts at 0, so the first frames do not wait.
    const std::uint64_t slot_free = frame > frames_in_flight ? frame - frames_in_flight : 0;
    if (const Result<void> ready = wait_for_slot(device, frames_done_, slot_free, waited);
        !ready.has_value()) {
        return ready.error();
    }
    // That frame's timestamps are in, then, and the slot can be timed again.
    times_.gpu = gpu_timer_.read(slot_index);
    const VkSemaphoreSubmitInfo frame_done =
        semaphore_submit(frames_done_, frame, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

    // The slot's origins are free with the slot.
    const BlockAndFraction split = split_position(inputs.camera.position);
    const Buffer& origins = origins_[frame % frames_in_flight];
    const Buffer& fog = fog_[frame % frames_in_flight];
    if (const Result<void> written =
            write_cubes_and_fog(context_.device().allocator(), origins, fog, inputs, split.block);
        !written.has_value()) {
        return written.error();
    }
    const auto cubes = static_cast<std::uint32_t>(inputs.cubes.size());
    BodyDraw bodies{.pipeline = &body_drawing_.pipeline, .push = {}, .count = 0, .skins = {}};
    const auto body_draw = [&](VkExtent2D extent) {
        BodyDraw draw = bodies;
        draw.push = {.draw = make_draw_push(inputs.camera, split, extent,
                                            body_vertices_[frame % frames_in_flight].address(),
                                            body_origins_[frame % frames_in_flight].address(), 0),
                     .fog = fog.address(),
                     .padding = 0};
        return draw;
    };
    TagDraw tags{.see_through = &text_drawing_.see_through,
                 .normal = &text_drawing_.normal,
                 .push = {},
                 .count = 0,
                 .runs = {}};
    const Result<FigureCounts> figures =
        write_figures(inputs, split.block, frame % frames_in_flight, bodies.skins, tags.runs);
    if (!figures.has_value()) {
        return figures.error();
    }
    bodies.count = figures.value().bodies;
    tags.count = figures.value().tags;
    const auto tag_draw = [&](VkExtent2D extent) {
        TagDraw draw = tags;
        draw.push.draw = make_draw_push(
            inputs.camera, split, extent, tag_vertices_[frame % frames_in_flight].address(),
            tag_origins_[frame % frames_in_flight].address(), text_drawing_.slot);
        return draw;
    };
    // The chunks as a target of `extent` sees them; the arena, which a
    // repack replaces, and the frame's origins and commands come once the
    // arena's upkeep is done and the draw lists are built.
    const auto chunk_draw = [&](VkExtent2D extent) {
        return ChunkDraw{.opaque_pipeline = &chunk_drawing_.opaque,
                         .cutout_pipeline = &chunk_drawing_.cutout,
                         .wireframe_pipeline = wireframe() ? &*chunk_drawing_.wireframe : nullptr,
                         .indices = chunk_drawing_.indices.handle(),
                         .push = make_chunk_push(inputs.camera, split, extent, 0, 0, block_filter_,
                                                 occlusion_strength_, fog.address()),
                         .frame = {},
                         .draw_limit = chunks_.draw_limit()};
    };
    const auto chunk_view = [&](VkExtent2D extent) {
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        return ChunkView{.device = &context_.device(),
                         .changes = inputs.chunk_changes,
                         .camera_block = split.block,
                         .camera_fraction = split.fraction,
                         .frustum = view_frustum(inputs.camera, aspect)};
    };

    if (offscreen_.has_value()) {
        vkResetCommandPool(device, slot.command_pool, 0);
        const VkExtent2D extent = offscreen_->extent();
        if (const Result<void> recorded = record_frame(
                slot.command_buffer, gpu_timer_, slot_index, gpu_profiler_,
                {.kind = Target::offscreen,
                 .image = offscreen_->image(),
                 .view = offscreen_->view(),
                 .extent = extent,
                 .depth_image = depth_.image(),
                 .depth_view = depth_.view()},
                {.clear = inputs.sky,
                 .pipeline = &cube_.pipeline,
                 .textures = textures_.set(),
                 .push = make_draw_push(inputs.camera, split, extent, cube_.vertices.address(),
                                        origins.address(), cube_.checker_slot),
                 .cubes = cubes,
                 .chunk = chunk_draw(extent),
                 .bodies = body_draw(extent),
                 .tags = tag_draw(extent),
                 .overlay = inputs.overlay},
                chunks_, chunk_view(extent));
            !recorded.has_value()) {
            return recorded.error();
        }
        if (const Result<void> submitted =
                submit_frame(queue, slot.command_buffer, {}, std::span(&frame_done, 1), waited);
            !submitted.has_value()) {
            return submitted.error();
        }
        frame_ = frame;
        times_.waited = waited;
        return Frame::drawn;
    }

    assert(swapchain_.has_value());
    const Swapchain& swapchain = *swapchain_;
    std::uint32_t index = 0;
    const VkResult acquired =
        acquire_image(device, swapchain.handle(), slot.image_acquired, index, waited);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        swapchain_stale_ = true;
        return Frame::skipped;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        return Error{"vkAcquireNextImageKHR failed: " + vk_result_name(acquired)};
    }
    swapchain_stale_ = acquired == VK_SUBOPTIMAL_KHR; // draw this frame, replace after it

    // The arena changes only now, past the last way to skip the frame, so
    // its staged copies are always submitted.
    vkResetCommandPool(device, slot.command_pool, 0);
    const VkExtent2D extent = swapchain.choice().extent;
    if (const Result<void> recorded = record_frame(
            slot.command_buffer, gpu_timer_, slot_index, gpu_profiler_,
            {.kind = Target::swapchain,
             .image = swapchain.images()[index],
             .view = swapchain.views()[index],
             .extent = extent,
             .depth_image = depth_.image(),
             .depth_view = depth_.view()},
            {.clear = inputs.sky,
             .pipeline = &cube_.pipeline,
             .textures = textures_.set(),
             .push = make_draw_push(inputs.camera, split, extent, cube_.vertices.address(),
                                    origins.address(), cube_.checker_slot),
             .cubes = cubes,
             .chunk = chunk_draw(extent),
             .bodies = body_draw(extent),
             .tags = tag_draw(extent),
             .overlay = inputs.overlay},
            chunks_, chunk_view(extent));
        !recorded.has_value()) {
        return recorded.error();
    }

    const VkSemaphoreSubmitInfo wait =
        semaphore_submit(slot.image_acquired, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    const std::array<VkSemaphoreSubmitInfo, 2> signals = {
        semaphore_submit(image_rendered_[index], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT),
        frame_done,
    };
    if (const Result<void> submitted =
            submit_frame(queue, slot.command_buffer, std::span(&wait, 1), signals, waited);
        !submitted.has_value()) {
        return submitted.error();
    }
    frame_ = frame;

    const VkResult presented =
        present_image(queue, swapchain.handle(), index, image_rendered_[index], waited);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        swapchain_stale_ = true;
    } else if (presented != VK_SUCCESS) {
        return Error{"vkQueuePresentKHR failed: " + vk_result_name(presented)};
    }
    times_.waited = waited;
    return Frame::drawn;
}

Result<std::vector<std::uint8_t>> Renderer::capture() {
    assert(offscreen_.has_value() && "capture reads back headless frames");
    if (frame_ == 0) {
        return Error{"there is no frame to capture yet"};
    }
    const Image& image = *offscreen_;
    VkDevice device = context_.device().handle();
    VmaAllocator allocator = context_.device().allocator();
    VkQueue queue = context_.device().queue();
    vkDeviceWaitIdle(device); // every frame is done, and every slot is free

    const VkExtent2D extent = image.extent();
    Result<Buffer> readback = Buffer::create(
        context_.device(), VkDeviceSize{extent.width} * extent.height * 4,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::readback, "capture readback");
    if (!readback.has_value()) {
        return readback.error();
    }
    const Buffer& buffer = readback.value();

    VkCommandBuffer commands = slots_[0].command_buffer;
    vkResetCommandPool(device, slots_[0].command_pool, 0);
    begin_commands(commands);
    transition(commands, image.image(), VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .pNext = nullptr,
        .bufferOffset = 0,
        .bufferRowLength = 0, // tightly packed
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {.x = 0, .y = 0, .z = 0},
        .imageExtent = {.width = extent.width, .height = extent.height, .depth = 1},
    };
    const VkCopyImageToBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .pNext = nullptr,
        .srcImage = image.image(),
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = buffer.handle(),
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyImageToBuffer2(commands, &copy);
    // The host reads the buffer once the queue is idle; the copy must be visible to it.
    const VkBufferMemoryBarrier2 to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .pNext = nullptr,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.handle(),
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext = nullptr,
        .dependencyFlags = 0,
        .memoryBarrierCount = 0,
        .pMemoryBarriers = nullptr,
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &to_host,
        .imageMemoryBarrierCount = 0,
        .pImageMemoryBarriers = nullptr,
    };
    vkCmdPipelineBarrier2(commands, &dependency);
    vkEndCommandBuffer(commands);

    if (const Result<void> submitted = submit(queue, commands, {}, {}); !submitted.has_value()) {
        return submitted.error();
    }
    vkQueueWaitIdle(queue);
    vmaInvalidateAllocation(allocator, buffer.allocation(), 0, VK_WHOLE_SIZE);

    const std::span<const std::byte> bytes = buffer.mapping();
    std::vector<std::uint8_t> pixels(bytes.size());
    std::memcpy(pixels.data(), bytes.data(), bytes.size());
    return pixels;
}

Result<void> Renderer::set_chunk(ChunkPos position, const ChunkMesh& mesh) {
    ZoneScopedN("Set chunk");
    const Device& device = context_.device();
    // Outside any frame: once the frames in flight are done, no frame slot's
    // commands are in use.
    vkDeviceWaitIdle(device.handle());
    ChunkUploads& uploads = chunks_.uploads();
    if (set_chunk_at_.has_value()) {
        uploads.remove(*set_chunk_at_);
        set_chunk_at_.reset();
    }
    if (mesh.all().empty()) {
        return {};
    }
    // Through frame slot 0's commands, as one edit's batch: outside the
    // budget, written in place or staged.
    const FrameSlot& slot = slots_[0];
    vkResetCommandPool(device.handle(), slot.command_pool, 0);
    begin_commands(slot.command_buffer);
    UploadContext staging(device);
    ArenaWriter writer(staging, slot.command_buffer, chunks_.arena());
    const ChunkMeshUpdate update{
        .pos = position, .generation = ++set_chunk_generation_, .mesh = mesh};
    const Result<Uploaded> uploaded = uploads.upload_batch(std::span(&update, 1), writer);
    staging.finish(slot.command_buffer);
    vkEndCommandBuffer(slot.command_buffer);
    if (!uploaded.has_value()) {
        return uploaded.error();
    }
    if (uploaded.value() == Uploaded::full) {
        return Error{"the chunk arena has no room for " + std::to_string(mesh.all().size()) +
                     " quads"};
    }
    if (const Result<void> submitted = submit(device.queue(), slot.command_buffer, {}, {});
        !submitted.has_value()) {
        return submitted.error();
    }
    vkQueueWaitIdle(device.queue());
    set_chunk_at_ = position;
    return {};
}

void Renderer::destroy() noexcept {
    VkDevice device = context_.device().handle();
    if (device == VK_NULL_HANDLE) {
        return; // moved from
    }
    vkDeviceWaitIdle(device);
    if (gpu_profiler_ != nullptr) {
        TracyVkDestroy(gpu_profiler_);
        gpu_profiler_ = nullptr;
    }
    if (overlay_ != nullptr) {
        ImGui::SetCurrentContext(overlay_);
        ImGui_ImplVulkan_Shutdown();
        overlay_ = nullptr;
    }
    for (VkSemaphore semaphore : image_rendered_) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    image_rendered_.clear();
    vkDestroySemaphore(device, frames_done_, nullptr);
    for (const FrameSlot& slot : slots_) {
        vkDestroySemaphore(device, slot.image_acquired, nullptr);
        vkDestroyCommandPool(device, slot.command_pool, nullptr); // frees its command buffer
    }
}

} // namespace vx::render
