"""The dyld opcode streams of ``LC_DYLD_INFO(_ONLY)``, rebase and bind: a part of ``mtport.macho``.

:mod:`mtport.macho` is the public face (ADR 0012): it re-exports every name defined here, and
``MachO.binds`` / ``MachO.rebases`` call the decoders below with the image's segments. This part
also holds what both need and the face builds on: :class:`MachOError` and the LEB128 readers.
The input is untrusted data: every read is bounds-checked, and an address outside its segment
is an error.

Both streams are bytes of ``opcode (high nibble) | immediate (low nibble)`` (<mach-o/loader.h>).

**Rebase** opcodes set the type and a *segment + offset* address, then record rebases there,
each advancing the address by the pointer size (4) plus any skip; the stream ends at ``DONE``::

    0x00 DONE                               0x50 DO_REBASE_IMM_TIMES (imm)
    0x10 SET_TYPE_IMM (imm)                 0x60 DO_REBASE_ULEB_TIMES (uleb count)
    0x20 SET_SEGMENT_AND_OFFSET_ULEB        0x70 DO_REBASE_ADD_ADDR_ULEB (then + uleb)
         (imm = segment index, uleb offset) 0x80 DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
    0x30 ADD_ADDR_ULEB                           (uleb count, uleb skip)
    0x40 ADD_ADDR_IMM_SCALED (imm x 4)

**Bind** opcodes set the state (library ordinal, symbol name and flags, type, addend, segment +
offset) and ``DO_BIND*`` record a binding of the symbol at the current address, then advance it
by the pointer size. The bind and weak streams end at ``DONE``; the lazy stream is a run of
``DONE``-terminated records, one per lazy pointer.

Offsets are 64-bit and ``ADD_ADDR`` may wrap them "backwards" (``0xFFFF...F0``); addresses are
32-bit.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Protocol

from mtport.bytereader import FormatError

# Re-exported by the mtport.macho face.
__all__ = [
    "BIND_IMMEDIATE_MASK",
    "BIND_KINDS",
    "BIND_OPCODE_ADD_ADDR_ULEB",
    "BIND_OPCODE_DONE",
    "BIND_OPCODE_DO_BIND",
    "BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED",
    "BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB",
    "BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB",
    "BIND_OPCODE_MASK",
    "BIND_OPCODE_SET_ADDEND_SLEB",
    "BIND_OPCODE_SET_DYLIB_ORDINAL_IMM",
    "BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB",
    "BIND_OPCODE_SET_DYLIB_SPECIAL_IMM",
    "BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB",
    "BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM",
    "BIND_OPCODE_SET_TYPE_IMM",
    "BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION",
    "BIND_SYMBOL_FLAGS_WEAK_IMPORT",
    "BIND_TYPE_POINTER",
    "Bind",
    "MachOError",
    "POINTER_SIZE",
    "REBASE_IMMEDIATE_MASK",
    "REBASE_OPCODE_ADD_ADDR_IMM_SCALED",
    "REBASE_OPCODE_ADD_ADDR_ULEB",
    "REBASE_OPCODE_DONE",
    "REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB",
    "REBASE_OPCODE_DO_REBASE_IMM_TIMES",
    "REBASE_OPCODE_DO_REBASE_ULEB_TIMES",
    "REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB",
    "REBASE_OPCODE_MASK",
    "REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB",
    "REBASE_OPCODE_SET_TYPE_IMM",
    "REBASE_TYPE_POINTER",
    "REBASE_TYPE_TEXT_ABSOLUTE32",
    "REBASE_TYPE_TEXT_PCREL32",
    "Rebase",
    "decode_binds",
    "decode_rebases",
    "read_sleb128",
    "read_uleb128",
]

POINTER_SIZE = 4
_U64 = 0xFFFFFFFFFFFFFFFF

# dyld rebase opcodes (<mach-o/loader.h>)
REBASE_OPCODE_MASK = 0xF0
REBASE_IMMEDIATE_MASK = 0x0F
REBASE_OPCODE_DONE = 0x00
REBASE_OPCODE_SET_TYPE_IMM = 0x10
REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x20
REBASE_OPCODE_ADD_ADDR_ULEB = 0x30
REBASE_OPCODE_ADD_ADDR_IMM_SCALED = 0x40
REBASE_OPCODE_DO_REBASE_IMM_TIMES = 0x50
REBASE_OPCODE_DO_REBASE_ULEB_TIMES = 0x60
REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB = 0x70
REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB = 0x80
REBASE_TYPE_POINTER = 1
REBASE_TYPE_TEXT_ABSOLUTE32 = 2
REBASE_TYPE_TEXT_PCREL32 = 3

# dyld bind opcodes (<mach-o/loader.h>)
BIND_OPCODE_MASK = 0xF0
BIND_IMMEDIATE_MASK = 0x0F
BIND_OPCODE_DONE = 0x00
BIND_OPCODE_SET_DYLIB_ORDINAL_IMM = 0x10
BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB = 0x20
BIND_OPCODE_SET_DYLIB_SPECIAL_IMM = 0x30
BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM = 0x40
BIND_OPCODE_SET_TYPE_IMM = 0x50
BIND_OPCODE_SET_ADDEND_SLEB = 0x60
BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB = 0x70
BIND_OPCODE_ADD_ADDR_ULEB = 0x80
BIND_OPCODE_DO_BIND = 0x90
BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB = 0xA0
BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED = 0xB0
BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB = 0xC0
BIND_TYPE_POINTER = 1
BIND_SYMBOL_FLAGS_WEAK_IMPORT = 0x1
BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION = 0x8
BIND_KINDS = ("bind", "weak", "lazy")


class MachOError(FormatError):
    """The input is not a well-formed thin 32-bit little-endian Mach-O file."""


class _Segment(Protocol):
    @property
    def name(self) -> str: ...
    @property
    def vmaddr(self) -> int: ...
    @property
    def vmsize(self) -> int: ...


@dataclass(frozen=True)
class Bind:
    """One dyld binding: the pointer at ``address`` is set to ``symbol`` (+ ``addend``).

    ``ordinal`` is the 1-based index into :attr:`MachO.dylibs` (0 = this image, -1 = the main
    executable, -2 = flat lookup; weak binds have no ordinal and report 0). ``kind`` is one of
    :data:`BIND_KINDS`; ``flags`` holds the ``BIND_SYMBOL_FLAGS_*`` bits.
    """

    kind: str
    address: int
    symbol: str
    ordinal: int
    type: int
    addend: int
    flags: int


@dataclass(frozen=True)
class Rebase:
    """One dyld rebase: the word at ``address`` holds a VM address of this image, which moves
    with the image's slide. ``type`` is a ``REBASE_TYPE_*`` value."""

    address: int
    type: int


