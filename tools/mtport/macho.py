"""Reader for thin 32-bit little-endian Mach-O files (the armv6/armv7 slices).

The shared base for the reverse-engineering tools: segments and sections, reads by VM
address, ``LC_FUNCTION_STARTS``, the ``LC_UNIXTHREAD`` entry point, dylibs, and the raw
``(offset, size)`` of every load command so a tool can decode what this module leaves alone
(bind opcodes, symbol tables). The input is untrusted data, so every offset and size is
bounds-checked before it is used.

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
LC_DYLD_INFO_ONLY = 0x22 | LC_REQ_DYLD

ARM_THREAD_STATE = 1
ARM_THREAD_STATE_COUNT = 17  # r0-r12, sp, lr, pc, cpsr

_HEADER = struct.Struct("<IiiIIII")
_SEGMENT = struct.Struct("<II16sIIIIiiII")
_SECTION = struct.Struct("<16s16sIIIIIIIII")


class MachOError(ValueError):
    """The input is not a well-formed thin 32-bit little-endian Mach-O file."""


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
