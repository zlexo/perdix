#include <vx/world/block_registry.hpp>

#include <vx/core/diagnostic.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_state.hpp>
#include <vx/world/block_visual.hpp>
#include <vx/world/coordinates.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx {
namespace {

// A 16-bit state id numbers 65,536 states, air among them (RFC 0002).
constexpr std::uint64_t max_states = std::uint64_t{std::numeric_limits<std::uint16_t>::max()} + 1;

// 70000 as "70,000", for messages.
std::string with_commas(std::uint64_t number) {
    std::string digits = std::to_string(number);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

// The product of the property value counts. It saturates instead of
// overflowing, so a block of many properties cannot wrap it, and short of
// that the error for too many states gives every count as it is.
std::uint64_t states_of(const BlockDefinition& block) {
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t states = 1;
    for (const PropertyDefinition& property : block.properties) {
        const std::uint64_t values = property.values.size();
        states = values != 0 && states > most / values ? most : states * values;
    }
    return states;
}

// The error for more states than ids, naming the largest blocks, as Bedrock
// names them when its permutations overflow.
Diagnostic too_many_states(std::uint64_t total, std::span<const BlockDefinition> blocks) {
    std::vector<const BlockDefinition*> largest;
    largest.reserve(blocks.size());
    for (const BlockDefinition& block : blocks) {
        largest.push_back(&block);
    }
    std::ranges::stable_sort(largest, std::ranges::greater{},
                             [](const BlockDefinition* block) { return states_of(*block); });
    largest.resize(std::min<std::size_t>(largest.size(), 3));
    std::string message = "the blocks have " + with_commas(total) + " states, more than the " +
                          with_commas(max_states) + " a state id numbers; the largest: ";
    for (std::size_t at = 0; at < largest.size(); ++at) {
        message += (at == 0 ? "" : ", ") + largest[at]->name + " (" +
                   with_commas(states_of(*largest[at])) + ")";
    }
    return {.severity = Severity::error,
            .file = largest.front()->path,
            .line = 0,
            .column = 0,
            .message = message};
}

// A 16-bit texture layer numbers 65,536 layers, the checker among them.
constexpr std::size_t max_layers = std::size_t{std::numeric_limits<std::uint16_t>::max()} + 1;
// An 8-bit cull group numbers 255 groups; 0 is none.
constexpr std::size_t max_cull_groups = std::numeric_limits<std::uint8_t>::max();

// Every cull group the blocks name, in name order: group 1 is the first.
std::vector<std::string_view> cull_groups_of(std::span<const BlockDefinition> blocks) {
    std::vector<std::string_view> groups;
    for (const BlockDefinition& block : blocks) {
        if (!block.cull_group.empty()) {
            groups.emplace_back(block.cull_group);
        }
    }
    std::ranges::sort(groups);
    const auto repeated = std::ranges::unique(groups);
    groups.erase(repeated.begin(), repeated.end());
    return groups;
}

// A direction along one axis. North is -z, east +x and top +y, as in
// Minecraft (RFC 0004).
struct Direction {
    int x = 0;
    int y = 0;
    int z = 0;

    friend constexpr bool operator==(const Direction&, const Direction&) = default;
};

constexpr Direction operator-(Direction d) {
    return {.x = -d.x, .y = -d.y, .z = -d.z};
}

constexpr Direction cross(Direction a, Direction b) {
    return {.x = (a.y * b.z) - (a.z * b.y),
            .y = (a.z * b.x) - (a.x * b.z),
            .z = (a.x * b.y) - (a.y * b.x)};
}

// Indexed by Face: the way each face looks out of the block...
constexpr std::array<Direction, 6> outward = {{
    {.x = 1, .y = 0, .z = 0},
    {.x = -1, .y = 0, .z = 0},
    {.x = 0, .y = 1, .z = 0},
    {.x = 0, .y = -1, .z = 0},
    {.x = 0, .y = 0, .z = 1},
    {.x = 0, .y = 0, .z = -1},
}};
// ...and the way its image's top edge points, seen from outside: up on the
// side faces, north on the top face, south on the bottom face (RFC 0004).
constexpr std::array<Direction, 6> image_top = {{
    {.x = 0, .y = 1, .z = 0},
    {.x = 0, .y = 1, .z = 0},
    {.x = 0, .y = 0, .z = -1},
    {.x = 0, .y = 0, .z = 1},
    {.x = 0, .y = 1, .z = 0},
    {.x = 0, .y = 1, .z = 0},
}};

// One quarter of rotate_x turns the top to the north; one quarter of
// rotate_y turns the north to the east, clockwise seen from above.
constexpr Direction turn_x(Direction d) {
    return {.x = d.x, .y = d.z, .z = -d.y};
}
constexpr Direction turn_y(Direction d) {
    return {.x = -d.z, .y = d.y, .z = d.x};
}

// Where a face of the block ends up once the block is turned, and how many
// clockwise quarter turns its image makes there, seen from outside.
struct Placement {
    Face face = Face::pos_x;
    std::uint8_t quarter_turns = 0;

    friend constexpr bool operator==(const Placement&, const Placement&) = default;
};

// For each face, indexed by Face: where x_turns quarters of rotate_x, then
// y_turns quarters of rotate_y, put it. The image's top edge turns with the
// face, and the number of quarter turns is wherever it then points: a
// clockwise quarter turn moves it to the right, which is top x outward.
constexpr std::array<Placement, 6> placements(std::size_t x_turns, std::size_t y_turns) {
    std::array<Placement, 6> placed{};
    for (std::size_t from = 0; from < placed.size(); ++from) {
        Direction out = outward[from];
        Direction top = image_top[from];
        for (std::size_t turn = 0; turn < x_turns; ++turn) {
            out = turn_x(out);
            top = turn_x(top);
        }
        for (std::size_t turn = 0; turn < y_turns; ++turn) {
            out = turn_y(out);
            top = turn_y(top);
        }
        const auto to = static_cast<std::size_t>(std::ranges::find(outward, out) - outward.begin());
        const Direction right = cross(image_top[to], outward[to]);
        const std::array<Direction, 4> turned = {image_top[to], right, -image_top[to], -right};
        placed[from] = {.face = static_cast<Face>(to),
                        .quarter_turns = static_cast<std::uint8_t>(std::ranges::find(turned, top) -
                                                                   turned.begin())};
    }
    return placed;
}

// All sixteen, indexed by 4 * x_turns + y_turns, worked out while compiling.
constexpr std::array<std::array<Placement, 6>, 16> all_placements = [] {
    std::array<std::array<Placement, 6>, 16> all{};
    for (std::size_t x_turns = 0; x_turns < 4; ++x_turns) {
        for (std::size_t y_turns = 0; y_turns < 4; ++y_turns) {
            all[(4 * x_turns) + y_turns] = placements(x_turns, y_turns);
        }
    }
    return all;
}();

// RFC 0004's oak log along x, rotate_x = 90 then rotate_y = 90: its top
// shows on +x, upside down, and its north side lies underneath, turned once
// so the bark runs along x.
static_assert(all_placements[5][static_cast<std::size_t>(Face::pos_y)] ==
              Placement{.face = Face::pos_x, .quarter_turns = 2});
static_assert(all_placements[5][static_cast<std::size_t>(Face::neg_z)] ==
              Placement{.face = Face::neg_y, .quarter_turns = 1});

// The face a block file's key names on its own.
constexpr Face face_named(TextureFace face) {
    switch (face) {
    case TextureFace::east:
        return Face::pos_x;
    case TextureFace::west:
        return Face::neg_x;
    case TextureFace::top:
        return Face::pos_y;
    case TextureFace::bottom:
        return Face::neg_y;
    case TextureFace::south:
        return Face::pos_z;
    case TextureFace::north:
        return Face::neg_z;
    case TextureFace::all:
    case TextureFace::side:
        break;
    }
    assert(false && "all and side name more than one face");
    return Face::neg_z;
}

constexpr std::array<Face, 4> side_faces = {Face::pos_x, Face::neg_x, Face::pos_z, Face::neg_z};

// The faces one [textures] table names, over what earlier tables named: a
// single face beats `side`, which beats `all`. Each face holds its texture's
// index in the list of every texture the block names, where this table's
// start at `first`.
void name_faces(std::span<const TextureAssignment> textures, std::size_t first,
                std::array<std::size_t, 6>& faces) {
    for (std::size_t at = 0; at < textures.size(); ++at) {
        if (textures[at].face == TextureFace::all) {
            faces.fill(first + at);
        }
    }
    for (std::size_t at = 0; at < textures.size(); ++at) {
        if (textures[at].face == TextureFace::side) {
            for (const Face face : side_faces) {
                faces[static_cast<std::size_t>(face)] = first + at;
            }
        }
    }
    for (std::size_t at = 0; at < textures.size(); ++at) {
        const TextureFace face = textures[at].face;
        if (face != TextureFace::all && face != TextureFace::side) {
            faces[static_cast<std::size_t>(face_named(face))] = first + at;
        }
    }
}

// "oak_log" in core's block file is "core:oak_log"; "ns:oak_log" stays.
std::string texture_name(std::string_view block, std::string_view texture) {
    if (texture.find(':') != std::string_view::npos) {
        return std::string(texture);
    }
    return std::string(block.substr(0, block.find(':') + 1)) + std::string(texture);
}

// The number of a block's cull group: 1 for the first in name order, 0 for none.
std::uint8_t cull_group_number(std::span<const std::string_view> groups, std::string_view group) {
    if (group.empty()) {
        return 0;
    }
    return static_cast<std::uint8_t>(1 +
                                     (std::ranges::lower_bound(groups, group) - groups.begin()));
}

// The next state's value indices: the last property varies fastest.
void next_values(const BlockDefinition& block, std::vector<std::size_t>& values) {
    for (std::size_t digit = values.size(); digit-- > 0;) {
        if (++values[digit] < block.properties[digit].values.size()) {
            return;
        }
        values[digit] = 0;
    }
}

// A block's faces in any of its states (RFC 0004): its own textures, then
// those of every variant that matches, in file order, then turned by the
// sum of the matching variants' turns. The variants are read once, so the
// work for each state compares indices, never text.
class BlockFaces {
public:
    explicit BlockFaces(const BlockDefinition& block) {
        std::size_t count = block.textures.size();
        for (const VariantDefinition& variant : block.variants) {
            count += variant.textures.size();
        }
        textures_.reserve(count);
        for (const TextureAssignment& texture : block.textures) {
            textures_.push_back(&texture);
        }
        rules_.reserve(block.variants.size());
        for (const VariantDefinition& variant : block.variants) {
            Rule rule{.when = {},
                      .textures = variant.textures,
                      .first = textures_.size(),
                      .x_turns = variant.rotate_x / 90,
                      .y_turns = variant.rotate_y / 90};
            for (const VariantCondition& condition : variant.when) {
                rule.when.push_back(condition_of(block, condition));
            }
            for (const TextureAssignment& texture : variant.textures) {
                textures_.push_back(&texture);
            }
            rules_.push_back(std::move(rule));
        }
        name_faces(block.textures, 0, own_);
    }

    // Every texture the block names: its own, then each variant's.
    [[nodiscard]] std::span<const TextureAssignment* const> textures() const noexcept {
        return textures_;
    }

    struct Faces {
        std::array<std::size_t, 6> texture{}; // by face before turning, into textures()
        std::size_t turns = 0;                // into all_placements
    };

    // The faces of the state with these value indices.
    [[nodiscard]] Faces of(std::span<const std::size_t> values) const {
        Faces faces{.texture = own_, .turns = 0};
        std::int64_t x_turns = 0;
        std::int64_t y_turns = 0;
        for (const Rule& rule : rules_) {
            if (std::ranges::all_of(rule.when, [&](const Condition& condition) {
                    return condition.matches[values[condition.property]];
                })) {
                name_faces(rule.textures, rule.first, faces.texture);
                x_turns += rule.x_turns;
                y_turns += rule.y_turns;
            }
        }
        faces.turns = static_cast<std::size_t>((4 * (x_turns % 4)) + (y_turns % 4));
        return faces;
    }

private:
    struct Condition {
        std::size_t property = 0;  // in name order
        std::vector<bool> matches; // by value index
    };
    struct Rule {
        std::vector<Condition> when;
        std::span<const TextureAssignment> textures;
        std::size_t first = 0; // its textures' place in textures_
        std::int64_t x_turns = 0;
        std::int64_t y_turns = 0;
    };

    // The parser has checked that every condition names a property and
    // values the block has.
    static Condition condition_of(const BlockDefinition& block, const VariantCondition& condition) {
        const auto property =
            std::ranges::find(block.properties, condition.property, &PropertyDefinition::name);
        Condition result{.property = static_cast<std::size_t>(property - block.properties.begin()),
                         .matches = std::vector<bool>(property->values.size(), false)};
        for (const std::string& value : condition.values) {
            result.matches[static_cast<std::size_t>(std::ranges::find(property->values, value) -
                                                    property->values.begin())] = true;
        }
        return result;
    }

    std::vector<const TextureAssignment*> textures_;
    std::vector<Rule> rules_;
    std::array<std::size_t, 6> own_{};
};

// Every texture and turn a face uses (RFC 0004), numbered in name order once
// all are known, after layer 0, the checker.
class LayerTable {
public:
    struct Layer {
        TextureRef ref;
        std::uint16_t number = 0;
    };

    // The layer of a block's texture in some quarter turns. Map nodes stay
    // where they are, so states can point at them until number() is called.
    Layer& use(const BlockDefinition& block, const TextureAssignment& texture,
               std::uint8_t quarter_turns) {
        auto [found, added] =
            layers_.try_emplace({texture_name(block.name, texture.texture), quarter_turns});
        TextureRef& ref = found->second.ref;
        if (added) {
            ref.name = found->first.first;
            ref.quarter_turns = quarter_turns;
            ref.first_use = block.path + ':' + std::to_string(texture.place.line) + ':' +
                            std::to_string(texture.place.column);
            if (layers_.size() == max_layers && !past_limit_) {
                past_limit_ = Diagnostic{.severity = Severity::error,
                                         .file = block.path,
                                         .line = texture.place.line,
                                         .column = texture.place.column,
                                         .message = {}};
            }
        }
        (block.opacity == Opacity::cutout ? ref.used_cutout : ref.used_opaque) = true;
        return found->second;
    }

    // More layers than a face numbers, reported where, in id order, the first
    // texture past the limit is used.
    [[nodiscard]] std::optional<Diagnostic> past_limit() const {
        if (!past_limit_) {
            return std::nullopt;
        }
        Diagnostic error = *past_limit_;
        error.message = "the blocks need " + with_commas(layers_.size() + 1) +
                        " texture layers with the checker, more than the " +
                        with_commas(max_layers) + " a layer number holds";
        return error;
    }

    // Gives every layer its number, and lists them by number.
    [[nodiscard]] std::vector<TextureRef> number() {
        std::vector<TextureRef> textures;
        textures.reserve(layers_.size() + 1);
        textures.emplace_back(); // the checker
        for (auto& [key, layer] : layers_) {
            layer.number = static_cast<std::uint16_t>(textures.size());
            textures.push_back(layer.ref);
        }
        return textures;
    }

private:
    std::map<std::pair<std::string, std::uint8_t>, Layer> layers_;
    std::optional<Diagnostic> past_limit_;
};

// Reading a pack from disk. Engine code builds without exceptions (RFC
// 0000), so every file system call takes an error code.
namespace fs = std::filesystem;

Diagnostic pack_error(std::string file, std::string message) {
    return {.severity = Severity::error,
            .file = std::move(file),
            .line = 0,
            .column = 0,
            .message = std::move(message)};
}

// The entries of `folder` that `wanted` keeps, in path order; an empty list
// when the folder does not exist, and nothing when it cannot be read.
template <typename Wanted>
std::optional<std::vector<fs::path>> entries(const fs::path& folder, Wanted wanted) {
    std::error_code error;
    const fs::file_type type = fs::status(folder, error).type();
    if (type == fs::file_type::not_found) {
        return std::vector<fs::path>{};
    }
    fs::directory_iterator entry(folder, error);
    if (error || type != fs::file_type::directory) {
        return std::nullopt;
    }
    std::vector<fs::path> kept;
    for (const fs::directory_iterator end; entry != end; entry.increment(error)) {
        if (wanted(*entry)) {
            kept.push_back(entry->path());
        }
    }
    if (error) {
        return std::nullopt;
    }
    std::ranges::sort(kept);
    return kept;
}

bool is_folder(const fs::directory_entry& entry) {
    std::error_code error;
    return entry.is_directory(error);
}

// A regular file named *<extension>, such as a block file, not .DS_Store.
auto file_with(std::string_view extension) {
    return [extension](const fs::directory_entry& entry) {
        std::error_code error;
        return entry.is_regular_file(error) && entry.path().extension() == extension;
    };
}

std::optional<std::string> read_text(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) {
        return std::nullopt;
    }
    return text;
}

// A warning for each texture file of the pack's namespaces that no block
// uses (RFC 0004), in path order.
void warn_unused_textures(const fs::path& root, std::span<const fs::path> namespaces,
                          const BlockRegistry& registry, std::vector<Diagnostic>& diagnostics) {
    std::set<std::string_view> used;
    for (const TextureRef& texture : registry.textures()) {
        used.insert(texture.name);
    }
    for (const fs::path& space : namespaces) {
        const fs::path folder = space / "textures" / "blocks";
        const std::string relative = folder.lexically_relative(root).generic_string();
        const std::optional<std::vector<fs::path>> pictures = entries(folder, file_with(".png"));
        if (!pictures) {
            diagnostics.push_back(pack_error(relative, "the folder cannot be read"));
            continue;
        }
        for (const fs::path& picture : *pictures) {
            const std::string name = space.filename().string() + ":" + picture.stem().string();
            if (!used.contains(name)) {
                diagnostics.push_back({.severity = Severity::warning,
                                       .file = picture.lexically_relative(root).generic_string(),
                                       .line = 0,
                                       .column = 0,
                                       .message = "no block uses this texture"});
            }
        }
    }
}

} // namespace

