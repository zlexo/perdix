#include <vx/core/job_system.hpp>

#include <vx/core/thread.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

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

    explicit State(const JobSystemDesc& desc)
        : slots(desc.slots), clock(desc.clock != nullptr ? desc.clock : &steady),
          workers(desc.workers) {
        free.reserve(desc.slots);
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

    std::vector<Slot> slots;
    std::vector<std::uint32_t> free; // a stack
    std::array<std::vector<std::uint32_t>, 3> heaps;
    std::uint32_t oldest_background = none;
    std::uint32_t newest_background = none;
    std::uint64_t next_sequence = 0;
    SteadyClock steady;
    const JobClock* clock;
    std::uint32_t workers;
    mutable std::mutex lock;
};

JobSystem::JobSystem(const JobSystemDesc& desc) : state_(std::make_unique<State>(desc)) {
    assert(desc.slots > 0 && desc.slots < none);
    // Workers come with RFC 0005's item 4.
    assert(desc.workers == 0);
}

JobSystem::~JobSystem() {
    assert(state_->free.size() == state_->slots.size() && "jobs still queued or running");
    // Every slot can be free with a finished job still in the aging list.
    assert(state_->oldest_background == none && state_->newest_background == none &&
           "a job left in the aging list");
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
    return true;
}

void JobSystem::run_until_idle() {
    State& state = *state_;
    assert(state.workers == 0 && "run_until_idle is the deterministic mode's");
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

std::uint32_t JobSystem::queued(JobLevel level) const noexcept {
    const std::scoped_lock guard(state_->lock);
    return static_cast<std::uint32_t>(state_->heaps[level_index(level)].size());
}

} // namespace vx
