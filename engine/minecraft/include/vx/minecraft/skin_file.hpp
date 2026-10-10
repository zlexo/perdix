#pragma once

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>
#include <vx/world/skin.hpp>

#include <array>
#include <string_view>

namespace vx::minecraft {

// The game's default skins (DefaultPlayerSkin, 26.2), in its order, each with
// slim and with wide arms.
inline constexpr std::array<std::string_view, 9> default_skin_names = {
    "alex", "ari", "efe", "kai", "makena", "noor", "steve", "sunny", "zuri"};

// One of the game's default player skins from the player's jar (RFC 0012):
// `name` is one of default_skin_names; slim arms read
// assets/minecraft/textures/entity/player/slim/<name>.png, wide arms
// wide/<name>.png. The game draws them as they are, not made ready as a skin
// file is, so they must be 64 by 64. A name it has no skin for, or an entry
// that is missing, cannot be read, is not a PNG or has another size, is an
// error that names it and says why.
[[nodiscard]] Result<Skin> default_skin(const Archive& jar, std::string_view name, Arms arms);

} // namespace vx::minecraft