BlockLoad BlockRegistry::load(const fs::path& root) {
    BlockLoad failed;
    const fs::path assets = root / "assets";
    const std::optional<std::vector<fs::path>> namespaces = entries(assets, is_folder);
    if (!namespaces || namespaces->empty()) {
        failed.diagnostics.push_back(pack_error(
            assets.generic_string(),
            namespaces ? "no such folder, or it holds no namespace" : "the folder cannot be read"));
        return failed;
    }
    std::vector<SourceFile> files;
    for (const fs::path& space : *namespaces) {
        const fs::path folder = space / "blocks";
        const std::optional<std::vector<fs::path>> found = entries(folder, file_with(".toml"));
        if (!found) {
            failed.diagnostics.push_back(pack_error(
                folder.lexically_relative(root).generic_string(), "the folder cannot be read"));
            continue;
        }
        for (const fs::path& path : *found) {
            const std::string relative = path.lexically_relative(root).generic_string();
            std::optional<std::string> text = read_text(path);
            if (!text) {
                failed.diagnostics.push_back(pack_error(relative, "the file cannot be read"));
                continue;
            }
            files.push_back({.path = relative, .text = *std::move(text)});
        }
    }
    if (has_errors(failed.diagnostics)) {
        return failed;
    }
    BlockLoad load = BlockRegistry::load(files);
    if (load.registry.has_value()) {
        warn_unused_textures(root, *namespaces, *load.registry, load.diagnostics);
    }
    return load;
}

