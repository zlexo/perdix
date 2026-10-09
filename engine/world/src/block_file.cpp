#include <vx/world/block_file.hpp>

#include <vx/core/diagnostic.hpp>
#include <vx/world/names.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx {
namespace {

constexpr std::array<std::string_view, 7> block_keys = {
    "opacity", "cull_group", "collision", "slipperiness", "properties", "textures", "variant"};
constexpr std::array<std::string_view, 4> variant_keys = {"when", "textures", "rotate_x",
                                                          "rotate_y"};
// In TextureFace's order.
constexpr std::array<std::string_view, 8> face_keys = {"all",   "side",  "top",  "bottom",
                                                       "north", "south", "east", "west"};

SourcePlace place_of(const toml::source_region& region) {
    return {.line = region.begin.line, .column = region.begin.column};
}

// "assets/core/blocks/oak_log.toml" names "core:oak_log": the folder above
// blocks/ is the namespace, and the file name is the name.
std::optional<std::string> name_from_path(std::string_view path) {
    constexpr std::string_view extension = ".toml";
    // Exactly four parts: "assets", the namespace, "blocks" and the file. A
    // path with more before them, or fewer, names no block.
    std::array<std::string_view, 4> parts{};
    std::string_view rest = path;
    for (std::string_view& part : parts) {
        const std::size_t slash = rest.find('/');
        part = rest.substr(0, slash);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
        if (slash == std::string_view::npos && &part != &parts.back()) {
            return std::nullopt;
        }
        if (slash != std::string_view::npos && &part == &parts.back()) {
            return std::nullopt;
        }
    }
    if (parts[0] != "assets" || parts[1].empty() || parts[2] != "blocks" ||
        !parts[3].ends_with(extension) || parts[3].size() == extension.size()) {
        return std::nullopt;
    }
    return std::string(parts[1]) + ':' +
           std::string(parts[3].substr(0, parts[3].size() - extension.size()));
}

// A property's value as text: strings as written, integers and booleans
// spelled out, so that every state has a text key.
std::optional<std::string> value_text(const toml::node& node) {
    if (const std::optional<std::string_view> text = node.value_exact<std::string_view>()) {
        return std::string(*text);
    }
    if (const std::optional<std::int64_t> number = node.value_exact<std::int64_t>()) {
        return std::to_string(*number);
    }
    if (const std::optional<bool> flag = node.value_exact<bool>()) {
        return *flag ? "true" : "false";
    }
    return std::nullopt;
}

// "oak_log" in the block's namespace, or "ns:oak_log".
bool is_texture_name(std::string_view text) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        return is_name(text);
    }
    return is_name(text.substr(0, colon)) && is_name(text.substr(colon + 1));
}

// The faces a block shows, and the keys that can give each a texture.
struct FaceRule {
    TextureFace face;
    std::string_view name;
    bool horizontal; // `side` covers it too
};
constexpr std::array<FaceRule, 6> face_rules = {{
    {.face = TextureFace::top, .name = "top", .horizontal = false},
    {.face = TextureFace::bottom, .name = "bottom", .horizontal = false},
    {.face = TextureFace::north, .name = "north", .horizontal = true},
    {.face = TextureFace::south, .name = "south", .horizontal = true},
    {.face = TextureFace::east, .name = "east", .horizontal = true},
    {.face = TextureFace::west, .name = "west", .horizontal = true},
}};

class Parser {
public:
    Parser(const SourceFile& file, std::vector<Diagnostic>& diagnostics)
        : file_(file), diagnostics_(diagnostics), first_(diagnostics.size()) {}

    std::optional<BlockDefinition> parse();

private:
    void error(SourcePlace place, std::string message) {
        diagnostics_.push_back({.severity = Severity::error,
                                .file = file_.path,
                                .line = place.line,
                                .column = place.column,
                                .message = std::move(message)});
        failed_ = true;
    }
    void error(const toml::source_region& region, std::string message) {
        error(place_of(region), std::move(message));
    }

    void read_opacity(const toml::node& node, BlockDefinition& block);
    void read_cull_group(const toml::node& node, BlockDefinition& block);
    // RFC 0010's keys: reads `key` if it is collision or slipperiness, and
    // says whether it was.
    bool read_physics(std::string_view key, const toml::node& node, BlockDefinition& block);
    void read_collision(const toml::node& node, BlockDefinition& block);
    void read_slipperiness(const toml::node& node, BlockDefinition& block);
    void read_properties(const toml::node& node, BlockDefinition& block);
    std::vector<TextureAssignment> read_textures(const toml::node& node);
    void read_variants(const toml::node& node, BlockDefinition& block);
    void read_variant(const toml::table& table, BlockDefinition& block);
    void read_when(const toml::node& node, VariantDefinition& variant);
    void read_angle(std::string_view key, const toml::node& node, std::int64_t& angle);
    // The rules across keys, once the whole file has been read.
    void check_faces(const BlockDefinition& block);
    void check_conditions(const BlockDefinition& block);

