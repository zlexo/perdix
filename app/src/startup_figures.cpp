#include "startup_figures.hpp"

#include "startup_pack.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/diagnostic.hpp>
#include <vx/core/file.hpp>
#include <vx/core/result.hpp>
#include <vx/minecraft/font_file.hpp>
#include <vx/minecraft/skin_file.hpp>
#include <vx/world/font.hpp>
#include <vx/world/skin.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::app {
namespace {

// A path in UTF-8, for the report. On Windows, path::string() converts
// through the system's code page and throws on a character it cannot map.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string_view arms_name(Arms arms) noexcept {
    return arms == Arms::slim ? "slim" : "wide";
}

Diagnostic warning(std::string file, std::string message) {
    return {.severity = Severity::warning,
            .file = std::move(file),
            .line = 0,
            .column = 0,
            .message = std::move(message)};
}

// A skin file, read the way the game reads one.
Result<Skin> skin_file(const std::filesystem::path& path, Arms arms) {
    const std::optional<std::string> bytes = read_file(path);
    if (!bytes) {
        return Error{"cannot be read"};
    }
    return minecraft::read_skin(std::as_bytes(std::span(bytes->data(), bytes->size())), arms);
}

// Perdix's own skin and the owner's, from the pack's core namespace.
void load_pack_skins(const std::filesystem::path& root, StartupFigures& figures) {
    for (const auto& [name, arms] :
         {std::pair{"perdix", Arms::wide}, std::pair{"zlex", Arms::slim}}) {
        const std::string file = std::string("assets/core/textures/entity/player/") + name + ".png";
        Result<Skin> skin = skin_file(root / file, arms);
        if (!skin) {
            figures.diagnostics.push_back(warning(file, skin.error().message));
            continue;
        }
        figures.skins.push_back(skin.value());
        figures.names.emplace_back(name);
    }
}

// The game's eighteen default skins and its font, from its jar.
void load_game_figures(const StartupPack::Game& game, StartupFigures& figures) {
    const Result<Archive> jar = Archive::open(game.jar);
    if (!jar) {
        figures.diagnostics.push_back(warning(utf8(game.jar), jar.error().message));
        figures.font_line = "Font: none, so no name tags; the game's jar " + jar.error().message;
        return;
    }
    for (const Arms arms : {Arms::slim, Arms::wide}) {
        for (const std::string_view name : minecraft::default_skin_names) {
            Result<Skin> skin = minecraft::default_skin(jar.value(), name, arms);
            if (!skin) {
                figures.diagnostics.push_back(warning(utf8(game.jar), skin.error().message));
                continue;
            }
            figures.skins.push_back(skin.value());
            figures.names.push_back(std::string(name) + " (" + std::string(arms_name(arms)) + ")");
        }
    }
    Result<Font> font = minecraft::default_font(jar.value());
    if (!font) {
        figures.diagnostics.push_back(warning(utf8(game.jar), font.error().message));
        figures.font_line = "Font: none, so no name tags; " + font.error().message;
        return;
    }
    figures.font = std::move(font).value();
    figures.font_line = "Font: Minecraft " + game.version + ", from " + utf8(game.jar);
}

// Past every skin: the renderer draws the missing texture.
constexpr std::uint32_t missing = std::numeric_limits<std::uint32_t>::max();

std::optional<std::uint32_t> index_of(const StartupFigures& figures, const std::string& name) {
    const auto found = std::ranges::find(figures.names, name);
    if (found == figures.names.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(found - figures.names.begin());
}

// The skin `choice` names, by its index: one already loaded, or a skin file
// added for it. A name the figures do not have is a warning, and Perdix's
// own skin stands in, or the missing texture without it.
std::uint32_t resolve(const SkinChoice& choice, std::string_view option, bool game,
                      StartupFigures& figures) {
    const auto fall_back = [&](const std::string& why) {
        figures.diagnostics.push_back(
            warning(std::string(option), why + "; Perdix's own skin is worn instead"));
        return index_of(figures, "perdix").value_or(missing);
    };
    const bool is_default = std::ranges::find(minecraft::default_skin_names, choice.name) !=
                            minecraft::default_skin_names.end();
    if (is_default) {
        if (!game) {
            return fall_back("'" + choice.name +
                             "' is one of the game's skins, and its jar is "
                             "not there");
        }
        const std::string name =
            choice.name + " (" + std::string(arms_name(choice.arms.value_or(Arms::wide))) + ")";
        if (const std::optional<std::uint32_t> index = index_of(figures, name)) {
            return *index;
        }
        return fall_back("'" + name + "' could not be read");
    }
    if (choice.name == "perdix" || choice.name == "zlex") {
        const std::optional<std::uint32_t> index = index_of(figures, choice.name);
        if (!index) {
            return fall_back("'" + choice.name + "' could not be read");
        }
        if (!choice.arms || *choice.arms == figures.skins[*index].arms) {
            return *index;
        }
        // The same texels on the other arms: one more entry.
        Skin other = figures.skins[*index];
        other.arms = *choice.arms;
        figures.skins.push_back(other);
        figures.names.push_back(choice.name + " (" + std::string(arms_name(other.arms)) + ")");
        return static_cast<std::uint32_t>(figures.skins.size() - 1);
    }
    // Anything else is a skin file.
    const Result<Skin> skin = skin_file(choice.name, choice.arms.value_or(Arms::wide));
    if (!skin) {
        return fall_back("the skin file '" + choice.name + "' " + skin.error().message);
    }
    figures.skins.push_back(skin.value());
    figures.names.push_back(choice.name);
    return static_cast<std::uint32_t>(figures.skins.size() - 1);
}

} // namespace

SkinChoice parse_skin_choice(std::string_view value) {
    for (const Arms arms : {Arms::slim, Arms::wide}) {
        const std::string suffix = ":" + std::string(arms_name(arms));
        if (value.size() > suffix.size() && value.ends_with(suffix)) {
            return {.name = std::string(value.substr(0, value.size() - suffix.size())),
                    .arms = arms};
        }
    }
    return {.name = std::string(value), .arms = std::nullopt};
}

StartupFigures load_startup_figures(const std::filesystem::path& root,
                                    const std::optional<StartupPack::Game>& game,
                                    const SkinChoice& yours, const SkinChoice& perdix) {
    StartupFigures figures;
    load_pack_skins(root, figures);
    const std::size_t from_pack = figures.skins.size();
    if (game) {
        load_game_figures(*game, figures);
    } else {
        figures.font_line = "Font: none, so no name tags without the player's Minecraft";
    }
    const std::size_t from_game = figures.skins.size() - from_pack;
    figures.perdix = resolve(perdix, "--perdix-skin", game.has_value(), figures);
    figures.yours = resolve(yours, "--skin", game.has_value(), figures);
    const auto named = [&](std::uint32_t index) {
        return index < figures.names.size() ? figures.names[index] : std::string("none");
    };
    figures.skins_line =
        "Skins: " + named(figures.perdix) + " (Perdix), " + named(figures.yours) + " (you)";
    if (game) {
        figures.skins_line +=
            ", " + std::to_string(from_game) + " of Minecraft " + game->version + "'s";
    }
    return figures;
}

} // namespace vx::app
