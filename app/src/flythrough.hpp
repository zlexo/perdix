#pragma once

#include <vx/core/vec3d.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace vx::app {

// RFC 0006's benchmark flythrough: the real engine over a fixed world and a
// fixed camera path. Within each phase the camera's place is a function of
// time, so every machine faces the same streaming demand per second. A
// change to the path or its inputs bumps the version, and results compare
// only within one.
inline constexpr int flythrough_version = 1;

enum class Phase : std::uint8_t { load, hover, cruise, turns, sprint, climb, teleport };
inline constexpr std::size_t phase_count = 7;
inline constexpr std::array<Phase, phase_count> flythrough_phases{
    Phase::load,   Phase::hover, Phase::cruise,  Phase::turns,
    Phase::sprint, Phase::climb, Phase::teleport};

[[nodiscard]] std::string_view phase_name(Phase phase) noexcept;

// How long a phase runs: its seconds, or for one that waits for the region
// to be complete (load and teleport), at most that.
struct PhaseLength {
    double seconds = 0.0;
    bool until_complete = false;
};

// How a flythrough runs: RFC 0006's, or the few seconds CI runs on lavapipe,
// which take the same path for less time, in a smaller region and window,
// sampling as often as it can to check the streaming invariants. Neither
// asserts a time: a wait's cap only ends a run that would hang.
struct FlythroughPlan {
    std::array<PhaseLength, phase_count> lengths{};
    std::uint64_t sample_every = 30; // frames
    std::int32_t view_distance = 32;
    std::uint32_t width = 2'560; // the framebuffer's, in pixels
    std::uint32_t height = 1'440;
    // Each full sample checks RFC 0006's invariants, and the run fails on a
    // broken one, on a wait that ran into its cap, or on chunk data after
    // the teleport more than 5% off its level after the load.
    bool check = false;

    [[nodiscard]] static FlythroughPlan full() noexcept;
    [[nodiscard]] static FlythroughPlan quick() noexcept;
    [[nodiscard]] const PhaseLength& length(Phase phase) const noexcept {
        return lengths[static_cast<std::size_t>(phase)];
    }
};

// Where the camera is and looks, as RFC 0001's camera takes its yaw and
// pitch.
struct FlythroughPose {
    Vec3d eye;
    float yaw = 0.0F;
    float pitch = 0.0F;
};

// The path: still over the origin for `load` and `hover`, east at 64 blocks
// a second, S-curves at 64 swinging the view 90 degrees each way, east at
// 512, up to y = 2,000 looking 45 degrees down, and a million blocks east.
// Each phase starts where the one before it ends.
class FlythroughPath {
public:
    explicit FlythroughPath(const FlythroughPlan& plan);

    // The camera `seconds` into `phase`, held at the phase's end past it.
    [[nodiscard]] FlythroughPose pose(Phase phase, double seconds) const noexcept;

    // RFC 0006's numbers.
    static constexpr Vec3d start{.x = 0.0, .y = 160.0, .z = 0.0};
    static constexpr double cruise_speed = 64.0;  // blocks a second, in cruise and turns
    static constexpr double turn_period = 7.5;    // seconds for one S, both ways
    static constexpr double sprint_speed = 512.0; // blocks a second
    static constexpr double climb_top = 2'000.0;
    static constexpr double climb_seconds = 15.0; // to the top, at RFC 0006's length
    static constexpr double teleport_distance = 1'000'000.0;

private:
    // The turns' offset from where they start, `seconds` into them.
    [[nodiscard]] Vec3d turned(double seconds) const noexcept;

    FlythroughPlan plan_;
    // The turns' offsets a millisecond apart, integrated once.
    std::vector<Vec3d> turns_;
    std::array<Vec3d, phase_count> starts_{};
};

} // namespace vx::app
