// Name tags (RFC 0012): each tag's box and glyph quads, laid out on the CPU
// to face the camera, pulled through a buffer address; a draw's firstVertex
// starts its tag's quads and its firstInstance picks the tag's block, from
// the camera's block, as with bodies.

// vx::TagVertex, 32 bytes: the position and whether it samples the font,
// then the texel. Two float4s are laid out the same under every buffer
// layout rule.
struct TagVertex {
    float4 position_ink; // blocks, relative to the tag's block; 1 on a glyph, 0 on the box
    float4 texel;        // on the font's atlas; zw unused
};

// The renderer's TagPush: all 128 bytes Vulkan 1.3 guarantees. The colors
// change from pass to pass.
struct TagPush {
    float4x4 view_projection; // offset 0: rotation and projection only
    TagVertex* vertices;      // offset 64: the frame's tags
    float4* origins;          // offset 72: each tag's block, from the camera's block
    float3 camera_fraction;   // offset 80
    uint font;                // offset 92: the font's atlas, a slot of the texture table
    float4 box;               // offset 96: the box's color, alpha 0 for none
    float4 ink;               // offset 112: the letters' color
};

[[vk::push_constant]] ConstantBuffer<TagPush> push;

// The texture table (RFC 0001): every texture, and one sampler.
[[vk::binding(0, 0)]] Texture2D textures[];
[[vk::binding(1, 0)]] SamplerState nearest;

struct Varyings {
    float4 position : SV_Position;
    float2 texel : TEXCOORD;
    float glyph : COLOR; // 1 on a glyph, 0 on the box
};

[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID) {
    const TagVertex vertex = push.vertices[vertex_index];
    Varyings out;
    out.position = mul(push.view_projection,
                       float4(push.origins[instance].xyz + vertex.position_ink.xyz -
                                  push.camera_fraction,
                              1.0));
    out.texel = vertex.texel.xy;
    out.glyph = vertex.position_ink.w;
    return out;
}

// A glyph's texel tints the letters' color; the box is its color alone. As
// the game's text does, what is below alpha 0.1 is discarded, so a box of
// alpha 0 draws nothing; the rest blends by its alpha.
[shader("fragment")]
float4 fragment_main(Varyings in) : SV_Target {
    spirv_asm { OpCapability SampledImageArrayDynamicIndexing; };
    const float4 color = in.glyph > 0.5 ? textures[push.font].Sample(nearest, in.texel) * push.ink
                                        : push.box;
    if (color.a < 0.1) {
        discard;
    }
    return color;
}
