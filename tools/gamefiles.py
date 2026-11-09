"""Read romfs files from the dump: zstd with the game's dictionaries, SARC archives, BYML.

    python gamefiles.py ls <romfs path>      list a SARC archive (e.g. romfs/Pack/Actor/X.pack.zs)
    python gamefiles.py byml <romfs path> [member]   print a BYML file (or a member of a SARC) as JSON

Set OOTMENU_NUP to the dump's path (see nupfs.py). Needs zstandard.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import byml  # noqa: E402
import nupfs  # noqa: E402

# SARC layout: header, then the SFAT (file table: 0x10-byte entries), then the SFNT (name table)
SARC_HEADER_SIZE_FIELD = 0x04  # u16: the header's size, so also where the SFAT starts
SARC_DATA_OFFSET_FIELD = 0x0C  # u32: where the file data starts
SFAT_HEADER_SIZE = 0x0C
SFAT_ENTRY_SIZE = 0x10
SFNT_HEADER_SIZE = 8
SFAT_HAS_NAME = 0x01000000  # in an entry's attributes; the low 16 bits are then the name's offset / 4

MAX_DICTS_SIZE = 1 << 26  # 64 MiB: generous limits for zstd frames that don't record their size
MAX_FILE_SIZE = 1 << 28  # 256 MiB

_nup = None
_dicts = None


def nup():
    """The dump (opened once), with .index: {romfs/exefs path: file record}."""
    global _nup
    if _nup is None:
        _nup = nupfs.Nup(nupfs.NUP)
        _nup.index = {path: record for path, record in _nup.walk()}
    return _nup


def sarc(data):
    """{name: bytes} from a SARC archive."""
    assert data[:4] == b"SARC", "not SARC"
    endian = "<" if data[6:8] == b"\xff\xfe" else ">"
    data_at = struct.unpack_from(endian + "I", data, SARC_DATA_OFFSET_FIELD)[0]
    sfat = struct.unpack_from(endian + "H", data, SARC_HEADER_SIZE_FIELD)[0]
    assert data[sfat:sfat + 4] == b"SFAT"
    count = struct.unpack_from(endian + "H", data, sfat + 6)[0]
    entries_at = sfat + SFAT_HEADER_SIZE
    names_at = entries_at + count * SFAT_ENTRY_SIZE + SFNT_HEADER_SIZE
    files = {}
    for i in range(count):
        _, attributes, start, end = struct.unpack_from(endian + "IIII", data, entries_at + i * SFAT_ENTRY_SIZE)
        name = ""
        if attributes & SFAT_HAS_NAME:
            name_at = names_at + (attributes & 0xFFFF) * 4
            name = data[name_at:data.index(b"\0", name_at)].decode("utf-8")
        files[name] = data[data_at + start:data_at + end]
    return files


def dicts():
    """The game's zstd dictionaries (romfs/Pack/ZsDic.pack.zs), by name."""
    global _dicts
    if _dicts is None:
        import zstandard
        raw = nup().read(nup().index["romfs/Pack/ZsDic.pack.zs"])
        if raw[:4] == nupfs.ZSTD:
            raw = zstandard.ZstdDecompressor().decompress(raw, max_output_size=MAX_DICTS_SIZE)
        _dicts = {name: zstandard.ZstdCompressionDict(data) for name, data in sarc(raw).items()}
    return _dicts


def unzs(path, data):
    """Decompress a .zs file with the dictionary its extension uses."""
    import zstandard
    name = path.rsplit("/", 1)[-1]
    all_dicts = dicts()
    if name.endswith(".bcett.byml.zs"):
        dictionary = all_dicts.get("bcett.byml.zsdic")
    elif name.endswith(".pack.zs"):
        dictionary = all_dicts.get("pack.zsdic")
    else:
        dictionary = all_dicts.get("zs.zsdic")
    if dictionary:
        decompressor = zstandard.ZstdDecompressor(dict_data=dictionary)
    else:
        decompressor = zstandard.ZstdDecompressor()
    return decompressor.decompress(data, max_output_size=MAX_FILE_SIZE)


def read(path):
    """A romfs file's contents, decompressed."""
    data = nup().read(nup().index[path])
    return unzs(path, data) if path.endswith(".zs") else data


def main():
    args = sys.argv[1:]
    if len(args) >= 2 and args[0] == "ls":
        for name, data in sarc(read(args[1])).items():
            print(f"{len(data):>10}  {name}")
    elif len(args) >= 2 and args[0] == "byml":
        data = read(args[1])
        if len(args) > 2:
            data = sarc(data)[args[2]]
        print(json.dumps(byml.load(data), indent=1, default=str))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
