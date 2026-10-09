#include <vx/core/job_system.hpp>

#include <vx/core/thread.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

namespace vx {
namespace {

constexpr std::uint32_t none = 0xFFFF'FFFFU;

// How long a background job waits before it moves up to normal.
constexpr std::chrono::seconds background_patience{1};

enum class SlotState : std::uint8_t { free, queued, running };

constexpr std::size_t level_index(JobLevel level) noexcept {
    return static_cast<std::size_t>(level);
}

class SteadyClock final : public JobClock {
public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const noexcept override {
        return std::chrono::steady_clock::now();
    }
};

} // namespace

struct JobSystem::State {
    // A job and its place in the queues: one cache line, so workers taking
    // neighboring slots never share one.
    struct alignas(cache_line) Slot {
        alignas(std::max_align_t) std::array<std::byte, 64> bytes{};
        Runner run = nullptr;
        std::chrono::steady_clock::time_point submitted; // background jobs only
        std::uint64_t sequence = 0;                      // the order jobs came in
        std::uint32_t generation = 1;
        std::uint32_t key = 0;
        std::uint32_t heap_at = none; // its place in its level's heap
        std::uint32_t older = none;   // queued background jobs, oldest first
        std::uint32_t newer = none;
        JobLevel level = JobLevel::normal;
        SlotState state = SlotState::free;
    };
    static_assert(sizeof(Slot) == cache_line, "RFC 0005: a slot is a cache line");

    // A thread that runs jobs. Each has its own wake-up, so whoever queues a
    // job knows which worker it wakes, and can raise that one for an urgent
    // job (RFC 0005).
    struct Worker {
        std::condition_variable wake;
        // The class it last set itself to, so a waker knows whether to raise
        // it.
        ThreadQos qos = ThreadQos::utility;
        bool sleeping = false; // waiting on wake, its index on the sleepers' stack
        std::thread thread;
#ifdef __APPLE__
        // Raised for the urgent job that woke it, until it takes a job or
        // finds none.
        pthread_override_t raised = nullptr;
#endif
    };

    explicit State(const JobSystemDesc& desc)
        : slots(desc.slots), clock(desc.clock != nullptr ? desc.clock : &steady) {
        free.reserve(desc.slots);
        sleepers.reserve(desc.workers);
        for (std::uint32_t index = 0; index < desc.workers; ++index) {
            workers.push_back(std::make_unique<Worker>());
        }
        // Slot 0 first, for tests that read handles.
        for (std::uint32_t at = desc.slots; at > 0; --at) {
            free.push_back(at - 1);
        }
        for (std::vector<std::uint32_t>& heap : heaps) {
            heap.reserve(desc.slots);
        }
    }

    // Whether the job in slot `a` runs before the one in slot `b` of the
    // same level: the lower key, then the one that came in first.
    [[nodiscard]] bool runs_before(std::uint32_t a, std::uint32_t b) const noexcept {
        const Slot& first = slots[a];
        const Slot& second = slots[b];
        return first.key != second.key ? first.key < second.key : first.sequence < second.sequence;
    }

    void place(std::vector<std::uint32_t>& heap, std::uint32_t at, std::uint32_t slot) noexcept {
        heap[at] = slot;
        slots[slot].heap_at = at;
    }

    void sift_up(std::vector<std::uint32_t>& heap, std::uint32_t at) noexcept {
        const std::uint32_t slot = heap[at];
        while (at > 0) {
            const std::uint32_t parent = (at - 1) / 2;
            if (!runs_before(slot, heap[parent])) {
                break;
            }
            place(heap, at, heap[parent]);
            at = parent;
        }
        place(heap, at, slot);
    }

    void sift_down(std::vector<std::uint32_t>& heap, std::uint32_t at) noexcept {
        const std::uint32_t slot = heap[at];
        const auto size = static_cast<std::uint32_t>(heap.size());
        for (;;) {
            std::uint32_t child = (2 * at) + 1;
            if (child >= size) {
                break;
            }
            if (child + 1 < size && runs_before(heap[child + 1], heap[child])) {
                ++child;
            }
            if (!runs_before(heap[child], slot)) {
                break;
            }
            place(heap, at, heap[child]);
            at = child;
        }
        place(heap, at, slot);
    }

    void push(JobLevel level, std::uint32_t slot) noexcept {
        std::vector<std::uint32_t>& heap = heaps[level_index(level)];
        heap.push_back(slot); // never allocates: reserved for every slot
        sift_up(heap, static_cast<std::uint32_t>(heap.size() - 1));
    }

    // Takes the job at `at` out of its level's heap.
    void remove(JobLevel level, std::uint32_t at) noexcept {
        std::vector<std::uint32_t>& heap = heaps[level_index(level)];
        const std::uint32_t last = heap.back();
        heap.pop_back();
        if (at < heap.size()) {
            place(heap, at, last);
            sift_down(heap, at);
            sift_up(heap, slots[last].heap_at);
        }
    }

    void link_newest(std::uint32_t slot) noexcept {
        slots[slot].older = newest_background;
        slots[slot].newer = none;
        if (newest_background != none) {
            slots[newest_background].newer = slot;
        } else {
            oldest_background = slot;
        }
        newest_background = slot;
    }

