"""GVariant serialised data, read-only (docs/formats/flatpak-bundle.md, ADR 0193).

GLib's binary format for typed values, which OSTree uses for its objects and static deltas and
so a Flatpak bundle for everything (mtport.flatpak_bundle). This decodes the *normal form* GLib
writes, following GLib's GVariant specification
(https://developer.gnome.org/documentation/specifications/gvariant-specification-1.0.html) for
the types OSTree uses: the basic types, strings, ``v``, ``m``, arrays, tuples and dict entries.
Values come back as Python values: ``ay`` as :class:`bytes`, ``a{..}`` as a list of pairs (keys
may repeat), a tuple as a tuple, ``v`` as :class:`Variant`, ``m`` as the value or ``None``.
Input is untrusted: every offset is checked, and malformed data raises :class:`GVariantError`.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from functools import cache

FIXED = {"b": 1, "y": 1, "n": 2, "q": 2, "i": 4, "u": 4, "x": 8, "t": 8, "h": 4, "d": 8}
STRUCT = {"b": "?", "y": "B", "n": "h", "q": "H", "i": "i", "u": "I", "x": "q", "t": "Q"}
STRUCT.update({"h": "i", "d": "d"})
MAX_DEPTH = 64


class GVariantError(ValueError):
    """The bytes are not a GVariant of the type asked for."""


@dataclass(frozen=True)
class Variant:
    """A ``v`` value: its type string and its value."""

    type: str
    value: object


def _split(sig: str, pos: int = 0) -> int:
    """The end of the single complete type starting at ``sig[pos]``."""
    if pos >= len(sig):
        raise GVariantError(f"type string {sig!r} ends early")
    c = sig[pos]
    if c in FIXED or c in "sogv":
        return pos + 1
    if c in "am":
        return _split(sig, pos + 1)
    if c in "({":
        close = ")" if c == "(" else "}"
        pos += 1
        while pos < len(sig) and sig[pos] != close:
            pos = _split(sig, pos)
        if pos >= len(sig):
            raise GVariantError(f"type string {sig!r} is not closed")
        return pos + 1
    raise GVariantError(f"unknown type {c!r} in {sig!r}")


@cache
def members(sig: str) -> tuple[str, ...]:
    """The member types of tuple or dict entry type ``sig``."""
    out = []
    pos = 1
    while pos < len(sig) - 1:
        end = _split(sig, pos)
        out.append(sig[pos:end])
        pos = end
    return tuple(out)


@cache
def alignment(sig: str) -> int:
    c = sig[0]
    if c in FIXED:
        return FIXED[c]
    if c in "sog":
        return 1
    if c == "v":
        return 8
    if c in "am":
        return alignment(sig[1:])
    return max((alignment(m) for m in members(sig)), default=1)


@cache
def fixed_size(sig: str) -> int | None:
    """The size of a fixed-size type, or None."""
    c = sig[0]
    if c in FIXED:
        return FIXED[c]
    if c in "({":
        size = 0
        for m in members(sig):
            ms = fixed_size(m)
            if ms is None:
                return None
            a = alignment(m)
            size = -(-size // a) * a + ms
        if not members(sig):
            return 1  # the unit type ()
        a = alignment(sig)
        return -(-size // a) * a
    return None


def _offset_size(size: int) -> int:
    if size < 0x100:
        return 1
    if size < 0x10000:
        return 2
    if size < 1 << 32:
        return 4
    return 8


def _read_offset(data: bytes, pos: int, osize: int) -> int:
    return int.from_bytes(data[pos : pos + osize], "little")


def decode(sig: str, data: bytes, big_endian: bool = False) -> object:
    """``data`` as a GVariant of type ``sig``. ``big_endian``: the fixed-size numbers are
    big-endian (GLib's byteswapped form)."""
    if _split(sig) != len(sig):
        raise GVariantError(f"{sig!r} is not one complete type")
    return _decode(sig, memoryview(data), ">" if big_endian else "<", 0)


def _decode(sig: str, data: memoryview, order: str, depth: int) -> object:
    if depth > MAX_DEPTH:
        raise GVariantError("nested too deep")
    c = sig[0]
    if c in FIXED:
        if len(data) != FIXED[c]:
            raise GVariantError(f"'{c}' of {len(data)} bytes")
        return struct.unpack(order + STRUCT[c], data)[0]
    if c in "sog":
        raw = bytes(data)
        if not raw or raw[-1] != 0 or 0 in raw[:-1]:
            raise GVariantError("string without its single NUL at the end")
        return raw[:-1].decode("utf-8", "replace")
    if c == "v":
        raw = bytes(data)
        nul = raw.rfind(b"\0")
        if nul < 0:
            raise GVariantError("variant without a type")
        inner = raw[nul + 1 :].decode("ascii", "replace")
        if not inner or _split(inner) != len(inner):
            raise GVariantError(f"variant of bad type {inner!r}")
        return Variant(inner, _decode(inner, data[:nul], order, depth + 1))
    if c == "m":
        inner = sig[1:]
        if len(data) == 0:
            return None
        if fixed_size(inner) is not None:
            return _decode(inner, data, order, depth + 1)
        if data[-1] != 0:
            raise GVariantError("maybe without its trailing NUL")
        return _decode(inner, data[:-1], order, depth + 1)
    if c == "a":
        return _array(sig[1:], data, order, depth)
    if c in "({":
        return _tuple(sig, data, order, depth)
    raise GVariantError(f"unknown type {c!r}")


def _array(elem: str, data: memoryview, order: str, depth: int) -> object:
    size = fixed_size(elem)
    if elem == "y":
        return bytes(data)
    if size is not None:
        if len(data) % size:
            raise GVariantError(f"array of {len(data)} bytes, elements of {size}")
        items = [data[i : i + size] for i in range(0, len(data), size)]
    else:
        if len(data) == 0:
            return []
        osize = _offset_size(len(data))
        last = _read_offset(data, len(data) - osize, osize)
        if last > len(data) or (len(data) - last) % osize:
            raise GVariantError("array framing offsets out of range")
        count = (len(data) - last) // osize
        items = []
        start = 0
        align = alignment(elem)
        for i in range(count):
            end = _read_offset(data, last + i * osize, osize)
            start = -(-start // align) * align
            if not start <= end <= last:
                raise GVariantError("array element out of range")
            items.append(data[start:end])
            start = end
    out = [_decode(elem, item, order, depth + 1) for item in items]
    return out


def tuple_slices(sig: str, data: bytes) -> list[bytes]:
    """The serialised bytes of each member of tuple type ``sig`` in ``data``: what an object
    checksum covers when a tuple embeds another object."""
    if sig[0] != "(" or _split(sig) != len(sig):
        raise GVariantError(f"{sig!r} is not a tuple type")
    return [bytes(m) for _, m in _members_of(sig, memoryview(data))]


def _members_of(sig: str, data: memoryview) -> list[tuple[str, memoryview]]:
    types = members(sig)
    osize = _offset_size(len(data))
    pos = 0
    offsets_used = 0
    out = []
    for i, m in enumerate(types):
        a = alignment(m)
        pos = -(-pos // a) * a
        size = fixed_size(m)
        if size is not None:
            end = pos + size
        elif i == len(types) - 1:
            end = len(data) - offsets_used * osize
        else:
            offsets_used += 1
            at = len(data) - offsets_used * osize
            if at < 0:
                raise GVariantError("tuple framing offsets out of range")
            end = _read_offset(data, at, osize)
        if not pos <= end <= len(data) - offsets_used * osize:
            raise GVariantError(f"tuple member {i} of {sig!r} out of range")
        out.append((m, data[pos:end]))
        pos = end
    return out


def _tuple(sig: str, data: memoryview, order: str, depth: int) -> tuple:
    return tuple(_decode(m, part, order, depth + 1) for m, part in _members_of(sig, data))
