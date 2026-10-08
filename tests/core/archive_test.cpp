#include "zip_bytes.hpp"

#include <vx/core/archive.hpp>
#include <vx/core/result.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <initializer_list>
#include <string>
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

TEST_CASE("an archive moved to another keeps its entries") {
    vx::Result<vx::Archive> archive = from(vx::test::make_zip(three).bytes);
    REQUIRE(archive.has_value());
    vx::Archive moved = std::move(archive).value();
    CHECK(moved.size() == 3);
    CHECK(moved.contains("hello.txt"));
}
