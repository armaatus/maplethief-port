"""A32 (ARM state) encodings (ARM ARM A5), for the binary's one ARM function (#33).

The armv7 slice has a single ARM-mode function, the C runtime's ``start`` stub at 0x27E0
(ANALYSIS.md), which uses data processing, single loads, a conditional branch, ``blx #imm``
into Thumb and a ``b`` to an import stub. This decoder covers those classes and their close
relatives, so that a hand-written A32 snippet can test each of them:

- data processing with an immediate (ARMExpandImm_C carry) or an immediate-shifted
  register, all sixteen opcodes; the MOV forms with a shift are ``lsl``/``lsr``/``asr``/
  ``ror``/``rrx`` (Capstone's spelling), and with a register-controlled shift
  ``lsl rd, rm, rs``; ``movw``/``movt``
- the multiplies ``mul``/``mla``/``mls`` and the long ``umull``/``umlal``/``smull``/
  ``smlal`` (without ``s``), ``clz``
- single loads and stores (word, byte, halfword, signed, dual) with immediate and register
  offsets, pre/post-indexed and with writeback; ``pop`` of one register (``ldr rt, [sp],
  #4``)
- ``ldm``/``stm`` (IA) and ``ldmdb``/``stmdb``, ``push``/``pop``
- ``b``/``bl``/``blx #imm``, ``bx``/``blx`` register

Anything else, notably register-shifted operands other than the MOV forms, decodes as
``<undefined>``, which the lifter traps. Every instruction carries its condition field
(``cond`` 0-14) and ``arm=True``, so a read of the PC is the address + 8.
"""

from __future__ import annotations

import struct

from recomp.decode.bits import decode_imm_shift, ror32, sign_extend
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
)

_DP = "and eor sub rsb add adc sbc rsc tst teq cmp cmn orr mov bic mvn".split()  # A5.2.1
_COMPARES = frozenset({"tst", "teq", "cmp", "cmn"})


def decode_arm(code: bytes, offset: int, address: int) -> Insn:
    """Decode the A32 instruction at ``code[offset:]`` (guest ``address``, word aligned)."""
    if offset + 4 > len(code) or address & 3:
        return _undefined(address, 0)
    (raw,) = struct.unpack_from("<I", code, offset)
    return decode_word(raw, address)


def _undefined(address: int, raw: int) -> Insn:
    return Insn(address, 4, raw, "<undefined>", arm=True)


def decode_word(raw: int, address: int) -> Insn:
    """Decode one A32 instruction word."""
    cond = raw >> 28

    def mk(mn: str, *ops, setflags: bool = False, **kw) -> Insn:
        c = AL if cond == 15 else cond
        return Insn(address, 4, raw, mn, ops, c, setflags, arm=True, **kw)

    if cond == 15:
        if raw >> 25 & 7 == 0b101:  # BLX (immediate): ARM -> Thumb
            offset = sign_extend((raw & 0xFFFFFF) << 2 | (raw >> 23 & 2), 26)
            return mk("blx", Label((address + 8 + offset) & 0xFFFFFFFF))
        return _undefined(address, raw)
    op1 = raw >> 25 & 7
    found: Insn | None = None
    if op1 == 0b000:
        found = _space_000(raw, mk)
    elif op1 == 0b001:
        found = _dp_immediate(raw, mk)
    elif op1 == 0b010 or (op1 == 0b011 and not raw >> 4 & 1):
        found = _ldst_word_byte(raw, mk)
    elif op1 == 0b100:
        found = _ldm_stm(raw, mk)
    elif op1 == 0b101:
        offset = sign_extend((raw & 0xFFFFFF) << 2, 26)
        found = mk("bl" if raw >> 24 & 1 else "b", Label((address + 8 + offset) & 0xFFFFFFFF))
    return found or _undefined(address, raw)


def _arm_expand_imm_c(imm12: int) -> Imm:
    """ARMExpandImm_C: the carry-out is bit 31 of a rotated value, unchanged if unrotated."""
    rot = 2 * (imm12 >> 8)
    value = ror32(imm12 & 0xFF, rot)
    return Imm(value, value >> 31 if rot else None)


