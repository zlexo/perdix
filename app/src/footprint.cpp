#include "footprint.hpp"

#include <cstdint>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>

#include <cstdio>
#endif

namespace vx::app {

std::uint64_t process_footprint() noexcept {
#ifdef __APPLE__
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    // The task API takes its info as an array of integers.
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(
                      &info), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                  &count) != KERN_SUCCESS) {
        return 0;
    }
    return info.phys_footprint;
#elif defined(__linux__)
    // The second number is the resident pages.
    std::FILE* statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) {
        return 0;
    }
    unsigned long long size = 0;
    unsigned long long resident = 0;
    // Two decimal page counts, checked by read == 2 below.
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
    const int read = std::fscanf(statm, "%llu %llu", &size, &resident);
    std::fclose(statm);
    const long page = sysconf(_SC_PAGESIZE);
    return read == 2 && page > 0 ? resident * static_cast<std::uint64_t>(page) : 0;
#else
    return 0;
#endif
}

} // namespace vx::app