BlockLoad BlockRegistry::load(std::span<const SourceFile> files) {
    // In path order, so the diagnostics come out the same whatever order the
    // files arrive in; the ids never depend on it.
    std::vector<const SourceFile*> ordered;
    ordered.reserve(files.size());
    for (const SourceFile& file : files) {
        ordered.push_back(&file);
    }
    std::ranges::sort(ordered, {}, &SourceFile::path);

    BlockLoad load;
    std::vector<BlockDefinition> definitions;
    for (const SourceFile* const file : ordered) {
        if (std::optional<BlockDefinition> definition = parse_block_file(*file, load.diagnostics)) {
            definitions.push_back(*std::move(definition));
        }
    }
    // Block names in byte order, which numbers the blocks.
    std::ranges::stable_sort(definitions, {}, &BlockDefinition::name);
    for (std::size_t at = 1; at < definitions.size(); ++at) {
        if (definitions[at].name == definitions[at - 1].name) {
            load.diagnostics.push_back({.severity = Severity::error,
                                        .file = definitions[at].path,
                                        .line = 0,
                                        .column = 0,
                                        .message = definitions[at].name +
                                                   " is defined twice, also in " +
                                                   definitions[at - 1].path});
        }
    }
    std::uint64_t total = 1; // air
    for (const BlockDefinition& definition : definitions) {
        const std::uint64_t states = states_of(definition);
        constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
        total = states > most - total ? most : total + states;
    }
    if (total > max_states) {
        load.diagnostics.push_back(too_many_states(total, definitions));
    }
    const std::vector<std::string_view> groups = cull_groups_of(definitions);
    if (groups.size() > max_cull_groups) {
        // Reported on the first block, in name order, of the first group past the limit.
        const std::string_view first_past = groups[max_cull_groups];
        const BlockDefinition& block =
            *std::ranges::find(definitions, first_past, &BlockDefinition::cull_group);
        load.diagnostics.push_back({.severity = Severity::error,
                                    .file = block.path,
                                    .line = block.cull_group_place.line,
                                    .column = block.cull_group_place.column,
                                    .message = "the blocks name " + with_commas(groups.size()) +
                                               " cull groups, more than the " +
                                               with_commas(max_cull_groups) +
                                               " a cull group number holds"});
    }
    if (has_errors(load.diagnostics)) {
        return load;
    }

    BlockRegistry registry;
    registry.blocks_.push_back({.name = "core:air", .first = 0, .states = 1, .properties = {}});
    std::uint32_t next = 1;
    for (const BlockDefinition& definition : definitions) {
        Block block{.name = definition.name,
                    .first = next,
                    .states = static_cast<std::uint32_t>(states_of(definition)),
                    .properties = {}};
        // Copied, not moved: the faces read the definitions next.
        for (const PropertyDefinition& property : definition.properties) {
            block.properties.push_back({.name = property.name, .values = property.values});
        }
        next += block.states;
        registry.blocks_.push_back(std::move(block));
    }
    registry.types_.reserve(next);
    for (std::size_t type = 0; type < registry.blocks_.size(); ++type) {
        registry.types_.insert(registry.types_.end(), registry.blocks_[type].states,
                               static_cast<BlockType>(type));
    }
    if (std::optional<Diagnostic> error = registry.build_faces(definitions)) {
        load.diagnostics.push_back(*std::move(error));
        return load;
    }
    load.registry = std::move(registry);
    return load;
}

