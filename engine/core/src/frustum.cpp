#include <vx/core/frustum.hpp>

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <array>

namespace vx {
namespace {

constexpr double dot(Vec3d a, Vec3d b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

} // namespace

Frustum::Frustum(Vec3d forward, Vec3d right, Vec3d up, double tan_half_fov_y, double aspect,
                 double near) noexcept
    : everything_(false) {
    const double tan_half_fov_x = tan_half_fov_y * aspect;
    // A side plane through the camera: inside, the point's offset along the
    // side axis is at most the tangent times its distance ahead.
    planes_ = {Plane{.normal = forward, .offset = -near},
               Plane{.normal = right + (forward * tan_half_fov_x), .offset = 0.0},
               Plane{.normal = (forward * tan_half_fov_x) - right, .offset = 0.0},
               Plane{.normal = up + (forward * tan_half_fov_y), .offset = 0.0},
               Plane{.normal = (forward * tan_half_fov_y) - up, .offset = 0.0}};
}

bool Frustum::may_contain(Vec3d min, Vec3d max) const noexcept {
    if (everything_) {
        return true;
    }
    // Out only when even the corner farthest along some plane's normal is
    // outside that plane.
    return std::ranges::all_of(planes_, [&](const Plane& plane) {
        const Vec3d farthest{.x = plane.normal.x >= 0.0 ? max.x : min.x,
                             .y = plane.normal.y >= 0.0 ? max.y : min.y,
                             .z = plane.normal.z >= 0.0 ? max.z : min.z};
        return dot(plane.normal, farthest) + plane.offset >= 0.0;
    });
}

} // namespace vx
