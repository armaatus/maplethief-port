"""16-bit Thumb encodings (ARM ARM A6.2). All of them are decoded here.

``cond`` is the IT condition of the instruction (None outside an IT block): inside an IT
block the flag-setting 16-bit forms do not set the flags, and the instruction is
conditional.
"""

from __future__ import annotations

from recomp.decode.bits import sign_extend
from recomp.decode.insn import (
    AL,
    LR,
    PC,
    SP,
    Imm,
    Insn,
    Label,
    Mem,
    Reg,
    RegList,
    undefined,
)

_DP = "and eor lsl lsr asr adc sbc ror tst rsb cmp cmn orr mul bic mvn".split()  # A6.2.2, by opcode
_LDST_REG = ("str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh")  # A6.2.4, opB
_HINTS = {0: "nop", 1: "yield", 2: "wfe", 3: "wfi", 4: "sev"}


def _regs(mask: int, extra: int | None = None) -> RegList:
    regs = [r for r in range(8) if mask >> r & 1]
    if extra is not None:
        regs.append(extra)
    return RegList(tuple(regs))


def decode16(hw: int, address: int, cond: int | None) -> Insn:
    c = AL if cond is None else cond
    s = cond is None  # 16-bit data processing sets the flags only outside an IT block

    def mk(mn: str, *ops, setflags: bool = False, **kw) -> Insn:
        return Insn(address, 2, hw, mn, ops, c, setflags, in_it=cond is not None, **kw)

    top = hw >> 10
    if top < 0b010000:  # A6.2.1 shift (immediate), add, subtract, move, compare
        op5 = (hw >> 9) & 0x1F
        rd, rm = hw & 7, (hw >> 3) & 7
        if op5 < 0b01100:
            kind = op5 >> 2
            imm5 = (hw >> 6) & 0x1F
            if kind == 0 and imm5 == 0:
                return mk("mov", Reg(rd), Reg(rm), setflags=True)  # MOVS (register) T2
            mn = ("lsl", "lsr", "asr")[kind]
            return mk(mn, Reg(rd), Reg(rm), Imm(imm5 if kind == 0 else imm5 or 32), setflags=s)
        rn, rm3 = (hw >> 3) & 7, (hw >> 6) & 7
        if op5 == 0b01100:
            return mk("add", Reg(rd), Reg(rn), Reg(rm3), setflags=s)
        if op5 == 0b01101:
            return mk("sub", Reg(rd), Reg(rn), Reg(rm3), setflags=s)
        if op5 == 0b01110:
            return mk("add", Reg(rd), Reg(rn), Imm(rm3), setflags=s)
        if op5 == 0b01111:
            return mk("sub", Reg(rd), Reg(rn), Imm(rm3), setflags=s)
        rdn, imm8 = (hw >> 8) & 7, hw & 0xFF
        op = (op5 >> 2) & 3
        if op == 0:
            return mk("mov", Reg(rdn), Imm(imm8), setflags=s)
        if op == 1:
            return mk("cmp", Reg(rdn), Imm(imm8), setflags=True)
        return mk(("add", "sub")[op - 2], Reg(rdn), Imm(imm8), setflags=s)

    if top == 0b010000:  # A6.2.2 data processing
        opc = (hw >> 6) & 0xF
        rdn, rm = hw & 7, (hw >> 3) & 7
        mn = _DP[opc]
        if mn in ("tst", "cmp", "cmn"):
            return mk(mn, Reg(rdn), Reg(rm), setflags=True)
        if mn == "rsb":
            return mk("rsb", Reg(rdn), Reg(rm), Imm(0), setflags=s)
        if mn == "mul":
            return mk("mul", Reg(rdn), Reg(rm), Reg(rdn), setflags=s)
        if mn == "mvn":
            return mk("mvn", Reg(rdn), Reg(rm), setflags=s)
        return mk(mn, Reg(rdn), Reg(rm), setflags=s)

    if top == 0b010001:  # A6.2.3 special data instructions and branch and exchange
        op = (hw >> 6) & 0xF
        rdn = (hw & 7) | ((hw >> 4) & 8)
        rm = (hw >> 3) & 0xF
        if op < 4:
            if rm == SP:  # ADD (SP plus register) T1: add rdm, sp, rdm
                return mk("add", Reg(rdn), Reg(SP), Reg(rdn))
            return mk("add", Reg(rdn), Reg(rm))
        if op < 8:
            if op == 4:
                return undefined(address, 2, hw)
            return mk("cmp", Reg(rdn), Reg(rm), setflags=True)
        if op < 12:
            return mk("mov", Reg(rdn), Reg(rm))
        if hw & 7:
            return undefined(address, 2, hw)
        return mk("bx" if op < 14 else "blx", Reg(rm))

    if hw >> 11 == 0b01001:  # LDR (literal) T1
        return mk("ldr", Reg((hw >> 8) & 7), Mem(PC, (hw & 0xFF) * 4))

    opa = hw >> 12
    if opa == 0b0101:  # A6.2.4 load/store, register offset
        rt, rn, rm = hw & 7, (hw >> 3) & 7, (hw >> 6) & 7
        return mk(_LDST_REG[(hw >> 9) & 7], Reg(rt), Mem(rn, index=rm))
    if opa in (0b0110, 0b0111, 0b1000):
        rt, rn, imm5 = hw & 7, (hw >> 3) & 7, (hw >> 6) & 0x1F
        load = (hw >> 11) & 1
        scale = {0b0110: 4, 0b0111: 1, 0b1000: 2}[opa]
        mn = {0b0110: "str", 0b0111: "strb", 0b1000: "strh"}[opa]
        if load:
            mn = "ld" + mn[2:]
        return mk(mn, Reg(rt), Mem(rn, imm5 * scale))
    if opa == 0b1001:  # STR/LDR (SP-relative)
        rt = (hw >> 8) & 7
        return mk("ldr" if hw >> 11 & 1 else "str", Reg(rt), Mem(SP, (hw & 0xFF) * 4))
    if hw >> 11 == 0b10100:  # ADR T1
        return mk("adr", Reg((hw >> 8) & 7), Imm((hw & 0xFF) * 4))
    if hw >> 11 == 0b10101:  # ADD (SP plus immediate) T1
        return mk("add", Reg((hw >> 8) & 7), Reg(SP), Imm((hw & 0xFF) * 4))
    if opa == 0b1011:
        return _misc(hw, address, mk, s)
    if hw >> 11 in (0b11000, 0b11001) and hw & 0xFF == 0:
        return undefined(address, 2, hw)  # empty register list: UNPREDICTABLE
    if hw >> 11 == 0b11000:  # STM T1 (always writes back)
        return mk("stm", Reg((hw >> 8) & 7), _regs(hw & 0xFF), wback=True)
    if hw >> 11 == 0b11001:  # LDM T1: writes back unless rn is in the list
        rn = (hw >> 8) & 7
        return mk("ldm", Reg(rn), _regs(hw & 0xFF), wback=not (hw >> rn & 1))
    if opa == 0b1101:
        bcond = (hw >> 8) & 0xF
        if bcond == 0b1110:
            if hw & 0xFF == 0xFE:
                return mk("trap")  # udf #254, the compiler's __builtin_trap (Capstone: trap)
            return mk("udf", Imm(hw & 0xFF))
        if bcond == 0b1111:
            return mk("svc", Imm(hw & 0xFF))
        if cond is not None:
            return undefined(address, 2, hw)  # B T1 is UNPREDICTABLE in an IT block
        target = address + 4 + sign_extend((hw & 0xFF) << 1, 9)
        return Insn(address, 2, hw, "b", (Label(target & 0xFFFFFFFF),), bcond)
    if hw >> 11 == 0b11100:  # B T2
        return mk("b", Label((address + 4 + sign_extend((hw & 0x7FF) << 1, 12)) & 0xFFFFFFFF))
    return undefined(address, 2, hw)


