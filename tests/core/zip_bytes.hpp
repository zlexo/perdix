#pragma once

#include <vx/core/crc32.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx::test {

// An entry for make_zip: its sizes and CRC are worked out.
struct ZipEntry {
    std::string name;
    std::string data;
    // Bit 3 puts the sizes and the CRC after the data instead of in the local
    // header, as Minecraft's client jars do.
    std::uint16_t flags = 0;
    // Extra fields and a comment, which readers step over. The local
    // header's extra field may differ from the central one, as Info-ZIP's do.
    std::string local_extra{};
    std::string central_extra{};
    std::string comment{};
    // Written deflated (method 8), as deflate_stored gives the data.
    bool deflate = false;
};

// An archive built byte by byte, with where each record starts, so that a
// test can damage one field of it.
struct Zip {
    std::vector<std::byte> bytes;
    std::vector<std::size_t> local_headers;
    std::vector<std::size_t> central_headers;
    std::size_t zip64_end_record = 0; // with Zip64::end_records
    std::size_t zip64_locator = 0;
    std::size_t end_record = 0;
};

// What make_zip moves into zip64 records.
struct Zip64 {
    // A zip64 end record and its locator, with every field of the end record
    // at its largest.
    bool end_records = false;
    // Every entry's sizes and place in a zip64 extra field, its header's
    // fields at their largest.
    bool entries = false;
};

inline void put16(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    bytes.at(at) = static_cast<std::byte>(value & 0xFFU);
    bytes.at(at + 1) = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

inline void put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    put16(bytes, at, value & 0xFFFFU);
    put16(bytes, at + 2, value >> 16U);
}

inline void append16(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 2);
    put16(bytes, bytes.size() - 2, value);
}

inline void append32(std::vector<std::byte>& bytes, std::uint32_t value) {
    bytes.resize(bytes.size() + 4);
    put32(bytes, bytes.size() - 4, value);
}

