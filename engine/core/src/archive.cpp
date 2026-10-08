#include <vx/core/archive.hpp>

#include <vx/core/result.hpp>

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx {
namespace {

// Record signatures and sizes, from the zip format's specification
// (PKWARE's APPNOTE.TXT, sections 4.3.12 and 4.3.16).
constexpr std::uint32_t central_header_signature = 0x0201'4b50;
constexpr std::uint32_t end_record_signature = 0x0605'4b50;
constexpr std::size_t central_header_size = 46;
constexpr std::size_t end_record_size = 22;
constexpr std::size_t longest_comment = 0xFFFF;
// A field at its largest says that the real value is in a zip64 record.
constexpr std::uint16_t in_zip64_16 = 0xFFFF;
constexpr std::uint32_t in_zip64_32 = 0xFFFF'FFFF;

// Zip stores every number little-endian.
std::uint16_t field16(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[at]) |
                                      (std::to_integer<unsigned>(bytes[at + 1]) << 8U));
}

std::uint32_t field32(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(field16(bytes, at)) |
           (static_cast<std::uint32_t>(field16(bytes, at + 2)) << 16U);
}

bool seek(std::FILE* file, std::uint64_t offset) noexcept {
#ifdef _WIN32
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

// The size of the open file itself, not of whatever its path names by now,
// or nothing when it is not a regular file, such as a folder, which opens
// on some systems.
std::optional<std::uint64_t> regular_file_size(std::FILE* file) noexcept {
#ifdef _WIN32
    struct _stat64 status{};
    if (_fstat64(_fileno(file), &status) != 0 || (status.st_mode & _S_IFMT) != _S_IFREG) {
        return std::nullopt;
    }
#else
    struct stat status{};
    if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode)) {
        return std::nullopt;
    }
#endif
    return static_cast<std::uint64_t>(status.st_size);
}

// The path in UTF-8, for messages. On Windows, path::string() converts
// through the system's code page and throws on a character it cannot map,
// which engine code, built without exceptions, cannot catch.
std::string utf8_name(const std::filesystem::path& path) {
    const std::u8string name = path.u8string();
    return {reinterpret_cast<const char*>(name.data()), name.size()};
}

} // namespace

struct Archive::State {
    struct Close {
        void operator()(std::FILE* stream) const noexcept { std::fclose(stream); }
    };

    // An entry as the central directory gives it.
    struct Entry {
        std::string_view name; // into `directory`
        std::uint64_t local_header = 0;
        std::uint64_t compressed_size = 0;
        std::uint64_t size = 0;
        std::uint32_t crc32 = 0;
        std::uint16_t method = 0;
        std::uint16_t flags = 0;
    };

    std::string name;                       // the path, or the name given, for messages
    std::unique_ptr<std::FILE, Close> file; // the archive in a file; without one,
    std::vector<std::byte> memory;          // the archive in memory
    std::uint64_t size = 0;                 // of the whole archive
    std::mutex reading;                     // a file has one position to read from
    std::vector<std::byte> directory;       // the central directory, which names point into
    std::uint64_t directory_offset = 0;     // where entries' data has to end
    std::vector<Entry> entries;             // by name, in byte order

    // Exactly out.size() bytes from `offset`, or false.
    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::byte> out) {
        if (offset > size || out.size() > size - offset) {
            return false;
        }
        if (file == nullptr) {
            std::copy_n(memory.begin() + static_cast<std::ptrdiff_t>(offset), out.size(),
                        out.begin());
            return true;
        }
        const std::scoped_lock lock(reading);
        return seek(file.get(), offset) &&
               std::fread(out.data(), 1, out.size(), file.get()) == out.size();
    }
};

Result<Archive> Archive::open(const std::filesystem::path& path) {
    auto state = std::make_unique<State>();
    state->name = utf8_name(path);
#ifdef _WIN32
    // _wfopen_s, since the C runtime marks _wfopen deprecated. Others may
    // read the file while it is open, not write it.
    std::FILE* opened = nullptr;
    if (_wfopen_s(&opened, path.c_str(), L"rb") != 0) {
        opened = nullptr;
    }
    state->file.reset(opened);
#else
    state->file.reset(std::fopen(path.c_str(), "rb"));
#endif
    if (state->file == nullptr) {
        return Error{state->name + ": cannot be opened"};
    }
    const std::optional<std::uint64_t> size = regular_file_size(state->file.get());
    if (!size) {
        return Error{state->name + ": cannot be read"};
    }
    state->size = *size;
    return read_directory(std::move(state));
}

Result<Archive> Archive::from_bytes(std::string name, std::vector<std::byte> bytes) {
    auto state = std::make_unique<State>();
    state->name = std::move(name);
    state->size = bytes.size();
    state->memory = std::move(bytes);
    return read_directory(std::move(state));
}

