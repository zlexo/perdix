#include <vx/core/vec3d.hpp>
#include <vx/world/life.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

// RFC 0012's look control: the head's yaw 10 degrees a tick toward a point,
// the pitch straight to the point's within 40 degrees, a point held for the
// tick it is given and the next, and with none the head back toward the
// body, level. The expected angles follow from those rules alone.

namespace {

using vx::Vec3d;

constexpr double radians(double degrees) {
    return degrees * std::numbers::pi / 180.0;
}

double degrees(double radians) {
    return radians * 180.0 / std::numbers::pi;
}

constexpr Vec3d eye{.x = 0.0, .y = 1.62, .z = 0.0};

// A point `distance` away from the eye, `yaw_degrees` round from -z to the
// left and `pitch_degrees` up.
Vec3d point_at(double yaw_degrees, double pitch_degrees, double distance = 10.0) {
    const double yaw = radians(yaw_degrees);
    const double pitch = radians(pitch_degrees);
    return eye + Vec3d{.x = -std::sin(yaw) * std::cos(pitch) * distance,
                       .y = std::sin(pitch) * distance,
                       .z = -std::cos(yaw) * std::cos(pitch) * distance};
}

double yaw_degrees(const vx::LookControl& look) {
    return degrees(static_cast<double>(look.yaw()));
}

double pitch_degrees(const vx::LookControl& look) {
    return degrees(static_cast<double>(look.pitch()));
}

} // namespace

TEST_CASE("the head turns toward a point by 10 degrees a tick either way and stops on it") {
    for (const double side : {1.0, -1.0}) {
        INFO("toward ", side * 95.0, " degrees");
        vx::LookControl look(0.0F);
        for (int i = 1; i <= 12; ++i) {
            look.look_at(point_at(side * 95.0, 0.0));
            look.tick(eye, 0.0F);
            const double expected = side * std::min(10.0 * i, 95.0);
            CHECK(yaw_degrees(look) == doctest::Approx(expected).epsilon(1e-5));
        }
    }
}

TEST_CASE("the head turns the shorter way round across a half turn") {
    vx::LookControl look(static_cast<float>(radians(170.0)));
    look.look_at(point_at(-170.0, 0.0));
    look.tick(eye, 0.0F);
    CHECK(std::abs(yaw_degrees(look)) == doctest::Approx(180.0).epsilon(1e-5));
    look.look_at(point_at(-170.0, 0.0));
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(-170.0).epsilon(1e-5));
}

TEST_CASE("the pitch goes straight to the point's held within 40 degrees up or down") {
    for (const double pitch : {0.0, 25.0, -30.0, 39.0, 41.0, 75.0, -60.0}) {
        INFO("a point ", pitch, " degrees up");
        vx::LookControl look(0.0F);
        look.look_at(point_at(0.0, pitch));
        look.tick(eye, 0.0F);
        CHECK(pitch_degrees(look) == doctest::Approx(std::clamp(pitch, -40.0, 40.0)).epsilon(1e-5));
    }
}

TEST_CASE("a point holds for the tick it is given and the next and then the head goes back") {
    vx::LookControl look(0.0F);
    look.look_at(point_at(90.0, 20.0));
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(10.0).epsilon(1e-5));
    CHECK(pitch_degrees(look) == doctest::Approx(20.0).epsilon(1e-5));
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(20.0).epsilon(1e-5));
    CHECK(pitch_degrees(look) == doctest::Approx(20.0).epsilon(1e-5));
    // The third tick has no point: back toward the body, level.
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(10.0).epsilon(1e-5));
    CHECK(look.pitch() == 0.0F);
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(0.0).epsilon(1e-5));
}

TEST_CASE("with no point the head turns back to the body by 10 degrees a tick and level") {
    vx::LookControl look(static_cast<float>(radians(-35.0)));
    const auto body = static_cast<float>(radians(20.0));
    for (const double expected : {-25.0, -15.0, -5.0, 5.0, 15.0, 20.0, 20.0}) {
        look.tick(eye, body);
        CHECK(yaw_degrees(look) == doctest::Approx(expected).epsilon(1e-5));
        CHECK(look.pitch() == 0.0F);
    }
}

TEST_CASE("a point straight above or below moves the pitch and leaves the yaw") {
    vx::LookControl look(static_cast<float>(radians(30.0)));
    look.look_at(eye + Vec3d{.x = 0.0, .y = 5.0, .z = 0.0});
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(30.0).epsilon(1e-5));
    CHECK(pitch_degrees(look) == doctest::Approx(40.0).epsilon(1e-5));
    look.look_at(eye - Vec3d{.x = 0.0, .y = 5.0, .z = 0.0});
    look.tick(eye, 0.0F);
    CHECK(yaw_degrees(look) == doctest::Approx(30.0).epsilon(1e-5));
    CHECK(pitch_degrees(look) == doctest::Approx(-40.0).epsilon(1e-5));
}
