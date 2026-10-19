// RFC 0013's fog, as Minecraft 26.2 draws it: the larger of a haze on the
// distance from the eye and a band at the view's edge on the larger of the
// distance across and the height difference, each linear from its start to
// its end, mixed toward the fog color in sRGB-encoded values, as 26.2 mixes
// them. Imported by the chunk and body shaders.
module fog;

// A frame's fog, read through its address: two float4s are laid out the
// same under every buffer layout rule.
public struct Fog {
    public float4 color; // the fog color, sRGB-encoded; w is 1 with the fog on, 0 off
    public float4 ramps; // the haze's end, the band's start and end, in blocks
};

// What the fog needs of a vertex `from_eye` blocks from the eye: its
// distance, and the larger of its distance across and its height
// difference. Per vertex and interpolated, as 26.2 does.
public float2 fog_distances(float3 from_eye) {
    return float2(length(from_eye), max(length(from_eye.xz), abs(from_eye.y)));
}

// 0 up to `start`, 1 from `end`, linear between.
float ramp(float at, float start, float end) {
    return saturate((at - start) / (end - start));
}

// The sRGB transfer function and its inverse, exact.
float3 encode(float3 linear) {
    return select(linear <= 0.0031308, linear * 12.92, (1.055 * pow(linear, 1.0 / 2.4)) - 0.055);
}

float3 decode(float3 encoded) {
    return select(encoded <= 0.04045, encoded / 12.92, pow((encoded + 0.055) / 1.055, 2.4));
}

// `color`, in linear light, under the frame's fog at `distances`.
public float3 apply_fog(Fog* fog, float3 color, float2 distances) {
    const Fog frame = fog[0];
    if (frame.color.w == 0.0) {
        return color;
    }
    const float amount = max(ramp(distances.x, 0.0, frame.ramps.x),
                             ramp(distances.y, frame.ramps.y, frame.ramps.z));
    return decode(lerp(encode(color), frame.color.rgb, amount));
}
