#pragma once

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>
#include <vx/world/font.hpp>

#include <cstddef>
#include <span>

namespace vx::minecraft {

// A font from an ASCII atlas in the game's format (RFC 0012), with the
// advances Minecraft 26.2 gives its default font's ASCII characters. The atlas
// must be 128 by 128, and is kept as it is but for the cells below. Each
// printable character, 33 to 126, advances one more than its ink width: one
// more than the rightmost column of its cell with any texel of alpha above 0,
// or 0 for an empty cell. The space advances 4 and draws nothing, its cell
// cleared, as the game's space provider gives it. Code points 0 to 31 and 127,
// which no font in the game's jar draws, get the game's missing glyph: a 5 by
// 8 box outline in white, advancing 6. A problem is worded to follow the
// file's name.
[[nodiscard]] Result<Font> read_font(std::span<const std::byte> png);

// The game's default font from the player's jar:
// assets/minecraft/textures/font/ascii.png, read through RFC 0009's archive
// reader and then as read_font reads it. An entry that is missing, cannot be
// read, is not a PNG or has another size is an error that names it and says
// why.
[[nodiscard]] Result<Font> default_font(const Archive& jar);

} // namespace vx::minecraft
