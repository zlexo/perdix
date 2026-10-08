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
namespace {

namespace fs = std::filesystem;

// Whether `folder` holds a pack: an assets folder with a block file in it,
// assets/<namespace>/blocks/<name>.toml. An assets folder of something
// else, such as images beside a copy of the executable, is passed over.
bool holds_pack(const fs::path& folder) {
    std::error_code error;
    fs::directory_iterator space(folder / "assets", error);
    for (const fs::directory_iterator end; !error && space != end; space.increment(error)) {
        std::error_code inner;
        fs::directory_iterator file(space->path() / "blocks", inner);
        for (const fs::directory_iterator files_end; !inner && file != files_end;
             file.increment(inner)) {
            if (file->path().extension() == ".toml") {
                return true;
            }
        }
    }
    return false;
}

} // namespace

Result<fs::path> find_pack(std::optional<std::string_view> given) {
    std::error_code error;
    if (given.has_value()) {
        if (given->empty()) {
            return Error{"--pack= needs the folder that holds the pack's assets folder"};
        }
        const fs::path root(*given);
        if (!fs::is_directory(root / "assets", error)) {
            return Error{"--pack=" + std::string(*given) + " has no assets folder"};
        }
        return root;
    }
    // The executable's folder. SDL finds it on every platform, though not
    // always the same way: on Linux through /proc/self/exe, which resolves
    // links, and on the Mac as the path it was started by, so a link to
    // the executable from elsewhere searches from the link's folder.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr) {
        const std::string why = SDL_GetError();
        return Error{"cannot tell where the executable is" + (why.empty() ? "" : " (" + why + ")") +
                     ": name the pack with --pack=DIR"};
    }
    fs::path folder(base);
    if (!folder.has_filename()) {
        folder = folder.parent_path(); // the path ends in a separator
    }
    for (;; folder = folder.parent_path()) {
        if (holds_pack(folder)) {
            return folder;
        }
        if (folder.parent_path() == folder) {
            return Error{"no block pack (assets/<namespace>/blocks/*.toml) in " +
                         std::string(base) +
                         " or any folder above it: name the pack with --pack=DIR"};
        }
    }
}

} // namespace vx::app
