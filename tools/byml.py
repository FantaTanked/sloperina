"""Minimal BYML (v2-v7, either byte order) reader: byml.load(bytes) -> Python objects."""
import struct

# node types
STRING = 0xA0
BINARY = 0xA1
ARRAY = 0xC0
DICT = 0xC1
STRING_TABLE = 0xC2
HASH_MAP32 = 0x20  # u32 hash -> value
HASH_MAP64 = 0x21  # u64 hash -> value
BOOL = 0xD0
INT = 0xD1
FLOAT = 0xD2
UINT = 0xD3
INT64 = 0xD4
UINT64 = 0xD5
DOUBLE = 0xD6
NULL = 0xFF

CONTAINERS = (ARRAY, DICT, HASH_MAP32, HASH_MAP64)
WIDE_FORMATS = {INT64: "q", UINT64: "Q", DOUBLE: "d"}  # 64-bit values: stored at an offset


class Byml:
    def __init__(self, data):
        assert data[:2] in (b"BY", b"YB"), "not BYML"
        self.endian = ">" if data[:2] == b"BY" else "<"
        self.byteorder = "big" if self.endian == ">" else "little"
        self.data = data
        self.version, keys_at, strings_at, root_at = struct.unpack_from(self.endian + "HIII", data, 2)
        self.keys = self._string_table(keys_at) if keys_at else []
        self.strings = self._string_table(strings_at) if strings_at else []
        self.root = self.node(data[root_at], root_at) if root_at else None

    def _u24(self, at):
        return int.from_bytes(self.data[at:at + 3], self.byteorder)

    def _u32(self, at):
        return struct.unpack_from(self.endian + "I", self.data, at)[0]

    def _string_table(self, at):
        """The strings of a string table node: a count, then count + 1 offsets from the node's start."""
        data = self.data
        assert data[at] == STRING_TABLE
        count = self._u24(at + 1)
        offsets = struct.unpack_from(self.endian + "%dI" % (count + 1), data, at + 4)
        strings = []
        for i in range(count):
            start = at + offsets[i]
            strings.append(data[start:data.index(b"\0", start)].decode("utf-8", "replace"))
        return strings

    def value(self, node_type, raw, at):
        """The value of type node_type whose 32-bit slot holds raw (at: the slot's offset, for errors)."""
        data = self.data
        if node_type == STRING:
            return self.strings[raw]
        if node_type in CONTAINERS:
            return self.node(node_type, raw)
        if node_type == BINARY:  # u32 size + data
            size = struct.unpack_from(self.endian + "I", data, raw)[0]
            return data[raw + 4:raw + 4 + size]
        if node_type == BOOL:
            return bool(raw)
        if node_type == INT:
            return struct.unpack("<i", struct.pack("<I", raw))[0]
        if node_type == FLOAT:
            return struct.unpack("<f", struct.pack("<I", raw))[0]
        if node_type == UINT:
            return raw
        if node_type in WIDE_FORMATS:
            return struct.unpack_from(self.endian + WIDE_FORMATS[node_type], data, raw)[0]
        if node_type == NULL:
            return None
        raise ValueError(f"unknown BYML type {node_type:#x} at {at:#x}")

    def node(self, node_type, at):
        """The container node at offset at: a list or a dict."""
        data = self.data
        assert data[at] == node_type, (hex(data[at]), hex(node_type), hex(at))
        count = self._u24(at + 1)
        if node_type == ARRAY:
            # count type bytes (padded to 4), then count 32-bit value slots
            types = data[at + 4:at + 4 + count]
            values_at = at + 4 + ((count + 3) & ~3)
            items = []
            for i in range(count):
                slot = values_at + 4 * i
                items.append(self.value(types[i], self._u32(slot), slot))
            return items
        if node_type == DICT:
            # entries of 8 bytes: u24 key index, u8 type, 32-bit value slot
            out = {}
            for i in range(count):
                entry = at + 4 + 8 * i
                key = self._u24(entry)
                out[self.keys[key]] = self.value(data[entry + 3], self._u32(entry + 4), entry)
            return out
        if node_type in (HASH_MAP32, HASH_MAP64):
            # entries of (hash, 32-bit value slot), then one type byte per entry after them all
            key_size = 4 if node_type == HASH_MAP32 else 8
            entry_size = key_size + 4
            types_at = at + 4 + entry_size * count
            types = data[types_at:types_at + count]
            out = {}
            for i in range(count):
                entry = at + 4 + entry_size * i
                key = int.from_bytes(data[entry:entry + key_size], self.byteorder)
                out[key] = self.value(types[i], self._u32(entry + key_size), entry)
            return out
        raise ValueError(f"container type {node_type:#x}")


def load(data):
    return Byml(data).root
