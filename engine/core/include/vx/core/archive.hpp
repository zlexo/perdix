#pragma once

#include <vx/core/result.hpp>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vx {

// A zip archive read in place (RFC 0009), such as the client jar of the
// player's own Minecraft: opening reads its end record and its central
// directory, and nothing else. Nothing is ever extracted, so an entry's name
// is only compared with the names asked for, never used as a path. Every
// problem is an Error that names the archive.
class Archive {
public:
    // The archive in a file, which stays open for reading entries.
    [[nodiscard]] static Result<Archive> open(const std::filesystem::path& path);
    // An archive already in memory, such as one a browser hands over. The
    // name only labels messages.
    [[nodiscard]] static Result<Archive> from_bytes(std::string name, std::vector<std::byte> bytes);

    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    // A moved-from archive may only be destroyed or assigned to.
    Archive(Archive&&) noexcept;
    Archive& operator=(Archive&&) noexcept;
    ~Archive();

    // The entries its central directory lists, folders included.
    [[nodiscard]] std::size_t size() const noexcept;
    // Whether it lists an entry of exactly this name: zip names are bytes, so
    // case counts.
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    // The entry's bytes, checked against its local header and its CRC-32, or
    // why not. Entries are read stored; deflated ones come with RFC 0009's
    // item 3. Several threads may read one archive at once.
    [[nodiscard]] Result<std::vector<std::byte>> read(std::string_view name) const;

private:
    struct State;

    explicit Archive(std::unique_ptr<State> state) noexcept;
    [[nodiscard]] static Result<Archive> read_directory(std::unique_ptr<State> state);

    std::unique_ptr<State> state_;
};

} // namespace vx
