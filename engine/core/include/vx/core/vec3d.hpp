#pragma once

namespace vx {

// Three doubles: a position in blocks, or a motion in blocks per tick
// (RFC 0010). Doubles keep positions precise far from the world's origin, as
// RFC 0001's camera does; the layers below the renderer use no math library,
// so the app converts at the camera.
struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    friend constexpr bool operator==(const Vec3d&, const Vec3d&) = default;
};

[[nodiscard]] constexpr Vec3d operator+(Vec3d a, Vec3d b) noexcept {
    return {.x = a.x + b.x, .y = a.y + b.y, .z = a.z + b.z};
}

[[nodiscard]] constexpr Vec3d operator-(Vec3d a, Vec3d b) noexcept {
    return {.x = a.x - b.x, .y = a.y - b.y, .z = a.z - b.z};
}

[[nodiscard]] constexpr Vec3d operator*(Vec3d v, double scale) noexcept {
    return {.x = v.x * scale, .y = v.y * scale, .z = v.z * scale};
}

} // namespace vx
