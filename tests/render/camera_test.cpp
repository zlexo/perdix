#include <vx/core/frustum.hpp>
#include <vx/core/vec3d.hpp>
#include <vx/render/camera.hpp>

#include <doctest/doctest.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cmath>
#include <numbers>
#include <random>

namespace {

constexpr float quarter_turn = std::numbers::pi_v<float> / 2.0F;

bool close(const glm::vec3& a, const glm::vec3& b) {
    return glm::distance(a, b) < 1.0e-5F;
}

// A box a hundredth of a block wide around `point`, relative to the camera.
bool sees(const vx::Frustum& view, const glm::vec3& point) {
    constexpr double half = 0.005;
    const vx::Vec3d at{.x = static_cast<double>(point.x),
                       .y = static_cast<double>(point.y),
                       .z = static_cast<double>(point.z)};
    const vx::Vec3d corner{.x = half, .y = half, .z = half};
    return view.may_contain(at - corner, at + corner);
}

} // namespace

TEST_CASE("with no yaw or pitch, the camera looks down -z") {
    CHECK(close(vx::render::forward(0.0F, 0.0F), {0.0F, 0.0F, -1.0F}));
    CHECK(vx::render::view_rotation(0.0F, 0.0F) == glm::mat4(1.0F));
}

TEST_CASE("yaw turns the camera left, and pitch tilts it up") {
    CHECK(close(vx::render::forward(quarter_turn, 0.0F), {-1.0F, 0.0F, 0.0F}));
    CHECK(close(vx::render::forward(0.0F, quarter_turn / 2.0F),
                {0.0F, std::numbers::sqrt2_v<float> / 2.0F, -std::numbers::sqrt2_v<float> / 2.0F}));
}

TEST_CASE("the view turns where the camera looks into -z, and keeps up as up") {
    for (const auto [yaw, pitch] :
         std::array{std::array{0.3F, 0.2F}, std::array{-2.0F, -1.1F}, std::array{3.0F, 1.4F}}) {
        INFO("yaw ", yaw, ", pitch ", pitch);
        const glm::mat4 view = vx::render::view_rotation(yaw, pitch);
        const glm::vec4 ahead = view * glm::vec4(vx::render::forward(yaw, pitch), 0.0F);
        CHECK(close(glm::vec3(ahead), {0.0F, 0.0F, -1.0F}));
        const glm::vec4 up =
            vx::render::view_rotation(yaw, 0.0F) * glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
        CHECK(close(glm::vec3(up), {0.0F, 1.0F, 0.0F}));
    }
}