inline void append64(std::vector<std::byte>& bytes, std::uint64_t value) {
    append32(bytes, static_cast<std::uint32_t>(value & 0xFFFF'FFFFU));
    append32(bytes, static_cast<std::uint32_t>(value >> 32U));
}

inline void append_text(std::vector<std::byte>& bytes, std::string_view text) {
    for (const char c : text) {
        bytes.push_back(static_cast<std::byte>(c));
    }
}

inline std::uint32_t crc_of(std::string_view text) {
    return vx::crc32(std::as_bytes(std::span(text.data(), text.size())));
}

// Raw deflate (RFC 1951) made of stored blocks: each holds up to 65,535
// bytes after a header byte, which marks the last block, and its length
// twice, the second inverted. Every inflater reads it; it only skips the
// compressing.
inline std::string deflate_stored(std::string_view data) {
    std::string out;
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(data.size() - at, 0xFFFF);
        const std::size_t inverted = ~length & 0xFFFFU;
        out.push_back(at + length == data.size() ? '\x01' : '\x00');
        out.push_back(static_cast<char>(length & 0xFFU));
        out.push_back(static_cast<char>(length >> 8U));
        out.push_back(static_cast<char>(inverted & 0xFFU));
        out.push_back(static_cast<char>(inverted >> 8U));
        out.append(data.substr(at, length));
        at += length;
    } while (at < data.size());
    return out;
}

// A zip64 extra field (id 1) holding these 8-byte values.
inline std::string zip64_extra(std::initializer_list<std::uint64_t> values) {
    std::vector<std::byte> bytes;
    append16(bytes, 0x0001);
    append16(bytes, static_cast<std::uint32_t>(values.size() * 8));
    for (const std::uint64_t value : values) {
        append64(bytes, value);
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A zip archive of these entries, laid out as APPNOTE.TXT gives it: each
// local header, name and data (and a data descriptor with flag bit 3), then
// the central directory, then, with zip64, the zip64 end record and its
// locator, then the end record with its comment.
inline Zip make_zip(std::span<const ZipEntry> entries, std::string_view comment = {},
                    Zip64 zip64 = {}) {
    constexpr std::uint32_t in_zip64 = 0xFFFF'FFFF;
    Zip zip;
    std::vector<std::byte>& out = zip.bytes;
    for (const ZipEntry& entry : entries) {
        const bool described = (entry.flags & 8U) != 0;
        const std::string written = entry.deflate ? deflate_stored(entry.data) : entry.data;
        const auto size = static_cast<std::uint32_t>(entry.data.size());
        const auto compressed = static_cast<std::uint32_t>(written.size());
        const std::uint32_t crc = crc_of(entry.data);
        const std::string extra =
            entry.local_extra + (zip64.entries ? zip64_extra({size, compressed}) : std::string());
        zip.local_headers.push_back(out.size());
        append32(out, 0x0403'4b50);
        append16(out, zip64.entries ? 45 : 20); // version needed: 4.5 for zip64
        append16(out, entry.flags);
        append16(out, entry.deflate ? 8 : 0);
        append32(out, 0); // time and date
        append32(out, described ? 0 : crc);
        append32(out, zip64.entries ? in_zip64 : (described ? 0 : compressed));
        append32(out, zip64.entries ? in_zip64 : (described ? 0 : size));
        append16(out, static_cast<std::uint32_t>(entry.name.size()));
        append16(out, static_cast<std::uint32_t>(extra.size()));
        append_text(out, entry.name);
        append_text(out, extra);
        append_text(out, written);
        if (described) {
            append32(out, 0x0807'4b50);
            append32(out, crc);
            append32(out, compressed);
            append32(out, size);
        }
    }
    const std::size_t directory = out.size();
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const ZipEntry& entry = entries[index];
        const auto size = static_cast<std::uint32_t>(entry.data.size());
        const auto compressed = static_cast<std::uint32_t>(
            entry.deflate ? deflate_stored(entry.data).size() : entry.data.size());
        const auto local = static_cast<std::uint32_t>(zip.local_headers[index]);
        const std::string extra =
            entry.central_extra +
            (zip64.entries ? zip64_extra({size, compressed, local}) : std::string());
        zip.central_headers.push_back(out.size());
        append32(out, 0x0201'4b50);
        append16(out, 45); // version made by
        append16(out, zip64.entries ? 45 : 20);
        append16(out, entry.flags);
        append16(out, entry.deflate ? 8 : 0);
        append32(out, 0); // time and date
        append32(out, crc_of(entry.data));
        append32(out, zip64.entries ? in_zip64 : compressed);
        append32(out, zip64.entries ? in_zip64 : size);
        append16(out, static_cast<std::uint32_t>(entry.name.size()));
        append16(out, static_cast<std::uint32_t>(extra.size()));
        append16(out, static_cast<std::uint32_t>(entry.comment.size()));
        append16(out, 0); // disk
        append16(out, 0); // internal attributes
        append32(out, 0); // external attributes
        append32(out, zip64.entries ? in_zip64 : local);
        append_text(out, entry.name);
        append_text(out, extra);
        append_text(out, entry.comment);
    }
    const std::size_t directory_size = out.size() - directory;
    if (zip64.end_records) {
        zip.zip64_end_record = out.size();
        append32(out, 0x0606'4b50);
        append64(out, 44); // the record's size after this field
        append16(out, 45); // version made by
        append16(out, 45); // version needed
        append32(out, 0);  // this disk
        append32(out, 0);  // the directory's disk
        append64(out, entries.size());
        append64(out, entries.size());
        append64(out, directory_size);
        append64(out, directory);
        zip.zip64_locator = out.size();
        append32(out, 0x0706'4b50);
        append32(out, 0); // the zip64 end record's disk
        append64(out, zip.zip64_end_record);
        append32(out, 1); // disks in all
    }
    zip.end_record = out.size();
    const bool large = zip64.end_records;
    append32(out, 0x0605'4b50);
    append16(out, large ? 0xFFFF : 0); // this disk
    append16(out, large ? 0xFFFF : 0); // the directory's disk
    append16(out, large ? 0xFFFF : static_cast<std::uint32_t>(entries.size()));
    append16(out, large ? 0xFFFF : static_cast<std::uint32_t>(entries.size()));
    append32(out, large ? in_zip64 : static_cast<std::uint32_t>(directory_size));
    append32(out, large ? in_zip64 : static_cast<std::uint32_t>(directory));
    append16(out, static_cast<std::uint32_t>(comment.size()));
    append_text(out, comment);
    return zip;
}

} // namespace vx::test
