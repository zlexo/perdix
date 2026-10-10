#include <vx/mesh/player_model.hpp>

#include <vx/core/vec3d.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/skin.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

namespace vx {
namespace {

// RFC 0012's model space is the game's, in its pixels: 1 px is 1/16 block,
// +y points down, -z is the face and -x the player's right. Everything below
// is in it until placement.

// A box of the model: its lowest corner relative to its part's pivot and its
// size, in pixels; the top left of its unwrap on the skin, in texels; and how
// far its faces are pushed out on every side, an outer layer's.
struct ModelBox {
    Vec3d corner;
    Vec3d size;
    int u = 0;
    int v = 0;
    double push = 0.0;
};

// A part: its pivot, its box and its outer layer, which moves with it.
struct ModelPart {
    Vec3d pivot;
    ModelBox base;
    ModelBox layer;
};

enum PartIndex : std::uint8_t { head, body, right_arm, left_arm, right_leg, left_leg, part_count };

using Model = std::array<ModelPart, part_count>;

constexpr Vec3d vec(double x, double y, double z) noexcept {
    return {.x = x, .y = y, .z = z};
}

constexpr ModelBox box(Vec3d corner, Vec3d size, int u, int v, double push = 0.0) noexcept {
    return {.corner = corner, .size = size, .u = u, .v = v, .push = push};
}

// 26.2's player model with wide arms (RFC 0012's table). No box is mirrored:
// the left limbs have texels of their own.
constexpr Model wide_model{{
    {.pivot = vec(0.0, 0.0, 0.0),
     .base = box(vec(-4.0, -8.0, -4.0), vec(8.0, 8.0, 8.0), 0, 0),
     .layer = box(vec(-4.0, -8.0, -4.0), vec(8.0, 8.0, 8.0), 32, 0, 0.5)},
    {.pivot = vec(0.0, 0.0, 0.0),
     .base = box(vec(-4.0, 0.0, -2.0), vec(8.0, 12.0, 4.0), 16, 16),
     .layer = box(vec(-4.0, 0.0, -2.0), vec(8.0, 12.0, 4.0), 16, 32, 0.25)},
    {.pivot = vec(-5.0, 2.0, 0.0),
     .base = box(vec(-3.0, -2.0, -2.0), vec(4.0, 12.0, 4.0), 40, 16),
     .layer = box(vec(-3.0, -2.0, -2.0), vec(4.0, 12.0, 4.0), 40, 32, 0.25)},
    {.pivot = vec(5.0, 2.0, 0.0),
     .base = box(vec(-1.0, -2.0, -2.0), vec(4.0, 12.0, 4.0), 32, 48),
     .layer = box(vec(-1.0, -2.0, -2.0), vec(4.0, 12.0, 4.0), 48, 48, 0.25)},
    {.pivot = vec(-1.9, 12.0, 0.0),
     .base = box(vec(-2.0, 0.0, -2.0), vec(4.0, 12.0, 4.0), 0, 16),
     .layer = box(vec(-2.0, 0.0, -2.0), vec(4.0, 12.0, 4.0), 0, 32, 0.25)},
    {.pivot = vec(1.9, 12.0, 0.0),
     .base = box(vec(-2.0, 0.0, -2.0), vec(4.0, 12.0, 4.0), 16, 48),
     .layer = box(vec(-2.0, 0.0, -2.0), vec(4.0, 12.0, 4.0), 0, 48, 0.25)},
}};

// Slim arms are 3 px wide, on the same pivots and texels: the right arm's
// box starts a pixel nearer the body, the left arm's ends a pixel sooner.
constexpr Model slim_model = [] {
    Model model = wide_model;
    for (const std::size_t arm : {right_arm, left_arm}) {
        for (ModelBox* const part_box : {&model[arm].base, &model[arm].layer}) {
            part_box->size.x = 3.0;
            if (arm == right_arm) {
                part_box->corner.x = -2.0;
            }
        }
    }
    return model;
}();

// A part as this frame poses it: its pivot, and its turns in radians about
// x (positive swings a hanging limb back), y and z, applied in that order.
struct PartPose {
    Vec3d pivot;
    double pitch = 0.0;
    double yaw = 0.0;
    double roll = 0.0;
};

// A rotation, as its three rows.
struct Rotation {
    Vec3d x;
    Vec3d y;
    Vec3d z;
};

constexpr double dot(Vec3d a, Vec3d b) noexcept {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

constexpr Vec3d operator*(const Rotation& rotation, Vec3d v) noexcept {
    return {.x = dot(rotation.x, v), .y = dot(rotation.y, v), .z = dot(rotation.z, v)};
}

// Rz * Ry * Rx: a vertex turns about x first, then y, then z, as a part of
// the game's model does.
Rotation part_rotation(const PartPose& part) noexcept {
    const double cx = std::cos(part.pitch);
    const double sx = std::sin(part.pitch);
    const double cy = std::cos(part.yaw);
    const double sy = std::sin(part.yaw);
    const double cz = std::cos(part.roll);
    const double sz = std::sin(part.roll);
    return {.x = {.x = cz * cy, .y = (cz * sy * sx) - (sz * cx), .z = (cz * sy * cx) + (sz * sx)},
            .y = {.x = sz * cy, .y = (sz * sy * sx) + (cz * cx), .z = (sz * sy * cx) - (cz * sx)},
            .z = {.x = -sy, .y = cy * sx, .z = cy * cx}};
}

// The game's poses, in its order: every part from rest, then the head, the
// walk swing, the leg splay, the crouch and the idle sway (RFC 0012).
std::array<PartPose, part_count> pose_parts(const PlayerPose& pose, const Model& model) noexcept {
    std::array<PartPose, part_count> parts{};
    for (std::size_t part = 0; part < part_count; ++part) {
        parts[part].pivot = model[part].pivot;
    }

    // The head turns from the body. The game turns to the right for a
    // positive yaw and looks down for a positive pitch; the engine turns left
    // and looks up. The game wraps the turn to the shorter way round, which a
    // rotation needs no help with.
    parts[head].yaw = static_cast<double>(pose.body_yaw) - static_cast<double>(pose.head_yaw);
    parts[head].pitch = -static_cast<double>(pose.head_pitch);

    // Each arm swings with the opposite leg, the legs 1.4 times as far.
    const double phase = 0.6662 * pose.limb_position;
    const double amount = pose.limb_amount;
    parts[right_arm].pitch = std::cos(phase + std::numbers::pi) * amount;
    parts[left_arm].pitch = std::cos(phase) * amount;
    parts[right_leg].pitch = std::cos(phase) * 1.4 * amount;
    parts[left_leg].pitch = std::cos(phase + std::numbers::pi) * 1.4 * amount;

    // The feet always point a little outward.
    parts[right_leg].yaw = 0.005;
    parts[right_leg].roll = 0.005;
    parts[left_leg].yaw = -0.005;
    parts[left_leg].roll = -0.005;

    if (pose.crouching) {
        parts[body].pitch = 0.5;
        parts[body].pivot.y += 3.2;
        for (const std::size_t arm : {right_arm, left_arm}) {
            parts[arm].pitch += 0.4;
            parts[arm].pivot.y += 3.2;
        }
        for (const std::size_t leg : {right_leg, left_leg}) {
            parts[leg].pivot.z += 4.0;
        }
        parts[head].pivot.y += 4.2;
    }

    // Both arms drift outward and back again, never in, over about 70 and
    // 94 ticks.
    const double sway_roll = (0.05 * std::cos(0.09 * pose.age)) + 0.05;
    const double sway_pitch = 0.05 * std::sin(0.067 * pose.age);
    parts[right_arm].roll += sway_roll;
    parts[right_arm].pitch += sway_pitch;
    parts[left_arm].roll -= sway_roll;
    parts[left_arm].pitch -= sway_pitch;
    return parts;
}

// A face of a box: its corners at the four corners of its texel rectangle,
// top left, top right, bottom right and bottom left as the skin shows it,
// each an index into the box's corners (bit 0 for the high x, 1 for the high
// y, 2 for the high z); its outward normal; and whether it shows the skin
// mirrored seen from outside, which turns its triangles' winding round.
struct BoxFace {
    std::array<std::uint8_t, 4> corners;
    Vec3d normal;
    bool mirrored = false;
};

// In RFC 0012's order: top, bottom, right side, front, left side, back. The
// top reads from above with the front at its lower edge, the bottom as if
// from above, so mirrored from below; every side reads from outside.
constexpr std::array<BoxFace, 6> box_faces{{
    {.corners = {4, 5, 1, 0}, .normal = vec(0.0, -1.0, 0.0)},
    {.corners = {6, 7, 3, 2}, .normal = vec(0.0, 1.0, 0.0), .mirrored = true},
    {.corners = {4, 0, 2, 6}, .normal = vec(-1.0, 0.0, 0.0)},
    {.corners = {0, 1, 3, 2}, .normal = vec(0.0, 0.0, -1.0)},
    {.corners = {1, 5, 7, 3}, .normal = vec(1.0, 0.0, 0.0)},
    {.corners = {5, 4, 6, 7}, .normal = vec(0.0, 0.0, 1.0)},
}};

// Each face's texel rectangle, from the box's texels at (u, v) and its size,
// unpushed: left, top, width and height (RFC 0012's layout).
std::array<std::array<double, 4>, 6> texel_rectangles(const ModelBox& part_box) noexcept {
    const auto u = static_cast<double>(part_box.u);
    const auto v = static_cast<double>(part_box.v);
    const double dx = part_box.size.x;
    const double dy = part_box.size.y;
    const double dz = part_box.size.z;
    return {{{u + dz, v, dx, dz},
             {u + dz + dx, v, dx, dz},
             {u, v + dz, dz, dy},
             {u + dz, v + dz, dx, dy},
             {u + dz + dx, v + dz, dz, dy},
             {u + (2.0 * dz) + dx, v + dz, dx, dy}}};
}

// Where the posed model lands relative to the body's block: model points in
// pixels turned half a turn about z into the body's space, facing -z,
// scaled 0.9375 and lifted 1.501 blocks inside its frame, so its soles float
// a millimeter above the ground; then crouched, turned by the body's yaw and
// moved to the feet.
struct Placement {
    Vec3d feet; // relative to the body's block
    double cos_yaw = 1.0;
    double sin_yaw = 0.0;

    // Turns -z toward -x as the yaw grows, as the engine's yaw does.
    [[nodiscard]] Vec3d turned(Vec3d v) const noexcept {
        return {.x = (v.x * cos_yaw) + (v.z * sin_yaw),
                .y = v.y,
                .z = (v.z * cos_yaw) - (v.x * sin_yaw)};
    }

    [[nodiscard]] Vec3d position(Vec3d model) const noexcept {
        constexpr double scale = 0.9375;
        constexpr double lift = 1.501;
        constexpr double pixel = 1.0 / 16.0;
        const Vec3d in_body{.x = -model.x * pixel * scale,
                            .y = (lift - (model.y * pixel)) * scale,
                            .z = model.z * pixel * scale};
        return feet + turned(in_body);
    }

    [[nodiscard]] Vec3d direction(Vec3d model) const noexcept {
        return turned({.x = -model.x, .y = -model.y, .z = model.z});
    }
};

std::array<float, 3> floats(Vec3d v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

// One box of a posed part: its eight corners placed, then its six faces,
// two triangles each, with the texels of their rectangles.
void pose_box(const ModelBox& part_box, const PartPose& part, const Rotation& rotation,
              const Placement& placement, std::span<BodyVertex, 36> out) noexcept {
    const Vec3d low{.x = part_box.corner.x - part_box.push,
                    .y = part_box.corner.y - part_box.push,
                    .z = part_box.corner.z - part_box.push};
    const Vec3d high{.x = part_box.corner.x + part_box.size.x + part_box.push,
                     .y = part_box.corner.y + part_box.size.y + part_box.push,
                     .z = part_box.corner.z + part_box.size.z + part_box.push};
    std::array<Vec3d, 8> corners{};
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
        const Vec3d local{.x = (corner & 1U) != 0 ? high.x : low.x,
                          .y = (corner & 2U) != 0 ? high.y : low.y,
                          .z = (corner & 4U) != 0 ? high.z : low.z};
        corners[corner] = placement.position(part.pivot + (rotation * local));
    }

    // Counter-clockwise seen from outside: down the left edge, across the
    // bottom and back up the right, or the other way round where the face
    // shows mirrored.
    constexpr std::array<std::size_t, 6> winding{0, 3, 2, 2, 1, 0};
    constexpr std::array<std::size_t, 6> mirrored_winding{0, 2, 3, 2, 0, 1};
    constexpr double texels = 1.0 / skin_size;
    const std::array<std::array<double, 4>, 6> rectangles = texel_rectangles(part_box);
    std::size_t next = 0;
    for (std::size_t face = 0; face < box_faces.size(); ++face) {
        const BoxFace& shape = box_faces[face];
        const std::array<double, 4>& rectangle = rectangles[face];
        const auto left = static_cast<float>(rectangle[0] * texels);
        const auto top = static_cast<float>(rectangle[1] * texels);
        const auto right = static_cast<float>((rectangle[0] + rectangle[2]) * texels);
        const auto bottom = static_cast<float>((rectangle[1] + rectangle[3]) * texels);
        // The texel rectangle's corners in the face's corner order.
        const std::array<std::array<float, 2>, 4> texel_corners{
            {{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
        const std::array<float, 3> normal = floats(placement.direction(rotation * shape.normal));
        for (const std::size_t at : shape.mirrored ? mirrored_winding : winding) {
            out[next++] = {.position = floats(corners[std::size_t{shape.corners[at]}]),
                           .texel = texel_corners[at],
                           .normal = normal};
        }
    }
}

} // namespace

void pose_player(const PlayerPose& pose, BlockPos origin,
                 std::span<BodyVertex, player_vertices> out) noexcept {
    const Model& model = pose.arms == Arms::slim ? slim_model : wide_model;
    const std::array<PartPose, part_count> parts = pose_parts(pose, model);

    // The whole body sits 2/16 block lower while crouching.
    const double crouch = pose.crouching ? -2.0 / 16.0 : 0.0;
    const Placement placement{.feet = {.x = pose.feet.x - origin.x,
                                       .y = pose.feet.y - origin.y + crouch,
                                       .z = pose.feet.z - origin.z},
                              .cos_yaw = std::cos(static_cast<double>(pose.body_yaw)),
                              .sin_yaw = std::sin(static_cast<double>(pose.body_yaw))};

    std::size_t box = 0;
    for (std::size_t part = 0; part < part_count; ++part) {
        const Rotation rotation = part_rotation(parts[part]);
        for (const ModelBox* const part_box : {&model[part].base, &model[part].layer}) {
            pose_box(*part_box, parts[part], rotation, placement,
                     out.subspan(box * 36).first<36>());
            ++box;
        }
    }
}

} // namespace vx