def _misc(hw: int, address: int, mk, s: bool) -> Insn:  # A6.2.5
    if (hw >> 7) & 0x1F == 0b00000:
        return mk("add", Reg(SP), Imm((hw & 0x7F) * 4))
    if (hw >> 7) & 0x1F == 0b00001:
        return mk("sub", Reg(SP), Imm((hw & 0x7F) * 4))
    if hw & 0x0500 == 0x0100:  # CBZ / CBNZ
        offset = ((hw >> 9) & 1) << 6 | ((hw >> 3) & 0x1F) << 1
        mn = "cbnz" if hw >> 11 & 1 else "cbz"
        return mk(mn, Reg(hw & 7), Label((address + 4 + offset) & 0xFFFFFFFF))
    op = (hw >> 6) & 0x3F
    rd, rm = hw & 7, (hw >> 3) & 7
    if op in (0b001000, 0b001001, 0b001010, 0b001011):
        mn = ("sxth", "sxtb", "uxth", "uxtb")[op & 3]
        return mk(mn, Reg(rd), Reg(rm))
    if (hw >> 9) & 3 == 0b10 and hw & 0x1FF == 0:
        return undefined(address, 2, hw)  # push/pop of no register: UNPREDICTABLE
    if (hw >> 9) & 7 == 0b010:
        return mk("push", _regs(hw & 0xFF, LR if hw >> 8 & 1 else None))
    if (hw >> 9) & 7 == 0b110:
        return mk("pop", _regs(hw & 0xFF, PC if hw >> 8 & 1 else None))
    if op in (0b101000, 0b101001, 0b101011):
        mn = {0b101000: "rev", 0b101001: "rev16", 0b101011: "revsh"}[op]
        return mk(mn, Reg(rd), Reg(rm))
    if (hw >> 8) & 0xF == 0b1110:
        return mk("bkpt", Imm(hw & 0xFF))
    if (hw >> 8) & 0xF == 0b1111:
        mask = hw & 0xF
        if mask:
            first = (hw >> 4) & 0xF
            count = 4 - ((mask & -mask).bit_length() - 1)
            letters = "".join(
                "t" if (mask >> (4 - k)) & 1 == first & 1 else "e" for k in range(1, count)
            )
            return Insn(address, 2, hw, "it" + letters, (Imm(first), Imm(mask)))
        hint = _HINTS.get((hw >> 4) & 0xF)
        if hint:
            return mk(hint)
    return undefined(address, 2, hw)