def _operation(raw: int, operand, mk) -> Insn | None:
    """Data processing with the decoded second operand (A5.2.1 / A5.2.3)."""
    opc = raw >> 21 & 0xF
    s = bool(raw >> 20 & 1)
    rn, rd = raw >> 16 & 0xF, raw >> 12 & 0xF
    mn = _DP[opc]
    if mn in _COMPARES:
        if not s or rd != 0:
            return None
        return mk(mn, Reg(rn), operand, setflags=True)
    if rd == PC and s:
        return None  # SUBS PC, LR and friends: exception returns
    if mn in ("mov", "mvn"):
        if rn != 0:
            return None
        if mn == "mov" and isinstance(operand, Shifted):
            if operand.kind == "rrx":
                return mk("rrx", Reg(rd), Reg(operand.n), setflags=s)
            return mk(operand.kind, Reg(rd), Reg(operand.n), Imm(operand.amount), setflags=s)
        return mk(mn, Reg(rd), operand, setflags=s)
    return mk(mn, Reg(rd), Reg(rn), operand, setflags=s)


def _dp_immediate(raw: int, mk) -> Insn | None:  # A5.2.3, A5.2.10 (movw/movt)
    op = raw >> 20 & 0x1F
    if op in (0b10000, 0b10100):
        imm16 = (raw >> 4 & 0xF000) | (raw & 0xFFF)
        rd = raw >> 12 & 0xF
        if rd == PC:
            return None
        return mk("movw" if op == 0b10000 else "movt", Reg(rd), Imm(imm16))
    if op & 0b11001 == 0b10000:
        return None  # msr (immediate), hints
    return _operation(raw, _arm_expand_imm_c(raw & 0xFFF), mk)


def _space_000(raw: int, mk) -> Insn | None:
    op = raw >> 20 & 0x1F
    bit4, bit7 = raw >> 4 & 1, raw >> 7 & 1
    if bit4 and bit7:
        if raw >> 5 & 3 == 0:
            return _multiply(raw, mk) if op < 0b10000 else None  # synchronization: no
        return _extra_ldst(raw, mk)
    if op & 0b11001 == 0b10000:
        return _misc(raw, mk) if not bit7 else None  # halfword multiplies: no
    rm = raw & 0xF
    if not bit4:  # A5.2.1 register, shifted by an immediate
        name, amount = decode_imm_shift(raw >> 5 & 3, raw >> 7 & 0x1F)
        operand = Reg(rm) if name == "lsl" and amount == 0 else Shifted(rm, name, amount)
        return _operation(raw, operand, mk)
    # A5.2.2 register-shifted register: only the MOV forms (lsl rd, rm, rs ...) are decoded
    if raw >> 21 & 0xF != 0b1101 or raw >> 16 & 0xF:
        return None
    rd, rs = raw >> 12 & 0xF, raw >> 8 & 0xF
    if PC in (rd, rm, rs):
        return None
    name = ("lsl", "lsr", "asr", "ror")[raw >> 5 & 3]
    return mk(name, Reg(rd), Reg(rm), Reg(rs), setflags=bool(raw >> 20 & 1))


def _misc(raw: int, mk) -> Insn | None:  # A5.2.12
    op, op2 = raw >> 21 & 3, raw >> 4 & 7
    rm = raw & 0xF
    if op == 0b01 and op2 in (0b001, 0b011) and raw >> 8 & 0xFFF == 0xFFF:
        if op2 == 0b011 and rm == PC:
            return None
        return mk("bx" if op2 == 0b001 else "blx", Reg(rm))
    if op == 0b11 and op2 == 0b001 and raw >> 16 & 0xF == 0xF and raw >> 8 & 0xF == 0xF:
        rd = raw >> 12 & 0xF
        if PC in (rd, rm):
            return None
        return mk("clz", Reg(rd), Reg(rm))
    return None


def _multiply(raw: int, mk) -> Insn | None:  # A5.2.5
    op = raw >> 20 & 0xF
    s = bool(op & 1)
    hi, lo, rm, rn = raw >> 16 & 0xF, raw >> 12 & 0xF, raw >> 8 & 0xF, raw & 0xF
    if PC in (hi, rm, rn):
        return None
    if op >> 1 == 0b000:  # MUL: rd = bits 19:16
        if lo:
            return None
        return mk("mul", Reg(hi), Reg(rn), Reg(rm), setflags=s)
    if op >> 1 == 0b001 or op == 0b0110:
        if lo == PC:
            return None
        mn = "mls" if op == 0b0110 else "mla"
        return mk(mn, Reg(hi), Reg(rn), Reg(rm), Reg(lo), setflags=s and mn == "mla")
    names = {0b100: "umull", 0b101: "umlal", 0b110: "smull", 0b111: "smlal"}
    if op >> 1 in names and not s:  # the flag-setting long forms are not decoded
        if lo == PC or lo == hi:
            return None
        return mk(names[op >> 1], Reg(lo), Reg(hi), Reg(rn), Reg(rm))
    return None


