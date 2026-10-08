#include <vx/minecraft/texture_map.hpp>

#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/world/block_file.hpp>
#include <vx/world/block_registry.hpp>
#include <vx/world/names.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vx::minecraft {
namespace {

constexpr std::array<std::string_view, 2> layer_keys = {"texture", "tint"};

constexpr std::string_view texture_kinds =
    R"(a texture is a name in quotes, such as "stone", a table such as )"
    R"({ texture = "grass_block_top", tint = "grass" }, or a list of them)";
constexpr std::string_view layer_kinds =
    R"(a layer is a name in quotes, such as "stone", or a table such as )"
    R"({ texture = "grass_block_top", tint = "grass" })";

SourcePlace place_of(const toml::source_region& region) {
    return {.line = region.begin.line, .column = region.begin.column};
}

// "assets/core/minecraft.toml" is the core namespace's map: the folder
// above it, and nothing more before it.
std::optional<std::string_view> namespace_from_path(std::string_view path) {
    constexpr std::string_view folder = "assets/";
    constexpr std::string_view file = "/minecraft.toml";
    if (!path.starts_with(folder) || !path.ends_with(file) ||
        path.size() <= folder.size() + file.size()) {
        return std::nullopt;
    }
    const std::string_view space =
        path.substr(folder.size(), path.size() - folder.size() - file.size());
    if (space.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    return space;
}

// "#619961" as a color, or nothing when it is not # and six hexadecimal
// digits, in either case.
std::optional<Color> parse_color(std::string_view text) {
    if (text.size() != 7 || text.front() != '#') {
        return std::nullopt;
    }
    Color color{};
    for (std::size_t channel = 0; channel < color.size(); ++channel) {
        const std::string_view digits = text.substr(1 + (channel * 2), 2);
        // from_chars takes no sign in an unsigned number, and no 0x.
        const auto [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), color[channel], 16);
        if (error != std::errc{} || end != digits.data() + digits.size()) {
            return std::nullopt;
        }
    }
    return color;
}

class Parser {
public:
    explicit Parser(const SourceFile& file) : file_(file) {}

    TextureMapLoad parse();

private:
    void error(SourcePlace place, std::string message) {
        diagnostics_.push_back({.severity = Severity::error,
                                .file = file_.path,
                                .line = place.line,
                                .column = place.column,
                                .message = std::move(message)});
    }
    void error(const toml::source_region& region, std::string message) {
        error(place_of(region), std::move(message));
    }

    MappedTexture read_texture(const toml::key& key, const toml::node& node);
    std::optional<Layer> read_layer(const toml::node& node, std::string_view kinds);
    Layer read_layer_table(const toml::table& table);
    void read_texture_name(const toml::node& node, Layer& layer);
    void read_tint(const toml::node& node, Layer& layer);

