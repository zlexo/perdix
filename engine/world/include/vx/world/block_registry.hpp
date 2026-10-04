#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_state.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx {

// One per block (RFC 0004): air, then every block in name order.
enum class BlockType : std::uint16_t { air = 0 }; // NOLINT(performance-enum-size)

struct BlockLoad;

// What every block state is (RFC 0004). Ids are a pure function of the block
// definitions, the same on every machine and in every run: air is 0, then
// each block in name order takes a contiguous range, one id for each
// combination of its property values, its properties in name order and the
// last varying fastest. Built once at startup and never changed, so the
// generator and mesher threads share it without locks.
class BlockRegistry {
public:
    // Block files given in memory: tests, and the loader that reads a pack.
    // Files are taken in path order, whatever order they come in.
    [[nodiscard]] static BlockLoad load(std::span<const SourceFile> files);

    // "core:furnace[lit=true]": properties left out take their defaults.
    [[nodiscard]] std::optional<BlockState> find(std::string_view key) const;
    // Every property, in name order: "core:furnace[facing=north,lit=false]".
    [[nodiscard]] std::string key(BlockState state) const;
    // `state` with one property changed, and nothing else.
    [[nodiscard]] std::optional<BlockState> with(BlockState state, std::string_view property,
                                                 std::string_view value) const;
    [[nodiscard]] BlockType type_of(BlockState state) const noexcept;
    // The state with every property at its first value.
    [[nodiscard]] BlockState default_state(BlockType type) const noexcept;
    // States are numbered 0 to state_count() - 1.
    [[nodiscard]] std::size_t state_count() const noexcept { return types_.size(); }

private:
    struct Property {
        std::string name;
        std::vector<std::string> values;
    };
    struct Block {
        std::string name; // "core:furnace"
        std::uint32_t first = 0;
        std::uint32_t states = 0;
        std::vector<Property> properties; // in name order
    };

    [[nodiscard]] const Block* find_block(std::string_view name) const;
    // A block's state from each property's value index, and back.
    [[nodiscard]] static BlockState state_of(const Block& block,
                                             std::span<const std::size_t> values);
    [[nodiscard]] std::vector<std::size_t> values_of(BlockState state) const;

    std::vector<Block> blocks_;    // indexed by BlockType
    std::vector<BlockType> types_; // indexed by BlockState
};

// The registry, or why it could not be built.
struct BlockLoad {
    std::optional<BlockRegistry> registry; // empty when any diagnostic is an error
    std::vector<Diagnostic> diagnostics;   // by file, each in its file's order
};

} // namespace vx
