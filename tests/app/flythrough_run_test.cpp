#include "flythrough.hpp"
#include "flythrough_run.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// RFC 0006's flythrough as the app runs it: phases that end at their
// lengths or with the region complete, full samples every 30th frame and
// completion ones between while a phase waits, and each phase's numbers.

namespace {

using namespace std::chrono_literals;
using vx::app::FlythroughPlan;
using vx::app::FlythroughRun;
using vx::app::FlythroughSample;
using vx::app::Phase;
using Clock = FlythroughRun::Clock;
using Request = FlythroughRun::SampleRequest;

// Frames at `at` until a sample is asked for, no upload waiting unless
// `uploads_waiting`.
Request next_request(FlythroughRun& run, Clock::time_point at, bool uploads_waiting = false) {
    for (int frame = 0; frame < 100; ++frame) {
        REQUIRE(run.frame(at).has_value());
        if (const std::optional<Request> request = run.sample_request(uploads_waiting)) {
            return *request;
        }
    }
    FAIL("no sample was asked for in 100 frames");
    return {};
}

// The id of the next full request, frames at `at` until one comes.
std::uint32_t next_sample(FlythroughRun& run, Clock::time_point at) {
    const Request request = next_request(run, at);
    REQUIRE(request.full);
    return request.id;
}

// The world's answer to `request`, taken at `at`: full when it was asked for
// one, or when the region is complete.
FlythroughSample answer(const Request& request, Clock::time_point at, bool complete) {
    return {.id = request.id, .full = request.full || complete, .at = at, .complete = complete};
}

} // namespace

TEST_CASE("phases end at their lengths, and those that wait for the region once theirs is "
          "complete") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    // Loading waits for a sample of its own that says the region is complete.
    Request request = next_request(run, start);
    CHECK(request.full);
    run.add(answer(request, start, false));
    REQUIRE(run.frame(start + 5s).has_value());
    CHECK(run.phase() == Phase::load);
    request = next_request(run, start + 6s);
    run.add(answer(request, start + 6s, true));
    REQUIRE(run.frame(start + 7s).has_value());
    CHECK(run.phase() == Phase::hover);
    // Then each phase its seconds: 10, 30, 15, 15 and 15.
    Clock::time_point at = start + 7s;
    for (const auto& [phase, seconds] : std::vector<std::pair<Phase, int>>{{Phase::hover, 10},
                                                                           {Phase::cruise, 30},
                                                                           {Phase::turns, 15},
                                                                           {Phase::sprint, 15},
                                                                           {Phase::climb, 15}}) {
        INFO(vx::app::phase_name(phase));
        REQUIRE(run.frame(at + std::chrono::seconds(seconds) - 1ms).has_value());
        CHECK(run.phase() == phase);
        at += std::chrono::seconds(seconds);
        REQUIRE(run.frame(at).has_value());
        CHECK(run.phase() != phase);
    }
    // The teleport never hears complete: it stops at its 30 seconds.
    CHECK(run.phase() == Phase::teleport);
    CHECK_FALSE(run.frame(at + 30s).has_value());
    CHECK(run.done());
    const std::vector<vx::app::PhaseResult>& results = run.results();
    REQUIRE(results.size() == vx::app::phase_count);
    CHECK(results[0].seconds == doctest::Approx(7.0));
    CHECK_FALSE(results[0].capped);
    CHECK(results[1].seconds == doctest::Approx(10.0));
    CHECK(results[6].capped);
    CHECK(results[6].seconds == doctest::Approx(30.0));
}

