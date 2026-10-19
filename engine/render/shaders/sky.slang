// RFC 0013's sky: one triangle over the whole target at the far end of
// reversed-Z, drawn after the chunks with its depth tested for equal, so
// only where nothing was drawn. Each pixel's view ray gives its rise, and
// fog.slang's sky_color the color 26.2's daytime sky has there.

import fog;

// The renderer's SkyPush: 80 of the 128 bytes Vulkan 1.3 guarantees.
struct SkyPush {
    float4x4 inverse_view_projection; // offset 0: from the target to directions from the eye
    Fog* fog;                         // offset 64: the frame's fog and sky (RFC 0013)
};

[[vk::push_constant]] ConstantBuffer<SkyPush> push;

struct Varyings {
    float4 position : SV_Position;
    float2 target : TARGET; // where the pixel is on the target, -1 to 1
};

[shader("vertex")]
Varyings vertex_main(uint vertex_index : SV_VulkanVertexID) {
    // (-1, -1), (3, -1), (-1, 3): every pixel of the target, once.
    const float2 corner = float2(float((vertex_index << 1) & 2), float(vertex_index & 2));
    Varyings out;
    out.target = (corner * 2.0) - 1.0;
    out.position = float4(out.target, 0.0, 1.0);
    return out;
}

[shader("fragment")]
float4 fragment_main(Varyings in) : SV_Target {
    // A point on the near plane, at depth 1 in reversed-Z: seen from the eye
    // at the origin, its direction is the pixel's.
    const float4 on_near = mul(push.inverse_view_projection, float4(in.target, 1.0, 1.0));
    const float3 direction = normalize(on_near.xyz / on_near.w);
    return float4(sky_color(push.fog, direction.y), 1.0);
}
