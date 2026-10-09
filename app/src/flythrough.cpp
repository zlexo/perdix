#include "flythrough.hpp"

#include <vx/core/vec3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>

namespace vx::app {
namespace {

constexpr double radians_per_degree = std::numbers::pi / 180.0;
// A little down at the ground ahead, except in the climb.
constexpr auto level_pitch = static_cast<float>(-10.0 * radians_per_degree);
constexpr auto climb_pitch = static_cast<float>(-45.0 * radians_per_degree);
constexpr double row_seconds = 0.001; // the turns' table, a millisecond a row

// The turns' heading `seconds` in, in the xz plane: 0 east and positive
// toward +z, swinging 90 degrees each way.
double heading(double seconds) noexcept {
    return (std::numbers::pi / 2.0) *
           std::sin(2.0 * std::numbers::pi * seconds / FlythroughPath::turn_period);
}

// RFC 0001's yaw for a heading: its camera looks along (-sin yaw, 0,
// -cos yaw).
float yaw_of(double heading) noexcept {
    return static_cast<float>(std::atan2(-std::cos(heading), -std::sin(heading)));
}

// yaw_of(0), exactly.
constexpr auto east = static_cast<float>(-std::numbers::pi / 2.0);

} // namespace

std::string_view phase_name(Phase phase) noexcept {
    switch (phase) {
    case Phase::load:
        return "load";
    case Phase::hover:
        return "hover";
    case Phase::cruise:
        return "cruise";
    case Phase::turns:
        return "turns";
    case Phase::sprint:
        return "sprint";
    case Phase::climb:
        return "climb";
    case Phase::teleport:
        return "teleport";
    }
    return "?";
}

FlythroughPlan FlythroughPlan::full() noexcept {
    return {.lengths = {PhaseLength{.seconds = 30.0, .until_complete = true},
                        PhaseLength{.seconds = 10.0, .until_complete = false},
                        PhaseLength{.seconds = 30.0, .until_complete = false},
                        PhaseLength{.seconds = 15.0, .until_complete = false},
                        PhaseLength{.seconds = 15.0, .until_complete = false},
                        PhaseLength{.seconds = 15.0, .until_complete = false},
                        PhaseLength{.seconds = 30.0, .until_complete = true}},
            .sample_every = 30,
            .view_distance = 32,
            .width = 2'560,
            .height = 1'440,
            .check = false};
}

FlythroughPlan FlythroughPlan::quick() noexcept {
    // Loading and the teleport wait for the region, which both must reach
    // for the comparison, at most 300 s: far beyond what a software driver
    // needs, so the cap stops a hang and never fails a slow machine. A view
    // of 16 chunks holds enough hills that two places keep about the same
    // chunk data; at 8 they were 23% apart.
    return {.lengths = {PhaseLength{.seconds = 300.0, .until_complete = true},
                        PhaseLength{.seconds = 0.25, .until_complete = false},
                        PhaseLength{.seconds = 0.5, .until_complete = false},
                        PhaseLength{.seconds = 0.5, .until_complete = false},
                        PhaseLength{.seconds = 0.5, .until_complete = false},
                        PhaseLength{.seconds = 0.5, .until_complete = false},
                        PhaseLength{.seconds = 300.0, .until_complete = true}},
            .sample_every = 1,
            .view_distance = 16,
            .width = 640,
            .height = 360,
            .check = true};
}

FlythroughPath::FlythroughPath(const FlythroughPlan& plan) : plan_(plan) {
    // The turns, integrated a millisecond at a time from each millisecond's
    // middle: a heading that swings at a steady speed has no closed form.
    const auto rows =
        static_cast<std::size_t>(std::ceil(plan.length(Phase::turns).seconds / row_seconds)) + 1;
    turns_.reserve(rows);
    Vec3d at{};
    turns_.push_back(at);
    for (std::size_t row = 1; row < rows; ++row) {
        const double toward = heading((static_cast<double>(row) - 0.5) * row_seconds);
        const double distance = cruise_speed * row_seconds;
        at = at +
             Vec3d{.x = std::cos(toward) * distance, .y = 0.0, .z = std::sin(toward) * distance};
        turns_.push_back(at);
    }
    const auto seconds = [&plan](Phase phase) { return plan.length(phase).seconds; };
    const auto at_start = [this](Phase phase) -> Vec3d& {
        return starts_[static_cast<std::size_t>(phase)];
    };
    at_start(Phase::load) = start;
    at_start(Phase::hover) = start;
    at_start(Phase::cruise) = start;
    at_start(Phase::turns) =
        start + Vec3d{.x = cruise_speed * seconds(Phase::cruise), .y = 0.0, .z = 0.0};
    at_start(Phase::sprint) = at_start(Phase::turns) + turned(seconds(Phase::turns));
    at_start(Phase::climb) = at_start(Phase::sprint) +
                             Vec3d{.x = sprint_speed * seconds(Phase::sprint), .y = 0.0, .z = 0.0};
    // East of the climb, back at the load's height, so the two regions
    // compare.
    const Vec3d top = at_start(Phase::climb);
    at_start(Phase::teleport) = {.x = top.x + teleport_distance, .y = start.y, .z = top.z};
}

Vec3d FlythroughPath::turned(double seconds) const noexcept {
    const double row =
        std::clamp(seconds / row_seconds, 0.0, static_cast<double>(turns_.size() - 1));
    const auto below = static_cast<std::size_t>(std::floor(row));
    const std::size_t above = std::min(below + 1, turns_.size() - 1);
    const double t = row - static_cast<double>(below);
    const Vec3d from = turns_[below];
    const Vec3d to = turns_[above];
    return from +
           Vec3d{.x = (to.x - from.x) * t, .y = (to.y - from.y) * t, .z = (to.z - from.z) * t};
}

FlythroughPose FlythroughPath::pose(Phase phase, double seconds) const noexcept {
    const double t = std::clamp(seconds, 0.0, plan_.length(phase).seconds);
    const Vec3d from = starts_[static_cast<std::size_t>(phase)];
    switch (phase) {
    case Phase::load:
    case Phase::hover:
    case Phase::teleport:
        break;
    case Phase::cruise:
        return {.eye = from + Vec3d{.x = cruise_speed * t, .y = 0.0, .z = 0.0},
                .yaw = east,
                .pitch = level_pitch};
    case Phase::turns:
        return {.eye = from + turned(t), .yaw = yaw_of(heading(t)), .pitch = level_pitch};
    case Phase::sprint:
        return {.eye = from + Vec3d{.x = sprint_speed * t, .y = 0.0, .z = 0.0},
                .yaw = east,
                .pitch = level_pitch};
    case Phase::climb: {
        const double rise = (climb_top - start.y) / climb_seconds;
        return {.eye = from + Vec3d{.x = 0.0, .y = rise * t, .z = 0.0},
                .yaw = east,
                .pitch = climb_pitch};
    }
    }
    return {.eye = from, .yaw = east, .pitch = level_pitch};
}

} // namespace vx::app