std::optional<Diagnostic> BlockRegistry::build_faces(std::span<const BlockDefinition> definitions) {
    LayerTable layers;
    std::vector<std::array<const LayerTable::Layer*, 6>> faces(types_.size()); // air's stay null
    const std::vector<std::string_view> groups = cull_groups_of(definitions);
    visuals_.assign(types_.size(), BlockVisual{});
    for (std::size_t at = 0; at < definitions.size(); ++at) {
        const BlockDefinition& definition = definitions[at];
        const Block& block = blocks_[at + 1];
        const BlockFaces block_faces(definition);
        const std::uint8_t group = cull_group_number(groups, definition.cull_group);
        // The layer of each texture the block names, in each quarter turn.
        std::vector<const LayerTable::Layer*> seen(block_faces.textures().size() * 4, nullptr);
        std::vector<std::size_t> values(definition.properties.size(), 0);
        for (std::uint32_t offset = 0; offset < block.states; ++offset) {
            const std::size_t state = block.first + offset;
            visuals_[state].opacity = definition.opacity;
            visuals_[state].cull_group = group;
            const BlockFaces::Faces named = block_faces.of(values);
            for (std::size_t from = 0; from < 6; ++from) {
                const Placement place = all_placements[named.turns][from];
                const LayerTable::Layer*& layer =
                    seen[(4 * named.texture[from]) + place.quarter_turns];
                if (layer == nullptr) {
                    layer = &layers.use(definition, *block_faces.textures()[named.texture[from]],
                                        place.quarter_turns);
                }
                faces[state][static_cast<std::size_t>(place.face)] = layer;
            }
            next_values(definition, values);
        }
    }
    if (std::optional<Diagnostic> error = layers.past_limit()) {
        return error;
    }
    textures_ = layers.number();
    for (std::size_t state = 1; state < faces.size(); ++state) {
        for (std::size_t face = 0; face < 6; ++face) {
            visuals_[state].texture[face] = faces[state][face]->number;
        }
    }
    return std::nullopt;
}

