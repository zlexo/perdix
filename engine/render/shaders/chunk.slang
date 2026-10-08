// A chunk's quads (RFC 0003), drawn with one shared index buffer: each quad
// is four vertices, 0, 1, 2, 2, 3, 0, so a vertex finds its quad at
// vertex_index >> 2 and its place in it at vertex_index & 3. The vertex
// shader rebuilds its corner from the quad's 8 bytes, as vx/mesh/quad.hpp
// lays them out; nothing else is stored per vertex.

struct Quad {
    uint geometry; // x, y, z: 5 bits each | width - 1, height - 1: 5 each | face: 3
    uint material; // texture layer: 16 | occlusion, 2 bits a corner, corner 0 lowest: 8
};

// The renderer's ChunkPush: 112 of the 128 bytes Vulkan 1.3 guarantees.
struct ChunkPush {
    float4x4 view_projection; // offset 0: rotation and projection only
    Quad* quads;              // offset 64: the chunk's quads, opaque then cutout
    float3 origin;            // offset 80: the chunk's lowest corner, from the camera's block
    float occlusion_strength; // offset 92: 0 leaves corners lit, 1 darkens them fully
    float3 camera_fraction;   // offset 96: the camera's place within its block
    uint filter;              // offset 108: 0 sharpened, 1 sharpened without anisotropy, 2 nearest
};

[[vk::push_constant]] ConstantBuffer<ChunkPush> push;

// Set 0 (RFC 0001, RFC 0004): the texture table's nearest sampler, the block
// texture array, and its linear samplers, anisotropic and not.
[[vk::binding(1, 0)]] SamplerState nearest;
[[vk::binding(2, 0)]] Texture2DArray blocks;
[[vk::binding(3, 0)]] SamplerState linear_anisotropic;
[[vk::binding(4, 0)]] SamplerState linear;

struct Varyings {
    float4 position : SV_Position;
    float3 uv : TEXCOORD; // one image a block, repeating; z is the layer
    float light : COLOR;  // the face's shade, times its corner's occlusion
};

// For each face, +x, -x, +y, -y, +z, -z: the axis it looks along and those
// its width and height run along (RFC 0003), and whether its corners 0, 1,
// 2, 3 turn clockwise seen from outside, so the vertices take them
// backwards. vertex_corner in vx/mesh/quad.hpp is the same rule.
static const uint3 axes[6] = { uint3(0, 2, 1), uint3(0, 2, 1), uint3(1, 0, 2),
                               uint3(1, 0, 2), uint3(2, 0, 1), uint3(2, 0, 1) };
static const bool clockwise[6] = { true, false, true, false, false, true };

// Each face's shade, as Minecraft shades faces by the way they look: tops
// fully, north and south 0.8, east and west 0.6, bottoms 0.5. The light
// RFC (M6) takes this over.
static const float shade[6] = { 0.6, 0.6, 1.0, 0.5, 0.8, 0.8 };

uint field(uint word, uint first, uint bits) {
    return (word >> first) & ((1u << bits) - 1u);
}

[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID) {
    const Quad quad = push.quads[vertex_index >> 2];
    const uint face = field(quad.geometry, 25, 3);
    const uint4 occlusion = uint4(field(quad.material, 16, 2), field(quad.material, 18, 2),
                                  field(quad.material, 20, 2), field(quad.material, 22, 2));
    // The split along the brighter diagonal, then the winding (RFC 0003).
    const bool rotate = occlusion.y + occlusion.w > occlusion.x + occlusion.z;
    uint corner = ((vertex_index & 3u) + (rotate ? 1u : 0u)) & 3u;
    if (clockwise[face]) {
        corner = (4u - corner) & 3u;
    }

    // The corner: the quad's block, one further along a +x, +y or +z face's
    // normal, then its width and height along their axes.
    float3 position = float3(field(quad.geometry, 0, 5), field(quad.geometry, 5, 5),
                             field(quad.geometry, 10, 5));
    const uint3 axis = axes[face];
    if ((face & 1u) == 0u) {
        position[axis.x] += 1.0;
    }
    if (corner == 1u || corner == 2u) {
        position[axis.y] += float(field(quad.geometry, 15, 5) + 1u);
    }
    if (corner == 2u || corner == 3u) {
        position[axis.z] += float(field(quad.geometry, 20, 5) + 1u);
    }

    // Each face shows its image unmirrored from outside (RFC 0004): on the
    // sides with its top edge up, on the top face toward north (-z), on the
    // bottom face toward south. In blocks, so it repeats once a block.
    float2 uv;
    switch (face) {
    case 0: uv = float2(-position.z, -position.y); break; // +x: right is -z
    case 1: uv = float2(position.z, -position.y); break;  // -x: right is +z
    case 2: uv = float2(position.x, position.z); break;   // +y: down the image is south
    case 3: uv = float2(position.x, -position.z); break;  // -y: down the image is north
    case 4: uv = float2(position.x, -position.y); break;  // +z: right is +x
    default: uv = float2(-position.x, -position.y); break; // -z: right is -x
    }

    // Lysenko's four levels, darkened as Minecraft's smooth lighting does: 1,
    // 0.8, 0.6 and 0.4 for 3, 2, 1 and 0.
    const float occluded = 0.4 + (0.2 * float(occlusion[corner]));

    Varyings out;
    out.position = mul(push.view_projection,
                       float4(push.origin + position - push.camera_fraction, 1.0));
    out.uv = float3(uv, float(field(quad.material, 0, 16)));
    out.light = shade[face] * lerp(1.0, occluded, push.occlusion_strength);
    return out;
}

// A block texture's color at `uv` (RFC 0004). Sharpened: inside a texel,
// its center; within half a pixel of a texel's edge, a blend across it, so up
// close texels are crisp without crawling at their edges. Where texels are
// smaller than pixels the coordinate is left alone, and mipmaps and
// anisotropic filtering do the work, chosen by the true derivatives. Nearest
// is the classic look, a toggle.
float4 block_color(float3 uv) {
    if (push.filter == 2u) {
        return blocks.Sample(nearest, uv);
    }
    const float2 t = uv.xy * 16.0; // in texels
    const float2 seam = floor(t + 0.5);
    const float2 sharp = seam + clamp((t - seam) / clamp(fwidth(t), 1e-6, 1.0), -0.5, 0.5);
    const float3 at = float3(sharp / 16.0, uv.z);
    if (push.filter == 0u) {
        return blocks.SampleGrad(linear_anisotropic, at, ddx(uv.xy), ddy(uv.xy));
    }
    return blocks.SampleGrad(linear, at, ddx(uv.xy), ddy(uv.xy));
}

[shader("fragment")]
float4 fragment_opaque(Varyings in) : SV_Target {
    const float4 color = block_color(in.uv);
    return float4(color.rgb * in.light, 1.0);
}

// Leaves and glass: a texel below half alpha is a hole (RFC 0004). In a pass
// of its own, after every opaque quad: Apple's GPUs hide surfaces less well
// behind fragments that may be discarded.
[shader("fragment")]
float4 fragment_cutout(Varyings in) : SV_Target {
    const float4 color = block_color(in.uv);
    if (color.a < 0.5) {
        discard;
    }
    return float4(color.rgb * in.light, 1.0);
}
