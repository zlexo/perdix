#include "../benchmarks.hpp"

#include <vx/core/job_system.hpp>

#include <nanobench.h>

#include <cstdint>
#include <random>
#include <vector>

namespace vx::bench {

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
}

} // namespace vx::bench
