#!/usr/bin/env python3
"""Makes stored.zip, which the archive tests read beside the archives they
build themselves: written by Python's zipfile, a zip writer apart from ours,
into a stream it cannot seek, so that every entry's sizes and CRC follow its
data in a data descriptor (flag bit 3), as in Minecraft's client jars.

    python3 tests/core/archives/make_stored.py
"""
import io
import pathlib
import zipfile


class Unseekable(io.RawIOBase):
    def __init__(self):
        self.data = bytearray()

    def writable(self):
        return True

    def write(self, b):
        self.data += b
        return len(b)


ENTRIES = [
    ("hello.txt", b"hello, archive\n"),
    ("folder/", b""),
    ("folder/bytes.bin", bytes(range(256))),
    ("empty.txt", b""),
]

out = Unseekable()
with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_STORED) as archive:
    archive.comment = b"made by make_stored.py"
    for name, data in ENTRIES:
        info = zipfile.ZipInfo(name, date_time=(2026, 10, 7, 0, 0, 0))
        info.compress_type = zipfile.ZIP_STORED
        with archive.open(info, "w") as entry:
            entry.write(data)
pathlib.Path(__file__).with_name("stored.zip").write_bytes(bytes(out.data))
