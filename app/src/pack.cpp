#include "pack.hpp"

#include <vx/core/result.hpp>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_filesystem.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace vx::app {

namespace fs = std::filesystem;

Result<fs::path> find_pack(std::optional<std::string_view> given) {
    std::error_code error;
    if (given.has_value()) {
        const fs::path root(*given);
        if (!fs::is_directory(root / "assets", error)) {
            return Error{"--pack=" + std::string(*given) + " has no assets folder"};
        }
        return root;
    }
    // The executable's folder, which SDL finds on every platform.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr) {
        return Error{std::string("cannot tell where the executable is (") + SDL_GetError() +
                     "): name the pack with --pack=DIR"};
    }
    fs::path folder(base);
    if (!folder.has_filename()) {
        folder = folder.parent_path(); // the path ends in a separator
    }
    for (;; folder = folder.parent_path()) {
        if (fs::is_directory(folder / "assets", error)) {
            return folder;
        }
        if (folder.parent_path() == folder) {
            return Error{"no assets folder in " + std::string(base) +
                         " or any folder above it: name the pack with --pack=DIR"};
        }
    }
}

} // namespace vx::app
