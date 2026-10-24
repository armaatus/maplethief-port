"""A synthetic armv7 image with a symbol table, stubs and symbol pointers (nothing from the IPA),
plus a tiny Thumb assembler for the instructions the scanner tests need.

Layout (VM address - 0x1000 = file offset)::

    __TEXT  0x1000-0x3000   __text 0x1800 (the caller's code), __objc_methname 0x2E00,
                            __symbolstub1 0x2F00: 3 ARM stubs (_foo, _objc_msgSend, _bar)
    __DATA  0x3000-0x4000   __lazy_symbol 0x3000 (3), __nl_symbol_ptr 0x300C (_kConst, LOCAL),
                            __objc_classrefs 0x3014 (NSString), __objc_selrefs 0x3018
                            ("length"), __data 0x301C (a word bound to _free)
    __LINKEDIT 0x4000       function starts, nlists, strings, indirect table, bind streams

Symbols: ``_main`` (defined) then the undefined ``_foo`` ``_objc_msgSend`` ``_bar`` ``_kConst``
``_OBJC_CLASS_$_NSString`` ``_free`` ``_unused``. Library ordinal 1 is libSystem, 2 libobjc,
3 a weak GameKit.
"""

from __future__ import annotations

import struct

from machofixtures import (
    BIND_DO,
    BIND_DONE,
    BIND_ORDINAL_IMM,
    BIND_SEGMENT_OFFSET,
    BIND_SYMBOL,
    BIND_TYPE_IMM,
    CPU_SUBTYPE_ARM_V7,
    CPU_TYPE_ARM,
    MH_MAGIC,
    uleb128,
)

TEXT = 0x1800
METHNAME = 0x2E00
STUBS = 0x2F00
LAZY = 0x3000
NL = 0x300C
CLASSREF = 0x3014
SELREF = 0x3018
DATA_WORD = 0x301C
STUB_FOO, STUB_MSGSEND, STUB_BAR = STUBS, STUBS + 4, STUBS + 8

UNDEFINED = [  # name, ordinal
    ("_foo", 1),
    ("_objc_msgSend", 2),
    ("_bar", 1),
    ("_kConst", 1),
    ("_OBJC_CLASS_$_NSString", 2),
    ("_free", 1),
    ("_unused", 3),
]
DYLIBS = [
    (b"/usr/lib/libSystem.B.dylib", False),
    (b"/usr/lib/libobjc.A.dylib", False),
    (b"/System/Library/Frameworks/GameKit.framework/GameKit", True),
]


def _seg(name: str, vmaddr: int, vmsize: int, fileoff: int, filesize: int, sections) -> bytes:
    body = b"".join(
        struct.pack("<16s16sIIIIIIIII", s.encode(), name.encode(), a, n, o, 2, 0, 0, f, r1, r2)
        for s, a, n, o, f, r1, r2 in sections
    )
    head = struct.pack(
        "<II16sIIIIiiII",
        0x1,
        56 + len(body),
        name.encode(),
        vmaddr,
        vmsize,
        fileoff,
        filesize,
        5,
        5,
        len(sections),
        0,
    )
    return head + body


def _bind(addr: int, symbol: str, ordinal: int) -> bytes:
    return (
        bytes([BIND_ORDINAL_IMM | ordinal, BIND_SYMBOL])
        + symbol.encode()
        + b"\0"
        + bytes([BIND_TYPE_IMM | 1, BIND_SEGMENT_OFFSET | 2])
        + uleb128(addr - 0x3000)
        + bytes([BIND_DO])
    )


