#pragma once

#include <vx/core/vec3d.hpp>

#include <array>

namespace vx {

// A view's frustum (RFC 0007): the near plane and the four side planes, with
// the far plane at infinity, as RFC 0001's projection has it. Positions are
// relative to the camera, in doubles, so the test is as exact far from the
// world's origin as near it. Streaming ranks chunks by it (RFC 0006) and the
// renderer culls with it.
class Frustum {
public:
    // A frustum that holds everything.
    Frustum() noexcept = default;
    // The view along `forward`, with `right` and `up` its other axes, all of
    // unit length and at right angles, roll included; `tan_half_fov_y` the
    // tangent of half the vertical field of view, `aspect` width over
    // height, and the near plane `near` ahead of the camera.
    Frustum(Vec3d forward, Vec3d right, Vec3d up, double tan_half_fov_y, double aspect,
            double near) noexcept;

    // False only when the box, relative to the camera, lies wholly outside
    // one plane: a box crossing two planes outside the corner between them
    // still counts, a conservative test.
    [[nodiscard]] bool may_contain(Vec3d min, Vec3d max) const noexcept;

private:
    // Inside a plane: dot(normal, p) + offset >= 0.
    struct Plane {
        Vec3d normal;
        double offset = 0.0;
    };
    std::array<Plane, 5> planes_{};
    bool everything_ = true;
};

} // namespace vx
