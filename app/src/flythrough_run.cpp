#include "flythrough_run.hpp"

#include "flythrough.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace vx::app {
namespace {

double seconds_of(std::chrono::nanoseconds duration) {
    return std::chrono::duration<double>(duration).count();
}

double milliseconds_of(std::chrono::nanoseconds duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

// The nearest-rank percentile `q` (0 to 1) of `values`, which it sorts.
template <class T> T percentile(std::vector<T>& values, double q) {
    if (values.empty()) {
        return T{};
    }
    std::ranges::sort(values);
    const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(values.size())));
    return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

// `value` with `decimals` places.
std::string fixed(double value, int decimals) {
    std::array<char, 64> text{};
    const std::to_chars_result written = std::to_chars(text.data(), text.data() + text.size(),
                                                       value, std::chars_format::fixed, decimals);
    return written.ec == std::errc{} ? std::string(text.data(), written.ptr) : std::string("?");
}

std::string mebibytes(std::uint64_t bytes) {
    return fixed(static_cast<double>(bytes) / (1024.0 * 1024.0), 1);
}

std::string per_second(const std::optional<double>& rate) {
    return rate ? fixed(*rate, 0) : std::string("-");
}

// A phase's longest step and pass, with a sample's in them.
void add_longest(PhaseResult& result, const FlythroughSample& sample) {
    result.longest_step_ms = std::max(result.longest_step_ms, milliseconds_of(sample.longest_step));
    result.longest_pass_ms = std::max(result.longest_pass_ms, milliseconds_of(sample.longest_pass));
}

// A phase's lowest view distance, with a sample's in it. A sample without
// one says nothing.
void add_view_distance(PhaseResult& result, const FlythroughSample& sample) {
    if (sample.view_distance != 0 &&
        (result.least_view_distance == 0 || sample.view_distance < result.least_view_distance)) {
        result.least_view_distance = sample.view_distance;
    }
}

// Whether the world held the plan's view distance at every full sample, or
// the phases where it fell below and how far; nothing without a sample.
std::string view_distance_note(std::int32_t planned, std::span<const PhaseResult> results) {
    bool sampled = false;
    std::string lowered;
    for (const PhaseResult& result : results) {
        if (result.least_view_distance == 0) {
            continue;
        }
        sampled = true;
        if (result.least_view_distance < planned) {
            lowered += (lowered.empty() ? "" : ", ") + std::string(phase_name(result.phase)) +
                       " to " + std::to_string(result.least_view_distance);
        }
    }
    if (!sampled) {
        return {};
    }
    if (lowered.empty()) {
        return "\nThe world held view distance " + std::to_string(planned) + " at every sample.\n";
    }
    return "\nThe world's view distance fell below " + std::to_string(planned) + ": " + lowered +
           ".\n";
}

} // namespace

FlythroughRun::FlythroughRun(const FlythroughPlan& plan, Clock::time_point start)
    : plan_(plan), path_(plan), phase_start_(start) {}

Phase FlythroughRun::phase() const noexcept {
    return flythrough_phases[std::min(phase_, phase_count - 1)];
}

std::optional<FlythroughPose> FlythroughRun::frame(Clock::time_point now) {
    while (!done_) {
        const Phase current = flythrough_phases[phase_];
        const PhaseLength& length = plan_.length(current);
        const double t = seconds_of(now - phase_start_);
        if (!(length.until_complete && complete_) && t < length.seconds) {
            ++frames_since_full_;
            return path_.pose(current, t);
        }
        finish_phase(now, length.until_complete && !complete_);
    }
    return std::nullopt;
}

std::optional<FlythroughRun::SampleRequest> FlythroughRun::sample_request(bool uploads_waiting) {
    if (done_ || out_) {
        return std::nullopt;
    }
    const bool closing = unclosed_.has_value();
    const bool full = closing || !phase_sampled_ || frames_since_full_ >= plan_.sample_every;
    // While uploads wait the region is not complete, and the world would
    // answer a completion request in full each time it is.
    if (!full && (uploads_waiting || !plan_.length(phase()).until_complete)) {
        return std::nullopt;
    }
    if (full) {
        phase_sampled_ = true;
        frames_since_full_ = 0;
    }
    out_ = next_id_;
    out_closes_ = unclosed_;
    return SampleRequest{.id = next_id_++, .full = full, .before_camera = closing};
}

