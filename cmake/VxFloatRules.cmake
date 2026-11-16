# RFC 0008's float rules, so the same seed gives the same terrain bits on
# every compiler and CPU.
#
# Fast math never reaches the build: it lets the compiler reorder and
# approximate float arithmetic, so results would differ between compilers
# and versions. A flag that slips in through the cache or the environment
# stops the configure here, before anything is built.
foreach(flags_var IN ITEMS CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
        CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_CXX_FLAGS_MINSIZEREL)
    if("${${flags_var}}" MATCHES "(^| )(-ffast-math|-Ofast|-funsafe-math-optimizations|-fassociative-math|/fp:fast)( |$)")
        message(FATAL_ERROR
            "${flags_var} holds '${CMAKE_MATCH_2}': Perdix is never built with fast math (RFC 0008)")
    endif()
endforeach()

# vx_target_float_rules(<target>)
#
# No fused multiply-adds: Clang fuses a * b + c by default where the CPU has
# FMA (every arm64, not baseline x86-64), and GCC outside ISO modes, so the
# same expression rounds once on one machine and twice on another. MSVC 2022
# does not fuse under /fp:precise, its default, said here outright.
# vx_target_warnings applies it, so every target of ours, tests included,
# has it.
function(vx_target_float_rules target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
endfunction()
