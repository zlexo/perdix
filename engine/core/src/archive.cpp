#include <vx/core/archive.hpp>

#include <vx/core/crc32.hpp>
#include <vx/core/result.hpp>

#include <stb_image.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iterator>
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
constexpr std::uint32_t local_header_signature = 0x0403'4b50;
constexpr std::uint32_t central_header_signature = 0x0201'4b50;
constexpr std::uint32_t zip64_end_record_signature = 0x0606'4b50;
constexpr std::uint32_t zip64_locator_signature = 0x0706'4b50;
constexpr std::uint32_t end_record_signature = 0x0605'4b50;
constexpr std::size_t local_header_size = 30;
constexpr std::size_t central_header_size = 46;
constexpr std::size_t zip64_end_record_size = 56;
constexpr std::size_t zip64_locator_size = 20;
constexpr std::size_t end_record_size = 22;
constexpr std::uint16_t zip64_extra_id = 0x0001;
constexpr std::size_t longest_comment = 0xFFFF;
constexpr std::uint16_t encrypted = 1U << 0U; // general purpose flag bit 0
constexpr std::uint16_t stored = 0;           // compression methods
constexpr std::uint16_t deflated = 8;
// A larger entry is refused before anything is allocated for it: Minecraft's
// largest block texture is 122 KiB, the largest entry in its jar 4.5 MiB.
constexpr std::uint64_t largest_entry = std::uint64_t{16} << 20U;
// Deflate grows what it cannot compress by 5 bytes a 64 KiB block, so no
// entry under the limit needs more than this, compressed.
constexpr std::uint64_t largest_compressed = largest_entry + (largest_entry >> 4U);
// A field at its largest says that the real value is in a zip64 record
// (APPNOTE.TXT, sections 4.3.14 to 4.3.15 and 4.5.3).
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

