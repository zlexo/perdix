#include <vx/render/chunk_arena.hpp>
#include <vx/render/context.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>

// RFC 0007's arena on a real driver: one buffer with a device address.

TEST_CASE("the chunk arena is one buffer of quads with a device address") {
    const vx::Result<vx::render::Context> context =
        vx::render::Context::create({.surface = std::nullopt, .validation = true});
    INFO((context.has_value() ? std::string() : context.error().message));
    REQUIRE(context.has_value());
    vx::Result<vx::render::ChunkArena> arena =
        vx::render::ChunkArena::create(context.value().device(), 1'024, 3);
    INFO((arena.has_value() ? std::string() : arena.error().message));
    REQUIRE(arena.has_value());
    CHECK(arena.value().buffer().size() == 8 * 1'024);
    CHECK(arena.value().buffer().address() != 0);
    // Unified memory, as on Apple's GPUs and lavapipe: written in place.
    if (arena.value().buffer().mapping().empty()) {
        MESSAGE("the arena is not host-visible here: uploads stage into it");
    }
    const std::optional<vx::render::ArenaRange> range = arena.value().space().allocate(1'024);
    REQUIRE(range.has_value());
    CHECK_FALSE(arena.value().space().allocate(1).has_value());
    CHECK(context.value().instance().validation_counts().errors == 0);
    CHECK(context.value().instance().validation_counts().warnings == 0);
}