void FlythroughRun::frame_time(std::chrono::nanoseconds took) {
    if (!std::exchange(took_full_, false) && !done_) {
        frame_times_.push_back(took);
    }
}

void FlythroughRun::add(const FlythroughSample& sample) {
    std::optional<std::size_t> closes;
    if (out_ == sample.id) {
        out_.reset();
        closes = std::exchange(out_closes_, std::nullopt);
    }
    took_full_ = took_full_ || sample.full;
    if (done_) {
        return;
    }
    if (closes) {
        if (sample.full) {
            close(*closes, sample);
        }
        return;
    }
    // Asked for in a phase over, and not the request out: nothing to say
    // which phase it saw.
    if (sample.id < phase_first_id_) {
        return;
    }
    if (!first_) {
        first_ = sample;
    }
    last_ = sample;
    complete_ = complete_ || sample.complete;
    if (sample.full) {
        samples_.push_back(sample);
    }
}

void FlythroughRun::close(std::size_t index, const FlythroughSample& sample) {
    PhaseResult& over = results_[index];
    add_longest(over, sample);
    add_view_distance(over, sample);
    over.broken += sample.broken;
    over.chunk_bytes_end = sample.chunk_bytes;
    over.chunk_bytes_peak = std::max(over.chunk_bytes_peak, sample.chunk_bytes);
    over.mesh_bytes_end = sample.mesh_bytes;
    over.mesh_bytes_peak = std::max(over.mesh_bytes_peak, sample.mesh_bytes);
    over.footprint_bytes_end = sample.footprint_bytes;
    over.footprint_bytes_peak = std::max(over.footprint_bytes_peak, sample.footprint_bytes);
    // Where the phase just before this one ended, this one's rates start.
    if (index + 1 == results_.size() && !first_) {
        first_ = sample;
    }
    if (unclosed_ == index) {
        unclosed_.reset();
    }
}

void FlythroughRun::finish_phase(Clock::time_point now, bool capped) {
    PhaseResult result;
    result.phase = flythrough_phases[phase_];
    result.seconds = seconds_of(now - phase_start_);
    result.capped = capped;
    result.frames = frame_times_.size();
    if (!frame_times_.empty()) {
        result.frame_p50_ms = milliseconds_of(percentile(frame_times_, 0.50));
        result.frame_p99_ms = milliseconds_of(percentile(frame_times_, 0.99));
        result.frame_max_ms = milliseconds_of(frame_times_.back()); // sorted
    }
    if (!samples_.empty()) {
        std::vector<std::uint32_t> near_holes;
        std::vector<std::uint32_t> holes;
        for (const FlythroughSample& sample : samples_) {
            add_longest(result, sample);
            add_view_distance(result, sample);
            near_holes.push_back(sample.near_holes);
            holes.push_back(sample.holes);
            result.chunk_bytes_peak = std::max(result.chunk_bytes_peak, sample.chunk_bytes);
            result.mesh_bytes_peak = std::max(result.mesh_bytes_peak, sample.mesh_bytes);
            result.footprint_bytes_peak =
                std::max(result.footprint_bytes_peak, sample.footprint_bytes);
            result.broken += sample.broken;
        }
        result.near_holes_p99 = percentile(near_holes, 0.99);
        result.holes_p99 = percentile(holes, 0.99);
        const FlythroughSample& last = samples_.back();
        result.chunk_bytes_end = last.chunk_bytes;
        result.mesh_bytes_end = last.mesh_bytes;
        result.footprint_bytes_end = last.footprint_bytes;
    }
    if (first_ && last_) {
        const FlythroughSample& from = *first_;
        const FlythroughSample& to = *last_;
        const double between = seconds_of(to.at - from.at);
        if (between > 0.0) {
            const auto rate = [between](std::uint64_t start, std::uint64_t end) {
                return static_cast<double>(end - start) / between;
            };
            result.generated_per_second = rate(from.generated, to.generated);
            result.uniform_per_second = rate(from.uniform, to.uniform);
            result.meshed_per_second = rate(from.meshed, to.meshed);
        }
    }
    results_.push_back(result);
    frame_times_.clear();
    samples_.clear();
    first_.reset();
    last_.reset();
    ++phase_;
    phase_start_ = now;
    phase_sampled_ = false;
    complete_ = false;
    phase_first_id_ = next_id_;
    done_ = phase_ == phase_count;
    // A request of the phase's own still out was asked for before it ended:
    // its answer closes it. Otherwise the next phase's first does.
    if (out_ && !out_closes_) {
        out_closes_ = results_.size() - 1;
    }
    unclosed_.reset();
    if (!done_) {
        unclosed_ = results_.size() - 1;
    }
}