std::uint64_t field64(std::span<const std::byte> bytes, std::size_t at) noexcept {
    return static_cast<std::uint64_t>(field32(bytes, at)) |
           (static_cast<std::uint64_t>(field32(bytes, at + 4)) << 32U);
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

// Raw deflate (RFC 1951) into exactly out.size() bytes, through the decoder
// stb_image already inflates every PNG with. False for a stream that is
// damaged, ends early, or holds more or fewer bytes than that. stb's sizes
// are ints; entries are far smaller.
bool inflate(std::span<const std::byte> in, std::span<std::byte> out) noexcept {
    // stb adds a block's length to its buffer pointers before comparing them
    // with the ends, which is undefined for the null pointer an empty vector
    // may hold: an empty buffer is handed over as one byte of length 0.
    std::byte none{};
    const int got = stbi_zlib_decode_noheader_buffer(
        reinterpret_cast<char*>(out.empty() ? &none : out.data()), static_cast<int>(out.size()),
        reinterpret_cast<const char*>(in.empty() ? &none : in.data()), static_cast<int>(in.size()));
    return got >= 0 && static_cast<std::size_t>(got) == out.size();
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

    // Where the central directory is, as the end records give it.
    struct Place {
        std::uint64_t entry_count = 0;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        std::uint64_t end = 0; // what follows it: the zip64 end record, or the end record
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

    // The end record, and the zip64 end record when the end record's fields
    // say their values are there and its locator is found.
    [[nodiscard]] Result<Place> locate_directory();
    [[nodiscard]] Result<Place> read_zip64_end(std::uint64_t locator_offset,
                                               std::span<const std::byte> locator);
    // The entry's sizes and place from its zip64 extra field, for each of its
    // own fields that says the value is there. False when the field is
    // missing or too short.
    [[nodiscard]] static bool read_zip64_extra(std::span<const std::byte> extra, Entry& entry);
    // Why the entry cannot be read, found from the directory alone, before
    // anything is allocated for it; nothing when it can be.
    [[nodiscard]] static std::optional<std::string> refusal(const Entry& entry);
    // Where its data starts, past its local header, which must lie before the
    // directory and agree with it on the method and the name.
    [[nodiscard]] Result<std::uint64_t> data_start(const Entry& entry);
    // Its bytes from there, inflated if need be, checked against its CRC-32.
    [[nodiscard]] Result<std::vector<std::byte>> contents(const Entry& entry, std::uint64_t data);
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

Result<Archive::State::Place> Archive::State::locate_directory() {
    // The end record closes the archive, followed only by a comment of up to
    // 65,535 bytes, so it is looked for backward over that much of the end:
    // the last record whose comment length reaches exactly to the end, as
    // Python's zipfile and unzip take it. A signature inside a comment is
    // passed over unless it, too, reaches the end.
    const std::uint64_t tail_size =
        std::min<std::uint64_t>(size, end_record_size + longest_comment);
    std::vector<std::byte> tail(static_cast<std::size_t>(tail_size));
    if (!read(size - tail_size, tail)) {
        return Error{"cannot be read"};
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
        return Error{"not a zip archive: it has no end of central directory record"};
    }
    const auto record = std::span<const std::byte>(tail).subspan(*end, end_record_size);
    const std::uint64_t end_offset = size - tail.size() + *end;
    // Any field at its largest may mean its value is in the zip64 end record,
    // whose locator then sits right before the end record.
    const bool at_largest =
        field16(record, 4) == in_zip64_16 || field16(record, 6) == in_zip64_16 ||
        field16(record, 8) == in_zip64_16 || field16(record, 10) == in_zip64_16 ||
        field32(record, 12) == in_zip64_32 || field32(record, 16) == in_zip64_32;
    std::array<std::byte, zip64_locator_size> locator{};
    if (at_largest && end_offset >= locator.size() && read(end_offset - locator.size(), locator) &&
        field32(locator, 0) == zip64_locator_signature) {
        return read_zip64_end(end_offset - locator.size(), locator);
    }
    // Without one, the end record's values are taken as they are: Python's
    // zipfile writes exactly 65,535 entries as 0xFFFF there, with no zip64
    // records. The directory still has to add up.
    if (field16(record, 4) != 0 || field16(record, 6) != 0 ||
        field16(record, 8) != field16(record, 10)) {
        return Error{"is split across several disks"};
    }
    return Place{.entry_count = field16(record, 10),
                 .size = field32(record, 12),
                 .offset = field32(record, 16),
                 .end = end_offset};
}

Result<Archive::State::Place> Archive::State::read_zip64_end(std::uint64_t locator_offset,
                                                             std::span<const std::byte> locator) {
    // The locator gives the zip64 end record's place, which must come before
    // it.
    const std::uint64_t record_offset = field64(locator, 8);
    std::array<std::byte, zip64_end_record_size> record{};
    if (record_offset > locator_offset || locator_offset - record_offset < record.size() ||
        !read(record_offset, record) || field32(record, 0) != zip64_end_record_signature) {
        return Error{"has a damaged zip64 end record"};
    }
    if (field32(locator, 4) != 0 || field32(locator, 16) > 1 || field32(record, 16) != 0 ||
        field32(record, 20) != 0 || field64(record, 24) != field64(record, 32)) {
        return Error{"is split across several disks"};
    }
    return Place{.entry_count = field64(record, 32),
                 .size = field64(record, 40),
                 .offset = field64(record, 48),
                 .end = record_offset};
}

bool Archive::State::read_zip64_extra(std::span<const std::byte> extra, Entry& entry) {
    // Each extra field is an id, a length and that many bytes. The zip64
    // one holds 8-byte values in this order, only for the fields at their
    // largest.
    const std::array<std::uint64_t*, 3> wanted = {
        entry.size == in_zip64_32 ? &entry.size : nullptr,
        entry.compressed_size == in_zip64_32 ? &entry.compressed_size : nullptr,
        entry.local_header == in_zip64_32 ? &entry.local_header : nullptr};
    std::size_t at = 0;
    while (extra.size() - at >= 4) {
        const std::size_t length = field16(extra, at + 2);
        if (extra.size() - at - 4 < length) {
            return false;
        }
        if (field16(extra, at) == zip64_extra_id) {
            std::size_t value = at + 4;
            for (std::uint64_t* field : wanted) {
                if (field == nullptr) {
                    continue;
                }
                if (at + 4 + length - value < 8) {
                    return false;
                }
                *field = field64(extra, value);
                value += 8;
            }
            return true;
        }
        at += 4 + length;
    }
    return false;
}

Result<Archive> Archive::read_directory(std::unique_ptr<State> state) {
    const std::string& name = state->name;
    const Result<State::Place> found = state->locate_directory();
    if (!found) {
        return Error{name + ": " + found.error().message};
    }
    const State::Place& place = found.value();
    if (place.offset > place.end || place.size > place.end - place.offset) {
        return Error{name + ": its central directory runs past its end record"};
    }
    state->directory.resize(static_cast<std::size_t>(place.size));
    if (!state->read(place.offset, state->directory)) {
        return Error{name + ": cannot be read"};
    }
    state->directory_offset = place.offset;

    // Each entry's header, then its name, extra field and comment. Only the
    // names are kept as text, pointing into the directory.
    const std::span<const std::byte> directory = state->directory;
    const auto problem = [&name](std::uint64_t index, std::string_view what) {
        return Error{name + ": entry " + std::to_string(index + 1) + " of its central directory " +
                     std::string(what)};
    };
    // In proportion to the bytes there are, not to what the end record says.
    state->entries.reserve(static_cast<std::size_t>(
        std::min<std::uint64_t>(place.entry_count, directory.size() / central_header_size)));
    std::size_t at = 0;
    for (std::uint64_t index = 0; index < place.entry_count; ++index) {
        if (directory.size() - at < central_header_size) {
            return problem(index, "runs past the directory's end");
        }
        if (field32(directory, at) != central_header_signature) {
            return problem(index, "is damaged");
        }
        const std::size_t name_length = field16(directory, at + 28);
        const std::size_t extra_length = field16(directory, at + 30);
        const std::size_t length =
            central_header_size + name_length + extra_length + field16(directory, at + 32);
        if (directory.size() - at < length) {
            return problem(index, "runs past the directory's end");
        }
        State::Entry entry{
            .name = std::string_view(
                reinterpret_cast<const char*>(directory.data() + at + central_header_size),
                name_length),
            .local_header = field32(directory, at + 42),
            .compressed_size = field32(directory, at + 20),
            .size = field32(directory, at + 24),
            .crc32 = field32(directory, at + 16),
            .method = field16(directory, at + 10),
            .flags = field16(directory, at + 8)};
        if ((entry.compressed_size == in_zip64_32 || entry.size == in_zip64_32 ||
             entry.local_header == in_zip64_32) &&
            !State::read_zip64_extra(
                directory.subspan(at + central_header_size + name_length, extra_length), entry)) {
            return problem(index, "has no zip64 extra field for its sizes and place");
        }
        state->entries.push_back(entry);
        at += length;
    }
    if (at != directory.size()) {
        return Error{name + ": its central directory holds more than its " +
                     std::to_string(place.entry_count) + " entries"};
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

std::vector<std::string_view> Archive::names() const {
    std::vector<std::string_view> names;
    names.reserve(state_->entries.size());
    for (const State::Entry& entry : state_->entries) {
        names.push_back(entry.name);
    }
    return names;
}

std::optional<std::string> Archive::State::refusal(const Entry& entry) {
    if ((entry.flags & encrypted) != 0) {
        return "is encrypted";
    }
    if (entry.method != stored && entry.method != deflated) {
        return "uses compression method " + std::to_string(entry.method) +
               ", which is neither stored (0) nor deflate (8)";
    }
    if (entry.size > largest_entry) {
        return "is larger than 16 MiB";
    }
    if (entry.compressed_size > largest_compressed) {
        return "is larger than 17 MiB compressed";
    }
    if (entry.method == stored && entry.compressed_size != entry.size) {
        return "is stored, but its compressed and uncompressed sizes differ";
    }
    return std::nullopt;
}

Result<std::uint64_t> Archive::State::data_start(const Entry& entry) {
    // Its CRC and sizes are not compared: with a data descriptor after the
    // data, as in Minecraft's jars, they are zeros.
    std::vector<std::byte> local(local_header_size + entry.name.size());
    if (entry.local_header > directory_offset ||
        directory_offset - entry.local_header < local.size()) {
        return Error{"has a local header outside the archive's entries"};
    }
    if (!read(entry.local_header, local)) {
        return Error{"cannot be read"};
    }
    if (field32(local, 0) != local_header_signature) {
        return Error{"has a damaged local header"};
    }
    const auto local_name = std::span<const std::byte>(local).subspan(local_header_size);
    if (field16(local, 8) != entry.method || field16(local, 26) != entry.name.size() ||
        !std::ranges::equal(local_name, std::as_bytes(std::span(entry.name)))) {
        return Error{"has a local header that disagrees with the central directory"};
    }
    const std::uint64_t data = entry.local_header + local.size() + field16(local, 28);
    if (data > directory_offset || directory_offset - data < entry.compressed_size) {
        return Error{"runs past the start of the central directory"};
    }
    return data;
}

Result<std::vector<std::byte>> Archive::State::contents(const Entry& entry, std::uint64_t data) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(entry.size));
    if (entry.method == stored) {
        if (!read(data, bytes)) {
            return Error{"cannot be read"};
        }
    } else {
        std::vector<std::byte> compressed(static_cast<std::size_t>(entry.compressed_size));
        if (!read(data, compressed)) {
            return Error{"cannot be read"};
        }
        if (!inflate(compressed, bytes)) {
            return Error{"does not inflate to its " + std::to_string(entry.size) + " bytes"};
        }
    }
    if (crc32(bytes) != entry.crc32) {
        return Error{"fails its CRC-32 check"};
    }
    return bytes;
}

Result<std::vector<std::byte>> Archive::read(std::string_view name) const {
    State& state = *state_;
    const auto problem = [&state, name](std::string_view what) {
        return Error{state.name + ": '" + std::string(name) + "' " + std::string(what)};
    };
    const auto found = std::ranges::lower_bound(state.entries, name, {}, &State::Entry::name);
    if (found == state.entries.end() || found->name != name) {
        return Error{state.name + " has no entry '" + std::string(name) + "'"};
    }
    // Zip readers disagree on which of two entries of one name is meant.
    if (std::next(found) != state.entries.end() && std::next(found)->name == name) {
        return problem("appears twice");
    }
    const State::Entry& entry = *found;
    if (const std::optional<std::string> why = State::refusal(entry)) {
        return problem(*why);
    }
    const Result<std::uint64_t> data = state.data_start(entry);
    if (!data) {
        return problem(data.error().message);
    }
    Result<std::vector<std::byte>> bytes = state.contents(entry, data.value());
    if (!bytes) {
        return problem(bytes.error().message);
    }
    return bytes;
}

} // namespace vx