std::optional<BlockState> BlockRegistry::find(std::string_view key) const {
    const std::size_t bracket = key.find('[');
    const Block* const block = find_block(key.substr(0, bracket));
    if (block == nullptr) {
        return std::nullopt;
    }
    std::vector<std::size_t> values(block->properties.size(), 0);
    if (bracket == std::string_view::npos) {
        return state_of(*block, values);
    }
    // "[facing=east,lit=true]": each property once, in any order.
    if (!key.ends_with(']') || key.size() - bracket < 3) {
        return std::nullopt;
    }
    std::vector<bool> given(block->properties.size(), false);
    std::string_view list = key.substr(bracket + 1, key.size() - bracket - 2);
    while (true) {
        const std::size_t comma = list.find(',');
        const std::string_view pair = list.substr(0, comma);
        const std::size_t equals = pair.find('=');
        if (equals == std::string_view::npos) {
            return std::nullopt;
        }
        const auto property =
            std::ranges::find(block->properties, pair.substr(0, equals), &Property::name);
        if (property == block->properties.end()) {
            return std::nullopt;
        }
        const auto value = std::ranges::find(property->values, pair.substr(equals + 1));
        const auto index = static_cast<std::size_t>(property - block->properties.begin());
        if (value == property->values.end() || given[index]) {
            return std::nullopt;
        }
        given[index] = true;
        values[index] = static_cast<std::size_t>(value - property->values.begin());
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return state_of(*block, values);
}

std::string BlockRegistry::key(BlockState state) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    std::string key = block.name;
    if (block.properties.empty()) {
        return key;
    }
    const std::vector<std::size_t> values = values_of(state);
    key += '[';
    for (std::size_t at = 0; at < block.properties.size(); ++at) {
        key += (at == 0 ? "" : ",") + block.properties[at].name + '=' +
               block.properties[at].values[values[at]];
    }
    key += ']';
    return key;
}