def build(text: bytes, function_starts: list[int], stub_words: list[int] | None = None) -> bytes:
    """The image with ``text`` at 0x1800 and ``function_starts`` (Thumb bit kept)."""
    assert len(text) <= 0x500
    methname = b"length\0"
    stubs = stub_words or [0xE59FF000 | (LAZY + 4 * i - (STUBS + 4 * i) - 8) for i in range(3)]
    lazy_words = [0x1F00, 0x1F00, 0x1F00]  # would point into __stub_helper
    data = struct.pack("<8I", *lazy_words, 0, 0x2000, 0, METHNAME, 0)

    starts = b""
    prev = 0x1000
    for addr in function_starts:
        starts += uleb128(addr - prev)
        prev = addr
    starts += b"\0" * (1 + (-len(starts) - 1) % 4)
    names = ["_main"] + [n for n, _ in UNDEFINED]
    strtab = b"\0"
    offsets = []
    for n in names:
        offsets.append(len(strtab))
        strtab += n.encode() + b"\0"
    strtab += b"\0" * (-len(strtab) % 4)
    nlist = struct.pack("<IBBHI", offsets[0], 0x0F, 1, 0, TEXT | 1)
    for i, (_, ordinal) in enumerate(UNDEFINED):
        nlist += struct.pack("<IBBHI", offsets[i + 1], 0x01, 0, ordinal << 8, 0)
    indirect = struct.pack("<8I", 1, 2, 3, 1, 2, 3, 4, 0x80000000)
    bind = (
        _bind(NL, "_kConst", 1)
        + _bind(CLASSREF, "_OBJC_CLASS_$_NSString", 2)
        + _bind(DATA_WORD, "_free", 1)
        + bytes([BIND_DONE])
    )
    lazy = b"".join(
        _bind(LAZY + 4 * i, n, o) + bytes([BIND_DONE]) for i, (n, o) in enumerate(UNDEFINED[:3])
    )
    bind += b"\0" * (-len(bind) % 4)
    lazy += b"\0" * (-len(lazy) % 4)

    link = 0x3000  # file offset of __LINKEDIT
    parts = [starts, nlist, strtab, indirect, bind, lazy]
    offs = []
    pos = link
    for p in parts:
        offs.append(pos)
        pos += len(p)
    linkedit = b"".join(parts)
    o_starts, o_nlist, o_str, o_ind, o_bind, o_lazy = offs

    commands = [
        _seg("__PAGEZERO", 0, 0x1000, 0, 0, []),
        _seg(
            "__TEXT",
            0x1000,
            0x2000,
            0,
            0x2000,
            [
                ("__text", TEXT, len(text), TEXT - 0x1000, 0x80000400, 0, 0),
                ("__objc_methname", METHNAME, len(methname), METHNAME - 0x1000, 0x2, 0, 0),
                ("__symbolstub1", STUBS, 12, STUBS - 0x1000, 0x80000408, 0, 4),
            ],
        ),
        _seg(
            "__DATA",
            0x3000,
            0x1000,
            0x2000,
            0x1000,
            [
                ("__lazy_symbol", LAZY, 12, LAZY - 0x1000, 0x7, 3, 0),
                ("__nl_symbol_ptr", NL, 8, NL - 0x1000, 0x6, 6, 0),
                ("__objc_classrefs", CLASSREF, 4, CLASSREF - 0x1000, 0x10000000, 0, 0),
                ("__objc_selrefs", SELREF, 4, SELREF - 0x1000, 0x10000005, 0, 0),
                ("__data", DATA_WORD, 4, DATA_WORD - 0x1000, 0, 0, 0),
            ],
        ),
        _seg("__LINKEDIT", 0x4000, 0x1000, link, len(linkedit), []),
        struct.pack("<IIIIII", 0x2, 24, o_nlist, len(names), o_str, len(strtab)),
        struct.pack(
            "<20I", 0xB, 80, 0, 0, 0, 1, 1, len(UNDEFINED), 0, 0, 0, 0, 0, 0, o_ind, 8, 0, 0, 0, 0
        ),
        struct.pack("<12I", 0x80000022, 48, 0, 0, o_bind, len(bind), 0, 0, o_lazy, len(lazy), 0, 0),
        struct.pack("<IIII", 0x26, 16, o_starts, len(starts)),
    ]
    for name, weak in DYLIBS:
        raw = name + b"\0" * (4 - len(name) % 4)
        commands.append(
            struct.pack("<IIIIII", 0x80000018 if weak else 0xC, 24 + len(raw), 24, 2, 0, 0) + raw
        )
    cmds = b"".join(commands)
    header = struct.pack(
        "<IiiIIII", MH_MAGIC, CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, 2, len(commands), len(cmds), 0
    )
    out = bytearray(header + cmds)
    assert len(out) <= TEXT - 0x1000, "load commands overlap __text"
    out += b"\0" * (link - len(out))
    out[TEXT - 0x1000 : TEXT - 0x1000 + len(text)] = text
    out[METHNAME - 0x1000 : METHNAME - 0x1000 + len(methname)] = methname
    out[STUBS - 0x1000 : STUBS - 0x1000 + 12] = struct.pack("<3I", *stubs)
    out[0x2000 : 0x2000 + len(data)] = data
    out += linkedit
    return bytes(out)


