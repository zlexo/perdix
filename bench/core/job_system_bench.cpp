#include "../benchmarks.hpp"

#include <vx/core/job_system.hpp>

#include <nanobench.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace vx::bench {
namespace {

// Notes when it started, in steady clock ticks.
struct Started {
    std::atomic<std::int64_t>* at;

    void operator()(JobContext& /*context*/) const noexcept {
        at->store(std::chrono::steady_clock::now().time_since_epoch().count(),
                  std::memory_order_release);
    }
};

// RFC 0005's budget for an idle worker starting an urgent job: under 50 us at
// the 99th percentile. Each submit follows a pause long enough for every
// worker to be asleep again; the times are printed as percentiles, which
// nanobench does not give.
void wake_up_latency(bool quick) {
    const int count = quick ? 5 : 1'000;
    JobSystem jobs({.workers = 6});
    std::atomic<std::int64_t> started{0};
    std::vector<double> waits;
    waits.reserve(static_cast<std::size_t>(count));
    const auto nothing = [](JobContext& /*context*/) noexcept {};
    for (int i = 0; i < count; ++i) {
        // A normal job first: the worker that ran it wakes next, asleep at
        // utility as workers are between streaming jobs, so the urgent job
        // raises it. Woken again after an urgent job, it would already be at
        // user-initiated, and the raise would go untimed.
        while (!jobs.submit({}, nothing)) {
            std::this_thread::yield();
        }
        jobs.wait_until_idle();
        std::this_thread::sleep_for(std::chrono::microseconds(300));
        started.store(0, std::memory_order_relaxed);
        const auto submitted = std::chrono::steady_clock::now();
        while (!jobs.submit({.level = JobLevel::urgent}, Started{&started})) {
            std::this_thread::yield();
        }
        std::int64_t at = 0;
        while ((at = started.load(std::memory_order_acquire)) == 0) {
            std::this_thread::yield();
        }
        const std::chrono::steady_clock::duration wait =
            std::chrono::steady_clock::duration(at) - submitted.time_since_epoch();
        waits.push_back(std::chrono::duration<double, std::micro>(wait).count());
    }
    jobs.wait_until_idle();
    std::ranges::sort(waits);
    const auto percentile = [&waits](double p) {
        return waits[static_cast<std::size_t>(p * static_cast<double>(waits.size() - 1))];
    };
    std::cout << "job system, 6 workers: a sleeping worker starts an urgent job: p50 "
              << percentile(0.5) << " us, p99 " << percentile(0.99) << " us, max " << waits.back()
              << " us (" << count << " wake-ups)\n";
}

} // namespace

void job_system(ankerl::nanobench::Bench& bench) {
    JobSystem jobs({});
    // Keys as streaming's are: squared distances, in no order.
    std::vector<std::uint32_t> keys(1'000);
    std::mt19937 generator(17);
    std::uniform_int_distribution<std::uint32_t> distance(0, 3 * 32 * 32);
    for (std::uint32_t& key : keys) {
        key = distance(generator);
    }
    std::uint64_t ran = 0;
    const auto empty = [&ran](JobContext& /*context*/) noexcept { ++ran; };

    // RFC 0005's budget: submitting and running an empty job, in the
    // deterministic mode, under 100 ns.
    bench.batch(keys.size())
        .unit("job")
        .run("job system, deterministic: submit and run an empty job", [&] {
            for (const std::uint32_t key : keys) {
                static_cast<void>(jobs.submit({.key = key}, empty));
            }
            jobs.run_until_idle();
        });
    ankerl::nanobench::doNotOptimizeAway(ran);

    // RFC 0005's budget through six workers: over a million empty jobs a
    // second, so under a microsecond each, submitting included.
    JobSystem workers({.workers = 6});
    std::atomic<std::uint64_t> through{0};
    const auto counted = [&through](JobContext& /*context*/) noexcept {
        through.fetch_add(1, std::memory_order_relaxed);
    };
    bench.batch(10'000).unit("job").run("job system, 6 workers: an empty job through them", [&] {
        for (std::uint32_t i = 0; i < 10'000; ++i) {
            while (!workers.submit({.key = i}, counted)) {
                std::this_thread::yield();
            }
        }
        workers.wait_until_idle();
    });

    wake_up_latency(bench.epochs() == 1);
}

} // namespace vx::bench
