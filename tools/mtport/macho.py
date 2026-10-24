"""Reader for thin 32-bit little-endian Mach-O files (the armv6/armv7 slices).

The shared base for the reverse-engineering tools: segments and sections, reads by VM
address, ``LC_FUNCTION_STARTS``, the ``LC_UNIXTHREAD`` entry point, dylibs, and the raw
``(offset, size)`` of every load command so a tool can decode what this module leaves alone
(symbol tables), and the dyld bind opcodes of ``LC_DYLD_INFO(_ONLY)``. The input is
untrusted data, so every offset and size is bounds-checked before it is used.

Layout (little-endian, see <mach-o/loader.h>)::

    mach_header      { u32 magic; i32 cputype; i32 cpusubtype; u32 filetype;
                       u32 ncmds; u32 sizeofcmds; u32 flags; }          then the load commands
    load_command     { u32 cmd; u32 cmdsize; }                          prefix of every command
    segment_command  { u32 cmd; u32 cmdsize; char segname[16]; u32 vmaddr, vmsize, fileoff,
                       filesize; i32 maxprot, initprot; u32 nsects, flags; }  then nsects x
    section          { char sectname[16], segname[16]; u32 addr, size, offset, align,
                       reloff, nreloc, flags, reserved1, reserved2; }

``LC_FUNCTION_STARTS`` is a ``linkedit_data_command`` pointing at a run of ULEB128 deltas,
ended by a 0 delta: the first is relative to the ``__TEXT`` vmaddr, each later one to the
previous start. Thumb starts keep their low bit set.

``LC_DYLD_INFO(_ONLY)`` (``dyld_info_command``: ``cmd, cmdsize`` then ``(off, size)`` pairs for
rebase, bind, weak_bind, lazy_bind and export) points at three bind-opcode streams in
__LINKEDIT. Each byte is ``opcode (high nibble) | immediate (low nibble)``; the opcodes set the
state (library ordinal, symbol name, type, addend, segment + offset) and ``DO_BIND*`` record a
binding of the symbol at the current address, then advance it by the pointer size (4). The bind
and weak streams end at ``DONE``; the lazy stream is a run of ``DONE``-terminated records.

The export region is a prefix trie. Each node is ``uleb terminal_size``, then (if it is not 0)
the export info ``uleb flags, uleb address`` (an offset from the ``__TEXT`` vmaddr; re-exports
carry ``uleb ordinal, cstring name`` and stub-and-resolver exports a second ``uleb``), then
``u8 child_count`` and per child ``cstring edge, uleb child_offset`` (from the trie start). A
symbol's name is the concatenation of the edges from the root.

Command line::

    python3 -m mtport.macho info    SLICE   # segments, sections, entry, function count
    python3 -m mtport.macho starts  SLICE   # one function start per line (hex, Thumb bit kept)
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path

MH_MAGIC = 0xFEEDFACE

LC_SEGMENT = 0x1
LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_LOAD_DYLIB = 0xC
LC_ENCRYPTION_INFO = 0x21
LC_FUNCTION_STARTS = 0x26
LC_REQ_DYLD = 0x80000000
LC_LOAD_WEAK_DYLIB = 0x18 | LC_REQ_DYLD
LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD

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
EXPORT_SYMBOL_FLAGS_REEXPORT = 0x08
EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER = 0x10
_POINTER_SIZE = 4

ARM_THREAD_STATE = 1
ARM_THREAD_STATE_COUNT = 17  # r0-r12, sp, lr, pc, cpsr

_HEADER = struct.Struct("<IiiIIII")
_SEGMENT = struct.Struct("<II16sIIIIiiII")
_SECTION = struct.Struct("<16s16sIIIIIIIII")


class MachOError(ValueError):
    """The input is not a well-formed thin 32-bit little-endian Mach-O file."""


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
class Export:
    """One export-trie entry. ``address`` is a VM address (Thumb bit kept), or None for a
    re-export; ``flags`` holds the ``EXPORT_SYMBOL_FLAGS_*`` bits."""

    name: str
    address: int | None
    flags: int


@dataclass(frozen=True)
class LoadCommand:
    """One load command: its type and where it sits in the file."""

    cmd: int
    offset: int
    size: int


@dataclass(frozen=True)
class Section:
    segname: str
    sectname: str
    addr: int
    size: int
    offset: int
    align: int
    flags: int
    reserved1: int
    reserved2: int

    @property
    def end(self) -> int:
        return self.addr + self.size

    def contains(self, addr: int) -> bool:
        return self.addr <= addr < self.end


@dataclass(frozen=True)
class Segment:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int
    sections: tuple[Section, ...]


def _name(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


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


class MachO:
    """A parsed thin Mach-O image. Construct with the file's bytes."""

    def __init__(self, data: bytes) -> None:
        self.data = bytes(data)
        if len(self.data) < _HEADER.size:
            raise MachOError(f"file too short for a mach_header ({len(self.data)} bytes)")
        magic, self.cputype, self.cpusubtype, self.filetype, ncmds, sizeofcmds, self.flags = (
            _HEADER.unpack_from(self.data, 0)
        )
        if magic != MH_MAGIC:
            raise MachOError(f"not a thin 32-bit little-endian Mach-O (magic 0x{magic:08X})")
        end = _HEADER.size + sizeofcmds
        if end > len(self.data):
            raise MachOError(f"load commands need {end} bytes, file has {len(self.data)}")

        commands: list[LoadCommand] = []
        segments: list[Segment] = []
        pos = _HEADER.size
        for i in range(ncmds):
            if pos + 8 > end:
                raise MachOError(f"load command {i} starts past sizeofcmds")
            cmd, size = struct.unpack_from("<II", self.data, pos)
            if size < 8 or size % 4 or pos + size > end:
                raise MachOError(f"load command {i} (0x{cmd:X}) has bad cmdsize {size}")
            command = LoadCommand(cmd, pos, size)
            commands.append(command)
            if cmd == LC_SEGMENT:
                segments.append(self._parse_segment(command))
            pos += size
        self.commands = tuple(commands)
        self.segments = tuple(segments)
        self.sections = tuple(s for seg in segments for s in seg.sections)

    def _parse_segment(self, command: LoadCommand) -> Segment:
        if command.size < _SEGMENT.size:
            raise MachOError(f"LC_SEGMENT at {command.offset} is too short")
        _, _, name, vmaddr, vmsize, fileoff, filesize, _, _, nsects, _ = _SEGMENT.unpack_from(
            self.data, command.offset
        )
        if _SEGMENT.size + nsects * _SECTION.size > command.size:
            raise MachOError(f"segment {_name(name)}: {nsects} sections overflow the command")
        if fileoff + filesize > len(self.data):
            raise MachOError(f"segment {_name(name)}: file range runs past end of file")
        if filesize > vmsize:
            raise MachOError(f"segment {_name(name)}: filesize exceeds vmsize")
        sections = []
        for i in range(nsects):
            sectname, segname, addr, size, offset, align, _, _, flags, r1, r2 = (
                _SECTION.unpack_from(self.data, command.offset + _SEGMENT.size + i * _SECTION.size)
            )
            section = Section(
                _name(segname), _name(sectname), addr, size, offset, align, flags, r1, r2
            )
            if not (vmaddr <= addr and addr + size <= vmaddr + vmsize):
                where = f"{section.segname},{section.sectname}"
                raise MachOError(f"section {where} lies outside its segment")
            sections.append(section)
        return Segment(_name(name), vmaddr, vmsize, fileoff, filesize, tuple(sections))

    # -- lookups -------------------------------------------------------------------------

    def segment(self, name: str) -> Segment:
        for seg in self.segments:
            if seg.name == name:
                return seg
        raise MachOError(f"no segment {name}")

    def section(self, segname: str, sectname: str) -> Section | None:
        for s in self.sections:
            if s.segname == segname and s.sectname == sectname:
                return s
        return None

    def section_at(self, addr: int) -> Section | None:
        """The section containing VM address ``addr`` (Thumb bit already cleared)."""
        for s in self.sections:
            if s.contains(addr):
                return s
        return None

    def commands_of(self, cmd: int) -> list[LoadCommand]:
        return [c for c in self.commands if c.cmd == cmd]

    def command_words(self, command: LoadCommand) -> tuple[int, ...]:
        """The command's body as little-endian u32s, including the cmd/cmdsize prefix."""
        return struct.unpack_from(f"<{command.size // 4}I", self.data, command.offset)

    # -- VM address reads ----------------------------------------------------------------

    def vm_to_offset(self, addr: int) -> int:
        """File offset of VM address ``addr``; raises for zero-fill or unmapped addresses."""
        for seg in self.segments:
            if seg.vmaddr <= addr < seg.vmaddr + seg.filesize:
                return seg.fileoff + (addr - seg.vmaddr)
        raise MachOError(f"0x{addr:X} is not backed by file data")

    def read(self, addr: int, size: int) -> bytes:
        """``size`` bytes at VM address ``addr``; the range must stay in one segment."""
        offset = self.vm_to_offset(addr)
        if size and self.vm_to_offset(addr + size - 1) != offset + size - 1:
            raise MachOError(f"read 0x{addr:X}+{size} crosses a segment boundary")
        return self.data[offset : offset + size]

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr: int) -> int:
        return struct.unpack("<H", self.read(addr, 2))[0]

    def cstring(self, addr: int, limit: int = 4096) -> bytes:
        """The NUL-terminated byte string at ``addr`` (without the NUL)."""
        offset = self.vm_to_offset(addr)
        end = self.data.find(b"\0", offset, offset + limit)
        if end < 0:
            raise MachOError(f"no NUL within {limit} bytes of 0x{addr:X}")
        return self.data[offset:end]

    # -- load-command decoders -----------------------------------------------------------

    def linkedit_data(self, cmd: int) -> tuple[int, int] | None:
        """``(dataoff, datasize)`` of a ``linkedit_data_command`` such as LC_FUNCTION_STARTS."""
        found = self.commands_of(cmd)
        if not found:
            return None
        _, _, dataoff, datasize = self.command_words(found[0])[:4]
        if dataoff + datasize > len(self.data):
            raise MachOError(f"load command 0x{cmd:X}: data runs past end of file")
        return dataoff, datasize

    @cached_property
    def function_starts(self) -> tuple[int, ...]:
        """Every ``LC_FUNCTION_STARTS`` address in ascending order, Thumb bit kept."""
        span = self.linkedit_data(LC_FUNCTION_STARTS)
        if span is None:
            return ()
        pos, end = span[0], span[0] + span[1]
        addr = self.segment("__TEXT").vmaddr
        starts = []
        while pos < end:
            delta, pos = read_uleb128(self.data, pos, end)
            if delta == 0:
                break
            addr += delta
            starts.append(addr)
        return tuple(starts)

    def dyld_info(self) -> dict[str, tuple[int, int]] | None:
        """``{"rebase"|"bind"|"weak"|"lazy"|"export": (off, size)}`` of LC_DYLD_INFO(_ONLY)."""
        found = self.commands_of(LC_DYLD_INFO_ONLY) or self.commands_of(LC_DYLD_INFO)
        if not found:
            return None
        if found[0].size < 48:
            raise MachOError("LC_DYLD_INFO is too short")
        words = self.command_words(found[0])[2:12]
        out = {}
        for i, name in enumerate(("rebase", "bind", "weak", "lazy", "export")):
            off, size = words[2 * i], words[2 * i + 1]
            if off + size > len(self.data):
                raise MachOError(f"LC_DYLD_INFO {name} data runs past end of file")
            out[name] = (off, size)
        return out

    @cached_property
    def binds(self) -> tuple[Bind, ...]:
        """Every binding of the bind, weak and lazy streams, in stream order."""
        info = self.dyld_info()
        if info is None:
            return ()
        out: list[Bind] = []
        for kind in BIND_KINDS:
            off, size = info[kind]
            out.extend(self._decode_binds(kind, off, off + size))
        return tuple(out)

    @cached_property
    def _bound(self) -> dict[int, str]:
        bound: dict[int, str] = {}
        for b in self.binds:
            if b.kind != "lazy":
                bound.setdefault(b.address, b.symbol)
        return bound

    def bound_symbol(self, addr: int) -> str | None:
        """The symbol a non-lazy (bind or weak) binding stores at ``addr``, if any."""
        return self._bound.get(addr)

    def _decode_binds(self, kind: str, pos: int, end: int) -> list[Bind]:
        data = self.data
        out: list[Bind] = []
        ordinal = type_ = addend = flags = 0
        symbol: str | None = None
        segment: Segment | None = None
        offset = 0

        def bind() -> None:
            if segment is None or symbol is None:
                raise MachOError(f"{kind} bind before its segment or symbol was set")
            # Offsets are 64-bit and may wrap "backwards"; addresses are 32-bit.
            address = (segment.vmaddr + offset) & 0xFFFFFFFF
            if not segment.vmaddr <= address < segment.vmaddr + segment.vmsize:
                raise MachOError(f"{kind} bind of {symbol} at 0x{address:X} is outside its segment")
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
                if imm >= len(self.segments):
                    raise MachOError(f"{kind} bind names segment {imm} of {len(self.segments)}")
                segment = self.segments[imm]
                offset, pos = read_uleb128(data, pos, end)
            elif op == BIND_OPCODE_ADD_ADDR_ULEB:
                delta, pos = read_uleb128(data, pos, end)
                offset = (offset + delta) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND:
                bind()
                offset += _POINTER_SIZE
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB:
                bind()
                delta, pos = read_uleb128(data, pos, end)
                offset = (offset + _POINTER_SIZE + delta) & 0xFFFFFFFFFFFFFFFF
            elif op == BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED:
                bind()
                offset += _POINTER_SIZE + imm * _POINTER_SIZE
            elif op == BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB:
                count, pos = read_uleb128(data, pos, end)
                skip, pos = read_uleb128(data, pos, end)
                if segment is not None and count * (_POINTER_SIZE + skip) > segment.vmsize:
                    raise MachOError(f"{kind} bind repeat count {count} overruns its segment")
                for _ in range(count):
                    bind()
                    offset += _POINTER_SIZE + skip
            else:
                raise MachOError(f"unknown bind opcode 0x{byte:02X} in the {kind} stream")
        return out

    @cached_property
    def exports(self) -> tuple[Export, ...]:
        """Every entry of the ``LC_DYLD_INFO`` export trie, sorted by name."""
        info = self.dyld_info()
        if info is None or info["export"][1] == 0:
            return ()
        start, size = info["export"]
        end = start + size
        base = self.segment("__TEXT").vmaddr
        out: list[Export] = []
        stack = [(0, b"")]
        seen: set[int] = set()
        while stack:
            node, prefix = stack.pop()
            if node in seen or node >= size:
                raise MachOError(f"export trie node 0x{node:X} is a loop or out of range")
            seen.add(node)
            pos = start + node
            terminal, pos = read_uleb128(self.data, pos, end)
            children = pos + terminal
            if terminal:
                if children > end:
                    raise MachOError("export trie terminal runs past the trie")
                flags, pos = read_uleb128(self.data, pos, children)
                addr: int | None = None
                if not flags & EXPORT_SYMBOL_FLAGS_REEXPORT:
                    offset, pos = read_uleb128(self.data, pos, children)
                    addr = base + offset
                name = prefix.decode("utf-8", "replace")
                out.append(Export(name, addr, flags))
            if children >= end:
                raise MachOError("export trie node has no child count")
            count = self.data[children]
            pos = children + 1
            for _ in range(count):
                nul = self.data.find(b"\0", pos, end)
                if nul < 0:
                    raise MachOError("export trie edge label is not terminated")
                label = self.data[pos:nul]
                child, pos = read_uleb128(self.data, nul + 1, end)
                stack.append((child, prefix + label))
        return tuple(sorted(out, key=lambda e: e.name))

    @cached_property
    def entry_point(self) -> int | None:
        """``pc`` from the ARM ``LC_UNIXTHREAD`` state (the crt1 ``start``), if present."""
        for command in self.commands_of(LC_UNIXTHREAD):
            words = self.command_words(command)
            pos = 2
            while pos + 2 <= len(words):
                flavor, count = words[pos], words[pos + 1]
                state = words[pos + 2 : pos + 2 + count]
                if len(state) != count:
                    raise MachOError("LC_UNIXTHREAD state runs past its command")
                if flavor == ARM_THREAD_STATE and count == ARM_THREAD_STATE_COUNT:
                    return state[15]
                pos += 2 + count
        return None

    @cached_property
    def dylibs(self) -> tuple[tuple[str, bool], ...]:
        """``(install name, weak)`` for each LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB, in order."""
        out = []
        for command in self.commands:
            if command.cmd not in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB):
                continue
            name_off = struct.unpack_from("<I", self.data, command.offset + 8)[0]
            if not 24 <= name_off < command.size:
                raise MachOError(f"dylib command at {command.offset}: bad name offset")
            raw = self.data[command.offset + name_off : command.offset + command.size]
            out.append((_name(raw), command.cmd == LC_LOAD_WEAK_DYLIB))
        return tuple(out)


def load(path: str | Path) -> MachO:
    return MachO(Path(path).read_bytes())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.macho", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="segments, sections, entry point, function count").add_argument(
        "slice"
    )
    sub.add_parser("starts", help="list LC_FUNCTION_STARTS").add_argument("slice")
    args = parser.parse_args(argv)

    try:
        image = load(args.slice)
        if args.cmd == "starts":
            for addr in image.function_starts:
                print(f"0x{addr:X}")
            return 0
        for seg in image.segments:
            print(
                f"{seg.name:<12} vmaddr=0x{seg.vmaddr:X} vmsize=0x{seg.vmsize:X} "
                f"fileoff=0x{seg.fileoff:X} filesize=0x{seg.filesize:X}"
            )
            for s in seg.sections:
                print(f"  {s.sectname:<18} addr=0x{s.addr:X} size=0x{s.size:X}")
        entry = image.entry_point
        print(f"entry {'-' if entry is None else f'0x{entry:X}'}")
        starts = image.function_starts
        thumb = sum(a & 1 for a in starts)
        print(f"functions {len(starts)} ({thumb} Thumb, {len(starts) - thumb} ARM)")
    except MachOError as e:
        print(f"error: {args.slice}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
