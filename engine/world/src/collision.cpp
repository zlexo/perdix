#include <vx/world/collision.hpp>

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <utility>
#include <vector>

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

// The horizontal distance a motion covers, squared.
double horizontal_squared(const Vec3d& v) noexcept {
    return (v.x * v.x) + (v.z * v.z);
}

// One of the edge guard's steps: 0.05 blocks back toward 0, or 0 from under
// that.
double step_back(double motion) noexcept {
    constexpr double step = 0.05;
    if (std::abs(motion) < step) {
        return 0.0;
    }
    return motion > 0.0 ? motion - step : motion + step;
}

// The lowest height above `bottom` of a face of `obstacles` that is more than
// `after` and at most `step_height`, leaving out `skip`; infinity if none is.
double next_face(std::span<const Box> obstacles, double bottom, double after, double step_height,
                 double skip) noexcept {
    double next = std::numeric_limits<double>::infinity();
    const auto consider = [&](double face) {
        const double height = face - bottom;
        if (height > after + collision_tolerance && height <= step_height + collision_tolerance &&
            std::abs(height - skip) > collision_tolerance) {
            next = std::min(next, height);
        }
    };
    for (const Box& obstacle : obstacles) {
        consider(obstacle.min.y);
        consider(obstacle.max.y);
    }
    return next;
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

Move clip_move_stepping(const Box& box, Vec3d wanted, bool started_on_ground, double step_height,
                        std::span<const Box> obstacles) noexcept {
    const Move plain = clip_move(box, wanted, obstacles);
    if (!(step_height > 0.0) || !plain.horizontal_collision() ||
        !(started_on_ground || plain.on_ground)) {
        return plain;
    }
    // From where the box landed, if it did.
    const double landed = plain.on_ground ? plain.motion.y : 0.0;
    const Box base = moved(box, {.x = 0.0, .y = landed, .z = 0.0});
    const double reached = plain.motion.y - landed;
    const double plain_distance = horizontal_squared(plain.motion);
    // Faces from the bottom up: 0 too, unless the plain move reached it.
    double candidate = -2.0 * collision_tolerance;
    while (true) {
        candidate = next_face(obstacles, base.min.y, candidate, step_height, reached);
        if (!std::isfinite(candidate)) {
            return plain;
        }
        const Move raised =
            clip_move(base, {.x = wanted.x, .y = candidate, .z = wanted.z}, obstacles);
        if (horizontal_squared(raised.motion) > plain_distance) {
            return collided(
                wanted,
                {.x = raised.motion.x, .y = landed + raised.motion.y, .z = raised.motion.z});
        }
    }
}

bool overlaps_any(const Box& box, std::span<const Box> obstacles) noexcept {
    return std::ranges::any_of(obstacles, [&box](const Box& obstacle) {
        return overlap(box, obstacle, Axis::x) && overlap(box, obstacle, Axis::y) &&
               overlap(box, obstacle, Axis::z);
    });
}

Vec3d guard_edge(const Box& box, Vec3d wanted, double max_drop,
                 std::span<const Box> obstacles) noexcept {
    // The space under the moved box's bottom, down by the drop and a little
    // more, so that a block exactly that far down still counts.
    const auto supported = [&](double x, double z) {
        const Box below{.min = {.x = box.min.x + x,
                                .y = box.min.y - max_drop - (2.0 * collision_tolerance),
                                .z = box.min.z + z},
                        .max = {.x = box.max.x + x, .y = box.min.y, .z = box.max.z + z}};
        return overlaps_any(below, obstacles);
    };
    // Moving up, or already over a deeper drop, there is no edge to hold the
    // box back at.
    if (wanted.y > 0.0 || !supported(0.0, 0.0)) {
        return wanted;
    }
    constexpr int most_steps = 256;
    const auto back = [](double motion, int steps) {
        return steps < most_steps ? step_back(motion) : 0.0;
    };
    double x = wanted.x;
    double z = wanted.z;
    for (int steps = 0; x != 0.0 && !supported(x, 0.0); ++steps) {
        x = back(x, steps);
    }
    for (int steps = 0; z != 0.0 && !supported(0.0, z); ++steps) {
        z = back(z, steps);
    }
    for (int steps = 0; x != 0.0 && z != 0.0 && !supported(x, z); ++steps) {
        x = back(x, steps);
        z = back(z, steps);
    }
    return {.x = x, .y = wanted.y, .z = z};
}

Move move_box(const Box& box, Vec3d wanted, const BlockBoxes& world, std::vector<Box>& scratch) {
    const auto finite = [](const Vec3d& v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite(wanted) || !finite(box.min) || !finite(box.max)) {
        return {};
    }
    scratch.clear();
    world.collect(swept(box, wanted), scratch);
    return clip_move(box, wanted, scratch);
}

} // namespace vx
