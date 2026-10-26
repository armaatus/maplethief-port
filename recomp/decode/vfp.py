"""VFP encodings (ARM ARM A7); Advanced SIMD (NEON) is in neon.py.

``word`` is the instruction as one 32-bit value: ``hw1 << 16 | hw2`` in Thumb, where the top
nibble is 1110 (the ARM encodings' condition field position). Decoded so far (the #30
prototype): VLDR/VSTR, VLDM/VSTM/VPUSH/VPOP, VMOV between core and single registers and the
64-bit form, VMRS/VMSR, and the data-processing instructions (arithmetic, multiply-
accumulate, VMOV register/immediate, VABS/VNEG/VSQRT, VCMP/VCMPE, all VCVT except fixed
point and half precision), and VMOV between two core and two single registers. Advanced SIMD
data processing goes to :mod:`recomp.decode.neon`.
"""

from __future__ import annotations

from recomp.decode import neon
from recomp.decode.bits import vfp_expand_imm
from recomp.decode.insn import (
    AL,
    PC,
    SP,
    DReg,
    FPImm,
    Imm,
    Insn,
    Mem,
    Reg,
    SReg,
    SysReg,
    VRegList,
    undefined,
)

_ARITH = {  # opc1 (bits 23, 21, 20) and opc3 bit 6 -> mnemonic
    (0b0000, 0): "vmla",
    (0b0000, 1): "vmls",
    (0b0001, 0): "vnmls",
    (0b0001, 1): "vnmla",
    (0b0010, 0): "vmul",
    (0b0010, 1): "vnmul",
    (0b0011, 0): "vadd",
    (0b0011, 1): "vsub",
    (0b1000, 0): "vdiv",
}


def _sreg(v: int, bit: int) -> SReg:
    return SReg(v << 1 | bit)


def _dreg(v: int, bit: int) -> DReg:
    return DReg(bit << 4 | v)


def decode_vfp(word: int, address: int, cond: int | None) -> Insn:
    c = AL if cond is None else cond
    in_it = cond is not None

    def mk(mn: str, *ops, dt: str = "", wback: bool = False) -> Insn:
        return Insn(address, 4, word, mn, ops, c, dt=dt, wback=wback, in_it=in_it)

    if neon.is_neon_dp(word):
        return neon.decode_neon(word, address, cond)
    if word >> 28 != 0b1110 or (word >> 9) & 7 != 0b101:
        return undefined(address, 4, word)  # NEON loads/stores, other coprocessors
    top = (word >> 24) & 0xF
    if top == 0b1110:
        if word >> 4 & 1:
            return _transfer(word, mk) or undefined(address, 4, word)
        return _data_processing(word, mk) or undefined(address, 4, word)
    if top in (0b1100, 0b1101):
        return _load_store(word, mk) or undefined(address, 4, word)
    return undefined(address, 4, word)


def _regs3(word: int, double: bool):
    d, n, m = (word >> 12) & 0xF, (word >> 16) & 0xF, word & 0xF
    bd, bn, bm = (word >> 22) & 1, (word >> 7) & 1, (word >> 5) & 1
    if double:
        return _dreg(d, bd), _dreg(n, bn), _dreg(m, bm)
    return _sreg(d, bd), _sreg(n, bn), _sreg(m, bm)