def _mem(raw: int, offset, imm: int | None) -> Mem | None:
    """The addressing mode of a single load/store: P U W, base, and either an immediate
    (``imm``) or the register ``offset``. None for the unprivileged (P=0, W=1) forms."""
    pre, add, wback = bool(raw >> 24 & 1), bool(raw >> 23 & 1), bool(raw >> 21 & 1)
    rn = raw >> 16 & 0xF
    if not pre and wback:
        return None  # ldrt/strt
    wb = wback or not pre
    if wb and rn == PC:
        return None
    if imm is not None:
        disp = imm if add else -imm
        return Mem(rn, disp, subtract=not add and not imm, pre=pre, wback=wback)
    rm, shift = offset
    return Mem(rn, index=rm, shift=shift, subtract=not add, pre=pre, wback=wback)


def _ldst_word_byte(raw: int, mk) -> Insn | None:  # A5.3
    load, byte = raw >> 20 & 1, raw >> 22 & 1
    rt, rn = raw >> 12 & 0xF, raw >> 16 & 0xF
    mn = ("ldr" if load else "str") + ("b" if byte else "")
    if raw >> 25 & 1:  # register offset
        name, amount = decode_imm_shift(raw >> 5 & 3, raw >> 7 & 0x1F)
        rm = raw & 0xF
        if name != "lsl" or rm == PC:
            return None  # only LSL index shifts are modelled
        mem = _mem(raw, (rm, amount), None)
    else:
        imm = raw & 0xFFF
        puw = (raw >> 24 & 1, raw >> 23 & 1, raw >> 21 & 1)
        if load and rn == SP and imm == 4 and rt != SP and not byte and puw == (0, 1, 0):
            return mk("pop", RegList((rt,)))  # ldr rt, [sp], #4 (Capstone has no push alias)
        mem = _mem(raw, None, imm)
    if mem is None or (byte and rt == PC) or (mem.wback or not mem.pre) and rn == rt:
        return None
    return mk(mn, Reg(rt), mem)


def _extra_ldst(raw: int, mk) -> Insn | None:  # A5.2.8
    op2, load = raw >> 5 & 3, raw >> 20 & 1
    rt, rn = raw >> 12 & 0xF, raw >> 16 & 0xF
    names = {(1, 0): "strh", (1, 1): "ldrh", (2, 0): "ldrd", (2, 1): "ldrsb",
             (3, 0): "strd", (3, 1): "ldrsh"}  # fmt: skip
    mn = names[op2, load]
    if raw >> 22 & 1:
        mem = _mem(raw, None, (raw >> 4 & 0xF0) | (raw & 0xF))
    else:
        if raw >> 8 & 0xF:
            return None
        mem = _mem(raw, (raw & 0xF, 0), None)
    if mem is None or rt == PC or (mem.index == PC):
        return None
    if mn in ("ldrd", "strd"):
        if rt & 1 or rt == 14:
            return None
        return mk(mn, Reg(rt), Reg(rt + 1), mem)
    if (mem.wback or not mem.pre) and rn == rt:
        return None
    return mk(mn, Reg(rt), mem)


def _ldm_stm(raw: int, mk) -> Insn | None:  # A5.5
    pre, add, user = raw >> 24 & 1, raw >> 23 & 1, raw >> 22 & 1
    wback, load = bool(raw >> 21 & 1), raw >> 20 & 1
    rn, mask = raw >> 16 & 0xF, raw & 0xFFFF
    if user or rn == PC or not mask:
        return None
    regs = RegList(tuple(r for r in range(16) if mask >> r & 1))
    if wback and rn in regs.regs and (load or rn != regs.regs[0]):
        return None  # UNPREDICTABLE (or UNKNOWN stored value)
    if pre == add:  # IB / DA: not modelled
        return None
    if rn == SP and wback and len(regs.regs) > 1:
        if load and not pre:
            return mk("pop", regs)
        if not load and pre:
            return mk("push", regs)
    mn = ("ldm" if load else "stm") + ("db" if pre else "")
    return mk(mn, Reg(rn), regs, wback=wback)
