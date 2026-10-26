"""32-bit Thumb-2 integer encodings (ARM ARM A6.3); the coprocessor space goes to vfp.py.

Decoded so far (the #30 prototype): load/store multiple (incl. push.w/pop.w), LDRD/STRD,
TBB/TBH, data processing with modified immediate, plain binary immediate and shifted
register, register-controlled shifts, extends, CLZ/REV, every single load/store form, the
multiplies, long multiplies and divides, and every branch plus the hints and barriers.
#33 fills in what its census finds missing (e.g. saturation, packing, exclusives).
"""

from __future__ import annotations

from recomp.decode.bits import decode_imm_shift, sign_extend, thumb_expand_imm_c
from recomp.decode.insn import (
    AL,
    PC,
    SP,
    Imm,
    Insn,
    Label,
    Mem,
    Reg,
    RegList,
    Shifted,
    SysReg,
    undefined,
)

_DP_MOD = {  # op (hw1[8:5]) -> mnemonic; the compare forms are taken when rd == 15 and S
    0b0000: "and",
    0b0001: "bic",
    0b0010: "orr",
    0b0011: "orn",
    0b0100: "eor",
    0b1000: "add",
    0b1010: "adc",
    0b1011: "sbc",
    0b1101: "sub",
    0b1110: "rsb",
}
_COMPARE_OF = {"and": "tst", "eor": "teq", "add": "cmn", "sub": "cmp"}
# Capstone spells these with ".w" (the narrow form exists): immediate and register forms.
_WIDE_IMM = frozenset({"add", "sub", "cmp", "cmn", "tst", "teq", "mov", "rsb"})
_WIDE_REG = frozenset(
    {"and", "bic", "orr", "eor", "adc", "sbc", "add", "sub", "cmp", "cmn", "tst", "teq", "mov"}
    | {"mvn", "lsl", "lsr", "asr", "ror"}
)
_BARRIERS = {0b0100: "dsb", 0b0101: "dmb", 0b0110: "isb"}
_BARRIER_OPTS = {15: "sy", 14: "st", 11: "ish", 10: "ishst", 7: "nsh", 6: "nshst", 3: "osh"}


def _reglist(mask: int) -> RegList:
    return RegList(tuple(r for r in range(16) if mask >> r & 1))


def decode32(hw1: int, hw2: int, address: int, cond: int | None) -> Insn:
    from recomp.decode import vfp

    raw = hw1 << 16 | hw2
    c = AL if cond is None else cond
    in_it = cond is not None

    def mk(mn: str, *ops, setflags: bool = False, wide: bool = False, **kw) -> Insn:
        return Insn(address, 4, raw, mn, ops, c, setflags, wide, in_it=in_it, **kw)

    op1 = (hw1 >> 11) & 3
    op2 = (hw1 >> 4) & 0x7F
    if op1 == 1:
        if op2 & 0b1100100 == 0b0000000:
            return _ldm_stm(hw1, hw2, mk) or undefined(address, 4, raw)
        if op2 & 0b1100100 == 0b0000100:
            return _dual_table(hw1, hw2, address, mk) or undefined(address, 4, raw)
        if op2 & 0b1100000 == 0b0100000:
            return _dp_shifted(hw1, hw2, mk) or undefined(address, 4, raw)
        return vfp.decode_vfp(raw, address, cond)
    if op1 == 2:
        if hw2 >> 15 == 0:
            if op2 & 0b0100000 == 0:
                return _dp_modified(hw1, hw2, mk) or undefined(address, 4, raw)
            return _dp_plain(hw1, hw2, address, mk) or undefined(address, 4, raw)
        return _branch_misc(hw1, hw2, address, cond, mk) or undefined(address, 4, raw)
    # op1 == 3
    if op2 & 0b1000000:
        return vfp.decode_vfp(raw, address, cond)
    if op2 & 0b1110001 == 0b0000000 or op2 & 0b1100000 == 0b0000000 and op2 & 1:
        return _ldst_single(hw1, hw2, address, mk) or undefined(address, 4, raw)
    if op2 & 0b1110001 == 0b0010000:
        return vfp.decode_vfp(raw, address, cond)  # Advanced SIMD element/structure: undefined
    if op2 & 0b1110000 == 0b0100000:
        return _dp_register(hw1, hw2, mk) or undefined(address, 4, raw)
    if op2 & 0b1111000 == 0b0110000:
        return _multiply(hw1, hw2, mk) or undefined(address, 4, raw)
    if op2 & 0b1111000 == 0b0111000:
        return _long_multiply(hw1, hw2, mk) or undefined(address, 4, raw)
    return undefined(address, 4, raw)


