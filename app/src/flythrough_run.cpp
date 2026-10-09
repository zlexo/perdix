#include "flythrough_run.hpp"

#include "flythrough.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// A phase's longest step and pass, with a sample's in them.
void add_longest(PhaseResult& result, const FlythroughSample& sample) {
    result.longest_step_ms = std::max(result.longest_step_ms, milliseconds_of(sample.longest_step));
    result.longest_pass_ms = std::max(result.longest_pass_ms, milliseconds_of(sample.longest_pass));
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

} // namespace vx::app
