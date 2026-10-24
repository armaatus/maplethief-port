"""Ghidra-free helpers shared by the Ghidra scripts in ``tools/ghidra/scripts/`` (ADR 0007).

Everything here runs without Ghidra, so the tests cover it in CI:

* :func:`seed_ranges`: the ``LC_FUNCTION_STARTS`` seeding plan (which code ranges get
  ``TMode`` = 1 or 0 before auto-analysis).
* :func:`compare_starts`: Ghidra's functions against ``LC_FUNCTION_STARTS`` (missing, wrong
  mode, extra).
* :func:`macho_strings`: every string in the slice. C-string sections, the UTF-16 literals in
  ``__TEXT,__ustring`` and the ``__DATA,__cfstring`` objects that point at either.
* :func:`dumps`: the deterministic JSON layout of every export file.

Address convention (CLAUDE.md): a function pointer carries the Thumb bit (``0x2A3C1``),
plain addresses and code ranges don't (``0x2A3C0``). Hex digits are upper case.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path

from .macho import MachO, MachOError

SCHEMA_VERSION = 1

S_CSTRING_LITERALS = 0x2
SECTION_TYPE = 0xFF
CFSTRING_SIZE = 16  # struct { Class isa; u32 flags; const char *str; u32 length; }
CFSTRING_FLAGS_8BIT = 0x7C8
CFSTRING_FLAGS_UTF16 = 0x7D0


def hexaddr(addr: int) -> str:
    return f"0x{addr:X}"


def pointer(addr: int, thumb: bool) -> str:
    """Function-pointer form: the Thumb bit set for Thumb code."""
    return hexaddr(addr | 1 if thumb else addr)


# -- seeding ----------------------------------------------------------------------------


@dataclass(frozen=True)
class SeedRange:
    """``[start, end)`` of code from one function start up to the next one."""

    start: int
    end: int
    thumb: bool


def seed_ranges(image: MachO) -> list[SeedRange]:
    """One range per ``LC_FUNCTION_STARTS`` entry: from the start (Thumb bit cleared) to the
    next start, the last one to the end of its section. Covering the gaps too means code
    that analysis finds later between two starts decodes in the right mode."""
    starts = sorted(image.function_starts)
    out: list[SeedRange] = []
    for i, raw in enumerate(starts):
        addr = raw & ~1
        section = image.section_at(addr)
        if section is None:
            raise MachOError(f"function start 0x{raw:X} is outside every section")
        end = section.end
        if i + 1 < len(starts):
            end = min(end, starts[i + 1] & ~1)
        out.append(SeedRange(addr, end, bool(raw & 1)))
    return out


# -- function starts vs. Ghidra ---------------------------------------------------------


def compare_starts(starts: tuple[int, ...] | list[int], functions: dict[int, bool]) -> dict:
    """Compare ``LC_FUNCTION_STARTS`` with Ghidra's functions (``{entry: is_thumb}``).

    ``missing``: starts with no function at that entry; ``wrong_mode``: a function exists
    but its mode differs from the start's Thumb bit; ``extra``: functions that are not a
    start. All lists are sorted, in function-pointer form."""
    by_addr = {raw & ~1: bool(raw & 1) for raw in starts}
    missing = sorted(a for a in by_addr if a not in functions)
    wrong = sorted(a for a, t in by_addr.items() if a in functions and functions[a] != t)
    extra = sorted(a for a in functions if a not in by_addr)
    return {
        "function_starts": len(by_addr),
        "matched": len(by_addr) - len(missing) - len(wrong),
        "missing": [pointer(a, by_addr[a]) for a in missing],
        "wrong_mode": [pointer(a, by_addr[a]) for a in wrong],
        "extra": [pointer(a, functions[a]) for a in extra],
    }


# -- strings ----------------------------------------------------------------------------


def _decode(raw: bytes, encoding: str) -> tuple[str, str]:
    """``(encoding, value)``: the decoded text, or ``("hex", raw.hex())`` if it isn't valid."""
    try:
        return encoding, raw.decode(encoding)
    except UnicodeDecodeError:
        return "hex", raw.hex()


