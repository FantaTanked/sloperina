"""List and extract files from the game's .nup dump (layout as documented by Hookshot's nx2/nup.py).

    python nupfs.py ls [pattern]            romfs/exefs paths containing pattern, with sizes
    python nupfs.py get <path> <out file>   extract one file (package-level zstd removed)
"""
import io
import struct
import sys
from pathlib import Path

import os

# The dump: $OOTMENU_NUP, else the first .nup two folders up from tools/.
NUP = os.environ.get("OOTMENU_NUP") or next(Path(__file__).resolve().parents[2].glob("*.nup"), None)
ZSTD = b"\x28\xb5\x2f\xfd"  # zstd frame magic

# Package layout: a 0x80-byte header, the TOC header (0x30 bytes), then the tables (titles, directories, files,
# directory children) and the string table
TOC_AT = 0x80
TOC_HEADER_SIZE = 0x30
TITLE_ENTRY_SIZE = 0x40
DIR_ENTRY_SIZE = 24
FILE_ENTRY_SIZE = 80

PROBE_SIZE = 1 << 16  # how much of a file to read to see whether it is zstd
READ_CHUNK = 8 << 20  # 8 MiB


class Nup:
    def __init__(self, path=NUP):
        self.f = open(path, "rb")
        header = self.f.read(TOC_AT + TOC_HEADER_SIZE)
        assert header[:4] == b"NUP\x01" and header[TOC_AT:TOC_AT + 4] == b"TOC\x01", "not a NUP package"
        n_titles, n_dirs, n_files, strings_offset, strings_size = struct.unpack_from("<IQQQQ", header, TOC_AT + 4)
        titles_at = TOC_AT + TOC_HEADER_SIZE
        dirs_at = titles_at + TITLE_ENTRY_SIZE * n_titles
        files_at = dirs_at + DIR_ENTRY_SIZE * n_dirs
        kids_at = files_at + FILE_ENTRY_SIZE * n_files
        strings_at = TOC_AT + strings_offset
        self.f.seek(0)
        meta = self.f.read(strings_at + strings_size)
        self.s = meta[strings_at:]  # the string table: names are (offset, length) into it
        self.nd = n_dirs
        # title: (?, ?, exefs root dir, romfs root dir, version: highest for the newest)
        # dir: (name offset, name length, first kid, kid count)
        # file: (name offset, name length, size, data offset)
        self.titles = [struct.unpack_from("<QIIII", meta, titles_at + TITLE_ENTRY_SIZE * i)
                       for i in range(n_titles)]
        self.dirs = [struct.unpack_from("<4I", meta, dirs_at + DIR_ENTRY_SIZE * i) for i in range(n_dirs)]
        self.files = [struct.unpack_from("<IIQQ", meta, files_at + FILE_ENTRY_SIZE * i) for i in range(n_files)]
        # a directory's children: indexes below n_dirs are directories, the rest files (index - n_dirs)
        self.kids = struct.unpack_from("<%dI" % ((strings_at - kids_at) // 4), meta, kids_at)

    def name(self, offset, length):
        return self.s[offset:offset + length].decode("utf-8")

    def walk(self, d=None, prefix=""):
        """(path, file record) of every file under directory d; by default the newest title's exefs and romfs."""
        if d is None:
            title = max(self.titles, key=lambda t: t[4])
            yield from self.walk(title[2], "exefs/")
            yield from self.walk(title[3], "romfs/")
            return
        _, _, first, count = self.dirs[d]
        for kid in self.kids[first:first + count]:
            if kid < self.nd:
                yield from self.walk(kid, prefix + self.name(*self.dirs[kid][:2]) + "/")
            else:
                record = self.files[kid - self.nd]
                yield prefix + self.name(record[0], record[1]), record

    def read(self, r):
        """A file's contents, with package-level zstd removed (files stored as is are returned as they are)."""
        _, _, size, offset = r
        self.f.seek(offset)
        head = self.f.read(min(size, PROBE_SIZE) + 18)
        if head[:4] == ZSTD:
            data = self._read_zstd(offset, size)
            if data is not None:
                return data
        self.f.seek(offset)
        return self.f.read(size)

    def _read_zstd(self, offset, size):
        """Decompress a file stored as zstd; size is its decompressed size. None if that doesn't work."""
        import zstandard
        self.f.seek(offset)
        decompressor = zstandard.ZstdDecompressor().decompressobj()
        out = io.BytesIO()
        left = size + (1 << 16)  # stop reading compressed data after this much
        try:
            while left and not decompressor.eof and out.tell() < size:
                chunk = self.f.read(min(left, READ_CHUNK))
                if not chunk:
                    break
                left -= len(chunk)
                out.write(decompressor.decompress(chunk))
            data = out.getvalue()
            if len(data) == size:
                return data
        except zstandard.ZstdError:
            pass
        return None


def main():
    args = sys.argv[1:]
    nup = Nup()
    if args and args[0] == "ls":
        pattern = args[1].lower() if len(args) > 1 else ""
        for path, record in nup.walk():
            if pattern in path.lower():
                print(f"{record[2]:>12}  {path}")
    elif args and args[0] == "get":
        record = next(record for path, record in nup.walk() if path == args[1])
        Path(args[2]).write_bytes(nup.read(record))
        print("wrote", args[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
