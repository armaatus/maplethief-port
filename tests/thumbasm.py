"""A tiny Thumb-2 assembler for the synthetic test images (the one assembler of the tests).

:class:`Thumb` appends encodings at ``base``; the free functions return the bytes of one
instruction for tests that splice code by hand. Only the encodings the tests need::

    t = Thumb(0x1800).push_r7_lr().movw(0, 0x1234).movt(0, 0x5678).add_pc(0).pop_r7_pc()
    code = bytes(t.code)
    thumb_bl(0x1806, 0x1901) + movw(3, 0x1234)
"""

from __future__ import annotations

import struct


def h(*halfwords: int) -> bytes:
    return struct.pack(f"<{len(halfwords)}H", *halfwords)


def _mov16(op: int, rd: int, imm: int) -> bytes:
    imm4, i, imm3, imm8 = (imm >> 12) & 0xF, (imm >> 11) & 1, (imm >> 8) & 7, imm & 0xFF
    return h(op | i << 10 | imm4, imm3 << 12 | rd << 8 | imm8)


def movw(rd: int, imm: int) -> bytes:
    """``movw rd, #imm`` (T3)."""
    return _mov16(0xF240, rd, imm)


def movt(rd: int, imm: int) -> bytes:
    """``movt rd, #imm`` (T1)."""
    return _mov16(0xF2C0, rd, imm)


def mov32(rd: int, value: int) -> bytes:
    """``movw rd, #lo`` + ``movt rd, #hi``."""
    return movw(rd, value & 0xFFFF) + movt(rd, value >> 16)


def add_pc(rd: int) -> bytes:
    """``add rd, pc`` (T2, any of r0-r15)."""
    return h(0x4478 | rd & 7 | (rd & 8) << 4)


def thumb_bl(at: int, target: int, blx: bool = False) -> bytes:
    """``BL target`` (Thumb -> Thumb) or ``BLX target`` (Thumb -> ARM, from Align(PC, 4)) at
    ``at``; the whole +/-16 MB range."""
    pc = at + 4
    offset = (target & ~3) - (pc & ~3) if blx else (target & ~1) - pc
    assert -(1 << 24) <= offset < (1 << 24)
    s = (offset >> 24) & 1
    j1 = (1 - ((offset >> 23) & 1)) ^ s
    j2 = (1 - ((offset >> 22) & 1)) ^ s
    hw1 = 0xF000 | s << 10 | (offset >> 12) & 0x3FF
    hw2 = 0xC000 | j1 << 13 | (0 if blx else 0x1000) | j2 << 11 | (offset >> 1) & 0x7FF
    return h(hw1, hw2)


PUSH_R7_LR = 0xB580
POP_R7_PC = 0xBD80
BX_LR = 0x4770
NOP = 0xBF00


class Thumb:
    """Appends Thumb-2 encodings at ``base``; ``here`` is the next instruction's address."""

    def __init__(self, base: int = 0) -> None:
        self.base = base
        self.code = bytearray()

    @property
    def here(self) -> int:
        return self.base + len(self.code)

    def raw(self, data: bytes) -> Thumb:
        self.code += data
        return self

    def h(self, *halfwords: int) -> Thumb:
        return self.raw(h(*halfwords))

    def word(self, value: int) -> Thumb:
        return self.raw(struct.pack("<I", value & 0xFFFFFFFF))

    def align4(self) -> Thumb:
        if len(self.code) % 4:
            self.nop()
        return self

    # instructions
    def push_r7_lr(self) -> Thumb:
        return self.h(PUSH_R7_LR)

    def pop_r7_pc(self) -> Thumb:
        return self.h(POP_R7_PC)

    def nop(self) -> Thumb:
        return self.h(NOP)

    def ldr_lit(self, rt: int, literal: int | None) -> Thumb:
        """``ldr rt, [pc, #n]`` reading the word at ``literal`` (None: a placeholder)."""
        if literal is None:
            return self.h(0x4800 | rt << 8)
        off = literal - ((self.here + 4) & ~3)
        assert 0 <= off < 1024 and off % 4 == 0
        return self.h(0x4800 | rt << 8 | off // 4)

    def add_pc(self, rd: int) -> Thumb:
        return self.raw(add_pc(rd))

    def ldr(self, rt: int, rn: int, imm: int = 0) -> Thumb:
        return self.h(0x6800 | (imm // 4) << 6 | rn << 3 | rt)

    def str_sp(self, rt: int, imm: int) -> Thumb:
        return self.h(0x9000 | rt << 8 | imm // 4)

    def ldr_sp(self, rt: int, imm: int) -> Thumb:
        return self.h(0x9800 | rt << 8 | imm // 4)

    def movs(self, rd: int, imm: int) -> Thumb:
        return self.h(0x2000 | rd << 8 | imm)

    def movw(self, rd: int, imm: int) -> Thumb:
        return self.raw(movw(rd, imm))

    def movt(self, rd: int, imm: int) -> Thumb:
        return self.raw(movt(rd, imm))

    def cbz(self, rn: int, target: int) -> Thumb:
        off = target - (self.here + 4)
        assert 0 <= off < 128 and off % 2 == 0
        return self.h(0xB100 | (off >> 6) << 9 | ((off >> 1) & 0x1F) << 3 | rn)

    def b(self, target: int) -> Thumb:
        off = target - (self.here + 4)
        return self.h(0xE000 | (off >> 1) & 0x7FF)

    def bl(self, target: int) -> Thumb:
        return self.raw(thumb_bl(self.here, target))

    def blx(self, target: int) -> Thumb:
        """``blx #target`` (Thumb -> ARM, T2 encoding)."""
        return self.raw(thumb_bl(self.here, target, blx=True))
