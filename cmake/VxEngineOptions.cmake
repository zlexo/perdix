# vx_target_engine_options(<target>)
#
# Engine code builds without exceptions and RTTI (RFC 0000): errors are
# values, and nothing needs run-time type information. Test targets keep
# both, because doctest uses them.
function(vx_target_engine_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    endif()
endfunction()
