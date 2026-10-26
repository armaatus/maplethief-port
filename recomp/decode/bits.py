"""Bit-level helpers of the ARM ARM decode pseudo-code (ARMv7-A/R, sections A5-A7)."""

from __future__ import annotations

_SHIFT_KINDS = ("lsl", "lsr", "asr", "ror")


def sign_extend(value: int, bits: int) -> int:
    """``value`` (``bits`` wide) read as two's complement."""
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def ror32(value: int, amount: int) -> int:
    amount &= 31
    return ((value >> amount) | (value << (32 - amount))) & 0xFFFFFFFF if amount else value


def thumb_expand_imm_c(imm12: int) -> tuple[int, int | None] | None:
    """ThumbExpandImm_C: ``(imm32, carry_out)``; carry_out None = the carry flag is unchanged.
    None for the UNPREDICTABLE encodings (a replicated pattern of a zero byte)."""
    if imm12 >> 10 == 0:
        imm8 = imm12 & 0xFF
        form = (imm12 >> 8) & 3
        if form == 0:
            return imm8, None
        if imm8 == 0:
            return None
        if form == 1:
            return imm8 << 16 | imm8, None
        if form == 2:
            return imm8 << 24 | imm8 << 8, None
        return imm8 * 0x01010101, None
    value = ror32(0x80 | (imm12 & 0x7F), imm12 >> 7)
    return value, value >> 31


def decode_imm_shift(kind: int, imm5: int) -> tuple[str, int]:
    """DecodeImmShift: ``(lsl|lsr|asr|ror|rrx, amount)``."""
    if kind == 0:
        return "lsl", imm5
    if kind in (1, 2):
        return _SHIFT_KINDS[kind], imm5 or 32
    return ("rrx", 1) if imm5 == 0 else ("ror", imm5)


def vfp_expand_imm(imm8: int, double: bool) -> int:
    """VFPExpandImm: the bit pattern of a ``vmov.f32/.f64 #imm``."""
    sign = imm8 >> 7
    b6 = (imm8 >> 6) & 1
    frac = imm8 & 0x3F
    if double:
        exp = ((1 - b6) << 10) | ((0xFF * b6) << 2) | ((imm8 >> 4) & 3)
        return (sign << 63) | (exp << 52) | ((frac & 0xF) << 48)
    exp = ((1 - b6) << 7) | ((0x1F * b6) << 2) | ((imm8 >> 4) & 3)
    return (sign << 31) | (exp << 23) | ((frac & 0xF) << 19)
