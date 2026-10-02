# Writes one SPIR-V module as a C++ header. vx_add_shaders (VxShaders.cmake)
# runs it as a script:
#
#   cmake -DSPIRV=<module.spv> -DHEADER=<name.hpp> -DNAME=<name> -P VxEmbedSpirv.cmake

file(READ "${SPIRV}" hex HEX)
string(LENGTH "${hex}" digits)
math(EXPR word_count "${digits} / 8")
math(EXPR leftover "${digits} % 8")
if(word_count EQUAL 0 OR NOT leftover EQUAL 0)
    message(FATAL_ERROR "${SPIRV} is not SPIR-V: its size is not a whole number of 32-bit words.")
endif()

# SPIR-V is a sequence of little-endian 32-bit words: each word's bytes come
# lowest first in the file.
string(REGEX REPLACE "(..)(..)(..)(..)" "0x\\4\\3\\2\\1U, " words "${hex}")
if(NOT words MATCHES "^0x07230203U, ")
    message(FATAL_ERROR "${SPIRV} is not SPIR-V: it does not start with the magic number 0x07230203.")
endif()

# Eight words to a line.
set(word "0x[0-9a-f]+U, ")
string(REGEX REPLACE "(${word}${word}${word}${word}${word}${word}${word}${word})" "\\1\n" words "${words}")
string(REPLACE ", \n" ",\n" words "${words}")
string(REGEX REPLACE ",[ \n]*$" "" words "${words}")
string(REPLACE "\n" "\n    " words "${words}")

file(WRITE "${HEADER}.new" "// Generated from ${NAME}.slang by cmake/VxEmbedSpirv.cmake: do not edit.

#pragma once

#include <array>
#include <cstdint>

namespace vx::shaders {

inline constexpr std::array<std::uint32_t, ${word_count}> ${NAME} = {
    ${words},
};

} // namespace vx::shaders
")
# An unchanged module leaves the header, and whatever includes it, alone.
file(COPY_FILE "${HEADER}.new" "${HEADER}" ONLY_IF_DIFFERENT)
file(REMOVE "${HEADER}.new")