TEST_CASE("a point straight ahead lands in the image's middle, in front of the camera") {
    vx::render::Camera camera;
    camera.yaw = 0.3F;
    camera.pitch = 0.2F;
    const glm::vec3 ahead = 10.0F * vx::render::forward(camera.yaw, camera.pitch);
    const glm::vec4 clip =
        vx::render::view_projection(camera, 16.0F / 9.0F) * glm::vec4(ahead, 1.0F);

    CHECK(clip.w > 0.0F);
    CHECK(static_cast<double>(clip.x / clip.w) == doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(static_cast<double>(clip.y / clip.w) == doctest::Approx(0.0).epsilon(1.0e-5));
    CHECK(static_cast<double>(clip.z / clip.w) ==
          doctest::Approx(0.005)); // the near distance, 0.05, over 10
}

TEST_CASE("flying forward follows the yaw and stays level, whatever the pitch") {
    vx::render::Camera camera;
    camera.yaw = quarter_turn; // facing -x
    camera.pitch = 0.8F;
    vx::render::fly(camera, {.move = {0.0F, 0.0F, 1.0F}}, 0.5F);

    // Ten blocks a second for half a second.
    CHECK(close(glm::vec3(camera.position), {-5.0F, 0.0F, 0.0F}));
}

TEST_CASE("up is the world's up, and a diagonal is no faster than a straight line") {
    vx::render::Camera rising;
    rising.pitch = -0.5F;
    vx::render::fly(rising, {.move = {0.0F, 1.0F, 0.0F}}, 1.0F);
    CHECK(close(glm::vec3(rising.position), {0.0F, 10.0F, 0.0F}));

    vx::render::Camera diagonal;
    vx::render::fly(diagonal, {.move = {1.0F, 0.0F, 1.0F}}, 1.0F);
    CHECK(static_cast<double>(glm::length(glm::vec3(diagonal.position))) == doctest::Approx(10.0));
}

TEST_CASE("Ctrl flies four times as fast") {
    vx::render::Camera camera;
    vx::render::fly(camera, {.move = {0.0F, 0.0F, 1.0F}, .fast = true}, 1.0F);
    CHECK(close(glm::vec3(camera.position), {0.0F, 0.0F, -40.0F}));
}

TEST_CASE("the pitch stops short of straight up and down, and the yaw wraps") {
    vx::render::Camera camera;
    vx::render::fly(camera, {.yaw_turn = 7.0F, .pitch_turn = 10.0F}, 0.0F);
    CHECK(camera.pitch < quarter_turn);
    CHECK(camera.pitch > quarter_turn - 0.01F);
    // 7 radians is a full turn and about 0.717 more.
    CHECK(static_cast<double>(camera.yaw) == doctest::Approx(7.0 - 2.0 * std::numbers::pi));

    vx::render::fly(camera, {.pitch_turn = -20.0F}, 0.0F);
    CHECK(camera.pitch > -quarter_turn);
}

TEST_CASE("a roll leans the view: rolled a quarter turn left, the world's up is to the right") {
    // Leaning left, counterclockwise as the camera sees it, the world turns
    // clockwise in the view: its up points right.
    const glm::vec4 up =
        vx::render::view_rotation(0.0F, 0.0F, quarter_turn) * glm::vec4(0.0F, 1.0F, 0.0F, 0.0F);
    CHECK(up.x == doctest::Approx(1.0F));
    CHECK(up.y == doctest::Approx(0.0F));
    // A roll of 0 is no roll at all, bit for bit.
    CHECK(vx::render::view_rotation(0.4F, -0.3F, 0.0F) == vx::render::view_rotation(0.4F, -0.3F));
    vx::render::Camera camera;
    camera.roll = 0.2F;
    CHECK(vx::render::view_projection(camera, 1.0F) !=
          vx::render::view_projection(vx::render::Camera{}, 1.0F));
}

TEST_CASE("the view frustum holds what is ahead within the field of view, at the target's aspect") {
    // Down -z with 70 degrees vertically: tan 35 degrees is 0.70, so at 10
    // blocks ahead the view reaches 7 blocks up and, twice as wide, 14 across.
    const vx::Frustum view = vx::render::view_frustum(vx::render::Camera{}, 2.0F);
    CHECK(sees(view, {0.0F, 0.0F, -10.0F}));
    CHECK_FALSE(sees(view, {0.0F, 0.0F, 10.0F}));
    CHECK(sees(view, {0.0F, 6.5F, -10.0F}));
    CHECK_FALSE(sees(view, {0.0F, 7.5F, -10.0F}));
    CHECK(sees(view, {13.5F, 0.0F, -10.0F}));
    CHECK_FALSE(sees(view, {14.5F, 0.0F, -10.0F}));
    // Turned a quarter turn left, the camera looks down -x.
    vx::render::Camera turned;
    turned.yaw = quarter_turn;
    const vx::Frustum left = vx::render::view_frustum(turned, 1.0F);
    CHECK(sees(left, {-10.0F, 0.0F, 0.0F}));
    CHECK_FALSE(sees(left, {0.0F, 0.0F, -10.0F}));
}

TEST_CASE("the view frustum agrees with the projection, whatever the yaw, pitch and roll") {
    // A point well inside the image is in, one well outside it is out, as
    // view_projection puts them on the screen.
    std::mt19937 random(0xF0C5);
    std::uniform_real_distribution<float> angle(-3.0F, 3.0F);
    std::uniform_real_distribution<float> coordinate(-40.0F, 40.0F);
    int inside = 0;
    int outside = 0;
    for (int view_index = 0; view_index < 50; ++view_index) {
        vx::render::Camera camera;
        camera.yaw = angle(random);
        camera.pitch = angle(random) / 2.0F;
        camera.roll = angle(random) / 4.0F;
        const float aspect = (view_index % 2 == 0) ? 16.0F / 9.0F : 0.75F;
        const glm::mat4 projection = vx::render::view_projection(camera, aspect);
        const vx::Frustum view = vx::render::view_frustum(camera, aspect);
        for (int point_index = 0; point_index < 200; ++point_index) {
            const glm::vec3 point(coordinate(random), coordinate(random), coordinate(random));
            const glm::vec4 clip = projection * glm::vec4(point, 1.0F);
            // w is the distance ahead: behind the camera is out, and the
            // first block ahead, around the near plane, is left alone.
            if (clip.w <= 0.0F) {
                ++outside;
                CHECK_FALSE(sees(view, point));
                continue;
            }
            if (clip.w < 1.0F) {
                continue;
            }
            const float x = std::abs(clip.x / clip.w);
            const float y = std::abs(clip.y / clip.w);
            if (x < 0.98F && y < 0.98F) {
                ++inside;
                CHECK(sees(view, point));
            } else if (x > 1.02F || y > 1.02F) {
                ++outside;
                CHECK_FALSE(sees(view, point));
            }
        }
    }
    CHECK(inside > 100);
    CHECK(outside > 100);
}
