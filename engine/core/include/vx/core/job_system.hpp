#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>

namespace vx {

// How soon a job runs (RFC 0005): every queued urgent job before any normal
// one, every normal one before any background one. Urgent is remeshing after
// an edit; normal is streaming, nearest first; background is work nobody
// waits for, like saving, which moves up to normal once it has waited a
// second. Nothing moves up to urgent, so edits always go first.
enum class JobLevel : std::uint8_t { urgent, normal, background };

struct JobPriority {
    JobLevel level = JobLevel::normal;
    // Lower runs first within a level: streaming uses RFC 0006's squared
    // score. Equal keys run in the order they were submitted.
    std::uint32_t key = 0;
};

// What a running job knows of where it runs.
class JobContext {
public:
    explicit JobContext(std::uint32_t worker) noexcept : worker_(worker) {}

    // The worker running the job, from 0: an index into per-worker scratch.
    [[nodiscard]] std::uint32_t worker() const noexcept { return worker_; }

private:
    std::uint32_t worker_;
};

// A job (RFC 0005): a callable the job system copies byte for byte into a
// slot, so it owns nothing: positions, ids and pointers, at most 64 bytes of
// them. Its inputs stay with whoever submitted it, its scratch belongs to
// the worker, and its results leave through inboxes. Its copy constructor
// is trivial too, so that copying its bytes creates one.
template <class F>
concept Job = std::is_trivially_copyable_v<F> && std::is_trivially_copy_constructible_v<F> &&
              sizeof(F) <= 64 && alignof(F) <= alignof(std::max_align_t) &&
              std::is_nothrow_invocable_v<const F&, JobContext&>;

// A submitted job. Generations start at 1, so a default handle names no job;
// a stale one, whose slot a later job has taken, names none either.
struct JobHandle {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    explicit operator bool() const noexcept { return generation != 0; }
};

// The time background jobs age by. Tests set it by hand.
class JobClock {
public:
    virtual ~JobClock() = default;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const noexcept = 0;
};

struct JobSystemDesc {
    // Threads that run jobs: RFC 0005's default is the core count less two,
    // 6 on the reference machine. 0 is the deterministic mode: nothing runs
    // until the owner calls run_until_idle, which runs the jobs on its own
    // thread.
    std::uint32_t workers = 0;
    // Jobs queued or running at once.
    std::uint32_t slots = 1'024;
    // What background jobs age by; null for the steady clock.
    const JobClock* clock = nullptr;
};

// RFC 0005's scheduler: a table of slots and a queue for each level, under
// one lock, each queue ordered by key, then by age, and workers that sleep
// on a condition variable until a job is queued, never spinning. Submitting
// never waits for a job and never allocates, but for one case on macOS: an
// urgent job that wakes a worker asleep at utility raises it, and libpthread
// allocates the raise.
class JobSystem {
public:
    explicit JobSystem(const JobSystemDesc& desc);
    // Asserts that nothing is queued or running, then joins the workers.
    ~JobSystem();
    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Copies `job` into a free slot and queues it at `priority`: an empty
    // handle when every slot is taken.
    template <Job F> [[nodiscard]] JobHandle submit(JobPriority priority, const F& job) noexcept {
        return submit_bytes(priority, &job, sizeof(F), &run_bytes<F>);
    }

    // True: the job had not started, and never will. False: it is running
    // or done, or the handle names no job.
    bool cancel(JobHandle job) noexcept;

    // The deterministic mode only: runs every queued job on this thread as
    // worker 0, most urgent first, until none is left, including any the
    // jobs themselves submit.
    void run_until_idle();

    // Waits until no job is queued or running: for tests, and for shutdown
    // once nothing more is submitted. In the deterministic mode, runs them.
    void wait_until_idle();

    // While streaming catches up with chunks missing near the player, workers
    // run at user-initiated QoS instead of utility (RFC 0005), changing class
    // between jobs.
    void set_catching_up(bool catching_up) noexcept;

    [[nodiscard]] std::uint32_t worker_count() const noexcept;

    // How many jobs wait at `level`.
    [[nodiscard]] std::uint32_t queued(JobLevel level) const noexcept;

private:
    using Runner = void (*)(const std::byte* bytes, JobContext& context) noexcept;

    // Runs the job whose bytes were copied to `bytes`: copying the bytes of
    // a type with a trivial copy constructor and destructor into suitably
    // aligned storage creates an object there.
    template <class F> static void run_bytes(const std::byte* bytes, JobContext& context) noexcept {
        (*std::launder(reinterpret_cast<const F*>(bytes)))(context);
    }

    JobHandle submit_bytes(JobPriority priority, const void* job, std::size_t size,
                           Runner run) noexcept;

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace vx
