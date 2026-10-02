# Runs in Tracy's source directory at every configure: backports upstream
# fixes to Tracy's Vulkan context. Safe to run again, and it writes only what
# changes, so nothing rebuilds for it.
#
# backport(<commit> <faulty> <fixed>) replaces the faulty code with upstream's
# fix, unless the fix is already in. A Tracy with neither stops the configure,
# so whoever bumps the version decides whether the backport is still needed.
set(file public/tracy/TracyVulkan.hpp)
file(READ "${file}" text)
set(original "${text}")

macro(backport commit faulty fixed)
    string(FIND "${text}" "${fixed}" fixed_at)
    if(fixed_at EQUAL -1)
        string(FIND "${text}" "${faulty}" faulty_at)
        if(faulty_at EQUAL -1)
            message(FATAL_ERROR "${file} has neither the code wolfpld/tracy ${commit} fixes "
                "nor its fix: check whether cmake/VxPatchTracy.cmake still applies to this Tracy.")
        endif()
        string(REPLACE "${faulty}" "${fixed}" text "${text}")
    endif()
endmacro()

# wolfpld/tracy 94e58d4e, after 0.14.1: the context made from a command buffer
# allocates its query results for 8 bytes a query, but each collection reads
# 16, a value and its availability, so collecting more than half the pool at
# once writes past the buffer.
backport(94e58d4e
    "m_res = (int64_t*)tracy_malloc( sizeof( int64_t ) * m_queryCount );"
    "m_res = (int64_t*)tracy_malloc( sizeof( int64_t ) * m_queryCount * 2 );")

# wolfpld/tracy e20c716b, in 0.14.0: Apple's drivers cannot make a pool of
# 65,536 timestamp queries. KosmicKrisp answers VK_ERROR_OUT_OF_DEVICE_MEMORY
# through the validation messenger, and Tracy halves the pool after each
# failure until 4,096 succeeds, one message per failure. 0.13.1 holds the
# count in an enum where upstream now has a constant.
backport(e20c716b
    [=[    enum { QueryCount = 64 * 1024 };]=]
    [=[#if !defined __APPLE__
    enum { QueryCount = 64 * 1024 };
#else
    enum { QueryCount = 4 * 1024 };
#endif]=])

if(NOT text STREQUAL original)
    file(WRITE "${file}" "${text}")
endif()
