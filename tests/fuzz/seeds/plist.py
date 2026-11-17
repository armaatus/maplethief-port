"""Seeds for fuzz_plist: binary plists ("bplist00", CFBinaryPList.c's layout), written here."""

from __future__ import annotations

import struct

MAX_LEN = 16384


class Writer:
    """Objects in order, then the offset table and the 32-byte trailer."""

    def __init__(self) -> None:
        self.objects: list[bytes] = []

    def add(self, obj: bytes) -> int:
        self.objects.append(obj)
        return len(self.objects) - 1

    @staticmethod
    def marker(kind: int, n: int) -> bytes:
        if n < 15:
            return bytes([kind | n])
        return bytes([kind | 0x0F, 0x13]) + struct.pack(">Q", n)

    def ascii(self, s: str) -> int:
        return self.add(self.marker(0x50, len(s)) + s.encode("ascii"))

    def utf16(self, s: str) -> int:
        return self.add(self.marker(0x60, len(s)) + s.encode("utf-16-be"))

    def integer(self, v: int, log2: int) -> int:
        size = 1 << log2
        return self.add(bytes([0x10 | log2]) + v.to_bytes(size, "big"))

    def collection(self, kind: int, refs: list[int], ref_size: int) -> int:
        n = len(refs) // 2 if kind == 0xD0 else len(refs)
        return self.add(self.marker(kind, n) + b"".join(r.to_bytes(ref_size, "big") for r in refs))

    def file(self, top: int, offset_size: int, ref_size: int) -> bytes:
        out = bytearray(b"bplist00")
        offsets = []
        for obj in self.objects:
            offsets.append(len(out))
            out += obj
        table = len(out)
        for o in offsets:
            out += o.to_bytes(offset_size, "big")
        out += bytes(6) + bytes([offset_size, ref_size])
        out += struct.pack(">QQQ", len(self.objects), top, table)
        return bytes(out)


def every_kind(offset_size: int, ref_size: int) -> bytes:
    """Every kind of object once, in a dictionary (object 0, filled in last)."""
    w = Writer()
    w.add(b"")
    keys: list[int] = []
    values: list[int] = []

    def entry(key: str, value: int) -> None:
        keys.append(w.ascii(key))
        values.append(value)

    entry("null", w.add(b"\x00"))
    entry("false", w.add(b"\x08"))
    entry("true", w.add(b"\x09"))
    entry("i8", w.integer(200, 0))
    entry("i16", w.integer(0xBEEF, 1))
    entry("i32", w.integer(0xDEADBEEF, 2))
    entry("i64", w.integer(0xFFFFFFFFFFFFFFFE, 3))
    entry("i128", w.integer(7, 4))
    entry("f32", w.add(b"\x22" + struct.pack(">f", 0.5)))
    entry("f64", w.add(b"\x23" + struct.pack(">d", 1.5)))
    entry("date", w.add(b"\x33" + struct.pack(">d", 6.0e8)))
    entry("data", w.add(b"\x43\x01\x02\x03"))
    entry("uid", w.add(b"\x81\x01\x02"))
    entry("ascii", w.ascii("a string longer than fifteen bytes"))
    entry("utf16", w.utf16("브루스"))
    one = w.integer(1, 0)
    entry("array", w.collection(0xA0, [one, one, w.ascii("two")], ref_size))
    entry("set", w.collection(0xC0, [one], ref_size))
    entry("empty", w.collection(0xD0, [], ref_size))
    w.objects[0] = Writer.marker(0xD0, len(keys)) + b"".join(
        r.to_bytes(ref_size, "big") for r in keys + values
    )
    return w.file(0, offset_size, ref_size)


def strings_table() -> bytes:
    """A .strings table as the bundle's are: string keys and values."""
    w = Writer()
    w.add(b"")
    refs = [w.ascii("Bruce.dialog.1"), w.ascii("empty"), w.utf16("I'm Bruce."), w.ascii("")]
    w.objects[0] = Writer.marker(0xD0, 2) + bytes(refs)
    return w.file(0, 1, 1)


def shared_levels() -> bytes:
    """[x, x] ten levels deep, each x the level below, then one data object."""
    w = Writer()
    for level in range(10):
        w.collection(0xA0, [level + 1, level + 1], 1)
    w.add(b"\x48" + bytes(range(1, 9)))
    return w.file(0, 1, 1)


def seeds() -> dict[str, bytes]:
    return {
        "every-kind-2-1.plist": every_kind(2, 1),
        "every-kind-4-2.plist": every_kind(4, 2),
        "every-kind-8-8.plist": every_kind(8, 8),
        "strings-table.plist": strings_table(),
        "shared-levels.plist": shared_levels(),
    }