    void unlink(std::uint32_t slot) noexcept {
        Slot& job = slots[slot];
        if (job.older != none) {
            slots[job.older].newer = job.newer;
        } else {
            oldest_background = job.newer;
        }
        if (job.newer != none) {
            slots[job.newer].older = job.older;
        } else {
            newest_background = job.older;
        }
        job.older = none;
        job.newer = none;
    }

    // Background jobs that have waited long enough move up to normal, keeping
    // their key and their place in line. The oldest is first in the list.
    void promote_aged() noexcept {
        if (oldest_background == none) {
            return;
        }
        const std::chrono::steady_clock::time_point now = clock->now();
        while (oldest_background != none &&
               now - slots[oldest_background].submitted >= background_patience) {
            const std::uint32_t slot = oldest_background;
            unlink(slot);
            remove(JobLevel::background, slots[slot].heap_at);
            slots[slot].level = JobLevel::normal;
            push(JobLevel::normal, slot);
        }
    }

    // The next job to run, out of the queues: none when none is queued.
    std::uint32_t take() noexcept {
        promote_aged();
        for (const JobLevel level : {JobLevel::urgent, JobLevel::normal, JobLevel::background}) {
            const std::vector<std::uint32_t>& heap = heaps[level_index(level)];
            if (heap.empty()) {
                continue;
            }
            const std::uint32_t slot = heap.front();
            remove(level, 0);
            if (level == JobLevel::background) {
                unlink(slot);
            }
            return slot;
        }
        return none;
    }

    // Frees a slot. Its next job gets a new generation, so handles to this
    // one go stale; generations skip 0, which marks an empty handle.
    void release(std::uint32_t slot) noexcept {
        Slot& job = slots[slot];
        job.state = SlotState::free;
        job.generation = job.generation + 1 == 0 ? 1 : job.generation + 1;
        free.push_back(slot);
    }

    [[nodiscard]] bool nothing_queued() const noexcept {
        return heaps[0].empty() && heaps[1].empty() && heaps[2].empty();
    }

    // Tells wait_until_idle when the last running job has finished and none
    // is queued.
    void note_if_idle() noexcept {
        if (running == 0 && nothing_queued()) {
            idle.notify_all();
        }
    }

    // A job was queued at `level`: wakes a sleeping worker, if any. One that
    // sleeps at utility and is woken for an urgent job is raised to
    // user-initiated until it starts a job, since a condition variable passes
    // no QoS to the thread it wakes (RFC 0005). One already at user-initiated
    // is not: starting a raise allocates, in libpthread.
    void wake_one(JobLevel level) noexcept {
        if (sleepers.empty()) {
            return;
        }
        Worker& worker = *workers[sleepers.back()];
        sleepers.pop_back();
        worker.sleeping = false;
#ifdef __APPLE__
        if (level == JobLevel::urgent && worker.qos == ThreadQos::utility &&
            worker.raised == nullptr) {
            worker.raised = pthread_override_qos_class_start_np(worker.thread.native_handle(),
                                                                QOS_CLASS_USER_INITIATED, 0);
        }
#else
        static_cast<void>(level);
#endif
        worker.wake.notify_one();
    }

    // A worker's life: take the next job, or sleep until one is queued or the
    // job system stops. A worker sets its own QoS between jobs: user-initiated
    // for an urgent job and while streaming catches up, utility otherwise.
    void work(std::uint32_t index) {
        set_thread_name(("Worker " + std::to_string(index)).c_str());
        set_thread_qos(ThreadQos::utility);
        Worker& self = *workers[index];
        JobContext context(index);
        std::unique_lock guard(lock);
        for (;;) {
            const std::uint32_t at = take();
            if (at == none) {
#ifdef __APPLE__
                // Woken for an urgent job another worker took or a cancel
                // removed: nothing waits on this worker now, so its raise
                // ends here, not at its next job, or never if it leaves.
                if (self.raised != nullptr) {
                    pthread_override_qos_class_end_np(std::exchange(self.raised, nullptr));
                }
#endif
                if (stopping) {
                    return;
                }
                // A cancel that took the job it was woken for told the
                // waiters already; telling them again keeps this loop right
                // on its own.
                note_if_idle();
                self.sleeping = true;
                sleepers.push_back(index);
                self.wake.wait(guard, [&self] { return !self.sleeping; });
                continue;
            }
            Slot& slot = slots[at];
            slot.state = SlotState::running;
            ++running;
            const ThreadQos wanted = slot.level == JobLevel::urgent || catching_up
                                         ? ThreadQos::user_initiated
                                         : ThreadQos::utility;
            const ThreadQos had = std::exchange(self.qos, wanted);
#ifdef __APPLE__
            pthread_override_t raised = std::exchange(self.raised, nullptr);
#endif
            guard.unlock();
            if (wanted != had) {
                set_thread_qos(wanted);
            }
#ifdef __APPLE__
            // Its own class set first, as the SDK asks of a thread ending an
            // override of itself.
            if (raised != nullptr) {
                pthread_override_qos_class_end_np(raised);
            }
#endif
            slot.run(slot.bytes.data(), context);
            guard.lock();
            release(at);
            --running;
            note_if_idle();
        }
    }