def _ldm_stm(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.5
    op = (hw1 >> 7) & 3
    wback = bool(hw1 >> 5 & 1)
    load = hw1 >> 4 & 1
    rn = hw1 & 0xF
    mask = hw2
    # UNPREDICTABLE lists: sp in any list, pc in a store, pc and lr in a load, < 2 registers
    if mask & 0x2000 or (not load and mask & 0x8000) or (load and mask & 0xC000 == 0xC000):
        return None
    if bin(mask).count("1") < 2:
        return None
    regs = _reglist(mask)
    if op == 0b01:
        if load and rn == SP and wback:
            return mk("pop", regs, wide=True)
        return mk("ldm" if load else "stm", Reg(rn), regs, wide=True, wback=wback)
    if op == 0b10:
        if not load and rn == SP and wback:
            return mk("push", regs, wide=True)
        return mk("ldmdb" if load else "stmdb", Reg(rn), regs, wback=wback)
    return None


def _dual_table(hw1: int, hw2: int, address: int, mk) -> Insn | None:  # A6.3.6
    op1, op2 = (hw1 >> 7) & 3, (hw1 >> 4) & 3
    rn = hw1 & 0xF
    if op1 == 0b01 and op2 == 0b01:
        op3 = (hw2 >> 4) & 0xF
        if op3 in (0, 1) and hw2 & 0xFF00 != 0xF000:
            return None
        if op3 == 0b0000:
            return mk("tbb", Mem(rn, index=hw2 & 0xF))
        if op3 == 0b0001:
            return mk("tbh", Mem(rn, index=hw2 & 0xF, shift=1))
        return None  # ldrex{b,h,d} (#33)
    if op1 & 2 or op2 & 2:
        pre, add, wback = bool(hw1 >> 8 & 1), bool(hw1 >> 7 & 1), bool(hw1 >> 5 & 1)
        disp = (hw2 & 0xFF) * 4 * (1 if add else -1)
        rt, rt2 = (hw2 >> 12) & 0xF, (hw2 >> 8) & 0xF
        mn = "ldrd" if hw1 >> 4 & 1 else "strd"
        if rn == PC and not wback:
            return mk(mn, Reg(rt), Reg(rt2), Mem(PC, disp))
        mem = Mem(rn, disp, subtract=not add and not disp, pre=pre, wback=wback or not pre)
        return mk(mn, Reg(rt), Reg(rt2), mem)
    return None  # strex / ldrex (#33)


def _shift_operand(rm: int, kind: int, imm5: int):
    name, amount = decode_imm_shift(kind, imm5)
    if name == "lsl" and amount == 0:
        return Reg(rm)
    return Shifted(rm, name, amount)


def _dp_shifted(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.11
    op = (hw1 >> 5) & 0xF
    s = bool(hw1 >> 4 & 1)
    rn, rd, rm = hw1 & 0xF, (hw2 >> 8) & 0xF, hw2 & 0xF
    imm5 = ((hw2 >> 12) & 7) << 2 | (hw2 >> 6) & 3
    kind = (hw2 >> 4) & 3
    if op == 0b0010 and rn == PC:  # MOV (register) and the immediate-shift aliases
        name, amount = decode_imm_shift(kind, imm5)
        if name == "lsl" and amount == 0:
            return mk("mov", Reg(rd), Reg(rm), setflags=s, wide=True)
        if name == "rrx":
            return mk("rrx", Reg(rd), Reg(rm), setflags=s)
        return mk(name, Reg(rd), Reg(rm), Imm(amount), setflags=s, wide=True)
    if op == 0b0011 and rn == PC:
        return mk("mvn", Reg(rd), _shift_operand(rm, kind, imm5), setflags=s, wide=True)
    mn = _DP_MOD.get(op)
    if mn is None:
        return None  # pkhbt/pkhtb (#33)
    operand = _shift_operand(rm, kind, imm5)
    if rd == PC and s and mn in _COMPARE_OF:
        cmp = _COMPARE_OF[mn]
        return mk(cmp, Reg(rn), operand, setflags=True, wide=True)
    return mk(mn, Reg(rd), Reg(rn), operand, setflags=s, wide=mn in _WIDE_REG)


def _dp_modified(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.1
    op = (hw1 >> 5) & 0xF
    s = bool(hw1 >> 4 & 1)
    rn, rd = hw1 & 0xF, (hw2 >> 8) & 0xF
    imm12 = ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)
    expanded = thumb_expand_imm_c(imm12)
    if expanded is None:
        return None
    imm = Imm(*expanded)
    if op == 0b0010 and rn == PC:
        return mk("mov", Reg(rd), imm, setflags=s, wide=True)
    if op == 0b0011 and rn == PC:
        return mk("mvn", Reg(rd), imm, setflags=s)
    mn = _DP_MOD.get(op)
    if mn is None:
        return None
    if rd == PC and s and mn in _COMPARE_OF:
        cmp = _COMPARE_OF[mn]
        return mk(cmp, Reg(rn), imm, setflags=True, wide=True)
    return mk(mn, Reg(rd), Reg(rn), imm, setflags=s, wide=mn in _WIDE_IMM)


def _dp_plain(hw1: int, hw2: int, address: int, mk) -> Insn | None:  # A6.3.3
    op = (hw1 >> 4) & 0x1F
    rn, rd = hw1 & 0xF, (hw2 >> 8) & 0xF
    imm12 = ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)
    if op in (0b00000, 0b01010):  # with rn == pc this is ADR (Capstone: addw/subw rd, pc, #n)
        return mk("addw" if op == 0 else "subw", Reg(rd), Reg(rn), Imm(imm12))
    if op in (0b00100, 0b01100):
        imm16 = rn << 12 | imm12
        return mk("movw" if op == 0b00100 else "movt", Reg(rd), Imm(imm16))
    if hw1 >> 10 & 1 or hw2 >> 5 & 1:
        return None  # the (0) bits of SSAT/SBFX/BFI/USAT/UBFX
    lsb = ((hw2 >> 12) & 7) << 2 | (hw2 >> 6) & 3
    field = hw2 & 0x1F
    if op in (0b10100, 0b11100):
        return mk("sbfx" if op == 0b10100 else "ubfx", Reg(rd), Reg(rn), Imm(lsb), Imm(field + 1))
    if op == 0b10110:
        if field < lsb:
            return None
        if rn == PC:
            return mk("bfc", Reg(rd), Imm(lsb), Imm(field - lsb + 1))
        return mk("bfi", Reg(rd), Reg(rn), Imm(lsb), Imm(field - lsb + 1))
    return None  # ssat/usat (#33)


def _branch_misc(hw1: int, hw2: int, address: int, cond: int | None, mk) -> Insn | None:
    op1 = (hw2 >> 12) & 7  # A6.3.4
    s = (hw1 >> 10) & 1
    j1, j2 = (hw2 >> 13) & 1, (hw2 >> 11) & 1
    if op1 & 0b101 == 0b000:
        if (hw1 >> 7) & 7 != 0b111:  # B T3 (conditional; not allowed in an IT block)
            if cond is not None:
                return None
            offset = sign_extend(
                s << 20 | j2 << 19 | j1 << 18 | (hw1 & 0x3F) << 12 | (hw2 & 0x7FF) << 1, 21
            )
            return Insn(
                address,
                4,
                hw1 << 16 | hw2,
                "b",
                (Label((address + 4 + offset) & 0xFFFFFFFF),),
                (hw1 >> 6) & 0xF,
                wide=True,
            )
        op = (hw1 >> 4) & 0x7F
        if op in (0b0111010, 0b0111011) and (hw1 & 0xF != 0xF or hw2 >> 13 & 1):
            return None
        if op == 0b0111010 and (hw2 >> 8) & 7 == 0:  # hints
            hint = {0: "nop", 1: "yield", 2: "wfe", 3: "wfi", 4: "sev"}.get(hw2 & 0xFF)
            return mk(hint, wide=True) if hint else None
        if op == 0b0111011 and (hw2 >> 8) & 0xF == 0xF:
            barrier = _BARRIERS.get((hw2 >> 4) & 0xF)
            if barrier:
                opt = _BARRIER_OPTS.get(hw2 & 0xF)
                return mk(barrier, SysReg(opt)) if opt else mk(barrier, Imm(hw2 & 0xF))
        if op == 0b1111111 and op1 == 0b010:
            return mk("udf", Imm((hw1 & 0xF) << 12 | (hw2 & 0xFFF)), wide=True)
        return None  # msr/mrs/... (#33)
    i1, i2 = 1 - (j1 ^ s), 1 - (j2 ^ s)
    offset = sign_extend(
        s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1, 25
    )
    if op1 & 0b101 == 0b001:  # B T4
        return mk("b", Label((address + 4 + offset) & 0xFFFFFFFF), wide=True)
    if op1 & 0b101 == 0b101:  # BL
        return mk("bl", Label((address + 4 + offset) & 0xFFFFFFFF))
    if hw2 & 1:
        return None
    return mk(
        "blx", Label((((address + 4) & ~3) + offset) & 0xFFFFFFFF)
    )  # BLX (immediate): Thumb -> ARM


def _ldst_single(hw1: int, hw2: int, address: int, mk) -> Insn | None:  # A6.3.7-A6.3.10
    load = hw1 >> 4 & 1
    signed = hw1 >> 8 & 1
    size = (hw1 >> 5) & 3
    if size == 3 or signed and (not load or size == 2):
        return None
    rn, rt = hw1 & 0xF, (hw2 >> 12) & 0xF
    mn = ("ldr" if load else "str") + ("s" if signed else "") + ("b", "h", "")[size]
    if load and rt == PC and size != 2:
        return None  # pld / pli (#33)
    if rn == PC:
        if not load:
            return None
        add = hw1 >> 7 & 1
        disp = (hw2 & 0xFFF) * (1 if add else -1)
        return mk(
            mn, Reg(rt), Mem(PC, disp, subtract=not add and not disp, neg_hex=True), wide=True
        )
    if hw1 >> 7 & 1:  # imm12, positive offset
        return mk(mn, Reg(rt), Mem(rn, hw2 & 0xFFF), wide=True)
    if hw2 >> 11 & 1:  # imm8: P U W
        pre, add, wback = bool(hw2 >> 10 & 1), bool(hw2 >> 9 & 1), bool(hw2 >> 8 & 1)
        if pre and add and not wback:
            return mk(mn + "t", Reg(rt), Mem(rn, hw2 & 0xFF))  # unprivileged form
        if not pre and not wback:
            return None
        disp = (hw2 & 0xFF) * (1 if add else -1)
        mem = Mem(rn, disp, subtract=not add and not disp, pre=pre, wback=wback, neg_hex=pre)
        return mk(mn, Reg(rt), mem)
    if (hw2 >> 6) & 0x3F == 0:
        return mk(mn, Reg(rt), Mem(rn, index=hw2 & 0xF, shift=(hw2 >> 4) & 3), wide=True)
    return None


def _dp_register(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.12
    if hw2 >> 12 != 0b1111:
        return None
    op1, op2 = (hw1 >> 4) & 0xF, (hw2 >> 4) & 0xF
    rn, rd, rm = hw1 & 0xF, (hw2 >> 8) & 0xF, hw2 & 0xF
    if op2 == 0 and op1 < 8:  # LSL/LSR/ASR/ROR (register)
        name = ("lsl", "lsr", "asr", "ror")[op1 >> 1]
        return mk(name, Reg(rd), Reg(rn), Reg(rm), setflags=bool(op1 & 1), wide=True)
    if op2 & 0b1100 == 0b1000 and op1 < 6:  # extends, with optional rotation
        names = ("sxth", "uxth", "sxtb16", "uxtb16", "sxtb", "uxtb")
        rot = ((hw2 >> 4) & 3) * 8
        src = Shifted(rm, "ror", rot) if rot else Reg(rm)
        if rn == PC:
            return mk(names[op1], Reg(rd), src, wide=op1 in (0, 1, 4, 5))
        return mk(names[op1].replace("xt", "xta"), Reg(rd), Reg(rn), src)
    if op1 & 0b1100 == 0b1000 and op2 & 0b1100 == 0b1000:  # misc operations
        key = (op1 & 3, op2 & 3)
        names = {(1, 0): "rev", (1, 1): "rev16", (1, 2): "rbit", (1, 3): "revsh", (3, 0): "clz"}
        mn = names.get(key)
        if mn is None or rn != rm:
            return None
        return mk(mn, Reg(rd), Reg(rm), wide=mn in ("rev", "rev16", "revsh"))
    return None  # parallel add/subtract, qadd, sel (#33)


def _multiply(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.16
    op1, op2 = (hw1 >> 4) & 7, (hw2 >> 4) & 3
    rn, ra, rd, rm = hw1 & 0xF, hw2 >> 12, (hw2 >> 8) & 0xF, hw2 & 0xF
    if (hw2 >> 6) & 3:
        return None
    if op1 == 0:
        if op2 == 0:
            if ra == PC:
                return mk("mul", Reg(rd), Reg(rn), Reg(rm))
            return mk("mla", Reg(rd), Reg(rn), Reg(rm), Reg(ra))
        if op2 == 1:
            return mk("mls", Reg(rd), Reg(rn), Reg(rm), Reg(ra))
        return None
    if op1 in (0b101, 0b110) and op2 > 1:
        return None
    if op1 == 0b101:
        mn = "smmul" if ra == PC else "smmla"
        ops = (Reg(rd), Reg(rn), Reg(rm)) + (() if ra == PC else (Reg(ra),))
        return mk(mn + ("r" if op2 & 1 else ""), *ops)
    if op1 == 0b110:
        return mk("smmls" + ("r" if op2 & 1 else ""), Reg(rd), Reg(rn), Reg(rm), Reg(ra))
    return None  # smla<x><y>, smuad, ... (#33)


def _long_multiply(hw1: int, hw2: int, mk) -> Insn | None:  # A6.3.17
    op1, op2 = (hw1 >> 4) & 7, (hw2 >> 4) & 0xF
    rn, lo, hi, rm = hw1 & 0xF, hw2 >> 12, (hw2 >> 8) & 0xF, hw2 & 0xF
    names = {(0, 0): "smull", (2, 0): "umull", (4, 0): "smlal", (6, 0): "umlal"}
    if (op1, op2) in names:
        return mk(names[op1, op2], Reg(lo), Reg(hi), Reg(rn), Reg(rm))
    if op2 == 0b1111 and op1 in (1, 3) and lo == PC:
        return mk("sdiv" if op1 == 1 else "udiv", Reg(hi), Reg(rn), Reg(rm))
    return None