def read_sleb128(data: bytes, pos: int, end: int) -> tuple[int, int]:
    """Decode one SLEB128 at ``pos`` (not past ``end``); return ``(value, next pos)``."""
    value = shift = 0
    while True:
        if pos >= end:
            raise MachOError("SLEB128 runs past the end of its data")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            if byte & 0x40:
                value -= 1 << shift
            return value, pos
        if shift > 63:
            raise MachOError("SLEB128 longer than 64 bits")


def read_uleb128(data: bytes, pos: int, end: int) -> tuple[int, int]:
    """Decode one ULEB128 at ``pos`` (not past ``end``); return ``(value, next pos)``."""
    value = shift = 0
    while True:
        if pos >= end:
            raise MachOError("ULEB128 runs past the end of its data")
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise MachOError("ULEB128 longer than 64 bits")


def _address(what: str, segment: _Segment | None, offset: int) -> int:
    """The 32-bit VM address of ``segment + offset``, which must lie inside the segment."""
    if segment is None:
        raise MachOError(f"{what} before its segment was set")
    address = (segment.vmaddr + offset) & 0xFFFFFFFF
    if not segment.vmaddr <= address < segment.vmaddr + segment.vmsize:
        raise MachOError(f"{what} at 0x{address:X} is outside its segment {segment.name}")
    return address


def _check_repeat(what: str, segment: _Segment | None, count: int, step: int) -> None:
    # A huge count would loop for ages before the per-address check fails; refuse it up front.
    if segment is not None and count * step > segment.vmsize:
        raise MachOError(f"{what} repeat count {count} overruns its segment {segment.name}")


