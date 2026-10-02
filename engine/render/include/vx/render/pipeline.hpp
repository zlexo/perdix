#pragma once

#include <vx/core/result.hpp>
#include <vx/render/device.hpp>

#include <volk.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace vx::render {

// What a graphics pipeline is made of: one SPIR-V module holding both entry
// points, as vx_add_shaders compiles them, and where it draws.
struct GraphicsPipelineDesc {
    std::span<const std::uint32_t> spirv;
    const char* vertex_entry = "vertex_main";
    const char* fragment_entry = "fragment_main";
    VkFormat color_format = VK_FORMAT_UNDEFINED;
    // With a depth format, the pipeline tests and writes depth, reversed-Z:
    // a fragment passes where it is nearer, its depth greater.
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    // Faces wound counterclockwise, seen from outside, are front faces.
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    // Set 0: the texture table's layout, or none.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    // Push constants from offset 0, and the stages that read them.
    std::uint32_t push_constant_size = 0;
    VkShaderStageFlags push_constant_stages = VK_SHADER_STAGE_VERTEX_BIT;
};

// A pipeline layout and a graphics pipeline that draws, through dynamic
// rendering, into one color attachment of `color_format` and, when given one,
// a depth attachment of `depth_format` (RFC 0001). It has
// no vertex input state: vertex pulling reads vertices through buffer
// addresses in push constants. The viewport and scissor are dynamic, so a
// resize does not rebuild it.
class GraphicsPipeline {
public:
    // `name` is the debug name of the pipeline, its layout and its shader
    // module (RFC 0001), and is in its errors too.
    [[nodiscard]] static Result<GraphicsPipeline>
    create(const Device& device, const GraphicsPipelineDesc& desc, std::string_view name);

    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    ~GraphicsPipeline();

    [[nodiscard]] VkPipeline handle() const noexcept { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const noexcept { return layout_; }
    [[nodiscard]] VkFormat color_format() const noexcept { return color_format_; }

private:
    GraphicsPipeline() = default;
    void destroy() noexcept;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkFormat color_format_ = VK_FORMAT_UNDEFINED;
};

} // namespace vx::render