TEST_CASE("a phase waiting for the region asks if it is complete on the frames between full "
          "samples, one request at a time") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    // The first frame of the load asks for a full sample, and while it is
    // out, nothing more.
    const Request first = next_request(run, start);
    CHECK(first.full);
    for (int frame = 0; frame < 10; ++frame) {
        REQUIRE(run.frame(start).has_value());
        CHECK_FALSE(run.sample_request(false).has_value());
    }
    run.add(answer(first, start, false));
    // Its answer back, each frame asks only whether the region is complete,
    // until a full sample is due again, 30 frames after the first.
    int completions = 0;
    Request request = next_request(run, start);
    while (!request.full) {
        ++completions;
        run.add(answer(request, start, false));
        request = next_request(run, start);
    }
    CHECK(completions == 19);
    run.add(answer(request, start, false));
    request = next_request(run, start + 1s);
    CHECK_FALSE(request.full);
    // Asked whether it is complete, the world answers a complete region in
    // full: the wait ends at once, and the totals end the phase's rates.
    run.add({.id = request.id,
             .full = true,
             .at = start + 1s,
             .generated = 2'000,
             .uniform = 500,
             .meshed = 1'000,
             .complete = true});
    REQUIRE(run.frame(start + 1s).has_value());
    CHECK(run.phase() == Phase::hover);
    const vx::app::PhaseResult& load = run.results().at(0);
    CHECK(load.seconds == doctest::Approx(1.0));
    REQUIRE(load.generated_per_second.has_value());
    CHECK(*load.generated_per_second == doctest::Approx(2'000.0));
    CHECK(*load.uniform_per_second == doctest::Approx(500.0));
    CHECK(*load.meshed_per_second == doctest::Approx(1'000.0));
    // A phase that does not wait asks for full samples only: on its first
    // frame, before its camera, and every 30th.
    request = next_request(run, start + 2s);
    CHECK(request.full);
    CHECK(request.before_camera);
    run.add(answer(request, start + 2s, false));
    int frames = 0;
    std::optional<Request> next;
    while (!next) {
        REQUIRE(run.frame(start + 2s).has_value());
        ++frames;
        next = run.sample_request(false);
    }
    CHECK(frames == 30);
    CHECK(next->full);
    CHECK_FALSE(next->before_camera);
}

TEST_CASE("a phase waiting for the region asks whether it is complete only once no upload waits") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    run.add(answer(next_request(run, start), start, false));
    // Uploads waiting: no completion request, the full ones every 30 frames.
    int frames = 0;
    std::optional<Request> next;
    while (!next) {
        REQUIRE(run.frame(start).has_value());
        ++frames;
        next = run.sample_request(true);
    }
    CHECK(frames == 30);
    CHECK(next->full);
    run.add(answer(*next, start, false));
    // None waiting: the next frame asks.
    REQUIRE(run.frame(start).has_value());
    const std::optional<Request> completion = run.sample_request(false);
    REQUIRE(completion.has_value());
    CHECK_FALSE(completion->full);
}

TEST_CASE("a phase's first request goes before its camera and closes the phase before, unless "
          "one asked for in that phase does") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    // The load's first request goes after the camera: no phase came before.
    Request request = next_request(run, start);
    CHECK_FALSE(request.before_camera);
    run.add({.id = request.id, .at = start, .longest_step = 1ms, .complete = true});
    REQUIRE(run.frame(start + 1ms).has_value());
    REQUIRE(run.phase() == Phase::hover);
    // The hover's first goes before its camera and closes the load: the step
    // and pass, the invariants broken and the memory where the load ended.
    request = next_request(run, start + 1ms);
    CHECK(request.full);
    CHECK(request.before_camera);
    run.add({.id = request.id,
             .at = start,
             .generated = 500,
             .longest_step = 2ms,
             .longest_pass = 3ms,
             .chunk_bytes = 10,
             .broken = 1});
    CHECK(run.results().at(0).longest_step_ms == doctest::Approx(2.0));
    CHECK(run.results().at(0).longest_pass_ms == doctest::Approx(3.0));
    CHECK(run.results().at(0).broken == 1);
    CHECK(run.results().at(0).chunk_bytes_end == 10);
    // The hover's own sample 30 frames on, then one asked for at its end and
    // answered in the cruise: that one closes the hover, the cruise asking
    // nothing while it is out and no first of its own before its camera.
    request = next_request(run, start + 1s);
    CHECK_FALSE(request.before_camera);
    run.add({.id = request.id,
             .at = start + 1s,
             .generated = 1'500,
             .longest_step = 1ms,
             .longest_pass = 1ms,
             .chunk_bytes = 20});
    const Request late = next_request(run, start + 2s);
    REQUIRE(run.frame(start + 10s + 1ms).has_value());
    REQUIRE(run.phase() == Phase::cruise);
    CHECK_FALSE(run.sample_request(false).has_value());
    run.add({.id = late.id,
             .at = start + 10s,
             .generated = 2'500,
             .longest_step = 5ms,
             .longest_pass = 6ms,
             .chunk_bytes = 40,
             .broken = 2});
    request = next_request(run, start + 10s + 2ms);
    CHECK_FALSE(request.before_camera);
    run.add({.id = request.id,
             .at = start + 11s,
             .generated = 3'500,
             .longest_step = 7ms,
             .longest_pass = 8ms});
    REQUIRE(run.frame(start + 40s + 2ms).has_value());
    REQUIRE(run.phase() == Phase::turns);
    const vx::app::PhaseResult& hover = run.results().at(1);
    CHECK(hover.longest_step_ms == doctest::Approx(5.0));
    CHECK(hover.longest_pass_ms == doctest::Approx(6.0));
    CHECK(hover.broken == 2);
    CHECK(hover.chunk_bytes_end == 40);
    CHECK(hover.chunk_bytes_peak == 40);
    // Its rates ran from where the load ended to its own sample, and the
    // cruise's from where the hover ended to its own.
    REQUIRE(hover.generated_per_second.has_value());
    CHECK(*hover.generated_per_second == doctest::Approx(1'000.0));
    const vx::app::PhaseResult& cruise = run.results().at(2);
    CHECK(cruise.longest_step_ms == doctest::Approx(7.0));
    CHECK(cruise.longest_pass_ms == doctest::Approx(8.0));
    CHECK(cruise.broken == 0);
    REQUIRE(cruise.generated_per_second.has_value());
    CHECK(*cruise.generated_per_second == doctest::Approx(1'000.0));
}

TEST_CASE("a closing sample closes the phase it was asked for, though another has ended since") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::quick(), start);
    Request request = next_request(run, start);
    run.add(
        {.id = request.id, .at = start, .longest_step = 1ms, .chunk_bytes = 50, .complete = true});
    REQUIRE(run.frame(start + 1ms).has_value());
    REQUIRE(run.phase() == Phase::hover);
    // The hover's first, to close the load, is still out when the hover's
    // quarter second ends.
    const Request hover_first = next_request(run, start + 1ms);
    CHECK(hover_first.before_camera);
    REQUIRE(run.frame(start + 300ms).has_value());
    REQUIRE(run.phase() == Phase::cruise);
    run.add({.id = hover_first.id,
             .at = start + 1ms,
             .generated = 100,
             .longest_step = 9ms,
             .chunk_bytes = 60,
             .broken = 1});
    // It closed the load, not the hover.
    CHECK(run.results().at(0).longest_step_ms == doctest::Approx(9.0));
    CHECK(run.results().at(0).broken == 1);
    CHECK(run.results().at(0).chunk_bytes_end == 60);
    CHECK(run.results().at(1).broken == 0);
    CHECK(run.results().at(1).longest_step_ms == doctest::Approx(0.0));
    // The hover is still to close: the cruise's next request does, before
    // its camera, and the cruise's rates start there.
    request = next_request(run, start + 301ms);
    CHECK(request.before_camera);
    run.add({.id = request.id,
             .at = start + 301ms,
             .generated = 200,
             .longest_step = 3ms,
             .chunk_bytes = 70});
    CHECK(run.results().at(1).longest_step_ms == doctest::Approx(3.0));
    CHECK(run.results().at(1).chunk_bytes_end == 70);
    request = next_request(run, start + 501ms);
    CHECK_FALSE(request.before_camera);
    run.add({.id = request.id, .at = start + 501ms, .generated = 300});
    REQUIRE(run.frame(start + 900ms).has_value());
    REQUIRE(run.phase() == Phase::turns);
    const vx::app::PhaseResult& cruise = run.results().at(2);
    REQUIRE(cruise.generated_per_second.has_value());
    CHECK(*cruise.generated_per_second == doctest::Approx(500.0));
}

TEST_CASE("only a phase's own sample ends its wait: not one asked for before it began, nor its "
          "first, which closes the phase before") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::quick(), start);
    run.add(answer(next_request(run, start), start, true));
    // Through the fixed phases to the climb, asking for nothing on the way.
    Clock::time_point at = start + 1ms;
    REQUIRE(run.frame(at).has_value());
    while (run.phase() != Phase::climb) {
        at += 10ms;
        REQUIRE(run.frame(at).has_value());
    }
    // The climb's first request, still out when the teleport begins.
    const Request from_climb = next_request(run, at);
    CHECK(from_climb.before_camera);
    at += 600ms;
    REQUIRE(run.frame(at).has_value());
    REQUIRE(run.phase() == Phase::teleport);
    run.add(answer(from_climb, at, true));
    REQUIRE(run.frame(at + 100ms).has_value());
    CHECK(run.phase() == Phase::teleport);
    // The teleport's first closes the climb: answered from where the climb
    // ended, which was complete, it ends no wait.
    const Request first = next_request(run, at + 200ms);
    CHECK(first.before_camera);
    run.add(answer(first, at + 200ms, true));
    REQUIRE(run.frame(at + 300ms).has_value());
    CHECK(run.phase() == Phase::teleport);
    // One of its own does.
    const Request own = next_request(run, at + 400ms);
    CHECK_FALSE(own.before_camera);
    run.add(answer(own, at + 400ms, true));
    CHECK_FALSE(run.frame(at + 500ms).has_value());
    CHECK_FALSE(run.results().back().capped);
}

TEST_CASE("frame times leave out the frames that took in a full sample, at nearest-rank "
          "percentiles") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    const std::uint32_t id = next_sample(run, start);
    run.add({.id = id, .at = start, .complete = true});
    run.frame_time(1ms); // the load's
    REQUIRE(run.frame(start + 1ms).has_value());
    REQUIRE(run.phase() == Phase::hover);
    // A hundred frames of 1 to 100 ms, every tenth taking in a full sample
    // and every tenth from the fifth a completion one: 90 timed.
    for (int ms = 1; ms <= 100; ++ms) {
        if (ms % 10 == 0) {
            run.add({.id = 100, .at = start + 1s});
        } else if (ms % 10 == 5) {
            run.add({.id = 100, .full = false, .at = start + 1s});
        }
        run.frame_time(std::chrono::milliseconds(ms));
    }
    REQUIRE(run.frame(start + 11s).has_value());
    REQUIRE(run.phase() == Phase::cruise);
    const vx::app::PhaseResult& hover = run.results().at(1);
    CHECK(hover.frames == 90);
    CHECK(hover.frame_p50_ms == doctest::Approx(49.0)); // the 45th of 90
    CHECK(hover.frame_p99_ms == doctest::Approx(99.0)); // the 90th
    CHECK(hover.frame_max_ms == doctest::Approx(99.0));
}

