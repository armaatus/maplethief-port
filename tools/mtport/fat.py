"""Portable fat (universal) Mach-O parser and slice splitter.

Replaces ``lipo -thin`` on hosts without Xcode (Linux, Windows). The input is
untrusted data, so every field is bounds-checked before it is used.

Layout (all fields big-endian, see <mach-o/fat.h>)::

    fat_header    { u32 magic; u32 nfat_arch; }
    fat_arch      { i32 cputype; i32 cpusubtype; u32 offset; u32 size; u32 align; }
    fat_arch_64   { i32 cputype; i32 cpusubtype; u64 offset; u64 size; u32 align; u32 reserved; }

``magic`` is ``0xCAFEBABE`` (32-bit fat_arch entries) or ``0xCAFEBABF``
(64-bit entries). ``align`` is a power of two exponent; each slice starts at a
multiple of ``2**align``. A slice is the complete thin Mach-O file, byte for
byte, which is exactly what ``lipo -thin`` writes.

Command line::

    python3 -m mtport.fat list  FAT
    python3 -m mtport.fat thin  FAT ARCH OUTPUT
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF

MH_MAGIC = 0xFEEDFACE
MH_MAGIC_64 = 0xFEEDFACF

CPU_ARCH_ABI64 = 0x01000000
CPU_TYPE_X86 = 7
CPU_TYPE_ARM = 12
CPU_TYPE_POWERPC = 18
CPU_SUBTYPE_MASK = 0xFF000000  # capability bits in the high byte of cpusubtype

# Generous upper bound: real universal binaries have a handful of slices.
# Also rejects Java class files, which share the 0xCAFEBABE magic.
MAX_FAT_ARCHS = 32
MAX_ALIGN = 16

_ARCH_NAMES: dict[tuple[int, int], str] = {
    (CPU_TYPE_ARM, 0): "arm",
    (CPU_TYPE_ARM, 5): "armv4t",
    (CPU_TYPE_ARM, 6): "armv6",
    (CPU_TYPE_ARM, 7): "armv5",
    (CPU_TYPE_ARM, 8): "xscale",
    (CPU_TYPE_ARM, 9): "armv7",
    (CPU_TYPE_ARM, 10): "armv7f",
    (CPU_TYPE_ARM, 11): "armv7s",
    (CPU_TYPE_ARM, 12): "armv7k",
    (CPU_TYPE_ARM, 14): "armv6m",
    (CPU_TYPE_ARM, 15): "armv7m",
    (CPU_TYPE_ARM, 16): "armv7em",
    (CPU_TYPE_ARM | CPU_ARCH_ABI64, 0): "arm64",
    (CPU_TYPE_ARM | CPU_ARCH_ABI64, 2): "arm64e",
    (CPU_TYPE_X86, 3): "i386",
    (CPU_TYPE_X86 | CPU_ARCH_ABI64, 3): "x86_64",
    (CPU_TYPE_X86 | CPU_ARCH_ABI64, 8): "x86_64h",
    (CPU_TYPE_POWERPC, 0): "ppc",
}


class FatError(ValueError):
    """The input is not a well-formed fat Mach-O file."""


def arch_name(cputype: int, cpusubtype: int) -> str:
    """Return the lipo-style name for a cputype/cpusubtype pair."""
    sub = cpusubtype & ~CPU_SUBTYPE_MASK & 0xFFFFFFFF
    name = _ARCH_NAMES.get((cputype & 0xFFFFFFFF, sub))
    if name is None:
        return f"cputype{cputype & 0xFFFFFFFF}_subtype{sub}"
    return name


@dataclass(frozen=True)
class FatArch:
    """One ``fat_arch`` entry."""

    cputype: int
    cpusubtype: int
    offset: int
    size: int
    align: int

    @property
    def name(self) -> str:
        return arch_name(self.cputype, self.cpusubtype)


def is_fat(data: bytes) -> bool:
    return len(data) >= 4 and struct.unpack_from(">I", data, 0)[0] in (FAT_MAGIC, FAT_MAGIC_64)


def parse_fat(data: bytes) -> list[FatArch]:
    """Parse and validate the fat header; return the entries in file order.

    Raises FatError when the header is malformed, a slice lies outside the
    file, slices overlap, an offset ignores its alignment, two slices share an
    architecture, or a slice's own Mach-O header disagrees with its entry.
    """
    if len(data) < 8:
        raise FatError(f"file too short for a fat header ({len(data)} bytes)")
    magic, nfat = struct.unpack_from(">II", data, 0)
    if magic == FAT_MAGIC:
        entry_fmt, entry_size = ">iiIII", 20
    elif magic == FAT_MAGIC_64:
        entry_fmt, entry_size = ">iiQQII", 32
    else:
        raise FatError(f"not a fat Mach-O (magic 0x{magic:08X})")
    if nfat == 0:
        raise FatError("fat header lists no architectures")
    if nfat > MAX_FAT_ARCHS:
        raise FatError(f"implausible architecture count {nfat} (max {MAX_FAT_ARCHS})")
    header_end = 8 + nfat * entry_size
    if header_end > len(data):
        raise FatError(f"fat header needs {header_end} bytes, file has {len(data)}")

    archs: list[FatArch] = []
    for i in range(nfat):
        fields = struct.unpack_from(entry_fmt, data, 8 + i * entry_size)
        cputype, cpusubtype, offset, size, align = fields[:5]
        arch = FatArch(cputype, cpusubtype, offset, size, align)
        where = f"fat_arch[{i}] ({arch.name})"
        if align > MAX_ALIGN:
            raise FatError(f"{where}: alignment 2^{align} is implausible")
        if offset % (1 << align):
            raise FatError(f"{where}: offset {offset} is not aligned to 2^{align}")
        if size == 0:
            raise FatError(f"{where}: empty slice")
        if offset < header_end:
            raise FatError(f"{where}: offset {offset} overlaps the fat header")
        if offset + size > len(data):
            raise FatError(
                f"{where}: slice {offset}+{size} runs past end of file ({len(data)} bytes)"
            )
        _check_thin_header(data, arch, where)
        archs.append(arch)

    names = [a.name for a in archs]
    dupes = sorted({n for n in names if names.count(n) > 1})
    if dupes:
        raise FatError(f"duplicate architectures: {', '.join(dupes)}")

    by_offset = sorted(archs, key=lambda a: a.offset)
    for prev, cur in zip(by_offset, by_offset[1:], strict=False):
        if cur.offset < prev.offset + prev.size:
            raise FatError(f"slices {prev.name} and {cur.name} overlap")
    return archs


def _check_thin_header(data: bytes, arch: FatArch, where: str) -> None:
    if arch.size < 8:
        raise FatError(f"{where}: slice too small for a Mach-O header")
    for endian in ("<", ">"):
        magic, cputype = struct.unpack_from(endian + "Ii", data, arch.offset)
        if magic in (MH_MAGIC, MH_MAGIC_64):
            if cputype != arch.cputype:
                raise FatError(
                    f"{where}: slice header says cputype {cputype}, fat_arch says {arch.cputype}"
                )
            return
    raise FatError(f"{where}: slice does not start with a Mach-O header")


def find_arch(archs: list[FatArch], name: str) -> FatArch:
    for arch in archs:
        if arch.name == name:
            return arch
    have = ", ".join(a.name for a in archs)
    raise FatError(f"no {name} slice (have: {have})")


def extract(data: bytes, arch: FatArch) -> bytes:
    """Return the bytes of one slice (identical to ``lipo -thin`` output)."""
    return bytes(data[arch.offset : arch.offset + arch.size])


def split(data: bytes) -> dict[str, tuple[FatArch, bytes]]:
    """Parse ``data`` and return ``{arch name: (entry, slice bytes)}`` in file order."""
    return {a.name: (a, extract(data, a)) for a in parse_fat(data)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.fat", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_list = sub.add_parser("list", help="list the slices of a fat file")
    p_list.add_argument("fat")
    p_thin = sub.add_parser("thin", help="write one slice to OUTPUT")
    p_thin.add_argument("fat")
    p_thin.add_argument("arch")
    p_thin.add_argument("output")
    args = parser.parse_args(argv)

    with open(args.fat, "rb") as f:
        data = f.read()
    try:
        archs = parse_fat(data)
        if args.cmd == "list":
            for a in archs:
                print(f"{a.name}\toffset={a.offset}\tsize={a.size}\talign=2^{a.align}")
        else:
            with open(args.output, "wb") as f:
                f.write(extract(data, find_arch(archs, args.arch)))
    except FatError as e:
        print(f"error: {args.fat}: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