    // toml++ iterates a table in key order, so this file's diagnostics are
    // sorted into the file's order once it has been read.
    void sort_diagnostics();

    const SourceFile& file_;
    std::vector<Diagnostic>& diagnostics_;
    std::size_t first_; // this file's first diagnostic
    bool failed_ = false;
};

void Parser::sort_diagnostics() {
    const auto first = diagnostics_.begin() + static_cast<std::ptrdiff_t>(first_);
    std::ranges::stable_sort(first, diagnostics_.end(), {}, [](const Diagnostic& diagnostic) {
        return SourcePlace{.line = diagnostic.line, .column = diagnostic.column};
    });
}

std::optional<BlockDefinition> Parser::parse() {
    std::optional<std::string> name = name_from_path(file_.path);
    if (!name) {
        error(SourcePlace{}, "a block file belongs at assets/<namespace>/blocks/<name>.toml");
        return std::nullopt;
    }
    toml::parse_result result = toml::parse(file_.text, file_.path);
    if (!result) {
        error(result.error().source(), std::string(result.error().description()));
        return std::nullopt;
    }
    BlockDefinition block;
    block.name = *std::move(name);
    block.path = file_.path;
    const std::size_t colon = block.name.find(':');
    if (!is_name(std::string_view(block.name).substr(0, colon))) {
        error(SourcePlace{},
              "namespace '" + block.name.substr(0, colon) + "'" + std::string(not_a_name));
    }
    if (!is_name(std::string_view(block.name).substr(colon + 1))) {
        error(SourcePlace{},
              "block name '" + block.name.substr(colon + 1) + "'" + std::string(not_a_name));
    }
    if (block.name == "core:air") {
        error(SourcePlace{}, "core:air is built in, and no file can define it");
    }
    // The rules across keys run only on keys read without a problem: a
    // misspelt face would otherwise also leave faces without textures, and a
    // broken property table would leave every condition unknown. A misspelt
    // key that is a near miss of one counts as that key read with a problem,
    // so "[propertes]" does not also leave every condition unknown.
    bool has_textures = false;
    bool textures_read = false;
    bool textures_misspelt = false;
    bool properties_read = true;
    bool opacity_read = true;
    for (const auto& [key, node] : result.table()) {
        const std::string_view key_name = key.str();
        const std::size_t problems = diagnostics_.size();
        if (key_name == "opacity") {
            read_opacity(node, block);
            opacity_read = diagnostics_.size() == problems;
        } else if (key_name == "cull_group") {
            read_cull_group(node, block);
        } else if (key_name == "properties") {
            read_properties(node, block);
            properties_read = diagnostics_.size() == problems;
        } else if (key_name == "textures") {
            block.textures = read_textures(node);
            block.textures_place = place_of(key.source());
            has_textures = true;
            textures_read = diagnostics_.size() == problems;
        } else if (key_name == "variant") {
            read_variants(node, block);
        } else if (!read_physics(key_name, node, block)) {
            error(key.source(), unknown_key(key_name, block_keys));
            const std::optional<std::string_view> meant = near_miss(key_name, block_keys);
            textures_misspelt = textures_misspelt || meant == "textures";
            properties_read = properties_read && meant != "properties";
            opacity_read = opacity_read && meant != "opacity";
        }
    }
    if (!has_textures) {
        if (!textures_misspelt) {
            error(SourcePlace{}, "a block names its textures in a [textures] table");
        }
    } else if (textures_read) {
        check_faces(block);
    }
    if (!block.cull_group.empty() && block.opacity != Opacity::cutout && opacity_read) {
        error(block.cull_group_place, "cull_group is for cutout blocks, and this block is opaque");
    }
    std::ranges::sort(block.properties, {}, &PropertyDefinition::name);
    if (properties_read) {
        check_conditions(block);
    }
    sort_diagnostics();
    if (failed_) {
        return std::nullopt;
    }
    return block;
}

void Parser::read_opacity(const toml::node& node, BlockDefinition& block) {
    const std::optional<std::string_view> value = node.value_exact<std::string_view>();
    if (value == "opaque") {
        block.opacity = Opacity::opaque;
    } else if (value == "cutout") {
        block.opacity = Opacity::cutout;
    } else {
        error(node.source(), R"(opacity is "opaque" or "cutout")");
    }
}

