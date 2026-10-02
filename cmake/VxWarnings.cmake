# vx_target_warnings(<target>)
#
# Applies the engine's warning set to one of our own targets. Third-party
# code never gets these flags: it is built with its own defaults and its
# headers are included as SYSTEM, so a dependency update cannot break the
# build through a warning we do not control.
function(vx_target_warnings target)
    if(MSVC)
        set(flags /W4 /permissive-)
        if(VX_WARNINGS_AS_ERRORS)
            list(APPEND flags /WX)
        endif()
    else()
        set(flags
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion
            -Wold-style-cast
            -Wcast-align
            -Wnull-dereference
            -Wnon-virtual-dtor
            -Woverloaded-virtual
            -Wdouble-promotion
            -Wformat=2
            -Wimplicit-fallthrough)
        if(VX_WARNINGS_AS_ERRORS)
            list(APPEND flags -Werror)
        endif()
    endif()
    target_compile_options(${target} PRIVATE ${flags})
endfunction()
