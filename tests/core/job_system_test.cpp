#include <vx/core/job_system.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// RFC 0005's job system in its deterministic mode: the order jobs run in,
// aging, cancellation and the slot table's limit.

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using vx::JobContext;
using vx::JobHandle;
using vx::JobLevel;
using vx::JobSystem;

// A clock the test moves by hand.
class ManualClock final : public vx::JobClock {
public:
    std::chrono::steady_clock::time_point time{};

    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return time;
    }
};

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

// What counts as a job: small, trivially copyable, aligned no more than the
// slot's storage, and invocable without throwing.
struct Small {
    int* out;
    void operator()(JobContext& /*context*/) const noexcept {}
};
struct Full {
    std::array<std::byte, 64> bytes;
    void operator()(JobContext& /*context*/) const noexcept {}
};
struct Large {
    std::array<std::byte, 72> bytes;
    void operator()(JobContext& /*context*/) const noexcept {}
};
struct Owning {
    std::string name;
    void operator()(JobContext& /*context*/) const noexcept {}
};
struct alignas(32) OverAligned {
    int value;
    void operator()(JobContext& /*context*/) const noexcept {}
};
struct Throwing {
    void operator()(JobContext& /*context*/) const {}
};
static_assert(vx::Job<Small>);
static_assert(vx::Job<Full>, "64 bytes, the limit");
static_assert(!vx::Job<Large>, "over 64 bytes");
static_assert(!vx::Job<Owning>, "not trivially copyable");
static_assert(!vx::Job<OverAligned>, "aligned past the slot's storage");
static_assert(!vx::Job<Throwing>, "may throw");

// submit takes jobs and nothing else.
template <class F>
concept Submittable = requires(JobSystem& jobs, const F& job) { jobs.submit({}, job); };
static_assert(Submittable<Small>);
static_assert(!Submittable<Large>);

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