void Parser::read_cull_group(const toml::node& node, BlockDefinition& block) {
    if (const std::optional<std::string_view> group = node.value_exact<std::string_view>()) {
        block.cull_group = std::string(*group);
        block.cull_group_place = place_of(node.source());
        if (!is_name(*group)) {
            error(node.source(), "cull group '" + block.cull_group + "'" + std::string(not_a_name));
        }
    } else {
        error(node.source(), "cull_group is a name in quotes");
    }
}

bool Parser::read_physics(std::string_view key, const toml::node& node, BlockDefinition& block) {
    if (key == "collision") {
        read_collision(node, block);
        return true;
    }
    if (key == "slipperiness") {
        read_slipperiness(node, block);
        return true;
    }
    return false;
}

void Parser::read_collision(const toml::node& node, BlockDefinition& block) {
    const std::optional<std::string_view> value = node.value_exact<std::string_view>();
    if (value == "full") {
        block.collision = Collision::full;
    } else if (value == "none") {
        block.collision = Collision::none;
    } else {
        error(node.source(), R"(collision is "full" or "none")");
    }
}

void Parser::read_slipperiness(const toml::node& node, BlockDefinition& block) {
    // A whole number counts: slipperiness = 1 is the most there is. Above 0,
    // as ground acceleration divides by it; NaN fails both comparisons.
    std::optional<double> value = node.value_exact<double>();
    if (const std::optional<std::int64_t> whole = node.value_exact<std::int64_t>()) {
        value = static_cast<double>(*whole);
    }
    if (value && *value > 0.0 && *value <= 1.0) {
        block.slipperiness = *value;
    } else {
        error(node.source(), "slipperiness is a number above 0 and at most 1");
    }
}

void Parser::read_properties(const toml::node& node, BlockDefinition& block) {
    const toml::table* const table = node.as_table();
    if (table == nullptr) {
        error(node.source(), "properties is a table of property names and their values");
        return;
    }
    for (const auto& [key, values] : *table) {
        PropertyDefinition property{
            .name = std::string(key.str()), .values = {}, .place = place_of(key.source())};
        if (!is_name(property.name)) {
            error(property.place,
                  "property name '" + property.name + "'" + std::string(not_a_name));
        }
        const toml::array* const list = values.as_array();
        if (list == nullptr) {
            error(values.source(),
                  "property '" + property.name + R"(' takes a list of values, such as ["a", "b"])");
            continue;
        }
        for (const toml::node& value : *list) {
            std::optional<std::string> text = value_text(value);
            if (!text) {
                error(value.source(), "a property's values are strings, integers or booleans");
            } else if (std::ranges::find(property.values, *text) != property.values.end()) {
                error(value.source(),
                      "property '" + property.name + "' lists '" + *text + "' twice");
            } else {
                if (!is_name(*text)) {
                    error(value.source(), "value '" + *text + "' of property '" + property.name +
                                              "'" + std::string(not_a_name));
                }
                property.values.push_back(*std::move(text));
            }
        }
        if (list->size() < 2) {
            error(property.place, "property '" + property.name + "' has " +
                                      (list->empty() ? "no values" : "one value") +
                                      ", and a property needs two or more");
        }
        block.properties.push_back(std::move(property));
    }
}

std::vector<TextureAssignment> Parser::read_textures(const toml::node& node) {
    std::vector<TextureAssignment> textures;
    const toml::table* const table = node.as_table();
    if (table == nullptr) {
        error(node.source(), "textures is a table of faces and texture names");
        return textures;
    }
    for (const auto& [key, value] : *table) {
        const auto* const face = std::ranges::find(face_keys, key.str());
        if (face == face_keys.end()) {
            error(key.source(), unknown_key(key.str(), face_keys));
            continue;
        }
        const std::optional<std::string_view> texture = value.value_exact<std::string_view>();
        if (!texture) {
            error(value.source(), R"(a texture is named in quotes, such as "oak_log")");
            continue;
        }
        if (!is_texture_name(*texture)) {
            error(value.source(),
                  "texture name '" + std::string(*texture) + "'" + std::string(not_a_name));
        }
        textures.push_back({.face = static_cast<TextureFace>(face - face_keys.begin()),
                            .texture = std::string(*texture),
                            .place = place_of(value.source())});
    }
    std::ranges::sort(textures, {}, &TextureAssignment::place);
    return textures;
}

void Parser::read_variants(const toml::node& node, BlockDefinition& block) {
    const toml::array* const list = node.as_array();
    if (list == nullptr || !list->is_array_of_tables()) {
        error(node.source(), "variants are tables, each written [[variant]]");
        return;
    }
    for (const toml::node& element : *list) {
        read_variant(*element.as_table(), block);
    }
}

