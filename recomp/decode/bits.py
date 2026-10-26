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


def arm_expand_imm_text(imm12: int) -> str:
    """How Capstone (LLVM's printer) spells an A32 modified immediate: ``#value`` (unsigned)
    when ``imm12`` is the canonical encoding of its value (LLVM's getSOImmVal, the least
    rotation), else ``#imm8, #rot``."""
    bits, rot = imm12 & 0xFF, 2 * (imm12 >> 8)
    value = ror32(bits, rot)
    if _so_imm_val(value) == imm12:
        return f"#{value}" if value <= 9 else f"#0x{value:x}"
    return f"#{bits}, #{rot}"


def _so_imm_val(value: int) -> int:
    """LLVM's ARM_AM::getSOImmVal: the canonical imm12 of ``value`` (-1 if none)."""
    if value & ~0xFF == 0:
        rot = 0
    else:
        tz = (value & -value).bit_length() - 1
        amount = tz & ~1
        if ror32(value, amount) & ~0xFF and value & 63:
            tz2 = ((value & ~63) & -(value & ~63)).bit_length() - 1
            if not ror32(value, tz2 & ~1) & ~0xFF:
                amount = tz2 & ~1
        rot = (32 - amount) & 31
    if ror32(0xFFFFFF00, rot) & value:
        return -1
    return ror32(value, (32 - rot) & 31) | (rot >> 1) << 8


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