def _data_processing(word: int, mk) -> Insn | None:  # A7.5
    opc1 = (word >> 20) & 0b1011
    opc2 = (word >> 16) & 0xF
    opc3 = (word >> 6) & 3
    double = bool(word >> 8 & 1)
    dt = ".f64" if double else ".f32"
    rd, rn, rm = _regs3(word, double)
    mn = _ARITH.get((opc1, opc3 & 1))
    if mn:
        return mk(mn, rd, rn, rm, dt=dt)
    if opc1 != 0b1011:
        return None  # fused multiply-add (VFPv4, not in ARMv7 VFPv3)
    if opc3 & 1 == 0:
        if word & 0xF0:
            return None  # the (0) bits of VMOV (immediate)
        imm8 = opc2 << 4 | word & 0xF
        return mk("vmov", rd, FPImm(vfp_expand_imm(imm8, double), double), dt=dt)
    if opc2 == 0b0000:
        return mk("vmov" if opc3 == 0b01 else "vabs", rd, rm, dt=dt)
    if opc2 == 0b0001:
        return mk("vneg" if opc3 == 0b01 else "vsqrt", rd, rm, dt=dt)
    if opc2 in (0b0100, 0b0101):
        mn = "vcmpe" if word >> 7 & 1 else "vcmp"
        if opc2 == 0b0101:
            if word & 0x2F:
                return None  # the (0) bits of VCMP{E} with zero
            return mk(mn, rd, Imm(0), dt=dt)
        return mk(mn, rd, rm, dt=dt)
    vd, bd, vm, bm = (word >> 12) & 0xF, (word >> 22) & 1, word & 0xF, (word >> 5) & 1
    if opc2 == 0b0111 and opc3 == 0b11:  # VCVT between double and single
        if double:
            return mk("vcvt", _sreg(vd, bd), _dreg(vm, bm), dt=".f32.f64")
        return mk("vcvt", _dreg(vd, bd), _sreg(vm, bm), dt=".f64.f32")
    if opc2 == 0b1000:  # VCVT to floating point from integer (source is always single)
        src = ".s32" if word >> 7 & 1 else ".u32"
        dst = _dreg(vd, bd) if double else _sreg(vd, bd)
        return mk("vcvt", dst, _sreg(vm, bm), dt=dt + src)
    if opc2 in (0b1100, 0b1101):  # VCVT{R} to integer (destination is always single)
        to = ".s32" if opc2 & 1 else ".u32"
        src = _dreg(vm, bm) if double else _sreg(vm, bm)
        return mk("vcvt" if word >> 7 & 1 else "vcvtr", _sreg(vd, bd), src, dt=to + dt)
    return None  # half precision and fixed point conversions: the binary has none


def _transfer(word: int, mk) -> Insn | None:  # A7.8
    load = word >> 20 & 1
    a = (word >> 21) & 7
    rt = (word >> 12) & 0xF
    if word >> 8 & 1:
        return None  # scalar moves: the binary has none
    if a == 0b000:
        if word & 0x7F != 0x10:
            return None  # the (0) bits of VMOV between a core and a single register
        sn = _sreg((word >> 16) & 0xF, (word >> 7) & 1)
        return mk("vmov", Reg(rt), sn) if load else mk("vmov", sn, Reg(rt))
    if a == 0b111 and (word >> 16) & 0xF == 0b0001 and word & 0xFF == 0x10:
        if load:
            dest = SysReg("apsr_nzcv") if rt == PC else Reg(rt)
            return mk("vmrs", dest, SysReg("fpscr"))
        return mk("vmsr", SysReg("fpscr"), Reg(rt))
    return None


def _load_store(word: int, mk) -> Insn | None:  # A7.6, A7.9
    opcode = (word >> 20) & 0x1F
    double = bool(word >> 8 & 1)
    rn = (word >> 16) & 0xF
    vd, bd = (word >> 12) & 0xF, (word >> 22) & 1
    if opcode & 0b11110 == 0b00100:  # 64-bit transfers
        if word & 0xD0 != 0x10:
            return None  # bits 7:6 are 00 and bit 4 is 1
        to_core = word >> 20 & 1
        rt, rt2 = (word >> 12) & 0xF, (word >> 16) & 0xF
        vm, bm = word & 0xF, (word >> 5) & 1
        if double:
            dm = _dreg(vm, bm)
            if to_core:
                return mk("vmov", Reg(rt), Reg(rt2), dm)
            return mk("vmov", dm, Reg(rt), Reg(rt2))
        m = vm << 1 | bm
        if m == 31:
            return None  # UNPREDICTABLE: the second single would be s32
        sm, sm1 = SReg(m), SReg(m + 1)
        if to_core:
            return mk("vmov", Reg(rt), Reg(rt2), sm, sm1)
        return mk("vmov", sm, sm1, Reg(rt), Reg(rt2))
    pre, add, wback, load = (word >> 24) & 1, (word >> 23) & 1, (word >> 21) & 1, word >> 20 & 1
    imm8 = word & 0xFF
    first = (bd << 4 | vd) if double else (vd << 1 | bd)
    if pre and not wback:  # VLDR / VSTR
        reg = DReg(first) if double else SReg(first)
        mem = Mem(rn, imm8 * 4 * (1 if add else -1), subtract=not add and not imm8)
        return mk("vldr" if load else "vstr", reg, mem)
    if pre == add:
        return None
    count = imm8 // 2 if double else imm8
    if count == 0 or (double and imm8 & 1) or first + count > 32 or (double and count > 16):
        return None  # fldmx/fstmx, or a list past the last register: UNPREDICTABLE
    regs = VRegList(double, first, count)
    if rn == SP and wback and (load and not pre or not load and pre):
        return mk("vpop" if load else "vpush", regs)
    mn = ("vldm" if load else "vstm") + ("db" if pre else "ia")
    return mk(mn, Reg(rn), regs, wback=bool(wback))
