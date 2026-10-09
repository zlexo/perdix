// RFC 0007's draw overhead benchmark: quads pulled from a buffer, as the chunk
// shader pulls RFC 0003's, each drawn flat. A run finds its quads through the
// command's vertexOffset and its place through firstInstance, as RFC 0007's
// commands do; a cluster finds both through its instance.

struct BenchQuad {
    float2 corner; // its lowest corner, in clip space
    float size;
    uint unused;
};

struct BenchPush {
    BenchQuad* quads;   // offset 0
    float4* origins;    // offset 8: one a run, at its firstInstance
    uint cluster_quads; // offset 16: quads a cluster, or 0 for runs
    uint padding[3];
};

[[vk::push_constant]] ConstantBuffer<BenchPush> push;

struct Varyings {
    float4 position : SV_Position;
};

[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID) {
    uint quad_index = vertex_index >> 2;
    float2 offset = float2(0.0, 0.0);
    if (push.cluster_quads != 0u) {
        quad_index += instance * push.cluster_quads;
    } else {
        offset = push.origins[instance].xy;
    }
    const BenchQuad quad = push.quads[quad_index];
    const uint corner = vertex_index & 3u;
    const float2 along = float2(corner == 1u || corner == 2u ? quad.size : 0.0,
                                corner >= 2u ? quad.size : 0.0);
    Varyings result;
    result.position = float4(quad.corner + along + offset, 0.5, 1.0);
    return result;
}

[shader("fragment")]
float4 fragment_main(Varyings input) : SV_Target {
    return float4(0.2, 0.6, 0.3, 1.0);
}
