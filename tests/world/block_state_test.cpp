#include <vx/world/block_state.hpp>

#include <doctest/doctest.h>

#include <type_traits>

TEST_CASE("a block state is 16 bits, and air is 0, so zeroed memory is air") {
    CHECK(sizeof(vx::BlockState) == 2);
    CHECK(static_cast<std::underlying_type_t<vx::BlockState>>(vx::BlockState::air) == 0);
    CHECK(vx::BlockState{} == vx::BlockState::air);
}
