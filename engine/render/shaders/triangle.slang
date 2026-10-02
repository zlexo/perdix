// The first pipeline's shaders (RFC 0001, pull request plan item 7): a
// triangle whose vertices the vertex shader pulls from a buffer, through the
// buffer's device address in a push constant. There is no vertex input state.

// Both fields are 16 bytes, so the layout is the same under every buffer
// layout rule: 32 bytes a vertex, as the renderer writes them.
struct Vertex {
    float4 position; // clip space
    float4 color;    // linear RGBA
};

struct Push {
    Vertex* vertices;
};

[[vk::push_constant]] ConstantBuffer<Push> push;

struct Varyings {
    float4 position : SV_Position;
    float4 color : COLOR;
};

// SV_VulkanVertexID is Vulkan's vertex index as it is. SV_VertexID would
// subtract the base vertex, as Direct3D does, which needs the
// shaderDrawParameters feature the feature floor does not require.
[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID) {
    const Vertex vertex = push.vertices[vertex_index];
    Varyings out;
    out.position = vertex.position;
    out.color = vertex.color;
    return out;
}

[shader("fragment")]
float4 fragment_main(Varyings in) : SV_Target {
    return in.color;
}
