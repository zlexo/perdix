// With VX_PROFILE only, and empty without it: every heap allocation through
// operator new goes to Tracy's memory view, where RFC 0001's budget of no
// allocations a frame can be seen. Each comes with its call stack, since the
// drivers allocate through it too, Apple's Metal driver a few times a frame.
// The secure reports skip themselves while the profiler is not running yet,
// or any more, as during static initialization and teardown.

#include <tracy/Tracy.hpp>

#ifdef TRACY_ENABLE

#include <cstddef>
#include <cstdlib>

namespace {

// Deep enough to climb out of the C++ runtime and a driver into the caller.
constexpr int call_stack_depth = 12;

} // namespace

// libc++ declares these with reserved parameter names, which no definition
// outside the library may repeat.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size) {
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        std::abort(); // engine code has no exceptions to report it with
    }
    TracySecureAllocS(memory, size, call_stack_depth);
    return memory;
}

void operator delete(void* memory) noexcept {
    if (memory != nullptr) {
        TracySecureFree(memory);
        std::free(memory);
    }
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    operator delete(memory);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)

#endif
