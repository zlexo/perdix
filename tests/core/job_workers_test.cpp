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
#include <mach/mach.h>
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

TEST_CASE("100,000 jobs from four threads, with random priorities and cancels, each run or are "
          "canceled, once") {
    constexpr int submitters = 4;
    constexpr int each = 25'000;
    JobSystem jobs({.workers = 6});
    std::vector<std::atomic<int>> runs(std::size_t{submitters} * each);
    // Each submitter writes only its own part; read after it is joined.
    std::vector<int> canceled(runs.size(), 0);
    std::vector<std::thread> threads;
    for (int thread = 0; thread < submitters; ++thread) {
        threads.emplace_back([&, thread] {
            std::mt19937 random(static_cast<std::uint32_t>(thread) + 1);
            std::uniform_int_distribution<int> level(0, 2);
            std::uniform_int_distribution<std::uint32_t> key(0, 1'000);
            std::uniform_int_distribution<int> roll(0, 2);
            std::vector<std::pair<std::size_t, JobHandle>> mine;
            mine.reserve(each);
            for (int i = 0; i < each; ++i) {
                const auto id =
                    (static_cast<std::size_t>(thread) * each) + static_cast<std::size_t>(i);
                const JobPriority priority{.level = static_cast<JobLevel>(level(random)),
                                           .key = key(random)};
                mine.emplace_back(id, submit_waiting(jobs, priority, Count{&runs[id]}));
                if (roll(random) == 0) {
                    std::uniform_int_distribution<std::size_t> earlier(0, mine.size() - 1);
                    const auto& [victim, handle] = mine[earlier(random)];
                    if (jobs.cancel(handle)) {
                        ++canceled[victim];
                    }
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    jobs.wait_until_idle();
    std::size_t wrong = 0;
    std::size_t total_canceled = 0;
    for (std::size_t id = 0; id < runs.size(); ++id) {
        if (runs[id].load() + canceled[id] != 1) {
            ++wrong;
        }
        total_canceled += static_cast<std::size_t>(canceled[id]);
    }
    CHECK(wrong == 0);
    // Cancels raced the workers: many lost to jobs already run, and with a
    // thousand slots queued, some always win.
    CHECK(total_canceled > 0);
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

#ifdef __APPLE__
namespace {

// Reads the priority its thread is scheduled at, which a raise lifts and
// pthread_get_qos_class_np does not show.
struct ReadPriority {
    int* read;

    void operator()(JobContext& /*context*/) const noexcept {
        thread_extended_info_data_t info{};
        mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO,
                        reinterpret_cast<thread_info_t>(&info), &count) == KERN_SUCCESS) {
            *read = info.pth_priority;
        }
    }
};

} // namespace
#endif

TEST_CASE("the raise an urgent job gives a worker asleep at utility ends when the job starts") {
#ifdef __APPLE__
    JobSystem jobs({.workers = 1});
    const auto run = [&jobs](JobLevel level) {
        int read = -1;
        REQUIRE(jobs.submit({.level = level}, ReadPriority{&read}));
        jobs.wait_until_idle();
        return read;
    };
    const int utility = run(JobLevel::normal);
    // Asleep at utility now, the worker is raised for this one.
    const int user_initiated = run(JobLevel::urgent);
    if (user_initiated <= utility) {
        MESSAGE("skipped: this machine schedules user-initiated no higher than utility");
        return;
    }
    // A raise never ended would keep it above utility.
    CHECK(run(JobLevel::normal) == utility);
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

namespace {

// Jobs that each wait until all three have started, so they finish only if
// three workers run them at once. The first queues the other two.
struct Meeting {
    std::mutex lock;
    std::condition_variable changed;
    int arrived = 0;
};

struct Meet {
    JobSystem* jobs; // the first job's, to queue the others; null in theirs
    Meeting* meeting;

    void operator()(JobContext& /*context*/) const noexcept {
        if (jobs != nullptr) {
            static_cast<void>(jobs->submit({}, Meet{nullptr, meeting}));
            static_cast<void>(jobs->submit({}, Meet{nullptr, meeting}));
        }
        std::unique_lock guard(meeting->lock);
        ++meeting->arrived;
        meeting->changed.notify_all();
        meeting->changed.wait(guard, [this] { return meeting->arrived == 3; });
    }
};

// Waits at the gate, then notes that it has finished.
struct Finisher {
    Gate* gate;
    std::atomic<bool>* done;

    void operator()(JobContext& context) const noexcept {
        Blocker{gate}(context);
        done->store(true);
    }
};

} // namespace

TEST_CASE("jobs that wait for each other run at once, and a job at any level wakes a worker") {
    JobSystem jobs({.workers = 3});
    {
        // Queued together, as the workers start or sleep.
        Meeting meeting;
        for (int i = 0; i < 3; ++i) {
            REQUIRE(jobs.submit({}, Meet{nullptr, &meeting}));
        }
        jobs.wait_until_idle();
        CHECK(meeting.arrived == 3);
    }
    {
        // Every worker sleeps now. The first job queues the other two as it
        // runs, and each needs a sleeping worker woken for it.
        Meeting meeting;
        REQUIRE(jobs.submit({}, Meet{&jobs, &meeting}));
        jobs.wait_until_idle();
        CHECK(meeting.arrived == 3);
    }
    // Every worker sleeps again, and a job at each level wakes one.
    std::atomic<int> runs{0};
    for (const JobLevel level : {JobLevel::urgent, JobLevel::normal, JobLevel::background}) {
        REQUIRE(jobs.submit({.level = level}, Count{&runs}));
        jobs.wait_until_idle();
    }
    CHECK(runs.load() == 3);
}

TEST_CASE("waiting for idle waits for the job still running, not only for the queue") {
    Gate gate;
    std::atomic<bool> done{false};
    JobSystem jobs({.workers = 1});
    for (int round = 0; round < 100; ++round) {
        {
            const std::scoped_lock guard(gate.lock);
            gate.started = false;
            gate.open = false;
        }
        done.store(false);
        REQUIRE(jobs.submit({}, Finisher{&gate, &done}));
        {
            std::unique_lock guard(gate.lock);
            gate.changed.wait(guard, [&gate] { return gate.started; });
            // Nothing is queued now, and the job runs until the gate opens.
            gate.open = true;
            gate.changed.notify_all();
        }
        jobs.wait_until_idle();
        REQUIRE(done.load());
    }
}
