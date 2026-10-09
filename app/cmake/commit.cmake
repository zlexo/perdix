# Writes the commit the source tree is at into a header, and only when it
# changed, so that the build stamps the flythrough's report (RFC 0006)
# without recompiling every time: `-dirty` when tracked files changed, and
# "unknown" outside a checkout of its own, such as a source archive unpacked
# inside another repository. Neither command takes git's index lock, as a
# describe or a plain status would, so a build never fails a git command
# running in the checkout at the same time. Run as:
#   cmake -D SOURCE=<source dir> -D OUT=<header> -P commit.cmake
set(commit "unknown")
execute_process(
    COMMAND git -C "${SOURCE}" rev-parse --show-toplevel
    OUTPUT_VARIABLE top
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE failed)
if(NOT failed)
    file(REAL_PATH "${top}" top)
    file(REAL_PATH "${SOURCE}" source)
    if(top STREQUAL source)
        execute_process(
            COMMAND git -C "${SOURCE}" rev-parse --short=7 HEAD
            OUTPUT_VARIABLE head
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE head_failed)
        execute_process(
            COMMAND git --no-optional-locks -C "${SOURCE}" status --porcelain
                    --untracked-files=no
            OUTPUT_VARIABLE changes
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE status_failed)
        if(NOT head_failed AND NOT status_failed AND NOT head STREQUAL "")
            set(commit "${head}")
            if(NOT changes STREQUAL "")
                string(APPEND commit "-dirty")
            endif()
        endif()
    endif()
endif()
set(text "#pragma once\n\n// Written at each build by app/cmake/commit.cmake.\n\n#include <string_view>\n\nnamespace vx::app {\n\ninline constexpr std::string_view commit = \"${commit}\";\n\n} // namespace vx::app\n")
set(old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
