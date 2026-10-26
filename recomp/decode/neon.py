"""Advanced SIMD (NEON) data-processing encodings (ARM ARM A7.4), only the forms the binary
uses (#34; the census lists them, ANALYSIS.md "Instruction mix").

``word`` is the Thumb encoding as one 32-bit value, ``111U 1111 ...``: the low 24 bits are
the same as in the ARM encoding ``1111 001U ...``, and U is bit 28. Decoded:

- three registers of the same length (A7.4.1): VORR (register), VADD/VSUB/VMUL (f32);
- one register and a modified immediate (A7.4.6): VMOV (immediate), .i8/.i16/.i32/.f32;
- two registers, miscellaneous (A7.4.5): VNEG.F32 and VCVT between f32 and s32/u32.

D and Q forms are both decoded (Q needs even register numbers, else UNDEFINED). Everything
else, including the half-precision forms that ARMv8.2 added in the sz=1 slots, decodes as
``<undefined>``; :func:`recomp.discover.is_simd` still recognizes it as Advanced SIMD.
"""

from __future__ import annotations

from recomp.decode.bits import vfp_expand_imm
from recomp.decode.insn import AL, DReg, FPImm, Imm, Insn, QReg, undefined


def is_neon_dp(word: int) -> bool:
    """An Advanced SIMD data-processing encoding (Thumb ``111x 1111``)."""
    return word >> 24 & 0xEF == 0xEF


def adv_simd_expand_imm(op: int, cmode: int, imm8: int) -> tuple[str, int] | None:
    """``(data type, element value)`` of VMOV (immediate), AdvSIMDExpandImm (A7.4.6) for op
    = 0; None for the encodings that are not VMOV."""
    if op:
        return None  # VMVN, VBIC and VMOV.I64: not used by the binary
    if cmode < 8:
        return (".i32", imm8 << (8 * (cmode >> 1))) if not cmode & 1 else None
    if cmode < 12:
        return (".i16", imm8 << (8 * ((cmode >> 1) & 1))) if not cmode & 1 else None
    if cmode == 12:
        return ".i32", imm8 << 8 | 0xFF
    if cmode == 13:
        return ".i32", imm8 << 16 | 0xFFFF
    if cmode == 14:
        return ".i8", imm8
    return ".f32", vfp_expand_imm(imm8, double=False)


def decode_neon(word: int, address: int, cond: int | None) -> Insn:
    c = AL if cond is None else cond
    in_it = cond is not None

    def mk(mn: str, *ops, dt: str = "") -> Insn:
        return Insn(address, 4, word, mn, ops, c, dt=dt, in_it=in_it, simd=True)

    u = word >> 28 & 1
    a = (word >> 19) & 0x1F
    if not a & 0x10:
        found = _three_same(word, u, mk)
    elif a & 0b10111 == 0b10000 and word & 0x90 == 0x10:
        found = _modified_imm(word, mk)
    elif u and a & 0b10110 == 0b10110 and not word >> 11 & 1 and not word & 0x10:
        found = _two_misc(word, mk)
    else:
        found = None
    return found or undefined(address, 4, word)


def _vreg(word: int, q: bool, hi: int, lo: int):
    """The register of the field ``hi:lo`` (D:Vd, N:Vn, M:Vm), or None for an odd Q."""
    n = (word >> hi & 1) << 4 | (word >> lo & 0xF)
    if q:
        return None if n & 1 else QReg(n >> 1)
    return DReg(n)


def _regs(word: int, q: bool, *fields: tuple[int, int]):
    regs = [_vreg(word, q, hi, lo) for hi, lo in fields]
    return None if None in regs else regs


def _three_same(word: int, u: int, mk) -> Insn | None:  # A7.4.1
    a, b, c = (word >> 8) & 0xF, word >> 4 & 1, (word >> 20) & 3
    q = bool(word >> 6 & 1)
    regs = _regs(word, q, (22, 12), (7, 16), (5, 0))
    if regs is None:
        return None
    if a == 0b0001 and b and not u and c == 0b10:
        return mk("vorr", *regs)
    if a == 0b1101 and not c & 1:  # sz = 1 is UNDEFINED in ARMv7 (half precision: v8.2)
        if not b and not u:
            return mk("vsub" if c & 2 else "vadd", *regs, dt=".f32")
        if b and u and not c & 2:
            return mk("vmul", *regs, dt=".f32")
    return None


def _modified_imm(word: int, mk) -> Insn | None:  # A7.4.6
    imm8 = (word >> 28 & 1) << 7 | (word >> 16 & 7) << 4 | word & 0xF
    found = adv_simd_expand_imm(word >> 5 & 1, (word >> 8) & 0xF, imm8)
    reg = _vreg(word, bool(word >> 6 & 1), 22, 12)
    if found is None or reg is None:
        return None
    dt, value = found
    imm = FPImm(value, False) if dt == ".f32" else Imm(value)
    return mk("vmov", reg, imm, dt=dt)


def _two_misc(word: int, mk) -> Insn | None:  # A7.4.5
    size, a, b = (word >> 18) & 3, (word >> 16) & 3, (word >> 6) & 0x1F
    if size != 0b10:
        return None  # only the 32-bit (f32/s32/u32) forms are decoded
    q = bool(b & 1)
    regs = _regs(word, q, (22, 12), (5, 0))
    if regs is None:
        return None
    if a == 0b01 and b >> 1 == 0b1111:  # F = 1, 111: VNEG (floating point)
        return mk("vneg", *regs, dt=".f32")
    if a == 0b11 and b >> 3 == 0b11:  # VCVT between floating point and integer
        dt = (".f32.s32", ".f32.u32", ".s32.f32", ".u32.f32")[(b >> 1) & 3]
        return mk("vcvt", *regs, dt=dt)
    return None