def decode_rebases(data: bytes, segments: Sequence[_Segment], pos: int, end: int) -> list[Rebase]:
    """Every rebase of the stream ``data[pos:end]``, in stream order."""
    out: list[Rebase] = []
    type_ = 0
    segment: _Segment | None = None
    offset = 0

    def rebase(count: int, step: int) -> None:
        nonlocal offset
        _check_repeat("rebase", segment, count, step)
        for _ in range(count):
            out.append(Rebase(_address("rebase", segment, offset), type_))
            offset = (offset + step) & _U64

    while pos < end:
        byte = data[pos]
        pos += 1
        op, imm = byte & REBASE_OPCODE_MASK, byte & REBASE_IMMEDIATE_MASK
        if op == REBASE_OPCODE_DONE:
            break
        if op == REBASE_OPCODE_SET_TYPE_IMM:
            type_ = imm
        elif op == REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            if imm >= len(segments):
                raise MachOError(f"rebase names segment {imm} of {len(segments)}")
            segment = segments[imm]
            offset, pos = read_uleb128(data, pos, end)
        elif op == REBASE_OPCODE_ADD_ADDR_ULEB:
            delta, pos = read_uleb128(data, pos, end)
            offset = (offset + delta) & _U64
        elif op == REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
            offset = (offset + imm * POINTER_SIZE) & _U64
        elif op == REBASE_OPCODE_DO_REBASE_IMM_TIMES:
            rebase(imm, POINTER_SIZE)
        elif op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES:
            count, pos = read_uleb128(data, pos, end)
            rebase(count, POINTER_SIZE)
        elif op == REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB:
            delta, pos = read_uleb128(data, pos, end)
            rebase(1, POINTER_SIZE + delta)
        elif op == REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB:
            count, pos = read_uleb128(data, pos, end)
            skip, pos = read_uleb128(data, pos, end)
            rebase(count, POINTER_SIZE + skip)
        else:
            raise MachOError(f"unknown rebase opcode 0x{byte:02X}")
    return out


def decode_binds(
    data: bytes, segments: Sequence[_Segment], kind: str, pos: int, end: int
) -> list[Bind]:
    """Every binding of the ``kind`` stream (one of :data:`BIND_KINDS`) ``data[pos:end]``."""
    out: list[Bind] = []
    ordinal = type_ = addend = flags = 0
    symbol: str | None = None
    segment: _Segment | None = None
    offset = 0

    def bind() -> None:
        if segment is None or symbol is None:
            raise MachOError(f"{kind} bind before its segment or symbol was set")
        address = _address(f"{kind} bind of {symbol}", segment, offset)
        out.append(Bind(kind, address, symbol, ordinal, type_, addend, flags))

    while pos < end:
        byte = data[pos]
        pos += 1
        op, imm = byte & BIND_OPCODE_MASK, byte & BIND_IMMEDIATE_MASK
        if op == BIND_OPCODE_DONE:
            if kind != "lazy":
                break
        elif op == BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            ordinal = imm
        elif op == BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
            ordinal, pos = read_uleb128(data, pos, end)
        elif op == BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
            ordinal = (imm | ~BIND_IMMEDIATE_MASK) if imm else 0
        elif op == BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM:
            stop = data.find(b"\0", pos, end)
            if stop < 0:
                raise MachOError(f"{kind} bind symbol name runs past the end of its data")
            symbol, flags, pos = data[pos:stop].decode("ascii", "replace"), imm, stop + 1
        elif op == BIND_OPCODE_SET_TYPE_IMM:
            type_ = imm
        elif op == BIND_OPCODE_SET_ADDEND_SLEB:
            addend, pos = read_sleb128(data, pos, end)
        elif op == BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
            if imm >= len(segments):
                raise MachOError(f"{kind} bind names segment {imm} of {len(segments)}")
            segment = segments[imm]
            offset, pos = read_uleb128(data, pos, end)
        elif op == BIND_OPCODE_ADD_ADDR_ULEB:
            delta, pos = read_uleb128(data, pos, end)
            offset = (offset + delta) & _U64
        elif op == BIND_OPCODE_DO_BIND:
            bind()
            offset += POINTER_SIZE
        elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
            bind()
            delta, pos = read_uleb128(data, pos, end)
            offset = (offset + POINTER_SIZE + delta) & _U64
        elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
            bind()
            offset += POINTER_SIZE + imm * POINTER_SIZE
        elif op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
            count, pos = read_uleb128(data, pos, end)
            skip, pos = read_uleb128(data, pos, end)
            _check_repeat(f"{kind} bind", segment, count, POINTER_SIZE + skip)
            for _ in range(count):
                bind()
                offset += POINTER_SIZE + skip
        else:
            raise MachOError(f"unknown bind opcode 0x{byte:02X} in the {kind} stream")
    return out
