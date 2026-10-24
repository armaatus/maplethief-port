"""Synthetic Mach-O fixtures built in code (nothing derived from the IPA)."""

from __future__ import annotations

import struct

MH_MAGIC = 0xFEEDFACE
CPU_TYPE_ARM = 12
CPU_SUBTYPE_ARM_V6 = 6
CPU_SUBTYPE_ARM_V7 = 9


def thin_macho(cputype: int, cpusubtype: int, payload: bytes) -> bytes:
    """A little-endian 32-bit mach_header (MH_EXECUTE, no load commands) + payload."""
    return struct.pack("<IiiIIII", MH_MAGIC, cputype, cpusubtype, 2, 0, 0, 0) + payload


def fat_macho(slices: list[tuple[int, int, bytes]], align: int = 12) -> bytes:
    """Lay out ``(cputype, cpusubtype, thin bytes)`` slices like lipo does.

    Each slice starts at the next multiple of ``2**align`` after the previous one,
    with zero padding in between.
    """
    step = 1 << align
    header = struct.pack(">II", 0xCAFEBABE, len(slices))
    offset = len(header) + 20 * len(slices)
    entries, layout = b"", []
    for cputype, cpusubtype, blob in slices:
        offset = -(-offset // step) * step
        entries += struct.pack(">iiIII", cputype, cpusubtype, offset, len(blob), align)
        layout.append((offset, blob))
        offset += len(blob)
    out = bytearray(header + entries)
    for off, blob in layout:
        out += b"\0" * (off - len(out))
        out += blob
    return bytes(out)


ARMV6_THIN = thin_macho(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6, b"armv6 code " * 300)
ARMV7_THIN = thin_macho(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, b"thumb-2 code " * 200)
SAMPLE_FAT = fat_macho(
    [
        (CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6, ARMV6_THIN),
        (CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, ARMV7_THIN),
    ]
)


def uleb128(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def _segment(
    name: str,
    vmaddr: int,
    vmsize: int,
    fileoff: int,
    filesize: int,
    sections: list[tuple[str, int, int, int]],
) -> bytes:
    """LC_SEGMENT with ``(sectname, addr, size, offset)`` sections."""
    body = b"".join(
        struct.pack(
            "<16s16sIIIIIIIII", s.encode(), name.encode(), addr, size, off, 2, 0, 0, 0, 0, 0
        )
        for s, addr, size, off in sections
    )
    nsects = len(sections)
    fields = (
        0x1,
        56 + len(body),
        name.encode(),
        vmaddr,
        vmsize,
        fileoff,
        filesize,
        5,
        5,
        nsects,
        0,
    )
    return struct.pack("<II16sIIIIiiII", *fields) + body


def thin_image(function_starts: list[int], text: bytes = b"\x70\x47" * 64) -> bytes:
    """A small armv7 MH_EXECUTE image with __TEXT,__text, __DATA,__data, LC_UNIXTHREAD
    (pc 0x1400), LC_FUNCTION_STARTS for ``function_starts``, one dylib and __LINKEDIT.

    Layout: __TEXT maps file 0x000-0x1000 at 0x1000 (``__text`` at file 0x400 = 0x1400),
    __DATA maps file 0x1000-0x1100 at 0x3000 (``__data`` holds ``b"hello\\0world\\0"`` then
    ``u32 0xCAFEF00D``), __LINKEDIT holds the ULEB128 starts at file 0x1100.
    """
    starts = b""
    prev = 0x1000
    for addr in function_starts:
        starts += uleb128(addr - prev)
        prev = addr
    starts += b"\0"
    starts += b"\0" * (-len(starts) % 4)
    linkedit_off = 0x1100

    data_bytes = b"hello\0world\0" + struct.pack("<I", 0xCAFEF00D)
    dylib_name = b"/usr/lib/libSystem.B.dylib\0"
    dylib_name += b"\0" * (-len(dylib_name) % 4)
    commands = [
        _segment("__PAGEZERO", 0, 0x1000, 0, 0, []),
        _segment("__TEXT", 0x1000, 0x1000, 0, 0x1000, [("__text", 0x1400, len(text), 0x400)]),
        _segment(
            "__DATA", 0x3000, 0x1000, 0x1000, 0x100, [("__data", 0x3000, len(data_bytes), 0x1000)]
        ),
        _segment("__LINKEDIT", 0x4000, 0x1000, linkedit_off, len(starts), []),
        struct.pack("<IIII", 0x5, 16 + 17 * 4, 1, 17) + struct.pack("<17I", *([0] * 15), 0x1400, 0),
        struct.pack("<IIIIII", 0xC, 24 + len(dylib_name), 24, 2, 0x10000, 0x10000) + dylib_name,
        struct.pack("<IIII", 0x26, 16, linkedit_off, len(starts)),
    ]
    cmds = b"".join(commands)
    header = struct.pack(
        "<IiiIIII", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, 2, len(commands), len(cmds), 0
    )
    out = bytearray(header + cmds)
    assert len(out) <= 0x400, "load commands overlap __text"
    out += b"\0" * (0x400 - len(out))
    out += text
    out += b"\0" * (0x1000 - len(out))
    out += data_bytes
    out += b"\0" * (linkedit_off - len(out))
    out += starts
    return bytes(out)


def sleb128(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        done = (value == 0 and not byte & 0x40) or (value == -1 and byte & 0x40)
        out.append(byte | (0 if done else 0x80))
        if done:
            return bytes(out)


# dyld bind opcodes, for hand-written bind streams.
BIND_DONE = 0x00
BIND_ORDINAL_IMM = 0x10
BIND_ORDINAL_ULEB = 0x20
BIND_SPECIAL_IMM = 0x30
BIND_SYMBOL = 0x40
BIND_TYPE_IMM = 0x50
BIND_ADDEND_SLEB = 0x60
BIND_SEGMENT_OFFSET = 0x70
BIND_ADD_ADDR = 0x80
BIND_DO = 0x90
BIND_DO_ADD_ADDR = 0xA0
BIND_DO_IMM_SCALED = 0xB0
BIND_DO_TIMES_SKIPPING = 0xC0

TEXT_VMADDR = 0x1000
DATA_VMADDR = 0x5000
LINKEDIT_VMADDR = 0x8000
DATA_SEGMENT_INDEX = 2  # __PAGEZERO, __TEXT, __DATA, __LINKEDIT

# (segment, section) -> (address, reserved bytes). VM address - 0x1000 = file offset.
BUILDER_SECTIONS = {
    ("__TEXT", "__text"): (0x1800, 0x100),
    ("__TEXT", "__objc_methname"): (0x1900, 0x300),
    ("__TEXT", "__objc_classname"): (0x1C00, 0x100),
    ("__TEXT", "__objc_methtype"): (0x1D00, 0x300),
    ("__TEXT", "__cstring"): (0x2000, 0x400),
    ("__DATA", "__objc_classlist"): (0x5000, 0x80),
    ("__DATA", "__objc_catlist"): (0x5080, 0x40),
    ("__DATA", "__objc_protolist"): (0x50C0, 0x40),
    ("__DATA", "__objc_const"): (0x5100, 0x900),
    ("__DATA", "__objc_selrefs"): (0x5A00, 0x80),
    ("__DATA", "__objc_protorefs"): (0x5A80, 0x40),
    ("__DATA", "__objc_classrefs"): (0x5AC0, 0x40),
    ("__DATA", "__objc_superrefs"): (0x5B00, 0x40),
    ("__DATA", "__objc_data"): (0x5B40, 0x200),
    ("__DATA", "__objc_ivar"): (0x5D40, 0x40),
    ("__DATA", "__data"): (0x5D80, 0x280),
}


class ImageBuilder:
    """Lays out a small armv7 MH_EXECUTE image with the sections in BUILDER_SECTIONS.

    ``put``/``words``/``cstring`` append to a section and return the VM address; ``patch``
    overwrites a word already placed. ``build`` emits __PAGEZERO, __TEXT (0x1000-0x4000),
    __DATA (0x5000-0x7000) and __LINKEDIT (0x8000, the bind streams) plus LC_DYLD_INFO_ONLY
    and one dylib (ordinal 1). Each section's size is the bytes actually placed in it.
    """

    def __init__(self) -> None:
        self.bufs = {key: bytearray() for key in BUILDER_SECTIONS}
        self._strings: dict[tuple[str, str], int] = {}

    def put(self, key: tuple[str, str], data: bytes, align: int = 4) -> int:
        buf = self.bufs[key]
        buf += b"\0" * (-len(buf) % align)
        addr = BUILDER_SECTIONS[key][0] + len(buf)
        buf += data
        assert len(buf) <= BUILDER_SECTIONS[key][1], f"{key} overflows its reservation"
        return addr

    def words(self, key: tuple[str, str], *values: int) -> int:
        return self.put(key, struct.pack(f"<{len(values)}I", *values))

    def cstring(self, sectname: str, text: str) -> int:
        """A NUL-terminated string in ``__TEXT,<sectname>`` (deduplicated)."""
        k = (sectname, text)
        if k not in self._strings:
            self._strings[k] = self.put(("__TEXT", sectname), text.encode() + b"\0", align=1)
        return self._strings[k]

    def patch(self, addr: int, value: int) -> None:
        for key, (base, _) in BUILDER_SECTIONS.items():
            buf = self.bufs[key]
            if base <= addr and addr + 4 <= base + len(buf):
                struct.pack_into("<I", buf, addr - base, value)
                return
        raise AssertionError(f"0x{addr:X} is not inside placed data")

    def _sections(self, segname: str) -> list[tuple[str, int, int, int]]:
        return [
            (sect, base, len(self.bufs[(seg, sect)]), base - TEXT_VMADDR)
            for (seg, sect), (base, _) in BUILDER_SECTIONS.items()
            if seg == segname
        ]

    def build(self, bind: bytes = b"", weak: bytes = b"", lazy: bytes = b"") -> bytes:
        streams = [s + b"\0" * (-len(s) % 4) for s in (bind, weak, lazy)]
        linkedit_off = LINKEDIT_VMADDR - TEXT_VMADDR
        offsets = [linkedit_off + sum(len(s) for s in streams[:i]) for i in range(3)]
        linkedit = b"".join(streams)
        sizes = [len(bind), len(weak), len(lazy)]
        dyld_info = [0x80000022, 48, 0, 0]
        for off, size in zip(offsets, sizes, strict=True):
            dyld_info += [off, size]
        dyld_info += [0, 0]

        dylib_name = b"/usr/lib/libobjc.A.dylib\0"
        dylib_name += b"\0" * (-len(dylib_name) % 4)
        data_off = DATA_VMADDR - TEXT_VMADDR
        commands = [
            _segment("__PAGEZERO", 0, TEXT_VMADDR, 0, 0, []),
            _segment("__TEXT", TEXT_VMADDR, 0x3000, 0, 0x3000, self._sections("__TEXT")),
            _segment("__DATA", DATA_VMADDR, 0x2000, data_off, 0x2000, self._sections("__DATA")),
            _segment("__LINKEDIT", LINKEDIT_VMADDR, 0x1000, linkedit_off, len(linkedit), []),
            struct.pack("<12I", *dyld_info),
            struct.pack("<IIIIII", 0xC, 24 + len(dylib_name), 24, 2, 0x10000, 0x10000) + dylib_name,
        ]
        cmds = b"".join(commands)
        header = struct.pack(
            "<IiiIIII", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, 2, len(commands), len(cmds), 0
        )
        out = bytearray(header + cmds)
        first = min(base for base, _ in BUILDER_SECTIONS.values()) - TEXT_VMADDR
        assert len(out) <= first, "load commands overlap the first section"
        out += b"\0" * (linkedit_off + len(linkedit) - len(out))
        for key, (base, _) in BUILDER_SECTIONS.items():
            off = base - TEXT_VMADDR
            out[off : off + len(self.bufs[key])] = self.bufs[key]
        out[linkedit_off:] = linkedit
        return bytes(out)


def bind_stream(binds: list[tuple[int, str]], ordinal: int = 1, flags: int = 0) -> bytes:
    """A plain bind stream: one record per ``(address in __DATA, symbol)``, then DONE."""
    out = bytearray()
    for addr, symbol in binds:
        out.append(BIND_ORDINAL_IMM | ordinal)
        out += bytes([BIND_SYMBOL | flags]) + symbol.encode() + b"\0"
        out.append(BIND_TYPE_IMM | 1)
        out += bytes([BIND_SEGMENT_OFFSET | DATA_SEGMENT_INDEX]) + uleb128(addr - DATA_VMADDR)
        out.append(BIND_DO)
    out.append(BIND_DONE)
    return bytes(out)