void Parser::read_variant(const toml::table& table, BlockDefinition& block) {
    VariantDefinition variant;
    variant.place = place_of(table.source());
    const std::size_t problems = diagnostics_.size();
    for (const auto& [key, value] : table) {
        const std::string_view key_name = key.str();
        if (key_name == "when") {
            read_when(value, variant);
        } else if (key_name == "textures") {
            variant.textures = read_textures(value);
        } else if (key_name == "rotate_x") {
            read_angle(key_name, value, variant.rotate_x);
        } else if (key_name == "rotate_y") {
            read_angle(key_name, value, variant.rotate_y);
        } else {
            error(key.source(), unknown_key(key_name, variant_keys));
        }
    }
    // Only for a variant read without a problem, which would explain it.
    if (diagnostics_.size() == problems && variant.textures.empty() && variant.rotate_x == 0 &&
        variant.rotate_y == 0) {
        error(variant.place, "a variant that names no textures and no turn changes nothing");
    }
    block.variants.push_back(std::move(variant));
}

void Parser::read_when(const toml::node& node, VariantDefinition& variant) {
    const toml::table* const table = node.as_table();
    if (table == nullptr) {
        error(node.source(), R"(when is a table of properties and values, such as { axis = "x" })");
        return;
    }
    for (const auto& [key, value] : *table) {
        VariantCondition condition{
            .property = std::string(key.str()), .values = {}, .place = place_of(key.source())};
        if (const toml::array* const list = value.as_array()) {
            for (const toml::node& one : *list) {
                if (std::optional<std::string> text = value_text(one)) {
                    condition.values.push_back(*std::move(text));
                } else {
                    error(one.source(), "a property's values are strings, integers or booleans");
                }
            }
        } else if (std::optional<std::string> text = value_text(value)) {
            condition.values.push_back(*std::move(text));
        } else {
            error(value.source(), "a condition names one value or a list of them");
        }
        if (value.is_array() && value.as_array()->empty()) {
            error(condition.place, "the condition on '" + condition.property + "' names no values");
        }
        variant.when.push_back(std::move(condition));
    }
    std::ranges::sort(variant.when, {}, &VariantCondition::place);
}

void Parser::read_angle(std::string_view key, const toml::node& node, std::int64_t& angle) {
    if (const std::optional<std::int64_t> degrees = node.value_exact<std::int64_t>()) {
        angle = *degrees;
        if (angle != 0 && angle != 90 && angle != 180 && angle != 270) {
            error(node.source(),
                  std::string(key) + " must be 0, 90, 180 or 270, not " + std::to_string(angle));
        }
    } else {
        error(node.source(), std::string(key) + " is a whole number of degrees");
    }
}

void Parser::check_faces(const BlockDefinition& block) {
    std::array<bool, face_keys.size()> named{};
    for (const TextureAssignment& texture : block.textures) {
        named[static_cast<std::size_t>(texture.face)] = true;
    }
    const bool all = named[static_cast<std::size_t>(TextureFace::all)];
    const bool side = named[static_cast<std::size_t>(TextureFace::side)];
    for (const FaceRule& rule : face_rules) {
        if (named[static_cast<std::size_t>(rule.face)] || all || (rule.horizontal && side)) {
            continue;
        }
        const std::string keys = rule.horizontal ? std::string(rule.name) + ", side or all"
                                                 : std::string(rule.name) + " or all";
        error(block.textures_place,
              "the " + std::string(rule.name) + " face has no texture: name " + keys);
    }
}

void Parser::check_conditions(const BlockDefinition& block) {
    std::vector<std::string_view> property_names;
    property_names.reserve(block.properties.size());
    for (const PropertyDefinition& property : block.properties) {
        property_names.emplace_back(property.name);
    }
    for (const VariantDefinition& variant : block.variants) {
        for (const VariantCondition& condition : variant.when) {
            const auto property =
                std::ranges::find(block.properties, condition.property, &PropertyDefinition::name);
            if (property == block.properties.end()) {
                error(condition.place, "the block has no property '" + condition.property + "'" +
                                           did_you_mean(condition.property, property_names));
                continue;
            }
            const std::vector<std::string_view> values(property->values.begin(),
                                                       property->values.end());
            for (const std::string& value : condition.values) {
                if (std::ranges::find(property->values, value) == property->values.end()) {
                    error(condition.place, "property '" + property->name + "' has no value '" +
                                               value + "'" + did_you_mean(value, values));
                }
            }
        }
    }
}

} // namespace

std::optional<BlockDefinition> parse_block_file(const SourceFile& file,
                                                std::vector<Diagnostic>& diagnostics) {
    return Parser(file, diagnostics).parse();
}

} // namespace vx
