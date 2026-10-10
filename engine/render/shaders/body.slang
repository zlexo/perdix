// Bodies (RFC 0012): each player's 432 posed vertices, the frame's bodies
// one after another in a buffer pulled through its address. A draw's
// firstVertex starts its body's vertices and its firstInstance picks the
// body's block, from the camera's block, so the GPU never sees a world
// position, as with the cube.

// vx::BodyVertex, 32 bytes: the position and the texel's u, then the
// texel's v and the normal. Two float4s are laid out the same under every
// buffer layout rule.
struct BodyVertex {
    float4 position_u; // blocks, relative to the body's block; the texel's u
    float4 v_normal;   // the texel's v; the outward normal, posed
};

// The renderer's DrawPush, as the cube's: 96 of the 128 bytes Vulkan 1.3
// guarantees. The skin changes from one body's draw to the next.
struct BodyPush {
    float4x4 view_projection; // offset 0: rotation and projection only
    BodyVertex* vertices;     // offset 64: the frame's bodies, 432 vertices each
    float4* origins;          // offset 72: each body's block, from the camera's block
    float3 camera_fraction;   // offset 80: the camera's place within its block
    uint skin;                // offset 92: the body's skin, a slot of the texture table
};

[[vk::push_constant]] ConstantBuffer<BodyPush> push;

// The texture table (RFC 0001): every texture, and one sampler.
[[vk::binding(0, 0)]] Texture2D textures[];
[[vk::binding(1, 0)]] SamplerState nearest;

struct Varyings {
    float4 position : SV_Position;
    float2 texel : TEXCOORD;
    float2 light : COLOR; // seen from outside, and from inside
};

// The game's light for entities (26.2's entity shader): two lights fixed in
// the world, each adding 0.6 of its cosine to an ambient 0.4, at most 1. Up
// is 1, down 0.4, north and south 0.7395, east and west 0.497.
static const float3 light0 = normalize(float3(0.2, 1.0, -0.7));
static const float3 light1 = normalize(float3(-0.2, 1.0, 0.7));

float entity_light(float3 normal) {
    const float2 lit = max(float2(0.0, 0.0), float2(dot(light0, normal), dot(light1, normal)));
    return min(1.0, ((lit.x + lit.y) * 0.6) + 0.4);
}

[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID, uint instance : SV_VulkanInstanceID) {
    const BodyVertex vertex = push.vertices[vertex_index];
    const float3 normal = vertex.v_normal.yzw;
    Varyings out;
    out.position = mul(push.view_projection,
                       float4(push.origins[instance].xyz + vertex.position_u.xyz -
                                  push.camera_fraction,
                              1.0));
    out.texel = float2(vertex.position_u.w, vertex.v_normal.x);
    // A face seen from inside its box, through an outer layer's clear
    // texels, is lit as if it faced the other way, as the game lights it.
    out.light = float2(entity_light(normal), entity_light(-normal));
    return out;
}

// Texels below alpha 0.1 are discarded, as the game's cutout does. No skin
// the game ships has partial alpha, so this draws the pixels its blending
// would, without sorting (RFC 0012, open question 5). The skin is the same
// for the whole draw, so the index needs dynamic indexing, not non-uniform
// access; declaring the capability lets the validation layers check that
// the device enabled it.
[shader("fragment")]
float4 fragment_main(Varyings in, bool front : SV_IsFrontFace) : SV_Target {
    spirv_asm { OpCapability SampledImageArrayDynamicIndexing; };
    const float4 color = textures[push.skin].Sample(nearest, in.texel);
    if (color.a < 0.1) {
        discard;
    }
    return float4(color.rgb * (front ? in.light.x : in.light.y), color.a);
}
