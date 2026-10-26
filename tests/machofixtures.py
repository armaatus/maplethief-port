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


def segment(
    name: str,
    vmaddr: int,
    vmsize: int,
    fileoff: int,
    filesize: int,
    sections: list[tuple],
    prot: int = 5,
) -> bytes:
    """LC_SEGMENT with ``(sectname, addr, size, offset)`` sections, or ``(sectname, addr, size,
    offset, flags, reserved1, reserved2)`` for stub and symbol-pointer sections. ``prot`` is
    both maxprot and initprot (default r-x)."""
    body = b""
    for sectname, addr, size, off, *rest in sections:
        flags, reserved1, reserved2 = rest or (0, 0, 0)
        body += struct.pack(
            "<16s16sIIIIIIIII",
            sectname.encode(),
            name.encode(),
            addr,
            size,
            off,
            2,
            0,
            0,
            flags,
            reserved1,
            reserved2,
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
        prot,
        prot,
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
        segment("__PAGEZERO", 0, 0x1000, 0, 0, []),
        segment("__TEXT", 0x1000, 0x1000, 0, 0x1000, [("__text", 0x1400, len(text), 0x400)]),
        segment(
            "__DATA", 0x3000, 0x1000, 0x1000, 0x100, [("__data", 0x3000, len(data_bytes), 0x1000)]
        ),
        segment("__LINKEDIT", 0x4000, 0x1000, linkedit_off, len(starts), []),
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

# dyld rebase opcodes, for hand-written rebase streams.
REBASE_DONE = 0x00
REBASE_TYPE_IMM = 0x10
REBASE_SEGMENT_OFFSET = 0x20
REBASE_ADD_ADDR = 0x30
REBASE_ADD_ADDR_IMM_SCALED = 0x40
REBASE_IMM_TIMES = 0x50
REBASE_ULEB_TIMES = 0x60
REBASE_ADD_ADDR_ULEB = 0x70
REBASE_ULEB_TIMES_SKIPPING = 0x80

TEXT_VMADDR = 0x1000
S_SYMBOL_STUBS = 0x8
# Section name -> (flags incl. the section type, bytes per slot) for ImageBuilder's ``indirect``.
INDIRECT_SECTIONS = {
    "__symbolstub1": (0x80000400 | S_SYMBOL_STUBS, 4),  # ARM ``ldr pc, [pc, #n]`` stubs
    "__lazy_symbol": (0x7, 4),
    "__nl_symbol_ptr": (0x6, 4),
}
S_MOD_INIT_FUNC_POINTERS = 0x9
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
    ``sections`` replaces BUILDER_SECTIONS with another ``(segment, section) -> (address,
    reserved bytes)`` layout inside the same segments.
    """

    def __init__(self, sections: dict[tuple[str, str], tuple[int, int]] | None = None) -> None:
        self.layout = dict(BUILDER_SECTIONS if sections is None else sections)
        self.bufs = {key: bytearray() for key in self.layout}
        self._strings: dict[tuple[str, str], int] = {}

    def put(self, key: tuple[str, str], data: bytes, align: int = 4) -> int:
        buf = self.bufs[key]
        buf += b"\0" * (-len(buf) % align)
        addr = self.layout[key][0] + len(buf)
        buf += data
        assert len(buf) <= self.layout[key][1], f"{key} overflows its reservation"
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
        for key, (base, _) in self.layout.items():
            buf = self.bufs[key]
            if base <= addr and addr + 4 <= base + len(buf):
                struct.pack_into("<I", buf, addr - base, value)
                return
        raise AssertionError(f"0x{addr:X} is not inside placed data")

    def _sections(self, segname: str, extra: dict | None = None) -> list[tuple]:
        """Section headers of ``segname``; ``extra`` adds ``(flags, reserved1, reserved2)``."""
        extra = extra or {}
        return [
            (
                sect,
                base,
                len(self.bufs[(seg, sect)]),
                base - TEXT_VMADDR,
                *extra.get((seg, sect), ()),
            )
            for (seg, sect), (base, _) in self.layout.items()
            if seg == segname
        ]

    def build(
        self,
        bind: bytes = b"",
        weak: bytes = b"",
        lazy: bytes = b"",
        function_starts: tuple[int, ...] | list[int] = (),
        export: bytes = b"",
        entry: int | None = None,
        undefined: tuple[str, ...] | list[str] = (),
        indirect: dict[tuple[str, str], list[str]] | None = None,
        rebase: bytes = b"",
        section_types: dict[tuple[str, str], int] | None = None,
    ) -> bytes:
        """The image; ``function_starts`` (Thumb bit kept) adds an LC_FUNCTION_STARTS,
        ``export`` (a raw export trie) the export range of LC_DYLD_INFO_ONLY and ``entry`` an
        ARM LC_UNIXTHREAD with that pc, ``rebase`` a rebase-opcode stream and
        ``section_types`` a section's flags (e.g. 0x9, ``S_MOD_INIT_FUNC_POINTERS``).

        ``undefined`` adds LC_SYMTAB with one undefined external ``nlist`` per name (library
        ordinal 1) and LC_DYSYMTAB; ``indirect`` maps a stub or symbol-pointer section (a key
        of :data:`INDIRECT_SECTIONS`) to the symbol of each of its slots, in order, which
        become the indirect symbol table and the sections' type and ``reserved1``/``2``."""
        streams = [s + b"\0" * (-len(s) % 4) for s in (rebase, bind, weak, lazy, export)]
        linkedit_off = LINKEDIT_VMADDR - TEXT_VMADDR
        offsets = [linkedit_off + sum(len(s) for s in streams[:i]) for i in range(5)]
        linkedit = b"".join(streams)
        starts_cmd = []
        if function_starts:
            starts, prev = b"", TEXT_VMADDR
            for addr in sorted(function_starts):
                starts += uleb128(addr - prev)
                prev = addr
            starts += b"\0" * (4 - len(starts) % 4)
            starts_cmd = [struct.pack("<IIII", 0x26, 16, linkedit_off + len(linkedit), len(starts))]
            linkedit += starts
        symtab_cmds = []
        extra = {key: (flags, 0, 0) for key, flags in (section_types or {}).items()}
        if undefined or indirect:
            table: list[int] = []
            for key, slots in (indirect or {}).items():
                kind, slot_size = INDIRECT_SECTIONS[key[1]]
                assert len(self.bufs[key]) == slot_size * len(slots), f"{key}: slot count"
                extra[key] = (kind, len(table), slot_size if kind & 0xFF == S_SYMBOL_STUBS else 0)
                table += [list(undefined).index(name) for name in slots]
            strtab, nlist = b"\0", b""
            for name in undefined:
                nlist += struct.pack("<IBBHI", len(strtab), 0x01, 0, 1 << 8, 0)  # N_UNDF | N_EXT
                strtab += name.encode() + b"\0"
            strtab += b"\0" * (-len(strtab) % 4)
            symoff = linkedit_off + len(linkedit)
            stroff = symoff + len(nlist)
            indoff = stroff + len(strtab)
            linkedit += nlist + strtab + struct.pack(f"<{len(table)}I", *table)
            n = len(undefined)
            symtab_cmds = [
                struct.pack("<IIIIII", 0x2, 24, symoff, n, stroff, len(strtab)),
                struct.pack(
                    "<20I",
                    0xB,
                    80,
                    0,
                    0,
                    0,
                    0,
                    0,
                    n,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    indoff,
                    len(table),
                    0,
                    0,
                    0,
                    0,
                ),
            ]
        sizes = [len(rebase), len(bind), len(weak), len(lazy), len(export)]
        dyld_info = [0x80000022, 48]
        for off, size in zip(offsets, sizes, strict=True):
            dyld_info += [off, size]

        dylib_name = b"/usr/lib/libobjc.A.dylib\0"
        dylib_name += b"\0" * (-len(dylib_name) % 4)
        data_off = DATA_VMADDR - TEXT_VMADDR
        commands = [
            segment("__PAGEZERO", 0, TEXT_VMADDR, 0, 0, []),
            segment("__TEXT", TEXT_VMADDR, 0x3000, 0, 0x3000, self._sections("__TEXT", extra)),
            segment(
                "__DATA",
                DATA_VMADDR,
                0x2000,
                data_off,
                0x2000,
                self._sections("__DATA", extra),
                prot=3,
            ),
            segment("__LINKEDIT", LINKEDIT_VMADDR, 0x1000, linkedit_off, len(linkedit), []),
            struct.pack("<12I", *dyld_info),
            struct.pack("<IIIIII", 0xC, 24 + len(dylib_name), 24, 2, 0x10000, 0x10000) + dylib_name,
            *starts_cmd,
            *symtab_cmds,
        ]
        if entry is not None:
            state = [0] * 15 + [entry, 0]
            commands.append(
                struct.pack("<IIII", 0x5, 16 + 17 * 4, 1, 17) + struct.pack("<17I", *state)
            )
        cmds = b"".join(commands)
        header = struct.pack(
            "<IiiIIII", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, 2, len(commands), len(cmds), 0
        )
        out = bytearray(header + cmds)
        first = min(base for base, _ in self.layout.values()) - TEXT_VMADDR
        assert len(out) <= first, "load commands overlap the first section"
        out += b"\0" * (linkedit_off + len(linkedit) - len(out))
        for key, (base, _) in self.layout.items():
            off = base - TEXT_VMADDR
            out[off : off + len(self.bufs[key])] = self.bufs[key]
        out[linkedit_off:] = linkedit
        return bytes(out)


def export_trie(symbols: list[tuple[str, int]], flags: int = 0) -> bytes:
    """A flat export trie: the root has one edge per symbol (the whole name) to a terminal
    node with ``flags`` and the offset ``address - TEXT_VMADDR``."""
    nodes = []
    for _name, addr in symbols:
        info = uleb128(flags) + uleb128(addr - TEXT_VMADDR)
        nodes.append(uleb128(len(info)) + info + b"\0")
    # Root size depends on the child offsets; offsets < 128 keep each ULEB one byte.
    root_len = 2 + sum(len(n.encode()) + 2 for n, _ in symbols)
    root = bytearray(b"\0" + bytes([len(symbols)]))
    pos = root_len
    for (name, _), node in zip(symbols, nodes, strict=True):
        root += name.encode() + b"\0" + uleb128(pos)
        pos += len(node)
    assert len(root) == root_len and pos < 128, "export_trie only builds small tries"
    return bytes(root) + b"".join(nodes)


def bind_stream(
    binds: list[tuple[int, str]] | list[tuple[int, str, int]], ordinal: int = 1, flags: int = 0
) -> bytes:
    """A plain bind stream: one record per ``(address in __DATA, symbol)`` or ``(address,
    symbol, addend)``, then DONE."""
    out = bytearray()
    for addr, symbol, *addend in binds:
        out.append(BIND_ORDINAL_IMM | ordinal)
        out += bytes([BIND_SYMBOL | flags]) + symbol.encode() + b"\0"
        out.append(BIND_TYPE_IMM | 1)
        if addend:
            out += bytes([BIND_ADDEND_SLEB]) + sleb128(addend[0])
        out += bytes([BIND_SEGMENT_OFFSET | DATA_SEGMENT_INDEX]) + uleb128(addr - DATA_VMADDR)
        out.append(BIND_DO)
    out.append(BIND_DONE)
    return bytes(out)
