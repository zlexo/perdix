// A probe for RFC 0004's block texture array in set 0, for tests: a triangle
// over the whole target, every pixel the array's color at one point, layer
// and mip level, given in a push constant, through binding 3's sampler.

struct Probe {
    float2 uv;   // 0 to 1 across a layer; past 1, the sampler's addressing
    float layer;
    float level; // a fraction samples between two levels
};

[[vk::push_constant]] ConstantBuffer<Probe> probe;

// The texture table's block texture array and its sampler (RFC 0004).
[[vk::binding(2, 0)]] Texture2DArray blocks;
[[vk::binding(3, 0)]] SamplerState block_sampler;

// (-1, -1), (3, -1) and (-1, 3): one triangle that covers the target.
[shader("vertex")]
float4 vertex_main(uint vertex_index : SV_VulkanVertexID) : SV_Position {
    const float2 corner = float2(float((vertex_index << 1) & 2), float(vertex_index & 2));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

[shader("fragment")]
float4 fragment_main() : SV_Target {
    return blocks.SampleLevel(block_sampler, float3(probe.uv, probe.layer), probe.level);
}
