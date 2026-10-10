#pragma once

#include "startup_pack.hpp"

#include <vx/core/diagnostic.hpp>
#include <vx/world/font.hpp>
#include <vx/world/skin.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vx::app {

// A skin named on the command line (RFC 0012): perdix, zlex, one of the
// game's default skins, or a skin file's path, and the arms to draw it on
// when they are given.
struct SkinChoice {
    std::string name;
    std::optional<Arms> arms = std::nullopt;
};

// `NAME`, `NAME:slim` or `NAME:wide`, from --skin= or --perdix-skin=.
[[nodiscard]] SkinChoice parse_skin_choice(std::string_view value);

// What the app draws players and their name tags with (RFC 0012), loaded at
// startup and uploaded with the renderer.
struct StartupFigures {
    // Every skin the god panel offers: Perdix's own and the owner's from the
    // pack, the game's eighteen defaults, slim then wide, when its jar is
    // there, and a skin file named on the command line. A pose's skin is an
    // index into them; one past them draws the missing texture.
    std::vector<Skin> skins;
    std::vector<std::string> names; // the panel's names for them
    std::uint32_t yours = 1;        // what you wear
    std::uint32_t perdix = 0;       // what Perdix wears
    // The game's font for name tags, when its jar is there; none draws no
    // tags until Perdix has a font of his own (RFC 0012, open question 10).
    std::optional<Font> font;
    // The startup report's lines: "Skins: perdix (Perdix), zlex (you), 18 of
    // Minecraft 26.2's" and "Font: Minecraft 26.2, from <jar>".
    std::string skins_line;
    std::string font_line;
    // Skins and fonts that cannot be read, and choices that fall back.
    std::vector<Diagnostic> diagnostics;
};

// The figures for the pack under `root` and the game the pack was drawn
// with, if any. A choice that cannot be honored, such as one of the game's
// skins without the game, is a warning, and that player wears Perdix's own
// skin, as the game's own fallback would have him.
[[nodiscard]] StartupFigures load_startup_figures(const std::filesystem::path& root,
                                                  const std::optional<StartupPack::Game>& game,
                                                  const SkinChoice& yours,
                                                  const SkinChoice& perdix);

} // namespace vx::app