TEST_CASE("a heap of random keys runs in key order, and equal keys first come first") {
    // Enough jobs for every sift to go several levels down and up.
    JobSystem jobs({});
    std::vector<int> log;
    std::uint32_t key = 7;
    for (int id = 0; id < 600; ++id) {
        key = ((key * 1'103'515'245U) + 12'345U) % 64;
        record(jobs, JobLevel::normal, key, log, (static_cast<int>(key) * 1'000) + id);
    }
    jobs.run_until_idle();
    REQUIRE(log.size() == 600);
    for (std::size_t i = 1; i < log.size(); ++i) {
        INFO("job ", i);
        CHECK(log[i - 1] < log[i]);
    }
}

TEST_CASE("canceling from the middle of a big heap keeps the rest in order") {
    // A removal fills the hole with the heap's last job, which may then have
    // to move down or up.
    JobSystem jobs({});
    std::vector<int> log;
    std::vector<JobHandle> handles;
    std::vector<int> expected;
    std::uint32_t key = 11;
    for (int id = 0; id < 600; ++id) {
        key = ((key * 1'103'515'245U) + 12'345U) % 997;
        const int tag = (static_cast<int>(key) * 1'000) + id;
        handles.push_back(record(jobs, JobLevel::normal, key, log, tag));
        if (id % 3 != 1) {
            expected.push_back(tag);
        }
    }
    for (std::size_t i = 1; i < handles.size(); i += 3) {
        REQUIRE(jobs.cancel(handles[i]));
    }
    jobs.run_until_idle();
    std::ranges::sort(expected);
    CHECK(log == expected);
}

TEST_CASE("a background job that has waited a second moves up to normal, keeping its key") {
    ManualClock clock;
    std::vector<int> log;
    SUBCASE("under a second, it waits behind every normal job") {
        JobSystem jobs({.clock = &clock});
        record(jobs, JobLevel::background, 0, log, 1);
        clock.time += milliseconds(999);
        record(jobs, JobLevel::normal, 10, log, 2);
        jobs.run_until_idle();
        CHECK(log == std::vector<int>{2, 1});
    }
    SUBCASE("after a second, it runs among the normal jobs by its key") {
        JobSystem jobs({.clock = &clock});
        record(jobs, JobLevel::background, 0, log, 1);
        record(jobs, JobLevel::background, 20, log, 2);
        clock.time += seconds(1);
        record(jobs, JobLevel::normal, 10, log, 3);
        record(jobs, JobLevel::background, 5, log, 4);
        jobs.run_until_idle();
        CHECK(log == std::vector<int>{1, 3, 2, 4});
    }
    SUBCASE("a normal job never moves up to urgent") {
        JobSystem jobs({.clock = &clock});
        record(jobs, JobLevel::normal, 0, log, 1);
        clock.time += seconds(10);
        record(jobs, JobLevel::urgent, 99, log, 2);
        jobs.run_until_idle();
        CHECK(log == std::vector<int>{2, 1});
    }
}

TEST_CASE("background jobs that have waited a second all move up at once, keeping their place") {
    ManualClock clock;
    JobSystem jobs({.clock = &clock});
    std::vector<int> log;
    record(jobs, JobLevel::background, 7, log, 1);
    record(jobs, JobLevel::background, 5, log, 2);
    clock.time += milliseconds(500);
    record(jobs, JobLevel::normal, 7, log, 3);
    record(jobs, JobLevel::normal, 6, log, 4);
    clock.time += milliseconds(500);
    jobs.run_until_idle();
    // Both move up before anything runs, so 2 goes before 4; and 1, which
    // came in before 3 with the same key, goes before it.
    CHECK(log == std::vector<int>{2, 4, 1, 3});
}

TEST_CASE("a background job leaves the aging list however it leaves the queue") {
    ManualClock clock;
    JobSystem jobs({.clock = &clock});
    std::vector<int> log;
    record(jobs, JobLevel::background, 3, log, 1);
    const JobHandle middle = record(jobs, JobLevel::background, 1, log, 2);
    record(jobs, JobLevel::background, 2, log, 3);
    // One canceled from the middle of the list; the other two leave as they
    // run, the newest first.
    CHECK(jobs.cancel(middle));
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{3, 1});
    // The list is empty again, and canceling a normal job, which is in no
    // list, leaves it alone: its new oldest job moves up after a second.
    record(jobs, JobLevel::background, 0, log, 4);
    record(jobs, JobLevel::normal, 5, log, 5);
    const JobHandle normal = record(jobs, JobLevel::normal, 0, log, 6);
    CHECK(jobs.cancel(normal));
    clock.time += milliseconds(500);
    record(jobs, JobLevel::background, 9, log, 7);
    clock.time += milliseconds(500);
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{3, 1, 4, 5, 7});
}

TEST_CASE("canceling a queued job takes it out of line, and it never runs") {
    ManualClock clock;
    JobSystem jobs({.clock = &clock});
    std::vector<int> log;
    record(jobs, JobLevel::normal, 1, log, 1);
    const JobHandle second = record(jobs, JobLevel::normal, 2, log, 2);
    record(jobs, JobLevel::normal, 3, log, 3);
    const JobHandle background = record(jobs, JobLevel::background, 0, log, 4);
    record(jobs, JobLevel::background, 1, log, 5);
    CHECK(jobs.cancel(second));
    CHECK(jobs.cancel(background));
    CHECK_FALSE(jobs.cancel(second));
    CHECK(jobs.queued(JobLevel::normal) == 2);
    CHECK(jobs.queued(JobLevel::background) == 1);
    // The canceled background job no longer ages: the other one moves up
    // alone, and runs among the normal jobs by its key, 1.
    clock.time += seconds(1);
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{1, 5, 3});
}

namespace {

// A job that cancels a job, itself or another, while it runs.
struct Cancel {
    JobSystem* jobs;
    const JobHandle* job;
    bool* canceled;

    void operator()(JobContext& /*context*/) const noexcept { *canceled = jobs->cancel(*job); }
};

} // namespace

TEST_CASE("a running or finished job cannot be canceled") {
    JobSystem jobs({});
    JobHandle self;
    bool canceled = true;
    self = jobs.submit({}, Cancel{&jobs, &self, &canceled});
    REQUIRE(self);
    jobs.run_until_idle();
    CHECK_FALSE(canceled);
    CHECK_FALSE(jobs.cancel(self));
}

TEST_CASE("a background job that moved up can still be canceled") {
    ManualClock clock;
    JobSystem jobs({.clock = &clock});
    std::vector<int> log;
    const JobHandle aged = record(jobs, JobLevel::background, 9, log, 1);
    clock.time += milliseconds(500);
    record(jobs, JobLevel::background, 0, log, 2);
    bool canceled = false;
    REQUIRE(jobs.submit({}, Cancel{&jobs, &aged, &canceled}));
    clock.time += milliseconds(500);
    // The first job moves up to normal, behind the one that cancels it.
    jobs.run_until_idle();
    CHECK(canceled);
    CHECK(log == std::vector<int>{2});
}

TEST_CASE("an empty handle, or a stale one whose slot a later job took, cancels nothing") {
    JobSystem jobs({.slots = 1});
    std::vector<int> log;
    const JobHandle first = record(jobs, JobLevel::normal, 0, log, 1);
    jobs.run_until_idle();
    const JobHandle second = record(jobs, JobLevel::normal, 0, log, 2);
    CHECK(second.slot == first.slot);
    CHECK(second.generation != first.generation);
    CHECK_FALSE(jobs.cancel(first));
    CHECK_FALSE(jobs.cancel(JobHandle{}));
    // Slots past the end of the table, near and far.
    CHECK_FALSE(jobs.cancel(JobHandle{.slot = 7, .generation = 1}));
    CHECK_FALSE(jobs.cancel(JobHandle{.slot = 0xFFFF'FFFEU, .generation = 1}));
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{1, 2});
}

TEST_CASE("a full slot table gives an empty handle, and slots free up as jobs finish") {
    JobSystem jobs({.slots = 4});
    std::vector<int> log;
    for (int id = 0; id < 4; ++id) {
        record(jobs, JobLevel::normal, 0, log, id);
    }
    CHECK_FALSE(jobs.submit({}, Record{&log, 4}));
    CHECK(jobs.queued(JobLevel::normal) == 4);
    jobs.run_until_idle();
    CHECK(log.size() == 4);
    CHECK(jobs.submit({}, Record{&log, 5}));
    jobs.run_until_idle();
}

namespace {

// A job that submits one more and notes whether it found a free slot.
struct TrySubmit {
    JobSystem* jobs;
    std::vector<int>* log;
    bool* submitted;

    void operator()(JobContext& /*context*/) const noexcept {
        *submitted = static_cast<bool>(jobs->submit({}, Record{log, 2}));
    }
};

} // namespace

TEST_CASE("a running job keeps its slot until it finishes") {
    JobSystem jobs({.slots = 1});
    std::vector<int> log;
    bool submitted = true;
    REQUIRE(jobs.submit({}, TrySubmit{&jobs, &log, &submitted}));
    jobs.run_until_idle();
    CHECK_FALSE(submitted);
    CHECK(log.empty());
    record(jobs, JobLevel::normal, 0, log, 1);
    jobs.run_until_idle();
    CHECK(log == std::vector<int>{1});
}

namespace {

// A job that notes the worker running it, and submits one more job.
struct Spawner {
    JobSystem* jobs;
    std::vector<int>* log;
    std::uint32_t* worker;

    void operator()(JobContext& context) const noexcept {
        *worker = context.worker();
        static_cast<void>(jobs->submit({.level = JobLevel::urgent}, Record{log, 2}));
    }
};

} // namespace

TEST_CASE("the deterministic mode runs jobs as worker 0, and the jobs they submit") {
    JobSystem jobs({});
    std::vector<int> log;
    std::uint32_t worker = 99;
    REQUIRE(jobs.submit({}, Spawner{&jobs, &log, &worker}));
    record(jobs, JobLevel::normal, 1, log, 1);
    jobs.run_until_idle();
    CHECK(worker == 0);
    // The urgent job the first submitted runs before the normal one queued
    // earlier.
    CHECK(log == std::vector<int>{2, 1});
}

TEST_CASE("a lambda capturing what it needs is a job") {
    JobSystem jobs({});
    int sum = 0;
    const int* addend = &sum;
    const auto add = [&sum, addend](JobContext& /*context*/) noexcept { sum += *addend + 1; };
    static_assert(vx::Job<decltype(add)>);
    REQUIRE(jobs.submit({}, add));
    REQUIRE(jobs.submit({}, add));
    jobs.run_until_idle();
    CHECK(sum == 3);
}
