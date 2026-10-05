#pragma once

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_visual.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vx {

// A file's text and the path it came from: read from disk, or written in
// memory by tests.
struct SourceFile {
    std::string path; // "assets/core/blocks/oak_log.toml"
    std::string text;
};

// Where something was written in its file, 1-based, for diagnostics.
struct SourcePlace {
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    friend constexpr auto operator<=>(const SourcePlace&, const SourcePlace&) = default;
};

// The faces a block file names textures for (RFC 0004): a single face beats
// `side`, which beats `all`.
enum class TextureFace : std::uint8_t { all, side, top, bottom, north, south, east, west };

struct TextureAssignment {
    TextureFace face = TextureFace::all;
    std::string texture; // as written: "oak_log", or "ns:oak_log" from another namespace
    SourcePlace place;
};

struct PropertyDefinition {
    std::string name;
    std::vector<std::string> values; // integers and booleans as text; the first is the default
    SourcePlace place;
};

struct VariantCondition {
    std::string property;
    std::vector<std::string> values; // a state matches with any one of them
    SourcePlace place;
};

struct VariantDefinition {
    std::vector<VariantCondition> when;
    std::vector<TextureAssignment> textures;
    std::int64_t rotate_x = 0;
    std::int64_t rotate_y = 0;
    SourcePlace place;
};

// One block file, parsed (RFC 0004): `assets/core/blocks/oak_log.toml`
// defines `core:oak_log`.
struct BlockDefinition {
    std::string name; // from the file's path
    std::string path;
    Opacity opacity = Opacity::opaque;
    std::string cull_group; // empty for none
    SourcePlace cull_group_place;
    std::vector<PropertyDefinition> properties; // in name order
    std::vector<TextureAssignment> textures;
    SourcePlace textures_place;
    std::vector<VariantDefinition> variants; // in file order
};

// Parses one block file and checks it against RFC 0004's rules, adding
// every problem it finds to `diagnostics` in the order of the file; the
// definition comes back only when none of them is an error. TOML tables
// keep no order, so a definition lists textures and conditions in the order
// the file writes them.
[[nodiscard]] std::optional<BlockDefinition> parse_block_file(const SourceFile& file,
                                                              std::vector<Diagnostic>& diagnostics);

} // namespace vx
