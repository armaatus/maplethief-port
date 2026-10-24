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
