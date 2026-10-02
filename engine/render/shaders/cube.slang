// The cube's shaders (RFC 0001, pull request plan item 8): vertices pulled
// through a buffer address, each instance placed relative to the camera's
// block, so the GPU never sees a world position.

// Laid out the same under every buffer layout rule, the padding included:
// 48 bytes a vertex, as the renderer writes them.
struct Vertex {
    float4 position; // a corner within the block, from 0 to 1 on each axis
    float4 color;    // linear RGBA, the face's tint
    float2 uv;       // where on the texture
    float2 padding;
};

// RFC 0001's DrawPush: 96 of the 128 bytes Vulkan 1.3 guarantees.
struct DrawPush {
    float4x4 view_projection; // offset 0: rotation and projection only
    Vertex* vertices;         // offset 64
    float4* origins;          // offset 72: one per instance, from the camera's block
    float3 camera_fraction;   // offset 80: the camera's place within its block
    uint texture;             // offset 92: the texture table's slot
};

[[vk::push_constant]] ConstantBuffer<DrawPush> push;

// The texture table (RFC 0001): every texture, and one sampler.
[[vk::binding(0, 0)]] Texture2D textures[];
[[vk::binding(1, 0)]] SamplerState nearest;

struct Varyings {
    float4 position : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD;
};

// An origin plus a block corner is a whole number, exact as a float, so
// neighbors meet without cracks; the first rounding comes when the camera's
// fraction is subtracted, on values that are small near the viewer.
[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID,
                     uint instance : SV_VulkanInstanceID) {
    const Vertex vertex = push.vertices[vertex_index];
    const float3 position = push.origins[instance].xyz + vertex.position.xyz;
    Varyings out;
    out.position = mul(push.view_projection, float4(position - push.camera_fraction, 1.0));
    out.color = vertex.color;
    out.uv = vertex.uv;
    return out;
}

// One texture for the whole draw, so the index needs no non-uniform access.
// It is not a constant, though, so it needs dynamic indexing, a capability
// Slang leaves undeclared: declaring it lets the validation layers check
// that the device enabled the feature.
[shader("fragment")]
float4 fragment_main(Varyings in) : SV_Target {
    spirv_asm { OpCapability SampledImageArrayDynamicIndexing; };
    return textures[push.texture].Sample(nearest, in.uv) * in.color;
}
