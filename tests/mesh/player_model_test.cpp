#include <vx/core/vec3d.hpp>
#include <vx/mesh/player_model.hpp>
#include <vx/world/coordinates.hpp>
#include <vx/world/skin.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <random>
#include <span>

// Every number below is RFC 0012's, written from its tables and its key
// points, not from the code.

namespace {

using vx::Arms;
using vx::BodyVertex;
using vx::PlayerPose;
using vx::Vec3d;

using Body = std::array<BodyVertex, vx::player_vertices>;

// The boxes and faces in pose_player's order.
enum : std::size_t {
    head,
    hat,
    torso,
    jacket,
    right_arm,
    right_sleeve,
    left_arm,
    left_sleeve,
    right_leg,
    right_pants,
    left_leg,
    left_pants,
    box_count
};
enum : std::size_t { top, bottom, right_side, front, left_side, back, face_count };

// The age at which the idle sway's roll is 0, cos(0.09 age) being -1: the
// arms hang straight down but for the sway's small pitch.
constexpr double unrolled = std::numbers::pi / 0.09;

Body posed(const PlayerPose& pose, vx::BlockPos origin = {}) {
    Body body{};
    vx::pose_player(pose, origin, body);
    return body;
}

std::span<const BodyVertex, 6> face_of(const Body& body, std::size_t box, std::size_t face) {
    return std::span<const BodyVertex, vx::player_vertices>(body)
        .subspan((box * face_count + face) * 6)
        .first<6>();
}

std::span<const BodyVertex, 36> box_of(const Body& body, std::size_t box) {
    return std::span<const BodyVertex, vx::player_vertices>(body).subspan(box * 36).first<36>();
}

Vec3d position(const BodyVertex& vertex) {
    return {.x = static_cast<double>(vertex.position[0]),
            .y = static_cast<double>(vertex.position[1]),
            .z = static_cast<double>(vertex.position[2])};
}

Vec3d normal(const BodyVertex& vertex) {
    return {.x = static_cast<double>(vertex.normal[0]),
            .y = static_cast<double>(vertex.normal[1]),
            .z = static_cast<double>(vertex.normal[2])};
}

double dot(Vec3d a, Vec3d b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3d cross(Vec3d a, Vec3d b) {
    return {.x = a.y * b.z - a.z * b.y, .y = a.z * b.x - a.x * b.z, .z = a.x * b.y - a.y * b.x};
}

double length(Vec3d v) {
    return std::sqrt(dot(v, v));
}

// The center of a face, or of a box: the mean of its vertices, since each
// face's two triangles share two opposite corners of a parallelogram.
template <std::size_t count> Vec3d center(std::span<const BodyVertex, count> vertices) {
    Vec3d sum;
    for (const BodyVertex& vertex : vertices) {
        sum = sum + position(vertex);
    }
    return sum * (1.0 / static_cast<double>(count));
}

// Within `tolerance` of `expected`, an absolute bound: doctest's Approx
// grows with the value.
void check_near(double actual, double expected, double tolerance = 1e-4) {
    INFO(actual, " against ", expected);
    CHECK(std::abs(actual - expected) <= tolerance);
}

} // namespace

TEST_CASE("every face of every box reads its texel rectangle from RFC 0012's layout") {
    // Each box's unwrap on the skin, (U, V), and its unpushed size, (dx, dy,
    // dz), in pose_player's order; outer layers use their part's size.
    struct Unwrap {
        int u;
        int v;
        int dx;
        int dy;
        int dz;
    };
    for (const Arms arms : {Arms::wide, Arms::slim}) {
        const int arm = arms == Arms::slim ? 3 : 4;
        const std::array<Unwrap, box_count> unwraps{{
            {.u = 0, .v = 0, .dx = 8, .dy = 8, .dz = 8},
            {.u = 32, .v = 0, .dx = 8, .dy = 8, .dz = 8},
            {.u = 16, .v = 16, .dx = 8, .dy = 12, .dz = 4},
            {.u = 16, .v = 32, .dx = 8, .dy = 12, .dz = 4},
            {.u = 40, .v = 16, .dx = arm, .dy = 12, .dz = 4},
            {.u = 40, .v = 32, .dx = arm, .dy = 12, .dz = 4},
            {.u = 32, .v = 48, .dx = arm, .dy = 12, .dz = 4},
            {.u = 48, .v = 48, .dx = arm, .dy = 12, .dz = 4},
            {.u = 0, .v = 16, .dx = 4, .dy = 12, .dz = 4},
            {.u = 0, .v = 32, .dx = 4, .dy = 12, .dz = 4},
            {.u = 16, .v = 48, .dx = 4, .dy = 12, .dz = 4},
            {.u = 0, .v = 48, .dx = 4, .dy = 12, .dz = 4},
        }};
        // Facing -z at yaw 0, the player's right is +x. Where u and v run on
        // each face, from the RFC's "Read" column: the top from above with
        // the front at its lower edge, the bottom as if from above, the
        // sides from outside, back to front on the right and front to back
        // on the left.
        const std::array<std::array<Vec3d, 2>, face_count> runs{{
            {Vec3d{.x = -1.0}, Vec3d{.z = -1.0}},
            {Vec3d{.x = -1.0}, Vec3d{.z = -1.0}},
            {Vec3d{.z = -1.0}, Vec3d{.y = -1.0}},
            {Vec3d{.x = -1.0}, Vec3d{.y = -1.0}},
            {Vec3d{.z = 1.0}, Vec3d{.y = -1.0}},
            {Vec3d{.x = 1.0}, Vec3d{.y = -1.0}},
        }};
        const Body body = posed({.age = unrolled, .arms = arms});
        for (std::size_t box = 0; box < box_count; ++box) {
            const Unwrap& w = unwraps[box];
            // Left, top, right and bottom of each face's rectangle.
            const std::array<std::array<int, 4>, face_count> rectangles{{
                {w.u + w.dz, w.v, w.u + w.dz + w.dx, w.v + w.dz},
                {w.u + w.dz + w.dx, w.v, w.u + w.dz + 2 * w.dx, w.v + w.dz},
                {w.u, w.v + w.dz, w.u + w.dz, w.v + w.dz + w.dy},
                {w.u + w.dz, w.v + w.dz, w.u + w.dz + w.dx, w.v + w.dz + w.dy},
                {w.u + w.dz + w.dx, w.v + w.dz, w.u + 2 * w.dz + w.dx, w.v + w.dz + w.dy},
                {w.u + 2 * w.dz + w.dx, w.v + w.dz, w.u + 2 * w.dz + 2 * w.dx, w.v + w.dz + w.dy},
            }};
            for (std::size_t face = 0; face < face_count; ++face) {
                INFO("arms ", arms == Arms::slim ? "slim" : "wide", " box ", box, " face ", face);
                const std::span<const BodyVertex, 6> vertices = face_of(body, box, face);
                const std::array<int, 4>& rectangle = rectangles[face];
                // The rectangle's corners, and where on the face each is.
                const auto corner = [&](int u, int v) -> const BodyVertex* {
                    for (const BodyVertex& vertex : vertices) {
                        if (vertex.texel[0] == static_cast<float>(u) / 64.0F &&
                            vertex.texel[1] == static_cast<float>(v) / 64.0F) {
                            return &vertex;
                        }
                    }
                    return nullptr;
                };
                const BodyVertex* const top_left = corner(rectangle[0], rectangle[1]);
                const BodyVertex* const top_right = corner(rectangle[2], rectangle[1]);
                const BodyVertex* const bottom_right = corner(rectangle[2], rectangle[3]);
                const BodyVertex* const bottom_left = corner(rectangle[0], rectangle[3]);
                REQUIRE(top_left != nullptr);
                REQUIRE(top_right != nullptr);
                REQUIRE(bottom_right != nullptr);
                REQUIRE(bottom_left != nullptr);
                // No texel outside the rectangle.
                for (const BodyVertex& vertex : vertices) {
                    CHECK(vertex.texel[0] * 64.0F >= static_cast<float>(rectangle[0]));
                    CHECK(vertex.texel[0] * 64.0F <= static_cast<float>(rectangle[2]));
                    CHECK(vertex.texel[1] * 64.0F >= static_cast<float>(rectangle[1]));
                    CHECK(vertex.texel[1] * 64.0F <= static_cast<float>(rectangle[3]));
                }
                // u runs along the face one way and v another, as the RFC
                // reads each face; within the sway's and the splay's small
                // turns.
                const Vec3d along_u = position(*top_right) - position(*top_left);
                const Vec3d along_v = position(*bottom_left) - position(*top_left);
                CHECK(dot(along_u, runs[face][0]) > 0.99 * length(along_u));
                CHECK(dot(along_v, runs[face][1]) > 0.99 * length(along_v));
                CHECK(length(position(*bottom_right) - position(*top_left)) > 0.0);
            }
        }
    }
}

TEST_CASE("the standing body's key points are where RFC 0012 puts them") {
    const Body body = posed({.age = unrolled});
    // The soles float a millimeter above the ground; the pants dip below.
    for (const std::size_t leg : {right_leg, left_leg}) {
        check_near(center(face_of(body, leg, bottom)).y, 0.0009);
    }
    for (const std::size_t pants : {right_pants, left_pants}) {
        check_near(center(face_of(body, pants, bottom)).y, -0.0137);
    }
    // The hips, at the legs' pivots, 0.1113 either side; the right is +x.
    check_near(center(face_of(body, right_leg, top)).x, 0.1113);
    check_near(center(face_of(body, left_leg, top)).x, -0.1113);
    for (const std::size_t leg : {right_leg, left_leg}) {
        check_near(center(face_of(body, leg, top)).y, 0.7041);
    }
    // The neck, the head's top and the hat's top.
    check_near(center(face_of(body, head, bottom)).y, 1.4072);
    check_near(center(face_of(body, torso, top)).y, 1.4072);
    check_near(center(face_of(body, head, top)).y, 1.8759);
    check_near(center(face_of(body, hat, top)).y, 1.9052);
    // The face's eye row, texel row 12 of the head's front, 4 texels down
    // its 8: halfway between the face's top and bottom edges.
    const std::span<const BodyVertex, 6> face = face_of(body, head, front);
    const auto [low, high] =
        std::minmax_element(face.begin(), face.end(), [](const BodyVertex& a, const BodyVertex& b) {
            return a.position[1] < b.position[1];
        });
    CHECK(low->texel[1] == 16.0F / 64.0F);
    CHECK(high->texel[1] == 8.0F / 64.0F);
    check_near((position(*low).y + position(*high).y) / 2.0, 1.6416);
}

TEST_CASE("the arms span 0.9375 blocks across wide and 0.8203 slim") {
    for (const auto& [arms, width] :
         {std::pair{Arms::wide, 0.9375}, std::pair{Arms::slim, 0.8203}}) {
        const Body body = posed({.age = unrolled, .arms = arms});
        double right = 0.0;
        double left = 0.0;
        for (const std::size_t arm : {right_arm, left_arm}) {
            for (const BodyVertex& vertex : box_of(body, arm)) {
                right = std::max(right, static_cast<double>(vertex.position[0]));
                left = std::min(left, static_cast<double>(vertex.position[0]));
            }
        }
        check_near(right - left, width);
        check_near(right, -left);
    }
}

TEST_CASE("the body faces where its yaw says") {
    // At yaw pi/2 the face looks down -x, as RFC 0010's yaw turns.
    const float quarter = std::numbers::pi_v<float> / 2.0F;
    const Body turned = posed({.age = unrolled, .body_yaw = quarter, .head_yaw = quarter});
    for (const std::size_t box : {head, torso}) {
        const Vec3d facing = normal(face_of(turned, box, front)[0]);
        check_near(facing.x, -1.0);
        check_near(facing.y, 0.0);
        check_near(facing.z, 0.0);
    }
}

TEST_CASE("the head looks where its look says") {
    // A head turned 0.5 left of the body and looking 0.3 up faces the look:
    // (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch).
    for (const float body_yaw : {0.0F, 1.0F, -2.5F}) {
        const float head_yaw = body_yaw + 0.5F;
        const Body looking = posed(
            {.age = unrolled, .body_yaw = body_yaw, .head_yaw = head_yaw, .head_pitch = 0.3F});
        const Vec3d facing = normal(face_of(looking, head, front)[0]);
        const auto yaw = static_cast<double>(head_yaw);
        check_near(facing.x, -std::sin(yaw) * std::cos(0.3));
        check_near(facing.y, std::sin(0.3));
        check_near(facing.z, -std::cos(yaw) * std::cos(0.3));
    }
}

TEST_CASE("positions are relative to the block the body stands in") {
    const Body body = posed({.feet = {.x = 100.5, .y = 64.0, .z = -20.25}, .age = unrolled},
                            {.x = 100, .y = 64, .z = -21});
    const Vec3d soles =
        (center(face_of(body, right_leg, bottom)) + center(face_of(body, left_leg, bottom))) * 0.5;
    check_near(soles.x, 0.5);
    check_near(soles.y, 0.0009);
    check_near(soles.z, 0.75);
}

TEST_CASE("every face points out of its box and winds counter-clockwise seen from outside") {
    std::mt19937 random(12);
    std::uniform_real_distribution<float> angle(-3.2F, 3.2F);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    for (int pose_number = 0; pose_number < 50; ++pose_number) {
        const PlayerPose pose{
            .feet = {.x = unit(random) * 8.0, .y = unit(random), .z = -unit(random)},
            .limb_position = unit(random) * 40.0,
            .limb_amount = unit(random),
            .age = unit(random) * 200.0,
            .body_yaw = angle(random),
            .head_yaw = angle(random),
            .head_pitch = angle(random) / 2.0F,
            .arms = pose_number % 2 == 0 ? Arms::wide : Arms::slim,
            .crouching = pose_number % 3 == 0};
        const Body body = posed(pose);
        for (std::size_t box = 0; box < box_count; ++box) {
            const Vec3d middle = center(box_of(body, box));
            for (std::size_t face = 0; face < face_count; ++face) {
                INFO("pose ", pose_number, " box ", box, " face ", face);
                const std::span<const BodyVertex, 6> vertices = face_of(body, box, face);
                const Vec3d outward = center(vertices) - middle;
                for (const BodyVertex& vertex : vertices) {
                    check_near(length(normal(vertex)), 1.0);
                    CHECK(dot(normal(vertex), outward) > 0.9 * length(outward));
                }
                for (std::size_t triangle = 0; triangle < 2; ++triangle) {
                    const Vec3d a = position(vertices[3 * triangle]);
                    const Vec3d b = position(vertices[3 * triangle + 1]);
                    const Vec3d c = position(vertices[3 * triangle + 2]);
                    CHECK(dot(cross(b - a, c - a), normal(vertices[0])) > 0.0);
                }
            }
        }
    }
}