std::vector<std::string> flythrough_problems(std::span<const PhaseResult> results) {
    std::vector<std::string> problems;
    const PhaseResult* load = nullptr;
    const PhaseResult* teleport = nullptr;
    for (const PhaseResult& result : results) {
        const std::string name(phase_name(result.phase));
        if (result.broken > 0) {
            problems.push_back(name + ": " + std::to_string(result.broken) +
                               " invariants broken over its samples");
        }
        if (result.capped) {
            problems.push_back(name + ": the region was not complete by the wait's cap, " +
                               fixed(result.seconds, 0) + " s");
        }
        load = result.phase == Phase::load ? &result : load;
        teleport = result.phase == Phase::teleport ? &result : teleport;
    }
    if (load == nullptr || teleport == nullptr) {
        problems.emplace_back("the run ended before the teleport");
    } else if (!load->capped && !teleport->capped) {
        const auto before = static_cast<double>(load->chunk_bytes_end);
        const auto after = static_cast<double>(teleport->chunk_bytes_end);
        if (before <= 0.0 || std::abs(after - before) > 0.05 * before) {
            problems.push_back("chunk data after the teleport, " +
                               mebibytes(teleport->chunk_bytes_end) +
                               " MiB, is more than 5% off its level after the load, " +
                               mebibytes(load->chunk_bytes_end) + " MiB");
        }
    }
    return problems;
}

std::string flythrough_report(const FlythroughHeader& header,
                              std::span<const PhaseResult> results) {
    std::string report = "### Perdix flythrough " + std::to_string(flythrough_version) + "\n\n";
    report += "Perdix " + header.engine + ", " + header.build + " build. " + header.device + ", " +
              header.driver + ". " + header.settings + ".\n\n";
    report += "| Phase | Time | Frame p50 / p99 / max | Longest step / pass | Generated / uniform "
              "/ meshed a second | Holes p99 within 256 / all | Chunk data end / peak | GPU "
              "meshes end / peak | Footprint end / peak |\n";
    report += "|---|---|---|---|---|---|---|---|---|\n";
    for (const PhaseResult& result : results) {
        report +=
            "| " + std::string(phase_name(result.phase)) + " | " + fixed(result.seconds, 2) + " s" +
            (result.capped ? " (cap)" : "") + " | " + fixed(result.frame_p50_ms, 1) + " / " +
            fixed(result.frame_p99_ms, 1) + " / " + fixed(result.frame_max_ms, 1) + " ms | " +
            fixed(result.longest_step_ms, 2) + " / " + fixed(result.longest_pass_ms, 2) + " ms | " +
            per_second(result.generated_per_second) + " / " +
            per_second(result.uniform_per_second) + " / " + per_second(result.meshed_per_second) +
            " | " + std::to_string(result.near_holes_p99) + " / " +
            std::to_string(result.holes_p99) + " | " + mebibytes(result.chunk_bytes_end) + " / " +
            mebibytes(result.chunk_bytes_peak) + " MiB | " + mebibytes(result.mesh_bytes_end) +
            " / " + mebibytes(result.mesh_bytes_peak) + " MiB | " +
            mebibytes(result.footprint_bytes_end) + " / " + mebibytes(result.footprint_bytes_peak) +
            " MiB |\n";
    }
    if (header.view_distance > 0) {
        report += view_distance_note(header.view_distance, results);
    }
    return report;
}

} // namespace vx::app
