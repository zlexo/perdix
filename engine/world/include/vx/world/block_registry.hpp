#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx {

// One per block (RFC 0004): air, then every block in name order.
enum class BlockType : std::uint16_t { air = 0 }; // NOLINT(performance-enum-size)

struct BlockLoad;

// One layer of the block texture array (RFC 0004): a texture turned by a
// number of clockwise quarter turns, each moving the image's top edge to its
// right. Every turned texture is a layer of its own, baked at load, so
// neither the quad nor the shader deals with rotation.
struct TextureRef {
    std::string name; // "core:oak_log"; empty for layer 0, the checker
    std::uint8_t quarter_turns = 0;
    bool used_opaque = false; // by which kinds of block, for the texture warnings
    bool used_cutout = false;
    std::string first_use; // "assets/core/blocks/oak_log.toml:6:8", for messages

    friend bool operator==(const TextureRef&, const TextureRef&) = default;
};

// What every block state is (RFC 0004). Ids are a pure function of the block
// definitions, the same on every machine and in every run: air is 0, then
// each block in name order takes a contiguous range, one id for each
// combination of its property values, its properties in name order and the
// last varying fastest. Built once at startup and never changed, so the
// generator and mesher threads share it without locks.
class BlockRegistry {
    // Only load makes one, so every registry has air at 0 and its blocks
    // after it: an empty one would leave find and default_state nothing.
    BlockRegistry() = default;

public:
    // Block files given in memory: tests, and the loader that reads a pack.
    // Files are taken in path order, whatever order they come in.
    [[nodiscard]] static BlockLoad load(std::span<const SourceFile> files);
    // A pack on disk: every assets/<namespace>/blocks/*.toml under `root`,
    // loaded as the files above, their paths relative to `root`. Each
    // texture file no block uses, assets/<namespace>/textures/blocks/*.png,
    // is a warning. No assets folder, or a folder or file that cannot be
    // read, is an error.
    [[nodiscard]] static BlockLoad load(const std::filesystem::path& root);

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

    // What the mesher reads for each state, indexed by BlockState: air has
    // no faces, and every other state a texture layer on each.
    [[nodiscard]] std::span<const BlockVisual> visuals() const noexcept { return visuals_; }
    // The layers of the block texture array, indexed by layer: the checker,
    // then every texture and turn a face uses, in name order.
    [[nodiscard]] std::span<const TextureRef> textures() const noexcept { return textures_; }

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
    // Every state's faces, and the layers they use, from the definitions in
    // name order; an error when they need more layers than a face numbers.
    [[nodiscard]] std::optional<Diagnostic>
    build_faces(std::span<const BlockDefinition> definitions);
    // A block's state from each property's value index, and back.
    [[nodiscard]] static BlockState state_of(const Block& block,
                                             std::span<const std::size_t> values);
    [[nodiscard]] std::vector<std::size_t> values_of(BlockState state) const;

    std::vector<Block> blocks_;        // indexed by BlockType
    std::vector<BlockType> types_;     // indexed by BlockState
    std::vector<BlockVisual> visuals_; // indexed by BlockState
    std::vector<TextureRef> textures_; // indexed by layer
};

// The registry, or why it could not be built.
struct BlockLoad {
    std::optional<BlockRegistry> registry; // empty when any diagnostic is an error
    std::vector<Diagnostic> diagnostics;   // by file, each in its file's order
};

} // namespace vx