    const SourceFile& file_;
    std::vector<Diagnostic> diagnostics_;
};

TextureMapLoad Parser::parse() {
    const std::optional<std::string_view> space = namespace_from_path(file_.path);
    if (!space) {
        error(SourcePlace{}, "a texture map belongs at assets/<namespace>/minecraft.toml");
        return {.map = std::nullopt, .diagnostics = std::move(diagnostics_)};
    }
    if (!is_name(*space)) {
        error(SourcePlace{}, "namespace '" + std::string(*space) + "'" + std::string(not_a_name));
    }
    toml::parse_result result = toml::parse(file_.text, file_.path);
    if (!result) {
        error(result.error().source(), std::string(result.error().description()));
        return {.map = std::nullopt, .diagnostics = std::move(diagnostics_)};
    }
    TextureMap map{.path = file_.path, .namespace_name = std::string(*space), .textures = {}};
    for (const auto& [key, node] : result.table()) {
        if (!is_name(key.str())) {
            error(key.source(),
                  "texture name '" + std::string(key.str()) + "'" + std::string(not_a_name));
        }
        map.textures.emplace(key.str(), read_texture(key, node));
    }
    // toml++ iterates a table in key order, so the diagnostics are sorted
    // into the file's order.
    std::ranges::stable_sort(diagnostics_, {}, [](const Diagnostic& diagnostic) {
        return SourcePlace{.line = diagnostic.line, .column = diagnostic.column};
    });
    if (has_errors(diagnostics_)) {
        return {.map = std::nullopt, .diagnostics = std::move(diagnostics_)};
    }
    return {.map = std::move(map), .diagnostics = std::move(diagnostics_)};
}

MappedTexture Parser::read_texture(const toml::key& key, const toml::node& node) {
    MappedTexture texture{.layers = {}, .place = place_of(key.source())};
    const toml::array* const list = node.as_array();
    if (list == nullptr) {
        if (std::optional<Layer> layer = read_layer(node, texture_kinds)) {
            texture.layers.push_back(*std::move(layer));
        }
        return texture;
    }
    if (list->empty()) {
        error(node.source(), "texture '" + std::string(key.str()) + "' lists no layers");
    }
    for (const toml::node& element : *list) {
        if (std::optional<Layer> layer = read_layer(element, layer_kinds)) {
            texture.layers.push_back(*std::move(layer));
        }
    }
    return texture;
}

// A layer written as a name or a table; anything else is an error that says
// what `kinds` of value belong there.
std::optional<Layer> Parser::read_layer(const toml::node& node, std::string_view kinds) {
    if (const toml::table* const table = node.as_table()) {
        return read_layer_table(*table);
    }
    if (!node.is_string()) {
        error(node.source(), std::string(kinds));
        return std::nullopt;
    }
    Layer layer;
    read_texture_name(node, layer);
    return layer;
}

Layer Parser::read_layer_table(const toml::table& table) {
    Layer layer;
    bool named = false;
    // A key that is a near miss of texture counts as the texture, so that
    // { textures = "stone" } is not also a layer without its texture.
    bool misspelt = false;
    for (const auto& [key, value] : table) {
        if (key.str() == "texture") {
            named = true;
            read_texture_name(value, layer);
        } else if (key.str() == "tint") {
            read_tint(value, layer);
        } else {
            error(key.source(), unknown_key(key.str(), layer_keys));
            misspelt = misspelt || near_miss(key.str(), layer_keys) == "texture";
        }
    }
    if (!named && !misspelt) {
        error(table.source(), R"(a layer names its texture, such as texture = "grass_block_top")");
    }
    return layer;
}

void Parser::read_texture_name(const toml::node& node, Layer& layer) {
    layer.place = place_of(node.source());
    const std::optional<std::string_view> name = node.value_exact<std::string_view>();
    if (!name) {
        error(node.source(), R"(texture is a name in quotes, such as "grass_block_top")");
        return;
    }
    layer.texture = std::string(*name);
    if (!is_name(*name)) {
        error(node.source(),
              "Minecraft texture name '" + layer.texture + "'" + std::string(not_a_name));
    }
}

void Parser::read_tint(const toml::node& node, Layer& layer) {
    layer.tint_place = place_of(node.source());
    const std::optional<std::string_view> text = node.value_exact<std::string_view>();
    if (!text) {
        error(
            node.source(),
            R"(tint is a color map's name in quotes, such as "grass", or a color such as "#619961")");
        return;
    }
    if (const std::optional<Color> color = parse_color(*text)) {
        layer.tint = *color;
    } else if (is_name(*text)) {
        layer.tint = std::string(*text);
    } else {
        error(node.source(), "tint '" + std::string(*text) +
                                 R"(' is neither a color map's name, such as "grass", nor a )"
                                 "color written #rrggbb");
    }
}

} // namespace

TextureMapLoad load_texture_map(const SourceFile& file) {
    return Parser(file).parse();
}

namespace {

namespace fs = std::filesystem;

// A path's last part in UTF-8. On Windows, path::string() converts through
// the system's code page and throws on a character it cannot map, which
// engine code, built without exceptions, cannot catch.
std::string utf8_name(const fs::path& path) {
    const std::u8string name = path.filename().u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

// Whether two names differ in the case of their letters alone.
bool same_but_case(std::string_view one, std::string_view other) {
    const auto lower = [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    };
    return one != other && std::ranges::equal(one, other, {}, lower, lower);
}

// What a folder holds, by name, in name order; nothing when it cannot be
// listed.
std::vector<fs::directory_entry> listing(const fs::path& folder) {
    std::vector<fs::directory_entry> entries;
    std::error_code error;
    for (fs::directory_iterator entry(folder, error); !error && entry != fs::directory_iterator();
         entry.increment(error)) {
        entries.push_back(*entry);
    }
    std::ranges::sort(entries, {},
                      [](const fs::directory_entry& entry) { return utf8_name(entry.path()); });
    return entries;
}

} // namespace

TextureMapsLoad load_texture_maps(const fs::path& root) {
    TextureMapsLoad load;
    const auto error = [&](std::string file, std::string message) {
        load.diagnostics.push_back({.severity = Severity::error,
                                    .file = std::move(file),
                                    .line = 0,
                                    .column = 0,
                                    .message = std::move(message)});
    };
    constexpr std::string_view file_name = "minecraft.toml";
    // The assets folder named exactly so; the block loader says what is wrong
    // with one named otherwise.
    std::vector<fs::directory_entry> spaces;
    for (const fs::directory_entry& entry : listing(root)) {
        std::error_code ignored;
        if (utf8_name(entry.path()) == "assets" && entry.is_directory(ignored)) {
            spaces = listing(entry.path());
        }
    }
    for (const fs::directory_entry& space : spaces) {
        const std::string space_name = utf8_name(space.path());
        std::error_code ignored;
        if (space_name.starts_with('.') || !space.is_directory(ignored)) {
            continue;
        }
        for (const fs::directory_entry& entry : listing(space.path())) {
            const std::string name = utf8_name(entry.path());
            std::string path = "assets/";
            path += space_name;
            path += '/';
            path += name;
            if (same_but_case(name, file_name)) {
                error(path, "'" + name + "' must be named '" + std::string(file_name) +
                                "': names in a pack match exactly, as on Linux");
                continue;
            }
            if (name != file_name) {
                continue;
            }
            // A regular file only, following links: a pipe or a device, such
            // as a link to /dev/zero, would be read until it ended, if ever.
            const std::optional<std::string> text =
                entry.is_regular_file(ignored) ? read_file(entry.path()) : std::nullopt;
            if (!text) {
                error(path, "the file cannot be read");
                continue;
            }
            TextureMapLoad map = load_texture_map({.path = path, .text = *text});
            load.diagnostics.insert(load.diagnostics.end(), map.diagnostics.begin(),
                                    map.diagnostics.end());
            if (map.map) {
                load.maps.push_back(*std::move(map.map));
            }
        }
    }
    return load;
}

void check_texture_map(const TextureMap& map, std::span<const TextureRef> textures,
                       std::vector<Diagnostic>& diagnostics) {
    const std::string space = map.namespace_name + ':';
    // The namespace's textures that blocks use, by their keys in the map,
    // each with where it is first used. A texture turned more than one way
    // is a layer for each turn; the first, the least turned, gives a place
    // to look.
    std::map<std::string_view, std::string_view, std::less<>> used;
    for (const TextureRef& texture : textures) {
        if (texture.name.starts_with(space)) {
            used.try_emplace(std::string_view(texture.name).substr(space.size()),
                             texture.first_use);
        }
    }
    const auto warn = [&](SourcePlace place, std::string message) {
        diagnostics.push_back({.severity = Severity::warning,
                               .file = map.path,
                               .line = place.line,
                               .column = place.column,
                               .message = std::move(message)});
    };
    // The textures without a key, which a key no block uses may have meant.
    std::vector<std::string_view> keyless;
    for (const auto& [name, use] : used) {
        if (!map.textures.contains(name)) {
            keyless.push_back(name);
            warn(SourcePlace{}, space + std::string(name) +
                                    " has no key, and is drawn from its own file (used at " +
                                    std::string(use) + ")");
        }
    }
    // The map holds its keys in name order; the warnings follow the file.
    std::vector<std::pair<SourcePlace, std::string_view>> unused;
    for (const auto& [name, texture] : map.textures) {
        if (!used.contains(name)) {
            unused.emplace_back(texture.place, name);
        }
    }
    std::ranges::sort(unused);
    for (const auto& [place, name] : unused) {
        warn(place, "no block uses the texture " + space + std::string(name) +
                        did_you_mean(name, keyless));
    }
}

} // namespace vx::minecraft