std::optional<BlockState> BlockRegistry::with(BlockState state, std::string_view property,
                                              std::string_view value) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    const auto changed = std::ranges::find(block.properties, property, &Property::name);
    if (changed == block.properties.end()) {
        return std::nullopt;
    }
    const auto index = std::ranges::find(changed->values, value);
    if (index == changed->values.end()) {
        return std::nullopt;
    }
    std::vector<std::size_t> values = values_of(state);
    values[static_cast<std::size_t>(changed - block.properties.begin())] =
        static_cast<std::size_t>(index - changed->values.begin());
    return state_of(block, values);
}

BlockType BlockRegistry::type_of(BlockState state) const noexcept {
    assert(static_cast<std::size_t>(state) < types_.size());
    return types_[static_cast<std::size_t>(state)];
}

BlockState BlockRegistry::default_state(BlockType type) const noexcept {
    assert(static_cast<std::size_t>(type) < blocks_.size());
    return static_cast<BlockState>(blocks_[static_cast<std::size_t>(type)].first);
}

const BlockRegistry::Block* BlockRegistry::find_block(std::string_view name) const {
    if (name == blocks_.front().name) {
        return &blocks_.front();
    }
    // Every block but air is in name order.
    const auto rest = std::span(blocks_).subspan(1);
    const auto found = std::ranges::lower_bound(rest, name, {}, &Block::name);
    return found != rest.end() && found->name == name ? &*found : nullptr;
}

BlockState BlockRegistry::state_of(const Block& block, std::span<const std::size_t> values) {
    // Mixed radix, the last property varying fastest: "facing=east,lit=true"
    // is 1 * 2 + 1 past the furnace's first state.
    std::size_t offset = 0;
    for (std::size_t at = 0; at < block.properties.size(); ++at) {
        offset = (offset * block.properties[at].values.size()) + values[at];
    }
    return static_cast<BlockState>(block.first + offset);
}

std::vector<std::size_t> BlockRegistry::values_of(BlockState state) const {
    const Block& block = blocks_[static_cast<std::size_t>(type_of(state))];
    std::vector<std::size_t> values(block.properties.size());
    std::size_t offset = static_cast<std::size_t>(state) - block.first;
    for (std::size_t at = block.properties.size(); at-- > 0;) {
        const std::size_t count = block.properties[at].values.size();
        values[at] = offset % count;
        offset /= count;
    }
    return values;
}

} // namespace vx
