#include "view.hpp"
#include "walking.hpp"

#include <vx/render/camera.hpp>
#include <vx/render/fog.hpp>

#include <doctest/doctest.h>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_double3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <initializer_list>
#include <optional>

// RFC 0010's view bobbing as the app draws it: Minecraft 26.2 multiplies its
// view by a shift in view space, then a turn around the view's z axis, then
// one around its x axis, after the camera's own rotation (from its jar). The
// app draws a camera that sees the same.

TEST_CASE("the bobbed camera sees every point where 26.2's bobbed view puts it") {
    const vx::render::Camera camera{.position = {10.5, 65.62, -3.25}, .yaw = 0.7F, .pitch = -0.3F};
    const vx::app::Bob bob{.shift_x = 0.04, .shift_y = -0.09, .turn_z = 0.004F, .turn_x = 0.007F};
    const vx::render::Camera shown = vx::app::bobbed(camera, bob);

    glm::mat4 game = glm::translate(glm::mat4(1.0F), glm::vec3(0.04F, -0.09F, 0.0F));
    game = glm::rotate(game, 0.004F, glm::vec3(0.0F, 0.0F, 1.0F));
    game = glm::rotate(game, 0.007F, glm::vec3(1.0F, 0.0F, 0.0F));
    game = game * vx::render::view_rotation(camera.yaw, camera.pitch);
    const glm::mat4 ours = vx::render::view_rotation(shown.yaw, shown.pitch, shown.roll);
    for (const glm::dvec3 point :
         {glm::dvec3{12.0, 66.0, -8.0}, glm::dvec3{4.0, 60.0, 2.0}, glm::dvec3{10.5, 70.0, -3.0}}) {
        const glm::vec4 seen_by_game = game * glm::vec4(glm::vec3(point - camera.position), 1.0F);
        const glm::vec4 seen_by_ours = ours * glm::vec4(glm::vec3(point - shown.position), 1.0F);
        CHECK(static_cast<double>(seen_by_ours.x) ==
              doctest::Approx(static_cast<double>(seen_by_game.x)).epsilon(1e-5));
        CHECK(static_cast<double>(seen_by_ours.y) ==
              doctest::Approx(static_cast<double>(seen_by_game.y)).epsilon(1e-5));
        CHECK(static_cast<double>(seen_by_ours.z) ==
              doctest::Approx(static_cast<double>(seen_by_game.z)).epsilon(1e-5));
    }
}

TEST_CASE("no bob leaves the camera as it was") {
    const vx::render::Camera camera{.position = {10.5, 65.62, -3.25}, .yaw = 0.7F, .pitch = -0.3F};
    const vx::render::Camera shown = vx::app::bobbed(camera, {});
    CHECK(shown.position.x == camera.position.x);
    CHECK(shown.position.y == camera.position.y);
    CHECK(shown.position.z == camera.position.z);
    CHECK(shown.yaw == camera.yaw);
    CHECK(shown.pitch == camera.pitch);
    CHECK(shown.roll == 0.0F);
}

TEST_CASE("a frame's fog is 26.2's at the view distance the world holds, and none when off") {
    CHECK_FALSE(vx::app::frame_fog(false, 32).has_value());
    // 32 chunks, 1,024 blocks: past 26.2's farthest view, so the fog color
    // is its own and the ramps are scaled.
    const std::optional<vx::render::Fog> far = vx::app::frame_fog(true, 32);
    REQUIRE(far.has_value());
    CHECK(far->color == vx::render::Srgb8{.r = 192, .g = 216, .b = 255});
    CHECK(far->ramps.haze_end == doctest::Approx(2'048.0));
    CHECK(far->ramps.band_start == doctest::Approx(921.6));
    CHECK(far->ramps.band_end == doctest::Approx(1'024.0));
    CHECK(far->sky == vx::render::Srgb8{.r = 120, .g = 167, .b = 255});
    // 4 chunks, 128 blocks: 26.2's own, its color pulled toward the sky.
    const std::optional<vx::render::Fog> near = vx::app::frame_fog(true, 4);
    REQUIRE(near.has_value());
    CHECK(near->color == vx::render::Srgb8{.r = 178, .g = 206, .b = 255});
    CHECK(near->ramps.haze_end == doctest::Approx(1'024.0));
    CHECK(near->ramps.band_start == doctest::Approx(115.2));
    CHECK(near->ramps.band_end == doctest::Approx(128.0));
}