    std::vector<Slot> slots;
    std::vector<std::uint32_t> free; // a stack
    std::array<std::vector<std::uint32_t>, 3> heaps;
    std::uint32_t oldest_background = none;
    std::uint32_t newest_background = none;
    std::uint64_t next_sequence = 0;
    SteadyClock steady;
    const JobClock* clock;
    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::uint32_t> sleepers; // a stack of the workers waiting for a job
    std::condition_variable idle;
    std::uint32_t running = 0;
    bool catching_up = false;
    bool stopping = false;
    mutable std::mutex lock;
};

JobSystem::JobSystem(const JobSystemDesc& desc) : state_(std::make_unique<State>(desc)) {
    assert(desc.slots > 0 && desc.slots < none);
    State& state = *state_;
    for (std::uint32_t index = 0; index < state.workers.size(); ++index) {
        state.workers[index]->thread = std::thread([&state, index] { state.work(index); });
    }
}

// RFC 0005's order: whoever owns the job system has canceled what was
// queued and waited for what ran, so it is idle; then the workers leave.
JobSystem::~JobSystem() {
    State& state = *state_;
    {
        const std::scoped_lock guard(state.lock);
        assert(state.free.size() == state.slots.size() && "jobs still queued or running");
        // Every slot can be free with a finished job still in the aging list.
        assert(state.oldest_background == none && state.newest_background == none &&
               "a job left in the aging list");
        state.stopping = true;
        for (const std::unique_ptr<State::Worker>& worker : state.workers) {
            worker->sleeping = false;
            worker->wake.notify_one();
        }
    }
    for (const std::unique_ptr<State::Worker>& worker : state.workers) {
        worker->thread.join();
    }
}

JobHandle JobSystem::submit_bytes(JobPriority priority, const void* job, std::size_t size,
                                  Runner run) noexcept {
    State& state = *state_;
    // Only background jobs age, so only they read the clock.
    const std::chrono::steady_clock::time_point now = priority.level == JobLevel::background
                                                          ? state.clock->now()
                                                          : std::chrono::steady_clock::time_point{};
    const std::scoped_lock guard(state.lock);
    if (state.free.empty()) {
        return {};
    }
    const std::uint32_t at = state.free.back();
    state.free.pop_back();
    State::Slot& slot = state.slots[at];
    std::memcpy(slot.bytes.data(), job, size);
    slot.run = run;
    slot.submitted = now;
    slot.sequence = state.next_sequence++;
    slot.key = priority.key;
    slot.level = priority.level;
    slot.state = SlotState::queued;
    state.push(priority.level, at);
    if (priority.level == JobLevel::background) {
        state.link_newest(at);
    }
    state.wake_one(priority.level);
    return {.slot = at, .generation = slot.generation};
}

bool JobSystem::cancel(JobHandle job) noexcept {
    State& state = *state_;
    if (!job) {
        return false;
    }
    const std::scoped_lock guard(state.lock);
    if (job.slot >= state.slots.size()) {
        return false;
    }
    const State::Slot& slot = state.slots[job.slot];
    if (slot.generation != job.generation || slot.state != SlotState::queued) {
        return false;
    }
    state.remove(slot.level, slot.heap_at);
    if (slot.level == JobLevel::background) {
        state.unlink(job.slot);
    }
    state.release(job.slot);
    state.note_if_idle();
    return true;
}

void JobSystem::run_until_idle() {
    State& state = *state_;
    assert(state.workers.empty() && "run_until_idle is the deterministic mode's");
    JobContext context(0);
    std::uint32_t done = none;
    for (;;) {
        std::uint32_t at = none;
        {
            // One lock a job: the last one's slot is freed as the next is
            // taken.
            const std::scoped_lock guard(state.lock);
            if (done != none) {
                state.release(done);
            }
            at = state.take();
            if (at == none) {
                return;
            }
            state.slots[at].state = SlotState::running;
        }
        const State::Slot& slot = state.slots[at];
        slot.run(slot.bytes.data(), context);
        done = at;
    }
}

void JobSystem::wait_until_idle() {
    State& state = *state_;
    if (state.workers.empty()) {
        run_until_idle();
        return;
    }
    std::unique_lock guard(state.lock);
    state.idle.wait(guard, [&state] { return state.running == 0 && state.nothing_queued(); });
}

void JobSystem::set_catching_up(bool catching_up) noexcept {
    const std::scoped_lock guard(state_->lock);
    state_->catching_up = catching_up;
}

std::uint32_t JobSystem::worker_count() const noexcept {
    return static_cast<std::uint32_t>(state_->workers.size());
}

std::uint32_t JobSystem::slot_count() const noexcept {
    return static_cast<std::uint32_t>(state_->slots.size());
}

std::uint32_t JobSystem::queued(JobLevel level) const noexcept {
    const std::scoped_lock guard(state_->lock);
    return static_cast<std::uint32_t>(state_->heaps[level_index(level)].size());
}

} // namespace vx
