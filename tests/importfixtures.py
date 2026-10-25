"""A synthetic armv7 image with a symbol table, stubs and symbol pointers (nothing from the IPA),
plus the Thumb assembler of ``thumbasm`` placed at its ``__text``.

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

import thumbasm
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
    segment,
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
        segment("__PAGEZERO", 0, 0x1000, 0, 0, []),
        segment(
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
        segment(
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
        segment("__LINKEDIT", 0x4000, 0x1000, link, len(linkedit), []),
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


# -- the Thumb assembler, at this image's __text by default ----------------------------------


class Thumb(thumbasm.Thumb):
    """:class:`thumbasm.Thumb` starting at ``TEXT`` unless told otherwise."""

    def __init__(self, base: int = TEXT) -> None:
        super().__init__(base)
