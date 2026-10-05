#include <vx/world/block_state.hpp>

#include <doctest/doctest.h>

#include <cstdint>
#include <type_traits>

TEST_CASE("a block state is 16 bits, and air is 0, so zeroed memory is air") {
    CHECK(sizeof(vx::BlockState) == 2);
    // Unsigned, so the ids from 32,768 up are not negative.
    CHECK(std::is_same_v<std::underlying_type_t<vx::BlockState>, std::uint16_t>);
    CHECK(static_cast<std::underlying_type_t<vx::BlockState>>(vx::BlockState::air) == 0);
    CHECK(vx::BlockState{} == vx::BlockState::air);
}
