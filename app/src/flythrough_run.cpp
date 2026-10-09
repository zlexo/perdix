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
        if (sample.full && unclosed_ == *closes) {
            unclosed_.reset();
        }
        return;
    }
    // Asked for in a phase over, and not the request out: nothing to say
    // which phase it saw.
    if (sample.id < phase_first_id_) {
        return;
    }
    complete_ = complete_ || sample.complete;
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
    results_.push_back(result);
    frame_times_.clear();
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
