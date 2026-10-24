"""Reader and writer for compiled ``NIBArchive`` nibs (spec: docs/formats/nibarchive.md).

The app has one nib, ``MainWindow.nib`` (``NSMainNibFile`` in ``Info.plist``), which
``UIApplicationMain`` loads at startup. ``ibtool`` compiles it into Apple's binary
``NIBArchive`` format, which is a flat object graph::

    char magic[10] = "NIBArchive";
    u32  format_version, coder_version;               // 1, 9 in MapleThief
    u32  object_count, objects_offset;                // the four tables, at these offsets
    u32  key_count, keys_offset;
    u32  value_count, values_offset;
    u32  class_count, classes_offset;
    objects: { varint class, varint first_value, varint value_count }
    keys:    { varint length; char name[length] }
    values:  { varint key; u8 type; payload }
    classes: { varint length; varint fallback_count; i32 fallback[fallback_count];
               char name[length] }                   // name includes its NUL terminator

A *varint* holds 7 bits per byte, least significant group first; the byte with the high bit
**set** is the last one (the reverse of LEB128). Value types: 0 ``i8``, 1 ``i16``, 2 ``i32``,
3 ``i64``, 4 true, 5 false, 6 ``f32``, 7 ``f64``, 8 data (``varint length`` + bytes), 9 nil,
10 object reference (``u32`` object index). All integers are little-endian.

The input is untrusted data: every read is bounds-checked, every index is checked against its
table, and :func:`parse` refuses gaps, overlaps and trailing bytes, so :func:`encode` gives
the file back byte for byte.

Command line::

    python3 -m mtport.nib dump FILE...     # every object with its values
    python3 -m mtport.nib outlets FILE...  # the outlet connections, resolved
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

MAGIC = b"NIBArchive"
HEADER_SIZE = len(MAGIC) + 10 * 4

I8, I16, I32, I64, TRUE, FALSE, F32, F64, DATA, NIL, OBJECT = range(11)
TYPE_NAMES = ("i8", "i16", "i32", "i64", "true", "false", "f32", "f64", "data", "nil", "object")
_FIXED = {I8: "<b", I16: "<h", I32: "<i", I64: "<q", F32: "<f", F64: "<d"}
# Inline geometry: a data value that starts with this byte holds little-endian f32s (UIBounds,
# UICenter in MainWindow.nib). Observed, not documented by Apple.
FLOATS_TAG = 0x06


@dataclass(frozen=True)
class Ref:
    """A type-10 value: a reference to object ``index``."""

    index: int


@dataclass(frozen=True)
class Value:
    key: str
    type: int
    value: object  # int, float, bool, bytes, None or Ref

    def floats(self) -> tuple[float, ...] | None:
        """The f32s of an inline-geometry data value (``0x06`` + floats), else None."""
        v = self.value
        if self.type != DATA or not isinstance(v, bytes) or not v or v[0] != FLOATS_TAG:
            return None
        if (len(v) - 1) % 4:
            return None
        return struct.unpack(f"<{(len(v) - 1) // 4}f", v[1:])


@dataclass(frozen=True)
class NibClass:
    name: str
    fallbacks: tuple[int, ...] = ()


@dataclass
class NibObject:
    index: int
    cls: int  # index into NibArchive.classes
    values: list[Value] = field(default_factory=list)


@dataclass
class NibArchive:
    format_version: int
    coder_version: int
    objects: list[NibObject]
    keys: list[str]
    classes: list[NibClass]

    def class_name(self, obj: NibObject | int) -> str:
        o = self.objects[obj] if isinstance(obj, int) else obj
        return self.classes[o.cls].name

    def get(self, obj: NibObject | int, key: str) -> object:
        """The first value of ``key`` on ``obj`` (KeyError if absent)."""
        o = self.objects[obj] if isinstance(obj, int) else obj
        for v in o.values:
            if v.key == key:
                return v.value
        raise KeyError(f"object {o.index} ({self.class_name(o)}) has no {key!r}")

    def deref(self, value: object) -> NibObject:
        if not isinstance(value, Ref):
            raise TypeError(f"not an object reference: {value!r}")
        return self.objects[value.index]

    def string(self, obj: NibObject | int) -> str:
        """The text of an ``NSString`` object (its ``NS.bytes``, UTF-8)."""
        o = self.objects[obj] if isinstance(obj, int) else obj
        data = self.get(o, "NS.bytes")
        if not isinstance(data, bytes):
            raise FormatError(f"object {o.index}: NS.bytes is not data")
        return data.decode("utf-8")

    def array(self, obj: NibObject | int) -> list[NibObject]:
        """The elements of an ``NSArray``/``NSMutableArray`` (``UINibEncoderEmptyKey`` refs)."""
        o = self.objects[obj] if isinstance(obj, int) else obj
        return [self.deref(v.value) for v in o.values if v.key == "UINibEncoderEmptyKey"]

    def root(self) -> NibObject:
        return self.objects[0]

    def label(self, obj: NibObject) -> str:
        """A readable name: the real class of a ``UIClassSwapper``, the identifier of a
        ``UIProxyObject`` (``IBFilesOwner``, ``IBFirstResponder``), else the class name."""
        name = self.class_name(obj)
        if name == "UIClassSwapper":
            return self.string(self.deref(self.get(obj, "UIClassName")))
        if name == "UIProxyObject":
            return self.string(self.deref(self.get(obj, "UIProxiedObjectIdentifier")))
        return name

    def outlets(self) -> list[tuple[str, NibObject, NibObject]]:
        """``(outlet name, source, destination)`` for each ``UIRuntimeOutletConnection``."""
        out = []
        for c in self.array(self.deref(self.get(self.root(), "UINibConnectionsKey"))):
            if self.class_name(c) != "UIRuntimeOutletConnection":
                continue
            name = self.string(self.deref(self.get(c, "UILabel")))
            out.append((name, self.deref(self.get(c, "UISource")), self.deref(self.get(c, "UIDestination"))))
        return out


# -- reading ------------------------------------------------------------------------------


def _varint(r: ByteReader, what: str) -> int:
    value = shift = 0
    for _ in range(5):
        b = r.u8(what)
        value |= (b & 0x7F) << shift
        shift += 7
        if b & 0x80:
            return value
    raise r.error(f"{what}: varint longer than 5 bytes")


def _value(r: ByteReader, keys: list[str], n_objects: int, i: int) -> Value:
    k = _varint(r, f"value {i} key")
    if k >= len(keys):
        raise r.error(f"value {i}: key index {k} out of range ({len(keys)} keys)")
    t = r.u8(f"value {i} type")
    if t in _FIXED:
        fmt = _FIXED[t]
        v: object = struct.unpack(fmt, r.raw(struct.calcsize(fmt), f"value {i}"))[0]
    elif t in (TRUE, FALSE):
        v = t == TRUE
    elif t == DATA:
        v = r.raw(_varint(r, f"value {i} length"), f"value {i} data")
    elif t == NIL:
        v = None
    elif t == OBJECT:
        idx = r.u32(f"value {i} object")
        if idx >= n_objects:
            raise r.error(f"value {i}: object index {idx} out of range ({n_objects} objects)")
        v = Ref(idx)
    else:
        raise r.error(f"value {i}: unknown type {t}")
    return Value(keys[k], t, v)


def parse(data: bytes, name: str = "<nib>") -> NibArchive:
    r = ByteReader(data, name)
    if r.raw(len(MAGIC), "magic") != MAGIC:
        raise r.error("not a NIBArchive (bad magic)", 0)
    fmt, coder, n_obj, o_obj, n_key, o_key, n_val, o_val, n_cls, o_cls = (
        r.u32(w) for w in (
            "format version", "coder version", "object count", "objects offset", "key count",
            "keys offset", "value count", "values offset", "class count", "classes offset",
        )
    )
    expected = HEADER_SIZE

    def seek(offset: int, what: str) -> None:
        if offset != expected:
            raise r.error(f"{what} at {offset}, expected {expected} (tables must be contiguous)")
        r.pos = offset

    seek(o_obj, "objects")
    raw_objects = []
    for i in range(n_obj):
        raw_objects.append(
            (_varint(r, f"object {i} class"), _varint(r, f"object {i} values"),
             _varint(r, f"object {i} value count"))
        )
    expected = r.pos
    seek(o_key, "keys")
    keys = []
    for i in range(n_key):
        raw = r.raw(_varint(r, f"key {i} length"), f"key {i}")
        try:
            keys.append(raw.decode("utf-8"))
        except UnicodeDecodeError as e:
            raise r.error(f"key {i} is not UTF-8") from e
    expected = r.pos
    seek(o_val, "values")
    values = [_value(r, keys, n_obj, i) for i in range(n_val)]
    expected = r.pos
    seek(o_cls, "classes")
    classes = []
    for i in range(n_cls):
        length = _varint(r, f"class {i} length")
        fallbacks = tuple(r.i32(f"class {i} fallback") for _ in range(_varint(r, f"class {i} fallbacks")))
        raw = r.raw(length, f"class {i} name")
        if not raw.endswith(b"\0") or b"\0" in raw[:-1]:
            raise r.error(f"class {i}: name is not one NUL-terminated string")
        try:
            classes.append(NibClass(raw[:-1].decode("utf-8"), fallbacks))
        except UnicodeDecodeError as e:
            raise r.error(f"class {i} name is not UTF-8") from e
    r.expect_end("NIBArchive")

    for c in classes:
        for fb in c.fallbacks:
            if not 0 <= fb < len(classes):
                raise FormatError(f"{name}: class {c.name!r}: fallback {fb} out of range")
    objects = []
    next_value = 0
    for i, (cls, first, count) in enumerate(raw_objects):
        if cls >= len(classes):
            raise FormatError(f"{name}: object {i}: class index {cls} out of range")
        if first != next_value or first + count > len(values):
            raise FormatError(f"{name}: object {i}: values {first}+{count} are not the next run")
        objects.append(NibObject(i, cls, values[first : first + count]))
        next_value = first + count
    if next_value != len(values):
        raise FormatError(f"{name}: {len(values) - next_value} values belong to no object")
    return NibArchive(fmt, coder, objects, keys, classes)


def load(path: str | Path) -> NibArchive:
    p = Path(path)
    return parse(p.read_bytes(), p.name)


# -- writing ------------------------------------------------------------------------------


def _put_varint(out: bytearray, value: int) -> None:
    if value < 0:
        raise ValueError(f"varint cannot hold {value}")
    while True:
        b = value & 0x7F
        value >>= 7
        if not value:
            out.append(b | 0x80)
            return
        out.append(b)


def encode(nib: NibArchive) -> bytes:
    """The archive as bytes, tables in the order objects, keys, values, classes."""
    key_index = {k: i for i, k in reversed(list(enumerate(nib.keys)))}
    objects, keys, values, classes = bytearray(), bytearray(), bytearray(), bytearray()
    first = 0
    for o in nib.objects:
        for part in (o.cls, first, len(o.values)):
            _put_varint(objects, part)
        first += len(o.values)
        for v in o.values:
            _put_varint(values, key_index[v.key])
            values.append(v.type)
            if v.type in _FIXED:
                values += struct.pack(_FIXED[v.type], v.value)
            elif v.type == DATA:
                assert isinstance(v.value, bytes)
                _put_varint(values, len(v.value))
                values += v.value
            elif v.type == OBJECT:
                assert isinstance(v.value, Ref)
                values += struct.pack("<I", v.value.index)
    for k in nib.keys:
        raw = k.encode("utf-8")
        _put_varint(keys, len(raw))
        keys += raw
    for c in nib.classes:
        raw = c.name.encode("utf-8") + b"\0"
        _put_varint(classes, len(raw))
        _put_varint(classes, len(c.fallbacks))
        for fb in c.fallbacks:
            classes += struct.pack("<i", fb)
        classes += raw
    n_values = sum(len(o.values) for o in nib.objects)
    o_obj = HEADER_SIZE
    o_key = o_obj + len(objects)
    o_val = o_key + len(keys)
    o_cls = o_val + len(values)
    header = MAGIC + struct.pack(
        "<10I", nib.format_version, nib.coder_version, len(nib.objects), o_obj, len(nib.keys),
        o_key, n_values, o_val, len(nib.classes), o_cls,
    )
    return bytes(header + objects + keys + values + classes)


# -- command line -------------------------------------------------------------------------


def _show(nib: NibArchive, v: Value) -> str:
    if isinstance(v.value, Ref):
        target = nib.objects[v.value.index]
        text = f"-> #{target.index} {nib.label(target)}"
        if nib.class_name(target) == "NSString":
            text += f" {nib.string(target)!r}"
        return text
    floats = v.floats()
    if floats is not None:
        return "floats " + ", ".join(f"{f:g}" for f in floats)
    return repr(v.value)


def dump(nib: NibArchive) -> list[str]:
    lines = [f"NIBArchive format {nib.format_version}, coder {nib.coder_version}: "
             f"{len(nib.objects)} objects, {len(nib.keys)} keys, {len(nib.classes)} classes"]
    for o in nib.objects:
        lines.append(f"#{o.index} {nib.label(o)}")
        lines.extend(f"    {v.key} = {_show(nib, v)}" for v in o.values)
    return lines


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.nib", description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=("dump", "outlets"))
    parser.add_argument("files", nargs="+", type=Path)
    args = parser.parse_args(argv)
    status = 0
    for path in args.files:
        try:
            nib = load(path)
        except (OSError, FormatError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            status = 1
            continue
        if args.command == "dump":
            print("\n".join(dump(nib)))
        else:
            for name, src, dst in nib.outlets():
                print(f"{path.name}: {nib.label(src)}.{name} -> {nib.label(dst)}")
    return status


if __name__ == "__main__":
    sys.exit(main())
