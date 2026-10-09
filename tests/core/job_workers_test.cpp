#include <vx/core/job_system.hpp>

#include <doctest/doctest.h>
#include <pthread.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

// RFC 0005's workers: every job runs once whatever the threads do, order
// holds where it can be seen, and each worker names itself and sets its own
// quality of service.

namespace {

using vx::JobContext;
using vx::JobHandle;
using vx::JobLevel;
using vx::JobPriority;
using vx::JobSystem;

// A job that counts its runs.
struct Count {
    std::atomic<int>* runs;

    void operator()(JobContext& /*context*/) const noexcept {
        runs->fetch_add(1, std::memory_order_relaxed);
    }
};

// Submits `job`, waiting for a free slot while the table is full.
template <class F> JobHandle submit_waiting(JobSystem& jobs, JobPriority priority, const F& job) {
    for (;;) {
        const JobHandle handle = jobs.submit(priority, job);
        if (handle) {
            return handle;
        }
        std::this_thread::yield();
    }
}

} // namespace

TEST_CASE("six workers run every job once") {
    JobSystem jobs({.workers = 6});
    CHECK(jobs.worker_count() == 6);
    std::vector<std::atomic<int>> runs(20'000);
    for (std::size_t id = 0; id < runs.size(); ++id) {
        submit_waiting(jobs, {.key = static_cast<std::uint32_t>(id % 97)}, Count{&runs[id]});
    }
    jobs.wait_until_idle();
    std::size_t wrong = 0;
    for (const std::atomic<int>& count : runs) {
        if (count.load() != 1) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
}

namespace {

// A gate one job waits at until the test opens it.
struct Gate {
    std::mutex lock;
    std::condition_variable changed;
    bool started = false;
    bool open = false;
};

// Waits at the gate, after telling the test it has started.
struct Blocker {
    Gate* gate;

    void operator()(JobContext& /*context*/) const noexcept {
        std::unique_lock guard(gate->lock);
        gate->started = true;
        gate->changed.notify_all();
        gate->changed.wait(guard, [this] { return gate->open; });
    }
};

// A log many jobs write to.
struct Log {
    std::mutex lock;
    std::vector<int> ids;
};

struct Record {
    Log* log;
    int id;

    void operator()(JobContext& /*context*/) const noexcept {
        const std::scoped_lock guard(log->lock);
        log->ids.push_back(id);
    }
};

} // namespace

TEST_CASE("behind a busy worker, an urgent job goes before the normal ones queued first") {
    JobSystem jobs({.workers = 1});
    Gate gate;
    Log log;
    REQUIRE(jobs.submit({}, Blocker{&gate}));
    {
        std::unique_lock guard(gate.lock);
        gate.changed.wait(guard, [&gate] { return gate.started; });
    }
    for (int id = 1; id <= 3; ++id) {
        REQUIRE(jobs.submit({.key = static_cast<std::uint32_t>(id)}, Record{&log, id}));
    }
    // A key that keeps it last even if it waited long enough to move up.
    REQUIRE(jobs.submit({.level = JobLevel::background, .key = 1'000}, Record{&log, 8}));
    REQUIRE(jobs.submit({.level = JobLevel::urgent, .key = 50}, Record{&log, 9}));
    {
        const std::scoped_lock guard(gate.lock);
        gate.open = true;
        gate.changed.notify_all();
    }
    jobs.wait_until_idle();
    const std::scoped_lock guard(log.lock);
    CHECK(log.ids == std::vector<int>{9, 1, 2, 3, 8});
}

namespace {

// Notes the worker running it and the name of its thread.
struct Whoami {
    std::array<char, 32>* name;
    std::uint32_t* worker;

    void operator()(JobContext& context) const noexcept {
        *worker = context.worker();
        pthread_getname_np(pthread_self(), name->data(), name->size());
    }
};

} // namespace

TEST_CASE("each worker names its thread for its index") {
    constexpr std::size_t count = 64;
    JobSystem jobs({.workers = 3});
    std::vector<std::array<char, 32>> names(count);
    std::vector<std::uint32_t> workers(count, 99);
    for (std::size_t i = 0; i < count; ++i) {
        REQUIRE(jobs.submit({}, Whoami{&names[i], &workers[i]}));
    }
    jobs.wait_until_idle();
    for (std::size_t i = 0; i < count; ++i) {
        INFO("job ", i);
        REQUIRE(workers[i] < 3);
        CHECK(std::string(names[i].data()) == "Worker " + std::to_string(workers[i]));
    }
}

#ifdef __APPLE__
namespace {

// Reads the QoS class its worker asked for.
struct ReadQos {
    qos_class_t* read;

    void operator()(JobContext& /*context*/) const noexcept {
        int relative = 0;
        pthread_get_qos_class_np(pthread_self(), read, &relative);
    }
};

} // namespace
#endif

TEST_CASE("workers run at utility, an urgent job at user-initiated, and all at user-initiated "
          "while catching up") {
#ifdef __APPLE__
    JobSystem jobs({.workers = 1});
    const auto run = [&jobs](JobLevel level) {
        qos_class_t read = QOS_CLASS_UNSPECIFIED;
        REQUIRE(jobs.submit({.level = level}, ReadQos{&read}));
        jobs.wait_until_idle();
        return read;
    };
    CHECK(run(JobLevel::normal) == QOS_CLASS_UTILITY);
    CHECK(run(JobLevel::urgent) == QOS_CLASS_USER_INITIATED);
    CHECK(run(JobLevel::background) == QOS_CLASS_UTILITY);
    jobs.set_catching_up(true);
    CHECK(run(JobLevel::normal) == QOS_CLASS_USER_INITIATED);
    CHECK(run(JobLevel::background) == QOS_CLASS_USER_INITIATED);
    jobs.set_catching_up(false);
    CHECK(run(JobLevel::normal) == QOS_CLASS_UTILITY);
#else
    MESSAGE("skipped: quality of service is macOS's");
#endif
}

TEST_CASE("an idle job system waits for nothing, and stops with its workers asleep") {
    JobSystem jobs({.workers = 4});
    jobs.wait_until_idle();
    std::atomic<int> runs{0};
    REQUIRE(jobs.submit({}, Count{&runs}));
    jobs.wait_until_idle();
    CHECK(runs.load() == 1);
}
