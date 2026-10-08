#include <vx/world/collision.hpp>

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>

namespace vx {
namespace {

double along(const Vec3d& v, Axis axis) noexcept {
    switch (axis) {
    case Axis::x:
        return v.x;
    case Axis::y:
        return v.y;
    case Axis::z:
        return v.z;
    }
    return 0.0;
}

// `length` along `axis`, nothing along the other two.
Vec3d on(Axis axis, double length) noexcept {
    switch (axis) {
    case Axis::x:
        return {.x = length, .y = 0.0, .z = 0.0};
    case Axis::y:
        return {.x = 0.0, .y = length, .z = 0.0};
    case Axis::z:
        return {.x = 0.0, .y = 0.0, .z = length};
    }
    return {};
}

std::pair<Axis, Axis> other_axes(Axis axis) noexcept {
    switch (axis) {
    case Axis::x:
        return {Axis::y, Axis::z};
    case Axis::y:
        return {Axis::x, Axis::z};
    case Axis::z:
        return {Axis::x, Axis::y};
    }
    return {Axis::x, Axis::y};
}

// Whether two boxes overlap on `axis` by more than the tolerance.
bool overlap(const Box& a, const Box& b, Axis axis) noexcept {
    return along(a.max, axis) - along(b.min, axis) > collision_tolerance &&
           along(b.max, axis) - along(a.min, axis) > collision_tolerance;
}

} // namespace

Box swept(const Box& box, Vec3d motion) noexcept {
    return {.min = {.x = box.min.x + std::min(motion.x, 0.0),
                    .y = box.min.y + std::min(motion.y, 0.0),
                    .z = box.min.z + std::min(motion.z, 0.0)},
            .max = {.x = box.max.x + std::max(motion.x, 0.0),
                    .y = box.max.y + std::max(motion.y, 0.0),
                    .z = box.max.z + std::max(motion.z, 0.0)}};
}

double clip_axis(const Box& box, Axis axis, double wanted,
                 std::span<const Box> obstacles) noexcept {
    const auto [first, second] = other_axes(axis);
    double allowed = wanted;
    for (const Box& obstacle : obstacles) {
        if (!overlap(box, obstacle, first) || !overlap(box, obstacle, second)) {
            continue;
        }
        if (wanted > 0.0) {
            // From the box's far face to the obstacle's near one.
            const double gap = along(obstacle.min, axis) - along(box.max, axis);
            if (gap >= -collision_tolerance) {
                allowed = std::min(allowed, std::max(gap, 0.0));
            }
        } else if (wanted < 0.0) {
            const double gap = along(obstacle.max, axis) - along(box.min, axis);
            if (gap <= collision_tolerance) {
                allowed = std::max(allowed, std::min(gap, 0.0));
            }
        }
    }
    return allowed;
}

Move clip_move(const Box& box, Vec3d wanted, std::span<const Box> obstacles) noexcept {
    Box at = box;
    const auto step = [&](Axis axis) {
        const double length = clip_axis(at, axis, along(wanted, axis), obstacles);
        at = moved(at, on(axis, length));
        return length;
    };
    Vec3d motion;
    motion.y = step(Axis::y);
    if (std::abs(wanted.x) >= std::abs(wanted.z)) {
        motion.x = step(Axis::x);
        motion.z = step(Axis::z);
    } else {
        motion.z = step(Axis::z);
        motion.x = step(Axis::x);
    }
    return collided(wanted, motion);
}

} // namespace vx
