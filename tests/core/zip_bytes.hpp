#pragma once

#include <vx/core/crc32.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
    std::size_t end_record = 0;
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

// A zip archive of these entries, laid out as APPNOTE.TXT gives it: each
// local header, name and data (and a data descriptor with flag bit 3), then
// the central directory, then the end record with its comment.
inline Zip make_zip(std::span<const ZipEntry> entries, std::string_view comment = {}) {
    Zip zip;
    std::vector<std::byte>& out = zip.bytes;
    for (const ZipEntry& entry : entries) {
        const bool described = (entry.flags & 8U) != 0;
        const std::string written = entry.deflate ? deflate_stored(entry.data) : entry.data;
        const auto size = static_cast<std::uint32_t>(entry.data.size());
        const auto compressed = static_cast<std::uint32_t>(written.size());
        const std::uint32_t crc = crc_of(entry.data);
        zip.local_headers.push_back(out.size());
        append32(out, 0x0403'4b50);
        append16(out, 20);          // version needed: 2.0
        append16(out, entry.flags); // flags
        append16(out, entry.deflate ? 8 : 0);
        append32(out, 0); // time and date
        append32(out, described ? 0 : crc);
        append32(out, described ? 0 : compressed);
        append32(out, described ? 0 : size);
        append16(out, static_cast<std::uint32_t>(entry.name.size()));
        append16(out, static_cast<std::uint32_t>(entry.local_extra.size()));
        append_text(out, entry.name);
        append_text(out, entry.local_extra);
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
        zip.central_headers.push_back(out.size());
        append32(out, 0x0201'4b50);
        append16(out, 20); // version made by
        append16(out, 20); // version needed
        append16(out, entry.flags);
        append16(out, entry.deflate ? 8 : 0);
        append32(out, 0); // time and date
        append32(out, crc_of(entry.data));
        append32(out, compressed);
        append32(out, size);
        append16(out, static_cast<std::uint32_t>(entry.name.size()));
        append16(out, static_cast<std::uint32_t>(entry.central_extra.size()));
        append16(out, static_cast<std::uint32_t>(entry.comment.size()));
        append16(out, 0); // disk
        append16(out, 0); // internal attributes
        append32(out, 0); // external attributes
        append32(out, static_cast<std::uint32_t>(zip.local_headers[index]));
        append_text(out, entry.name);
        append_text(out, entry.central_extra);
        append_text(out, entry.comment);
    }
    zip.end_record = out.size();
    append32(out, 0x0605'4b50);
    append16(out, 0); // this disk
    append16(out, 0); // the directory's disk
    append16(out, static_cast<std::uint32_t>(entries.size()));
    append16(out, static_cast<std::uint32_t>(entries.size()));
    append32(out, static_cast<std::uint32_t>(zip.end_record - directory));
    append32(out, static_cast<std::uint32_t>(directory));
    append16(out, static_cast<std::uint32_t>(comment.size()));
    append_text(out, comment);
    return zip;
}

} // namespace vx::test