TEST_CASE("a phase's rates run between its first and last samples, with holes at p99 and memory "
          "at its end and peak") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    const std::uint32_t id = next_sample(run, start);
    run.add({.id = id, .at = start, .complete = true});
    const Clock::time_point cruise = start + 10s + 1ms;
    REQUIRE(run.frame(start + 1ms).has_value());
    REQUIRE(run.frame(cruise).has_value());
    REQUIRE(run.phase() == Phase::cruise);
    const std::vector<FlythroughSample> samples{{.id = 0,
                                                 .at = cruise,
                                                 .generated = 1'000,
                                                 .uniform = 500,
                                                 .meshed = 100,
                                                 .longest_step = 300us,
                                                 .holes = 5,
                                                 .near_holes = 1,
                                                 .chunk_bytes = 10,
                                                 .mesh_bytes = 4,
                                                 .footprint_bytes = 100},
                                                {.id = 0,
                                                 .at = cruise + 1s,
                                                 .generated = 2'000,
                                                 .uniform = 900,
                                                 .meshed = 400,
                                                 .longest_step = 900us,
                                                 .holes = 0,
                                                 .near_holes = 0,
                                                 .chunk_bytes = 30,
                                                 .mesh_bytes = 8,
                                                 .footprint_bytes = 300},
                                                {.id = 0,
                                                 .at = cruise + 3s,
                                                 .generated = 4'000,
                                                 .uniform = 2'000,
                                                 .meshed = 1'300,
                                                 .longest_step = 200us,
                                                 .holes = 7,
                                                 .near_holes = 2,
                                                 .chunk_bytes = 20,
                                                 .mesh_bytes = 6,
                                                 .footprint_bytes = 200}};
    for (FlythroughSample sample : samples) {
        sample.id = next_sample(run, sample.at);
        run.add(sample);
    }
    REQUIRE(run.frame(cruise + 30s).has_value());
    const vx::app::PhaseResult& result = run.results().at(2);
    REQUIRE(result.phase == Phase::cruise);
    REQUIRE(result.generated_per_second.has_value());
    CHECK(*result.generated_per_second == doctest::Approx(1'000.0));
    CHECK(*result.uniform_per_second == doctest::Approx(500.0));
    CHECK(*result.meshed_per_second == doctest::Approx(400.0));
    CHECK(result.longest_step_ms == doctest::Approx(0.9));
    CHECK(result.holes_p99 == 7);
    CHECK(result.near_holes_p99 == 2);
    CHECK(result.chunk_bytes_end == 20);
    CHECK(result.chunk_bytes_peak == 30);
    CHECK(result.mesh_bytes_end == 6);
    CHECK(result.mesh_bytes_peak == 8);
    CHECK(result.footprint_bytes_end == 200);
    CHECK(result.footprint_bytes_peak == 300);
    // The hover had no sample of its own: no rates.
    CHECK_FALSE(run.results().at(1).generated_per_second.has_value());
}

TEST_CASE("a phase's view distance is the world's lowest at its own full samples and at the one "
          "that closes it") {
    const Clock::time_point start{};
    FlythroughRun run(FlythroughPlan::full(), start);
    const std::uint32_t id = next_sample(run, start);
    run.add({.id = id, .at = start, .view_distance = 32, .complete = true});
    const Clock::time_point cruise = start + 10s + 1ms;
    REQUIRE(run.frame(start + 1ms).has_value());
    REQUIRE(run.frame(cruise).has_value());
    REQUIRE(run.phase() == Phase::cruise);
    // The cruise's first sample closes the hover, which had none of its own;
    // the cruise's own two follow.
    for (const std::int32_t chunks : {20, 30, 26}) {
        run.add({.id = next_sample(run, cruise), .at = cruise, .view_distance = chunks});
    }
    REQUIRE(run.frame(cruise + 30s).has_value());
    REQUIRE(run.phase() == Phase::turns);
    CHECK(run.results().at(0).least_view_distance == 32);
    CHECK(run.results().at(1).least_view_distance == 20);
    CHECK(run.results().at(2).least_view_distance == 26);
}

TEST_CASE("the report opens with what was run, then a row for each phase") {
    vx::app::PhaseResult load;
    load.phase = Phase::load;
    load.seconds = 1.834;
    load.frame_p50_ms = 4.21;
    load.frame_p99_ms = 9.06;
    load.frame_max_ms = 15.3;
    load.longest_step_ms = 0.714;
    load.longest_pass_ms = 1.5;
    load.generated_per_second = 9'876.4;
    load.uniform_per_second = 21'000.0;
    load.meshed_per_second = 8'765.0;
    load.chunk_bytes_end = 100 * 1024 * 1024;
    load.chunk_bytes_peak = 101 * 1024 * 1024;
    vx::app::PhaseResult teleport;
    teleport.phase = Phase::teleport;
    teleport.seconds = 30.0;
    teleport.capped = true;
    const std::string report = vx::app::flythrough_report(
        {.engine = "0.0.0 (c3a3bff)",
         .build = "release",
         .device = "Apple M3 (integrated GPU)",
         .driver = "KosmicKrisp",
         .settings = "2560x1440, view distance 32, 6 workers, immediate present"},
        std::vector<vx::app::PhaseResult>{load, teleport});
    CHECK(report.starts_with("### Perdix flythrough 1\n"));
    CHECK(report.find("Perdix 0.0.0 (c3a3bff), release build. Apple M3 (integrated GPU), "
                      "KosmicKrisp. 2560x1440, view distance 32, 6 workers, immediate present.") !=
          std::string::npos);
    CHECK(report.find("| load | 1.83 s | 4.2 / 9.1 / 15.3 ms | 0.71 / 1.50 ms | 9876 / 21000 / "
                      "8765 | 0 / 0 | 100.0 / 101.0 MiB |") != std::string::npos);
    CHECK(report.find("| teleport | 30.00 s (cap) |") != std::string::npos);
    CHECK(report.find("| - / - / - |") != std::string::npos);
}

TEST_CASE("under the table the report says whether the world held the view distance, and where "
          "it did not") {
    const auto phase = [](Phase which, std::int32_t least) {
        vx::app::PhaseResult result;
        result.phase = which;
        result.least_view_distance = least;
        return result;
    };
    const vx::app::FlythroughHeader header{.engine = "0.0.0 (c3a3bff)",
                                           .build = "release",
                                           .device = "Apple M3 (integrated GPU)",
                                           .driver = "KosmicKrisp",
                                           .settings = "2560x1440, view distance 64",
                                           .view_distance = 64};
    CHECK(vx::app::flythrough_report(
              header,
              std::vector{phase(Phase::load, 64), phase(Phase::hover, 0), phase(Phase::cruise, 64)})
              .ends_with("|\n\nThe world held view distance 64 at every sample.\n"));
    CHECK(vx::app::flythrough_report(header,
                                     std::vector{phase(Phase::load, 64), phase(Phase::cruise, 48),
                                                 phase(Phase::sprint, 40)})
              .ends_with("|\n\nThe world's view distance fell below 64: cruise to 48, sprint to "
                         "40.\n"));
    // No sample, no claim; nor without a distance to check.
    CHECK(vx::app::flythrough_report(header, std::vector{phase(Phase::load, 0)}).ends_with("|\n"));
    vx::app::FlythroughHeader unplanned = header;
    unplanned.view_distance = 0;
    CHECK(vx::app::flythrough_report(unplanned, std::vector{phase(Phase::cruise, 48)})
              .ends_with("|\n"));
}

TEST_CASE("a checked run's problems: invariants broken, a wait that hit its cap, and chunk data "
          "that did not come back") {
    const auto phase = [](Phase which, std::uint64_t chunk_bytes) {
        vx::app::PhaseResult result;
        result.phase = which;
        result.seconds = 1.0;
        result.chunk_bytes_end = chunk_bytes;
        return result;
    };
    std::vector<vx::app::PhaseResult> results;
    for (const Phase which : vx::app::flythrough_phases) {
        results.push_back(phase(which, 100 * 1024 * 1024));
    }
    CHECK(vx::app::flythrough_problems(results).empty());
    // 4% more after the teleport is still the same level; 6% is not.
    results.back().chunk_bytes_end = 104 * 1024 * 1024;
    CHECK(vx::app::flythrough_problems(results).empty());
    results.back().chunk_bytes_end = 106 * 1024 * 1024;
    REQUIRE(vx::app::flythrough_problems(results).size() == 1);
    CHECK(vx::app::flythrough_problems(results).front().find("more than 5% off") !=
          std::string::npos);
    results.back().chunk_bytes_end = 100 * 1024 * 1024;
    // A broken invariant names its phase.
    results[2].broken = 3;
    REQUIRE(vx::app::flythrough_problems(results).size() == 1);
    CHECK(vx::app::flythrough_problems(results).front() ==
          "cruise: 3 invariants broken over its samples");
    results[2].broken = 0;
    // A wait that ran into its cap says so, and the comparison is skipped.
    results.front().capped = true;
    results.front().chunk_bytes_end = 1;
    REQUIRE(vx::app::flythrough_problems(results).size() == 1);
    CHECK(vx::app::flythrough_problems(results).front() ==
          "load: the region was not complete by the wait's cap, 1 s");
    // A run that stopped before the teleport cannot compare.
    results.front().capped = false;
    results.pop_back();
    REQUIRE(vx::app::flythrough_problems(results).size() == 1);
    CHECK(vx::app::flythrough_problems(results).front() == "the run ended before the teleport");
}
