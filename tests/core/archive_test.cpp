#include "zip_bytes.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using vx::test::put16;
using vx::test::put32;
using vx::test::ZipEntry;

std::string every_byte() {
    std::string bytes;
    for (int value = 0; value < 256; ++value) {
        bytes.push_back(static_cast<char>(value));
    }
    return bytes;
}

const std::vector<ZipEntry> three = {
    {.name = "hello.txt", .data = "hello, archive\n"},
    {.name = "folder/", .data = ""},
    {.name = "folder/bytes.bin", .data = every_byte()},
};

vx::Result<vx::Archive> from(std::vector<std::byte> bytes) {
    return vx::Archive::from_bytes("test.zip", std::move(bytes));
}

std::string why(const vx::Result<vx::Archive>& result) {
    REQUIRE_FALSE(result.has_value());
    return result.error().message;
}

vx::Archive must(vx::Result<vx::Archive> result) {
    REQUIRE(result.has_value());
    return std::move(result).value();
}

std::string text_of(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string read_text(const vx::Archive& archive, std::string_view name) {
    const vx::Result<std::vector<std::byte>> bytes = archive.read(name);
    REQUIRE(bytes.has_value());
    return text_of(bytes.value());
}

std::string why_not(const vx::Archive& archive, std::string_view name) {
    const vx::Result<std::vector<std::byte>> bytes = archive.read(name);
    REQUIRE_FALSE(bytes.has_value());
    return bytes.error().message;
}

// A path as the archive names it in messages.
std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// A file of these bytes in the system's temporary folder, removed when it
// goes out of scope, even after a failed check. Its name has a random part,
// since the folder may be shared.
class TemporaryFile {
public:
    explicit TemporaryFile(const std::vector<std::byte>& bytes)
        : path_(std::filesystem::temp_directory_path() /
                ("vx_archive_test_" + std::to_string(std::random_device{}()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".zip")) {
        std::ofstream file(path_, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

} // namespace

TEST_CASE("an archive lists every entry by its exact name, folders included") {
    const vx::Result<vx::Archive> archive = from(vx::test::make_zip(three).bytes);
    REQUIRE(archive.has_value());
    CHECK(archive.value().size() == 3);
    CHECK(archive.value().contains("hello.txt"));
    CHECK(archive.value().contains("folder/"));
    CHECK(archive.value().contains("folder/bytes.bin"));
    // Names are bytes: no case folding, no prefixes, no added or missing slash.
    CHECK_FALSE(archive.value().contains("HELLO.TXT"));
    CHECK_FALSE(archive.value().contains("hello"));
    CHECK_FALSE(archive.value().contains("folder"));
    CHECK_FALSE(archive.value().contains("hello.txt/"));
    CHECK_FALSE(archive.value().contains(""));
}

TEST_CASE("an archive with no entries is its end record alone") {
    const vx::test::Zip zip = vx::test::make_zip({});
    CHECK(zip.bytes.size() == 22);
    const vx::Result<vx::Archive> archive = from(zip.bytes);
    REQUIRE(archive.has_value());
    CHECK(archive.value().size() == 0);
    CHECK_FALSE(archive.value().contains("hello.txt"));
}

TEST_CASE("the end record is the last one whose comment reaches the end") {
    // A comment holding what looks like an end record: its comment length (0)
    // does not reach the archive's end, so it is not taken for the real one.
    std::string fake(22, '\0');
    fake.replace(0, 4, "PK\x05\x06");
    const vx::Result<vx::Archive> tricked = from(vx::test::make_zip(three, fake + "!").bytes);
    REQUIRE(tricked.has_value());
    CHECK(tricked.value().size() == 3);

    // The longest comment there can be, as far back as the search goes.
    const vx::Result<vx::Archive> longest =
        from(vx::test::make_zip(three, std::string(65'535, 'c')).bytes);
    REQUIRE(longest.has_value());
    CHECK(longest.value().size() == 3);

    // A comment that is itself a whole end record, reaching the end, is the
    // last record: it is taken, as Python's zipfile and unzip take it, and
    // the archive reads as empty.
    const vx::Result<vx::Archive> nested = from(vx::test::make_zip(three, fake).bytes);
    REQUIRE(nested.has_value());
    CHECK(nested.value().size() == 0);
}

TEST_CASE("an archive written by another zip writer, with data descriptors, reads the same") {
    // Python's zipfile wrote it (make_stored.py beside it), each entry's sizes
    // and CRC after its data, as in Minecraft's client jars.
    const vx::Result<vx::Archive> archive =
        vx::Archive::open(std::filesystem::path(VX_ARCHIVES_DIR) / "stored.zip");
    REQUIRE(archive.has_value());
    CHECK(archive.value().size() == 4);
    CHECK(archive.value().contains("hello.txt"));
    CHECK(archive.value().contains("folder/"));
    CHECK(archive.value().contains("folder/bytes.bin"));
    CHECK(archive.value().contains("empty.txt"));
}

TEST_CASE("bytes that are not a zip archive are refused, naming them") {
    const std::string not_zip =
        "test.zip: not a zip archive: it has no end of central directory record";
    CHECK(why(from({})) == not_zip);
    CHECK(why(from(std::vector<std::byte>(100, std::byte{'x'}))) == not_zip);

    // The end record's comment length must reach exactly to the end.
    std::vector<std::byte> cut = vx::test::make_zip(three, "comment").bytes;
    cut.pop_back();
    CHECK(why(from(cut)) == not_zip);
    std::vector<std::byte> longer = vx::test::make_zip(three, "comment").bytes;
    longer.push_back(std::byte{'!'});
    CHECK(why(from(longer)) == not_zip);
}

TEST_CASE("an archive split across disks is refused") {
    for (const std::size_t field : {4U, 6U, 8U}) { // this disk, the directory's, entries on it
        vx::test::Zip zip = vx::test::make_zip(three);
        put16(zip.bytes, zip.end_record + field, 2);
        CHECK(why(from(zip.bytes)) == "test.zip: is split across several disks");
    }
}

TEST_CASE("an archive that needs zip64 records is refused") {
    const std::string needs = "test.zip: needs zip64 records, which are not read";
    vx::test::Zip count = vx::test::make_zip(three);
    put16(count.bytes, count.end_record + 8, 0xFFFF);
    put16(count.bytes, count.end_record + 10, 0xFFFF);
    CHECK(why(from(count.bytes)) == needs);
    vx::test::Zip size = vx::test::make_zip(three);
    put32(size.bytes, size.end_record + 12, 0xFFFF'FFFF);
    CHECK(why(from(size.bytes)) == needs);
    vx::test::Zip offset = vx::test::make_zip(three);
    put32(offset.bytes, offset.end_record + 16, 0xFFFF'FFFF);
    CHECK(why(from(offset.bytes)) == needs);

    // An entry's size or place given as "in a zip64 record".
    for (const std::size_t field : {20U, 24U, 42U}) {
        vx::test::Zip zip = vx::test::make_zip(three);
        put32(zip.bytes, zip.central_headers[1] + field, 0xFFFF'FFFF);
        CHECK(why(from(zip.bytes)) ==
              "test.zip: entry 2 of its central directory needs zip64 records, which are not read");
    }
}

TEST_CASE("extra fields and entry comments are stepped over") {
    std::vector<ZipEntry> entries = three;
    entries[0].central_extra =
        std::string("\xFE\xCA\x00\x00", 4); // the header Java's jar tool writes
    entries[0].comment = "hi!";
    entries[1].local_extra = std::string(8, 'x');
    entries[1].central_extra = std::string(5, 'y');
    const vx::Result<vx::Archive> archive = from(vx::test::make_zip(entries).bytes);
    REQUIRE(archive.has_value());
    CHECK(archive.value().size() == 3);
    for (const ZipEntry& entry : entries) {
        CHECK(archive.value().contains(entry.name));
    }
}

TEST_CASE("a central directory that is damaged or does not add up is refused") {
    const vx::test::Zip good = vx::test::make_zip(three);

    vx::test::Zip past_end = good;
    put32(past_end.bytes, good.end_record + 12, // its size
          static_cast<std::uint32_t>(good.end_record - good.central_headers[0] + 1));
    CHECK(why(from(past_end.bytes)) == "test.zip: its central directory runs past its end record");

    vx::test::Zip damaged = good;
    put32(damaged.bytes, good.central_headers[1], 0x0403'4b50);
    CHECK(why(from(damaged.bytes)) == "test.zip: entry 2 of its central directory is damaged");

    vx::test::Zip long_name = good;
    put16(long_name.bytes, good.central_headers[2] + 28, 100);
    CHECK(why(from(long_name.bytes)) ==
          "test.zip: entry 3 of its central directory runs past the directory's end");

    vx::test::Zip too_many = good;
    put16(too_many.bytes, good.end_record + 8, 4);
    put16(too_many.bytes, good.end_record + 10, 4);
    CHECK(why(from(too_many.bytes)) ==
          "test.zip: entry 4 of its central directory runs past the directory's end");

    vx::test::Zip too_few = good;
    put16(too_few.bytes, good.end_record + 8, 2);
    put16(too_few.bytes, good.end_record + 10, 2);
    CHECK(why(from(too_few.bytes)) ==
          "test.zip: its central directory holds more than its 2 entries");
}

TEST_CASE("an archive in a file reads as one in memory, and what cannot be read is named") {
    std::filesystem::path gone;
    {
        const TemporaryFile file(vx::test::make_zip(three).bytes);
        const vx::Result<vx::Archive> archive = vx::Archive::open(file.path());
        REQUIRE(archive.has_value());
        CHECK(archive.value().size() == 3);
        CHECK(archive.value().contains("folder/bytes.bin"));
        gone = file.path();
    }
    CHECK(why(vx::Archive::open(gone)) == utf8(gone) + ": cannot be opened");
    // A folder opens on some systems, but is not a file to read.
    const std::filesystem::path folder = std::filesystem::temp_directory_path();
    CHECK(why(vx::Archive::open(folder)).starts_with(utf8(folder) + ": cannot be"));
}

TEST_CASE("an archive moved to another keeps its entries") {
    vx::Result<vx::Archive> archive = from(vx::test::make_zip(three).bytes);
    REQUIRE(archive.has_value());
    vx::Archive moved = std::move(archive).value();
    CHECK(moved.size() == 3);
    CHECK(moved.contains("hello.txt"));
}

TEST_CASE("an entry reads back exactly as it was stored, with or without a data descriptor") {
    for (const std::uint16_t flags : {std::uint16_t{0}, std::uint16_t{8}}) {
        std::vector<ZipEntry> entries = three;
        for (ZipEntry& entry : entries) {
            entry.flags = flags;
        }
        const vx::Archive archive = must(from(vx::test::make_zip(entries).bytes));
        for (const ZipEntry& entry : entries) {
            CHECK(read_text(archive, entry.name) == entry.data);
        }
    }
}

TEST_CASE("the archive another zip writer made reads back what it was given") {
    const vx::Archive archive =
        must(vx::Archive::open(std::filesystem::path(VX_ARCHIVES_DIR) / "stored.zip"));
    CHECK(read_text(archive, "hello.txt") == "hello, archive\n");
    CHECK(read_text(archive, "folder/bytes.bin") == every_byte());
    CHECK(read_text(archive, "folder/").empty());
    CHECK(read_text(archive, "empty.txt").empty());
}

TEST_CASE("an entry the archive lacks, or holds twice, is refused by name") {
    const vx::Archive archive = must(from(vx::test::make_zip(three).bytes));
    CHECK(why_not(archive, "HELLO.TXT") == "test.zip has no entry 'HELLO.TXT'");

    const std::vector<ZipEntry> twice = {
        {.name = "a", .data = "1"}, {.name = "twice", .data = "2"}, {.name = "twice", .data = "3"}};
    const vx::Archive doubled = must(from(vx::test::make_zip(twice).bytes));
    CHECK(doubled.contains("twice"));
    CHECK(why_not(doubled, "twice") == "test.zip: 'twice' appears twice");
    CHECK(read_text(doubled, "a") == "1");
}

TEST_CASE("an encrypted, compressed or oversized entry is refused before it is read") {
    const vx::test::Zip good = vx::test::make_zip(three);
    const std::size_t hello = good.central_headers[0];

    vx::test::Zip locked = good;
    put16(locked.bytes, hello + 8, 1); // flags: encrypted
    CHECK(why_not(must(from(locked.bytes)), "hello.txt") == "test.zip: 'hello.txt' is encrypted");

    for (const std::uint16_t method : {std::uint16_t{12}, std::uint16_t{14}, std::uint16_t{93}}) {
        vx::test::Zip other = good; // bzip2, LZMA, Zstandard
        put16(other.bytes, hello + 10, method);
        CHECK(why_not(must(from(other.bytes)), "hello.txt") ==
              "test.zip: 'hello.txt' uses compression method " + std::to_string(method) +
                  ", which is neither stored (0) nor deflate (8)");
    }

    vx::test::Zip huge = good;
    put32(huge.bytes, hello + 20, (16U << 20U) + 1);
    put32(huge.bytes, hello + 24, (16U << 20U) + 1);
    CHECK(why_not(must(from(huge.bytes)), "hello.txt") ==
          "test.zip: 'hello.txt' is larger than 16 MiB");

    // At exactly 16 MiB the size is allowed, and the entry fails further on.
    vx::test::Zip largest = good;
    put32(largest.bytes, hello + 20, 16U << 20U);
    put32(largest.bytes, hello + 24, 16U << 20U);
    CHECK(why_not(must(from(largest.bytes)), "hello.txt") ==
          "test.zip: 'hello.txt' runs past the start of the central directory");

    vx::test::Zip uneven = good;
    put32(uneven.bytes, hello + 20, 16); // compressed: one byte more than stored
    CHECK(why_not(must(from(uneven.bytes)), "hello.txt") ==
          "test.zip: 'hello.txt' is stored, but its compressed and uncompressed sizes differ");
}

TEST_CASE("an entry whose local header is misplaced, damaged or disagrees is refused") {
    const vx::test::Zip good = vx::test::make_zip(three);
    const std::size_t local = good.local_headers[0];
    const std::string disagrees =
        "test.zip: 'hello.txt' has a local header that disagrees with the central directory";

    vx::test::Zip damaged = good;
    put32(damaged.bytes, local, 0x0201'4b50);
    CHECK(why_not(must(from(damaged.bytes)), "hello.txt") ==
          "test.zip: 'hello.txt' has a damaged local header");

    vx::test::Zip method = good;
    put16(method.bytes, local + 8, 8);
    CHECK(why_not(must(from(method.bytes)), "hello.txt") == disagrees);
    vx::test::Zip name_length = good;
    put16(name_length.bytes, local + 26, 8);
    CHECK(why_not(must(from(name_length.bytes)), "hello.txt") == disagrees);
    vx::test::Zip name = good;
    name.bytes[local + 30] = std::byte{'H'};
    CHECK(why_not(must(from(name.bytes)), "hello.txt") == disagrees);

    // The central directory points into itself instead of at a local header.
    vx::test::Zip inside = good;
    put32(inside.bytes, good.central_headers[0] + 42,
          static_cast<std::uint32_t>(good.central_headers[0]));
    CHECK(why_not(must(from(inside.bytes)), "hello.txt") ==
          "test.zip: 'hello.txt' has a local header outside the archive's entries");
}

TEST_CASE("an entry whose data runs into the central directory is refused") {
    const vx::test::Zip good = vx::test::make_zip(three);
    const std::string runs_past =
        "test.zip: 'hello.txt' runs past the start of the central directory";

    vx::test::Zip longer = good;
    put32(longer.bytes, good.central_headers[0] + 20, 1000);
    put32(longer.bytes, good.central_headers[0] + 24, 1000);
    CHECK(why_not(must(from(longer.bytes)), "hello.txt") == runs_past);

    vx::test::Zip extra = good;
    put16(extra.bytes, good.local_headers[0] + 28, 1000); // the local extra field's length
    CHECK(why_not(must(from(extra.bytes)), "hello.txt") == runs_past);
}

TEST_CASE("an entry whose bytes were damaged fails its CRC-32 check") {
    for (const std::uint16_t flags : {std::uint16_t{0}, std::uint16_t{8}}) {
        std::vector<ZipEntry> entries = three;
        entries[2].flags = flags;
        vx::test::Zip zip = vx::test::make_zip(entries);
        zip.bytes[zip.local_headers[2] + 30 + entries[2].name.size() + 100] ^= std::byte{1};
        const vx::Archive archive = must(from(zip.bytes));
        CHECK(why_not(archive, "folder/bytes.bin") ==
              "test.zip: 'folder/bytes.bin' fails its CRC-32 check");
        CHECK(read_text(archive, "hello.txt") == "hello, archive\n");
    }
}

TEST_CASE("threads reading one archive in a file at once each get whole entries") {
    // A file has one position: without the archive's lock, one thread's seek
    // lands between another's seek and read.
    std::vector<ZipEntry> entries;
    for (int index = 0; index < 16; ++index) {
        entries.push_back({.name = "entry" + std::to_string(index),
                           .data = std::string(4096, static_cast<char>('a' + index))});
    }
    const TemporaryFile file(vx::test::make_zip(entries).bytes);
    {
        const vx::Archive archive = must(vx::Archive::open(file.path()));
        std::atomic<int> wrong{0};
        std::vector<std::thread> threads;
        for (int thread = 0; thread < 8; ++thread) {
            threads.emplace_back([&archive, &entries, &wrong] {
                for (int round = 0; round < 100; ++round) {
                    for (const ZipEntry& entry : entries) {
                        const vx::Result<std::vector<std::byte>> bytes = archive.read(entry.name);
                        if (!bytes.has_value() || text_of(bytes.value()) != entry.data) {
                            ++wrong;
                        }
                    }
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        CHECK(wrong == 0);
    }
}

TEST_CASE(
    "an entry's data is found past its local extra field, which may differ from the central") {
    std::vector<ZipEntry> entries = three;
    entries[0].local_extra = std::string(8, 'x');
    entries[0].central_extra = std::string(5, 'y');
    entries[0].comment = "hi!";
    entries[2].local_extra = std::string(28, 'z');
    const vx::Archive archive = must(from(vx::test::make_zip(entries).bytes));
    for (const ZipEntry& entry : entries) {
        CHECK(read_text(archive, entry.name) == entry.data);
    }
}

TEST_CASE("an entry of a file cut short after opening cannot be read, and says which") {
    // A large entry after the first keeps it out of what opening read, which
    // stdio may still hold in its buffer: reading it again goes to the file.
    std::vector<ZipEntry> entries = three;
    entries.insert(entries.begin() + 1, {.name = "padding", .data = std::string(256 * 1024, 'p')});
    const TemporaryFile file(vx::test::make_zip(entries).bytes);
    const vx::Archive archive = must(vx::Archive::open(file.path()));
    const std::string cannot = utf8(file.path()) + ": 'hello.txt' cannot be read";
    // Into the local header, then into the data after it.
    std::filesystem::resize_file(file.path(), 45);
    CHECK(why_not(archive, "hello.txt") == cannot);
    std::filesystem::resize_file(file.path(), 20);
    CHECK(why_not(archive, "hello.txt") == cannot);
}

TEST_CASE("an archive as large as Minecraft's jar reads back every entry") {
    // 26.2's client jar lists 31,709 entries, each with a data descriptor.
    std::vector<ZipEntry> entries;
    for (int index = 0; index < 31'709; ++index) {
        entries.push_back(
            {.name = "assets/minecraft/textures/block/e" + std::to_string(index) + ".png",
             .data = std::to_string(index * 7919),
             .flags = 8});
    }
    const vx::Archive archive = must(from(vx::test::make_zip(entries).bytes));
    CHECK(archive.size() == entries.size());
    int wrong = 0;
    for (const ZipEntry& entry : entries) {
        const vx::Result<std::vector<std::byte>> bytes = archive.read(entry.name);
        if (!bytes.has_value() || text_of(bytes.value()) != entry.data) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
}