def _record(addr: int, section: str, kind: str, raw: bytes, encoding: str, **extra) -> dict:
    enc, value = _decode(raw, encoding)
    rec = {"address": hexaddr(addr), "section": section, "kind": kind, "encoding": enc}
    rec.update(extra)
    rec["value"] = value
    return rec


def _split(data: bytes, base: int, unit: int) -> list[tuple[int, bytes]]:
    """NUL-terminated runs of ``unit``-byte code units; empty runs are skipped."""
    out = []
    start = 0
    for pos in range(0, len(data) - unit + 1, unit):
        if data[pos : pos + unit] == b"\0" * unit:
            if pos > start:
                out.append((base + start, data[start:pos]))
            start = pos + unit
    if start < len(data) - (len(data) % unit):
        out.append((base + start, data[start : len(data) - (len(data) % unit)]))
    return out


def macho_strings(image: MachO) -> list[dict]:
    """Every string in the image, sorted by address.

    * ``kind: "cstring"``: each NUL-terminated string in a ``S_CSTRING_LITERALS`` section
      (``__cstring``, ``__objc_methname``, ``__objc_classname``, ``__objc_methtype``),
      decoded as UTF-8.
    * ``kind: "ustring"``: each NUL-terminated UTF-16LE string in ``__TEXT,__ustring``.
    * ``kind: "cfstring"``: each 16-byte ``__DATA,__cfstring`` object, with its ``target``
      (the character data), ``length`` (in bytes for 8-bit, code units for UTF-16) and the
      decoded value.

    ``encoding`` is ``utf-8``/``utf-16-le``; bytes that don't decode are given as ``hex``.
    """
    out: list[dict] = []
    for s in image.sections:
        name = f"{s.segname},{s.sectname}"
        if s.flags & SECTION_TYPE == S_CSTRING_LITERALS:
            for addr, raw in _split(image.read(s.addr, s.size), s.addr, 1):
                out.append(_record(addr, name, "cstring", raw, "utf-8"))
        elif (s.segname, s.sectname) == ("__TEXT", "__ustring"):
            for addr, raw in _split(image.read(s.addr, s.size), s.addr, 2):
                out.append(_record(addr, name, "ustring", raw, "utf-16-le"))
        elif s.sectname == "__cfstring":
            for addr in range(s.addr, s.end - CFSTRING_SIZE + 1, CFSTRING_SIZE):
                flags, target, length = (
                    image.u32(addr + 4),
                    image.u32(addr + 8),
                    image.u32(addr + 12),
                )
                utf16 = flags == CFSTRING_FLAGS_UTF16
                try:
                    raw = image.read(target, length * 2 if utf16 else length)
                except MachOError:
                    raw = b""
                out.append(
                    _record(
                        addr,
                        name,
                        "cfstring",
                        raw,
                        "utf-16-le" if utf16 else "utf-8",
                        flags=hexaddr(flags),
                        target=hexaddr(target),
                        length=length,
                    )
                )
    out.sort(key=lambda r: int(r["address"], 16))
    return out


# -- output -----------------------------------------------------------------------------


def _compact(value: object) -> str:
    return json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def dumps(header: dict, key: str, records: list) -> str:
    """The export layout: header keys sorted, one per line, then ``key`` with one record
    per line. Records are written in the order given (callers sort them). Valid JSON, UTF-8,
    no timestamps, so equal inputs give byte-identical files that diff line by line."""
    if key in header:
        raise ValueError(f"header already has {key!r}")
    lines = ["{"]
    for k in sorted(header):
        lines.append(f"  {_compact(k)}: {_compact(header[k])},")
    lines.append(f"  {_compact(key)}: [")
    body = [f"    {_compact(r)}" for r in records]
    if body:
        lines.append(",\n".join(body))
    lines.append("  ]")
    lines.append("}")
    return "\n".join(lines) + "\n"


def write_text(path: Path, text: str) -> None:
    """Write ``text`` as UTF-8 via a temporary file, so readers never see half a file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(text.encode("utf-8"))
    os.replace(tmp, path)