# -- a tiny Thumb assembler -----------------------------------------------------------------


class Thumb:
    """Appends Thumb-2 encodings at ``base``; ``here`` is the next instruction's address."""

    def __init__(self, base: int = TEXT) -> None:
        self.base = base
        self.code = bytearray()

    @property
    def here(self) -> int:
        return self.base + len(self.code)

    def h(self, *halfwords: int) -> Thumb:
        for hw in halfwords:
            self.code += struct.pack("<H", hw)
        return self

    def word(self, value: int) -> Thumb:
        self.code += struct.pack("<I", value & 0xFFFFFFFF)
        return self

    def align4(self) -> Thumb:
        if len(self.code) % 4:
            self.h(0xBF00)  # nop
        return self

    # instructions
    def push_r7_lr(self):
        return self.h(0xB580)

    def pop_r7_pc(self):
        return self.h(0xBD80)

    def nop(self):
        return self.h(0xBF00)

    def ldr_lit(self, rt: int, literal: int | None):
        """``ldr rt, [pc, #n]`` reading the word at ``literal`` (None: a placeholder)."""
        if literal is None:
            return self.h(0x4800 | rt << 8)
        off = literal - ((self.here + 4) & ~3)
        assert 0 <= off < 1024 and off % 4 == 0
        return self.h(0x4800 | rt << 8 | off // 4)

    def add_pc(self, rd: int):
        return self.h(0x4478 | rd)

    def ldr(self, rt: int, rn: int, imm: int = 0):
        return self.h(0x6800 | (imm // 4) << 6 | rn << 3 | rt)

    def str_sp(self, rt: int, imm: int):
        return self.h(0x9000 | rt << 8 | imm // 4)

    def ldr_sp(self, rt: int, imm: int):
        return self.h(0x9800 | rt << 8 | imm // 4)

    def movs(self, rd: int, imm: int):
        return self.h(0x2000 | rd << 8 | imm)

    def _mov16(self, op: int, rd: int, imm: int):
        imm4, i, imm3, imm8 = imm >> 12, (imm >> 11) & 1, (imm >> 8) & 7, imm & 0xFF
        return self.h(op | i << 10 | imm4, imm3 << 12 | rd << 8 | imm8)

    def movw(self, rd: int, imm: int):
        return self._mov16(0xF240, rd, imm)

    def movt(self, rd: int, imm: int):
        return self._mov16(0xF2C0, rd, imm)

    def cbz(self, rn: int, target: int):
        off = target - (self.here + 4)
        assert 0 <= off < 128 and off % 2 == 0
        return self.h(0xB100 | (off >> 6) << 9 | ((off >> 1) & 0x1F) << 3 | rn)

    def b(self, target: int):
        off = target - (self.here + 4)
        return self.h(0xE000 | (off >> 1) & 0x7FF)

    def blx(self, target: int):
        """``blx #target`` (Thumb -> ARM, T2 encoding)."""
        off = target - ((self.here + 4) & ~3)
        return self._branch(off, 0xE800)

    def bl(self, target: int):
        return self._branch(target - (self.here + 4), 0xF800)

    def _branch(self, off: int, low: int):
        s = (off >> 24) & 1
        i1, i2 = (off >> 23) & 1, (off >> 22) & 1
        j1, j2 = (1 - i1) ^ s, (1 - i2) ^ s
        imm10, imm11 = (off >> 12) & 0x3FF, (off >> 1) & 0x7FF
        if low == 0xE800:
            imm11 &= ~1
        return self.h(0xF000 | s << 10 | imm10, low | j1 << 13 | j2 << 11 | imm11)
