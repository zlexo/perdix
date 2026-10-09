#include <vx/core/job_system.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

// RFC 0005's job system in its deterministic mode: the order jobs run in.

namespace {

using vx::JobContext;
using vx::JobHandle;
using vx::JobLevel;
using vx::JobSystem;

// A job that adds its id to a log when it runs.
struct Record {
    std::vector<int>* log;
    int id;

    void operator()(JobContext& /*context*/) const noexcept { log->push_back(id); }
};

// Submits a Record, which must find a slot.
JobHandle record(JobSystem& jobs, JobLevel level, std::uint32_t key, std::vector<int>& log,
                 int id) {
    const JobHandle handle = jobs.submit({.level = level, .key = key}, Record{&log, id});
    REQUIRE(handle);
    return handle;
}

} // namespace

TEST_CASE("jobs run by level, then by key, then in the order they came in") {
    JobSystem jobs({});
    std::vector<int> log;
    record(jobs, JobLevel::background, 0, log, 1);
    record(jobs, JobLevel::normal, 5, log, 2);
    record(jobs, JobLevel::urgent, 9, log, 3);
    record(jobs, JobLevel::normal, 5, log, 4);
    record(jobs, JobLevel::normal, 1, log, 5);
    record(jobs, JobLevel::urgent, 2, log, 6);
    record(jobs, JobLevel::background, 0, log, 7);
    CHECK(jobs.queued(JobLevel::urgent) == 2);
    CHECK(jobs.queued(JobLevel::normal) == 3);
    CHECK(jobs.queued(JobLevel::background) == 2);
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{6, 3, 5, 2, 4, 1, 7});
    CHECK(jobs.queued(JobLevel::normal) == 0);
}
