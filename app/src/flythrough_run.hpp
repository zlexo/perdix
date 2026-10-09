#pragma once

#include "flythrough.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vx::app {

// One sample of a flythrough: the world thread's numbers and the main
// thread's own, as the app puts them together. A completion sample, not
// full, holds only whether the region is complete, and the totals.
struct FlythroughSample {
    std::uint32_t id = 0; // the request's, in the order they were made
    bool full = true;
    // When the world thread took it. Its {} lets designated initializers
    // leave it out (Linux Clang's -Wmissing-designated-field-initializers).
    std::chrono::steady_clock::time_point at{}; // NOLINT(readability-redundant-member-init)
    // Totals so far: chunks built by a job, chunks found uniform without
    // one, and meshes made by a job.
    std::uint64_t generated = 0;
    std::uint64_t uniform = 0;
    std::uint64_t meshed = 0;
    // Since the full sample before: the world thread's longest streaming
    // step, and its longest pass.
    std::chrono::nanoseconds longest_step{0};
    std::chrono::nanoseconds longest_pass{0};
    std::uint32_t holes = 0;
    std::uint32_t near_holes = 0; // within 256 blocks
    std::uint64_t chunk_bytes = 0;
    std::uint64_t mesh_bytes = 0;      // the arena's quads, live and retired
    std::uint64_t footprint_bytes = 0; // the process's
    // The region is complete, and every mesh is uploaded.
    bool complete = false;
    // RFC 0006's invariants found broken, when the plan checks them.
    std::uint32_t broken = 0;
};

// A phase's numbers (RFC 0006).
struct PhaseResult {
    Phase phase = Phase::load;
    double seconds = 0.0;
    bool capped = false; // a phase waiting for the region stopped at its cap
    // Frame times, the frames that took in a full sample left out.
    std::size_t frames = 0;
    double frame_p50_ms = 0.0;
    double frame_p99_ms = 0.0;
    double frame_max_ms = 0.0;
    double longest_step_ms = 0.0;
    double longest_pass_ms = 0.0;
    // A second, between the phase's first and last samples, completion ones
    // included; none with fewer than two.
    std::optional<double> generated_per_second;
    std::optional<double> uniform_per_second;
    std::optional<double> meshed_per_second;
    std::uint32_t near_holes_p99 = 0;
    std::uint32_t holes_p99 = 0;
    // At the phase's last sample, and the most of any.
    std::uint64_t chunk_bytes_end = 0;
    std::uint64_t chunk_bytes_peak = 0;
    std::uint64_t mesh_bytes_end = 0;
    std::uint64_t mesh_bytes_peak = 0;
    std::uint64_t footprint_bytes_end = 0;
    std::uint64_t footprint_bytes_peak = 0;
    std::uint64_t broken = 0; // invariants broken, over the phase's samples
};

// A flythrough as the app runs it: the phase each frame falls in and the
// camera there, when to sample the world, and each phase's numbers.
class FlythroughRun {
public:
    using Clock = std::chrono::steady_clock;

    // A sample to ask the world thread for: full, or only whether the region
    // is complete. One that closes the phase before goes ahead of the
    // frame's camera, so that its answer sees the world where that phase
    // ended, or a frame or so after when a request still out held it back.
    struct SampleRequest {
        std::uint32_t id = 0;
        bool full = true;
        bool before_camera = false;
    };

    FlythroughRun(const FlythroughPlan& plan, Clock::time_point start);

    // The camera for the frame drawn at `now`, once the phases that are
    // over have ended; none after the last.
    [[nodiscard]] std::optional<FlythroughPose> frame(Clock::time_point now);
    // The sample to ask for this frame, if any; none while one is out, so
    // requests never pile up. A full one on the first frame of each phase
    // and every plan's sample_every frames (RFC 0006 says 30); in a phase
    // waiting for the region, a completion one on the frames between once no
    // upload waits on the main thread, so the wait ends a frame or two after
    // the region is complete.
    [[nodiscard]] std::optional<SampleRequest> sample_request(bool uploads_waiting);
    // How long the frame before took. One that took in a full sample is left
    // out of the frame times: it counted the main thread's holes and read
    // the arena and the process's memory, as RFC 0006 leaves out the frames
    // that count holes.
    void frame_time(std::chrono::nanoseconds took);
    // A sample came back. One asked for to close a phase, as the next
    // phase's first or in the phase itself before it ended, closes it: its
    // longest step and pass, the invariants it found broken and the memory
    // where the phase ended go to it, and when it closes the phase just
    // before this one, this phase's rates start from its totals. Whether the
    // region is complete counts only for the phase that asked.
    void add(const FlythroughSample& sample);

    [[nodiscard]] Phase phase() const noexcept;
    [[nodiscard]] const FlythroughPlan& plan() const noexcept { return plan_; }
    [[nodiscard]] bool done() const noexcept { return done_; }
    [[nodiscard]] const std::vector<PhaseResult>& results() const noexcept { return results_; }

private:
    void finish_phase(Clock::time_point now, bool capped);
    void close(std::size_t index, const FlythroughSample& sample);

    FlythroughPlan plan_;
    FlythroughPath path_;
    std::size_t phase_ = 0;
    Clock::time_point phase_start_;
    bool done_ = false;
    std::uint64_t frames_since_full_ = 0;
    bool phase_sampled_ = false;       // a full sample was asked for in this phase
    std::optional<std::uint32_t> out_; // the request not answered yet
    // The phase whose result the request out closes, if it closes one; and
    // the last phase over that no sample has closed yet.
    std::optional<std::size_t> out_closes_;
    std::optional<std::size_t> unclosed_;
    bool took_full_ = false; // a full sample came since the last frame time
    std::uint32_t next_id_ = 0;
    std::uint32_t phase_first_id_ = 0;
    bool complete_ = false;
    std::vector<std::chrono::nanoseconds> frame_times_;
    std::vector<FlythroughSample> samples_; // full ones
    // The phase's first and last samples of any kind, for its rates.
    std::optional<FlythroughSample> first_;
    std::optional<FlythroughSample> last_;
    std::vector<PhaseResult> results_;
};

// What the report opens with: the versions, the build, the machine and the
// settings.
struct FlythroughHeader {
    std::string engine; // version and commit
    std::string build;  // "release", "debug"
    std::string device; // the GPU
    std::string driver;
    std::string settings; // resolution, view distance, workers, present mode
};

// What a checked flythrough found wrong (RFC 0006's CI run): invariants
// broken, a wait for the region that ran into its cap, or chunk data after
// the teleport more than 5% off its level after the load. Empty when all is
// well.
[[nodiscard]] std::vector<std::string> flythrough_problems(std::span<const PhaseResult> results);

// RFC 0006's Markdown block, for pull requests and benchmark results.
[[nodiscard]] std::string flythrough_report(const FlythroughHeader& header,
                                            std::span<const PhaseResult> results);

} // namespace vx::app
