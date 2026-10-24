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
