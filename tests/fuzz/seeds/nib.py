"""Seeds for fuzz_nib: NIBArchive files (docs/formats/nibarchive.md), written here.

The header ("NIBArchive", two versions, then a count and an offset per table) and the four
tables back to back: objects (class, first value, value count), keys, values (key, type,
payload) and classes (name length, fallback count, fallbacks, the NUL-terminated name).
Varints carry 7 bits a byte, least significant first; the last byte has the high bit set.
"""

from __future__ import annotations

import struct

MAX_LEN = 16384

I8, I16, I32, I64, TRUE, FALSE, F32, F64, DATA, NIL, OBJECT = range(11)


def varint(v: int) -> bytes:
    out = bytearray()
    while v >= 0x80:
        out.append(v & 0x7F)
        v >>= 7
    out.append(v | 0x80)
    return bytes(out)


class Writer:
    def __init__(self) -> None:
        self.keys: list[str] = []
        self.classes: list[str] = []
        self.objects: list[tuple[int, list[tuple[int, int, bytes]]]] = []

    def key(self, k: str) -> int:
        if k not in self.keys:
            self.keys.append(k)
        return self.keys.index(k)

    def object(self, cls: str) -> int:
        if cls not in self.classes:
            self.classes.append(cls)
        self.objects.append((self.classes.index(cls), []))
        return len(self.objects) - 1

    def value(self, obj: int, key: str, kind: int, payload: bytes = b"") -> None:
        self.objects[obj][1].append((self.key(key), kind, payload))

    def ref(self, obj: int, key: str, target: int) -> None:
        self.value(obj, key, OBJECT, struct.pack("<I", target))

    def floats(self, obj: int, key: str, *fs: float) -> None:
        body = b"\x06" + b"".join(struct.pack("<f", f) for f in fs)
        self.value(obj, key, DATA, varint(len(body)) + body)

    def file(self) -> bytes:
        objects, keys, values, classes = bytearray(), bytearray(), bytearray(), bytearray()
        first = 0
        for cls, vals in self.objects:
            objects += varint(cls) + varint(first) + varint(len(vals))
            first += len(vals)
            for key, kind, payload in vals:
                values += varint(key) + bytes([kind]) + payload
        for k in self.keys:
            keys += varint(len(k)) + k.encode("ascii")
        for i, c in enumerate(self.classes):
            fallback = i > 0 and c == "UIProxyObject"  # one fallback, to the first class
            classes += varint(len(c) + 1) + varint(1 if fallback else 0)
            classes += struct.pack("<I", 0) if fallback else b""
            classes += c.encode("ascii") + b"\0"
        counts = (len(self.objects), len(self.keys), first, len(self.classes))
        tables = (objects, keys, values, classes)
        out = bytearray(b"NIBArchive" + struct.pack("<II", 1, 10))
        at = len(out) + 8 * 4
        for count, table in zip(counts, tables, strict=True):
            out += struct.pack("<II", count, at)
            at += len(table)
        for table in tables:
            out += table
        return bytes(out)


def main_window() -> bytes:
    """MainWindow-like: the top-level objects, a proxy, a window, a controller, every type."""
    w = Writer()
    root = w.object("NSObject")
    top = w.object("NSArray")
    owner = w.object("UIProxyObject")
    window = w.object("UIWindow")
    controller = w.object("UIViewController")
    w.ref(root, "UINibTopLevelObjectsKey", top)
    w.ref(root, "UINibObjectsKey", top)
    w.value(top, "NSInlinedValue", TRUE)
    for o in (owner, window, controller):
        w.ref(top, "UINibEncoderEmptyKey", o)
    ident = b"IBFilesOwner"
    w.value(owner, "UIProxiedObjectIdentifier", DATA, varint(len(ident)) + ident)
    w.floats(window, "UIBounds", 0, 0, 320, 480)
    w.floats(window, "UICenter", 160, 240)
    w.value(window, "UIOpaque", TRUE)
    w.value(window, "UIHidden", FALSE)
    w.value(window, "UIAutoresizingMask", I8, b"\x12")
    w.value(window, "UITag", I16, struct.pack("<h", 0x1234))
    w.value(window, "UIContentMode", I32, struct.pack("<i", 1))
    w.value(window, "UIBig", I64, struct.pack("<q", 0x0807060504030201))
    w.value(window, "UIAlpha", F32, struct.pack("<f", 1.0))
    w.value(window, "UIScale", F64, struct.pack("<d", 1.0))
    w.value(window, "UIBackgroundColor", NIL)
    w.ref(controller, "UIParentViewController", window)
    w.ref(window, "UIRootViewController", controller)
    return w.file()


def one_object() -> bytes:
    w = Writer()
    w.object("NSObject")
    return w.file()


def seeds() -> dict[str, bytes]:
    return {"main-window.nib": main_window(), "one-object.nib": one_object()}
