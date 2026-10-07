#!/usr/bin/env python3
"""Makes the archives the archive tests read beside the ones they build
themselves, written by Python's zipfile, a zip writer apart from ours, into a
stream it cannot seek, so that every entry's sizes and CRC follow its data in
a data descriptor (flag bit 3), as in Minecraft's client jars.

  stored.zip    entries stored as they are
  deflated.zip  entries deflated by zlib: dynamic Huffman blocks for text and
                a repeating pattern, a stored block for noise, fixed Huffman
                for one byte and for nothing
  zip64_python.zip   zipfile with its zip64 limits at zero: every size and
                place in zip64 extra fields, and zip64 end records, the end
                record keeping its own values
  zip64_infozip.zip  Info-ZIP's zip -fz (a writer apart from both): zip64 end
                records with the end record's directory offset at its largest,
                and each entry's uncompressed size in a zip64 extra field

The tests make the same contents with the same formulas.

    python3 tests/core/archives/make_archives.py
"""
import io
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
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


def write(name, comment, method, entries, force_zip64=False):
    out = Unseekable()
    with zipfile.ZipFile(out, "w", compression=method) as archive:
        archive.comment = comment
        for entry_name, data in entries:
            info = zipfile.ZipInfo(entry_name, date_time=(2026, 10, 7, 0, 0, 0))
            info.compress_type = method
            with archive.open(info, "w", force_zip64=force_zip64) as entry:
                entry.write(data)
    pathlib.Path(__file__).with_name(name).write_bytes(bytes(out.data))


ZIP64_ENTRIES = [("a.txt", b"zip64 " * 50), ("b.bin", bytes(range(256)))]


def write_zip64_python():
    saved = zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT
    zipfile.ZIP64_LIMIT = zipfile.ZIP_FILECOUNT_LIMIT = 0
    try:
        write("zip64_python.zip", b"made by make_archives.py", zipfile.ZIP_DEFLATED,
              ZIP64_ENTRIES, force_zip64=True)
    finally:
        zipfile.ZIP64_LIMIT, zipfile.ZIP_FILECOUNT_LIMIT = saved


def write_zip64_infozip():
    tool = shutil.which("zip")
    if tool is None:
        print("zip64_infozip.zip: no zip program here, so it is left as it is")
        return
    with tempfile.TemporaryDirectory() as folder:
        for entry_name, data in ZIP64_ENTRIES:
            path = pathlib.Path(folder) / entry_name
            path.write_bytes(data)
            path.chmod(0o644)
            os.utime(path, (1791331200, 1791331200))  # 2026-10-07, UTC
        # -X leaves out the file attributes the system would add.
        subprocess.run([tool, "-q", "-X", "-fz", "out.zip", *(n for n, _ in ZIP64_ENTRIES)],
                       cwd=folder, env={**os.environ, "TZ": "UTC"}, check=True)
        data = (pathlib.Path(folder) / "out.zip").read_bytes()
    pathlib.Path(__file__).with_name("zip64_infozip.zip").write_bytes(data)


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
write_zip64_python()
write_zip64_infozip()