Result<Archive> Archive::read_directory(std::unique_ptr<State> state) {
    const std::string& name = state->name;

    // The end record closes the archive, followed only by a comment of up to
    // 65,535 bytes, so it is looked for backward over that much of the end:
    // the last record whose comment length reaches exactly to the end, as
    // Python's zipfile and unzip take it. A signature inside a comment is
    // passed over unless it, too, reaches the end.
    const std::uint64_t tail_size =
        std::min<std::uint64_t>(state->size, end_record_size + longest_comment);
    std::vector<std::byte> tail(static_cast<std::size_t>(tail_size));
    if (!state->read(state->size - tail_size, tail)) {
        return Error{name + ": cannot be read"};
    }
    std::optional<std::size_t> end;
    for (std::size_t back = 0; back + end_record_size <= tail.size(); ++back) {
        const std::size_t at = tail.size() - end_record_size - back;
        if (field32(tail, at) == end_record_signature &&
            at + end_record_size + field16(tail, at + 20) == tail.size()) {
            end = at;
            break;
        }
    }
    if (!end) {
        return Error{name + ": not a zip archive: it has no end of central directory record"};
    }

    const auto record = std::span<const std::byte>(tail).subspan(*end, end_record_size);
    const std::uint16_t entry_count = field16(record, 10);
    const std::uint32_t directory_size = field32(record, 12);
    const std::uint32_t directory_offset = field32(record, 16);
    if (entry_count == in_zip64_16 || directory_size == in_zip64_32 ||
        directory_offset == in_zip64_32) {
        return Error{name + ": needs zip64 records, which are not read"};
    }
    if (field16(record, 4) != 0 || field16(record, 6) != 0 || field16(record, 8) != entry_count) {
        return Error{name + ": is split across several disks"};
    }
    const std::uint64_t end_offset = state->size - tail.size() + *end;
    if (std::uint64_t{directory_offset} + directory_size > end_offset) {
        return Error{name + ": its central directory runs past its end record"};
    }
    state->directory.resize(directory_size);
    if (!state->read(directory_offset, state->directory)) {
        return Error{name + ": cannot be read"};
    }
    state->directory_offset = directory_offset;

    // Each entry's header, then its name, extra field and comment. Only the
    // names are kept as text, pointing into the directory.
    const std::span<const std::byte> directory = state->directory;
    const auto problem = [&name](std::size_t index, std::string_view what) {
        return Error{name + ": entry " + std::to_string(index + 1) + " of its central directory " +
                     std::string(what)};
    };
    // In proportion to the bytes there are, not to what the end record says.
    state->entries.reserve(
        std::min<std::size_t>(entry_count, directory.size() / central_header_size));
    std::size_t at = 0;
    for (std::size_t index = 0; index < entry_count; ++index) {
        if (directory.size() - at < central_header_size) {
            return problem(index, "runs past the directory's end");
        }
        if (field32(directory, at) != central_header_signature) {
            return problem(index, "is damaged");
        }
        const std::size_t name_length = field16(directory, at + 28);
        const std::size_t length = central_header_size + name_length + field16(directory, at + 30) +
                                   field16(directory, at + 32);
        if (directory.size() - at < length) {
            return problem(index, "runs past the directory's end");
        }
        const State::Entry entry{
            .name = std::string_view(
                reinterpret_cast<const char*>(directory.data() + at + central_header_size),
                name_length),
            .local_header = field32(directory, at + 42),
            .compressed_size = field32(directory, at + 20),
            .size = field32(directory, at + 24),
            .crc32 = field32(directory, at + 16),
            .method = field16(directory, at + 10),
            .flags = field16(directory, at + 8)};
        if (entry.compressed_size == in_zip64_32 || entry.size == in_zip64_32 ||
            entry.local_header == in_zip64_32) {
            return problem(index, "needs zip64 records, which are not read");
        }
        state->entries.push_back(entry);
        at += length;
    }
    if (at != directory.size()) {
        return Error{name + ": its central directory holds more than its " +
                     std::to_string(entry_count) + " entries"};
    }
    std::ranges::sort(state->entries, {}, &State::Entry::name);
    return Archive(std::move(state));
}

Archive::Archive(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Archive::Archive(Archive&&) noexcept = default;
Archive& Archive::operator=(Archive&&) noexcept = default;
Archive::~Archive() = default;

std::size_t Archive::size() const noexcept {
    return state_->entries.size();
}

bool Archive::contains(std::string_view name) const noexcept {
    return std::ranges::binary_search(state_->entries, name, {}, &State::Entry::name);
}

} // namespace vx
