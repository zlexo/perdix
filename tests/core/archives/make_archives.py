#!/usr/bin/env python3
"""Makes the archives the archive tests read beside the ones they build
themselves, written by Python's zipfile, a zip writer apart from ours, into a
stream it cannot seek, so that every entry's sizes and CRC follow its data in
a data descriptor (flag bit 3), as in Minecraft's client jars.

  stored.zip    entries stored as they are
  deflated.zip  entries deflated by zlib: dynamic Huffman blocks for text and
                a repeating pattern, a stored block for noise, fixed Huffman
                for one byte and for nothing

The tests make the same contents with the same formulas.

    python3 tests/core/archives/make_archives.py
"""
import io
import pathlib
import struct
import sys
import zipfile


class Unseekable(io.RawIOBase):
    def __init__(self):
        self.data = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.data += b
        return len(b)


def text():
    return b"Minecraft's textures, read from the player's own install. " * 40


def pattern():
    return bytes((i * 7 + i // 256) & 0xFF for i in range(65536))


def noise():
    # xorshift32 from 2463534242, the low byte of each step.
    x, out = 2463534242, bytearray()
    for _ in range(4096):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(x & 0xFF)
    return bytes(out)


def first_blocks(name):
    """Each entry's first deflate block type: 0 stored, 1 fixed, 2 dynamic."""
    raw = pathlib.Path(__file__).with_name(name).read_bytes()
    kinds = {}
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        for info in archive.infolist():
            at = info.header_offset
            name_length, extra_length = struct.unpack("<HH", raw[at + 26:at + 30])
            kinds[info.filename] = (raw[at + 30 + name_length + extra_length] >> 1) & 3
    return kinds


def write(name, comment, method, entries):
    out = Unseekable()
    with zipfile.ZipFile(out, "w", compression=method) as archive:
        archive.comment = comment
        for entry_name, data in entries:
            info = zipfile.ZipInfo(entry_name, date_time=(2026, 10, 7, 0, 0, 0))
            info.compress_type = method
            with archive.open(info, "w") as entry:
                entry.write(data)
    pathlib.Path(__file__).with_name(name).write_bytes(bytes(out.data))


# stored.zip keeps the comment it was first made with, under this script's
# old name, so that its bytes stay the same.
write("stored.zip", b"made by make_stored.py", zipfile.ZIP_STORED, [
    ("hello.txt", b"hello, archive\n"),
    ("folder/", b""),
    ("folder/bytes.bin", bytes(range(256))),
    ("empty.txt", b""),
])
write("deflated.zip", b"made by make_archives.py", zipfile.ZIP_DEFLATED, [
    ("text.txt", text()),
    ("pattern.bin", pattern()),
    ("noise.bin", noise()),
    ("tiny.txt", b"a"),
    ("empty.txt", b""),
])
# Another zlib could choose other blocks, and the tests would quietly stop
# covering one kind.
blocks = {"text.txt": 2, "pattern.bin": 2, "noise.bin": 0, "tiny.txt": 1, "empty.txt": 1}
if first_blocks("deflated.zip") != blocks:
    sys.exit(f"deflated.zip: first blocks {first_blocks('deflated.zip')}, not {blocks}")
