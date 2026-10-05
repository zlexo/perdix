#include <vx/world/block_file.hpp>

#include <vx/core/diagnostic.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx {
namespace {

constexpr std::array<std::string_view, 5> block_keys = {"opacity", "cull_group", "properties",
                                                        "textures", "variant"};
constexpr std::array<std::string_view, 4> variant_keys = {"when", "textures", "rotate_x",
                                                          "rotate_y"};
// In TextureFace's order.
constexpr std::array<std::string_view, 8> face_keys = {"all",   "side",  "top",  "bottom",
                                                       "north", "south", "east", "west"};

SourcePlace place_of(const toml::source_region& region) {
    return {.line = region.begin.line, .column = region.begin.column};
}

// Insertions, deletions and substitutions from one key to another.
std::size_t edit_distance(std::string_view from, std::string_view to) {
    std::vector<std::size_t> row(to.size() + 1);
    for (std::size_t column = 0; column < row.size(); ++column) {
        row[column] = column;
    }
    for (std::size_t line = 1; line <= from.size(); ++line) {
        std::size_t diagonal = row[0];
        row[0] = line;
        for (std::size_t column = 1; column <= to.size(); ++column) {
            const std::size_t above = row[column];
            const std::size_t substitution = from[line - 1] == to[column - 1] ? 0U : 1U;
            row[column] = std::min({above + 1, row[column - 1] + 1, diagonal + substitution});
            diagonal = above;
        }
    }
    return row.back();
}

// "unknown key 'opacty'; did you mean 'opacity'?", guessing only for a near
// miss: up to one edit in three letters, and at most two.
std::string unknown_key(std::string_view key, std::span<const std::string_view> known) {
    std::string message = "unknown key '" + std::string(key) + "'";
    const auto nearest = std::ranges::min_element(
        known, {}, [key](std::string_view candidate) { return edit_distance(key, candidate); });
    const std::size_t allowed = std::clamp<std::size_t>(key.size() / 3, 1, 2);
    if (nearest != known.end() && edit_distance(key, *nearest) <= allowed) {
        message += "; did you mean '" + std::string(*nearest) + "'?";
    }
    return message;
}

// "assets/core/blocks/oak_log.toml" names "core:oak_log": the folder above
// blocks/ is the namespace, and the file name is the name.
std::optional<std::string> name_from_path(std::string_view path) {
    constexpr std::string_view extension = ".toml";
    std::array<std::string_view, 3> parts{}; // namespace, "blocks", file
    std::string_view rest = path;
    for (std::string_view& part : std::views::reverse(parts)) {
        const std::size_t slash = rest.rfind('/');
        part = slash == std::string_view::npos ? rest : rest.substr(slash + 1);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(0, slash);
    }
    if (parts[0].empty() || parts[1] != "blocks" || !parts[2].ends_with(extension) ||
        parts[2].size() == extension.size()) {
        return std::nullopt;
    }
    return std::string(parts[0]) + ':' +
           std::string(parts[2].substr(0, parts[2].size() - extension.size()));
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
    void read_properties(const toml::node& node, BlockDefinition& block);
    std::vector<TextureAssignment> read_textures(const toml::node& node);
    void read_variants(const toml::node& node, BlockDefinition& block);
    void read_variant(const toml::table& table, BlockDefinition& block);
    void read_when(const toml::node& node, VariantDefinition& variant);
    void read_angle(std::string_view key, const toml::node& node, std::int64_t& angle);

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
    bool has_textures = false;
    for (const auto& [key, node] : result.table()) {
        const std::string_view key_name = key.str();
        if (key_name == "opacity") {
            read_opacity(node, block);
        } else if (key_name == "cull_group") {
            read_cull_group(node, block);
        } else if (key_name == "properties") {
            read_properties(node, block);
        } else if (key_name == "textures") {
            block.textures = read_textures(node);
            has_textures = true;
        } else if (key_name == "variant") {
            read_variants(node, block);
        } else {
            error(key.source(), unknown_key(key_name, block_keys));
        }
    }
    if (!has_textures) {
        error(SourcePlace{}, "a block names its textures in a [textures] table");
    }
    std::ranges::sort(block.properties, {}, &PropertyDefinition::name);
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
    } else {
        error(node.source(), "cull_group is a name in quotes");
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
        const toml::array* const list = values.as_array();
        if (list == nullptr) {
            error(values.source(),
                  "property '" + property.name + R"(' takes a list of values, such as ["a", "b"])");
            continue;
        }
        for (const toml::node& value : *list) {
            if (std::optional<std::string> text = value_text(value)) {
                property.values.push_back(*std::move(text));
            } else {
                error(value.source(), "a property's values are strings, integers or booleans");
            }
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
        variant.when.push_back(std::move(condition));
    }
    std::ranges::sort(variant.when, {}, &VariantCondition::place);
}

void Parser::read_angle(std::string_view key, const toml::node& node, std::int64_t& angle) {
    if (const std::optional<std::int64_t> degrees = node.value_exact<std::int64_t>()) {
        angle = *degrees;
    } else {
        error(node.source(), std::string(key) + " is a whole number of degrees");
    }
}

} // namespace

std::optional<BlockDefinition> parse_block_file(const SourceFile& file,
                                                std::vector<Diagnostic>& diagnostics) {
    return Parser(file, diagnostics).parse();
}

} // namespace vx
