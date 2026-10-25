"""Data processing: moves, arithmetic, logic, compares, shifts, movw/movt, addw/subw (#33).

Semantics follow the ARM ARM: AddWithCarry for the arithmetic (C = carry out, V = signed
overflow), and for logic the shifter carry-out (an immediate's ThumbExpandImm_C carry, or
Shift_C of a shifted register) with V unchanged.
"""

from __future__ import annotations

from recomp.decode.insn import PC, Imm, Insn, Reg, Shifted
from recomp.lift.core import Ctx, Unsupported, const, lifter

_LOGIC = {
    "and": "{a} & {b}",
    "orr": "{a} | {b}",
    "orn": "{a} | ~{b}",
    "eor": "{a} ^ {b}",
    "bic": "{a} & ~{b}",
}
_SHIFTS = ("lsl", "lsr", "asr", "ror")


def shift_imm(ctx: Ctx, x: str, kind: str, amount: int) -> tuple[str, str | None]:
    """``(value, carry)`` of an immediate shift of the expression ``x`` (DecodeImmShift
    amounts: lsr/asr 32 are 32). carry is None when the carry flag is unchanged."""
    if kind == "rrx":
        return f"(({x} >> 1) | ({ctx.flag('c')} << 31))", f"({x} & 1u)"
    if amount == 0:
        return x, None
    if kind == "lsl":
        return f"({x} << {amount})", f"(({x} >> {32 - amount}) & 1u)"
    if kind == "lsr":
        if amount == 32:
            return "0u", f"({x} >> 31)"
        return f"({x} >> {amount})", f"(({x} >> {amount - 1}) & 1u)"
    if kind == "asr":
        n = min(amount, 31)
        return f"(uint32_t)((int32_t){x} >> {n})", f"(({x} >> {amount - 1}) & 1u)"
    if kind == "ror":
        return f"(({x} >> {amount}) | ({x} << {32 - amount}))", f"(({x} >> {amount - 1}) & 1u)"
    raise Unsupported(f"shift {kind}")


def operand2(ctx: Ctx, op) -> tuple[str, str | None]:
    """``(value, shifter carry)`` of a flexible second operand."""
    if isinstance(op, Imm):
        return const(op.value), None if op.carry is None else f"{op.carry}u"
    if isinstance(op, Reg):
        return ctx.r(op.n), None
    if isinstance(op, Shifted):
        return shift_imm(ctx, ctx.r(op.n), op.kind, op.amount)
    raise Unsupported(f"operand {op}")


def _write(ctx: Ctx, rd: int, value: str) -> None:
    if rd == PC:  # mov pc / add pc: BranchWritePC, stays in Thumb
        ctx.jump_indirect(f"(({value}) | 1u)")
    else:
        ctx.set_r(rd, value)


def _roles(insn: Insn):
    """``(rd or None, rn expression operand or None, op2 operand)`` in assembler order."""
    m, ops = insn.mnemonic, insn.operands
    if m in ("cmp", "cmn", "tst", "teq"):
        return None, ops[0], ops[1]
    if m in ("mov", "mvn"):
        return ops[0].n, None, ops[1]
    if len(ops) == 3:
        return ops[0].n, ops[1], ops[2]
    return ops[0].n, ops[0], ops[1]  # two-operand forms: rdn, op2


@lifter("and", "orr", "orn", "eor", "bic", "tst", "teq", "mov", "mvn")
def logic(ctx: Ctx, insn: Insn) -> None:
    rd, rn, op2 = _roles(insn)
    b, carry = operand2(ctx, op2)
    m = {"tst": "and", "teq": "eor"}.get(insn.mnemonic, insn.mnemonic)
    if m == "mov":
        value = b
    elif m == "mvn":
        value = f"~{b}"
    else:
        value = _LOGIC[m].format(a=ctx.r(rn.n), b=b)
    if not insn.setflags:
        _write(ctx, rd, value)
        return
    t = ctx.tmp(value) if rd is not None or ctx.live("n", "z") else "0u"
    ctx.set_nz(t)  # (t is only "0u" when neither N nor Z is computed: nothing is emitted)
    if carry is not None:
        ctx.set_flag("c", carry)
    if rd is not None:
        _write(ctx, rd, t)


@lifter("add", "adc", "sub", "sbc", "rsb", "cmp", "cmn")
def arithmetic(ctx: Ctx, insn: Insn) -> None:
    rd, rn, op2 = _roles(insn)
    b, _ = operand2(ctx, op2)
    a = ctx.r(rn.n)
    m = {"cmp": "sub", "cmn": "add"}.get(insn.mnemonic, insn.mnemonic)
    if m == "rsb":
        a, b = b, a
        m = "sub"
    if not insn.setflags:
        if m == "add":
            value = f"{a} + {b}"
        elif m == "sub":
            value = f"{a} - {b}"
        elif m == "adc":
            value = f"{a} + {b} + {ctx.flag('c')}"
        else:
            value = f"{a} - {b} - (1u - {ctx.flag('c')})"
        _write(ctx, rd, value)
        return
    if rd is None and not ctx.live(*"nzcv"):  # a compare nobody reads
        for f in "nzcv":
            ctx.set_flag(f, "0u")  # records the writes for the flag check; emits nothing
        return
    ta, tb = ctx.tmp(a), ctx.tmp(b)
    # the result is needed for rd, N, Z, V, and for an add's C (t < a)
    need_t = rd is not None or ctx.live("n", "z", "v") or (m == "add" and ctx.live("c"))
    if m == "add":
        t = ctx.tmp(f"{ta} + {tb}")
        ctx.set_nz(t)
        ctx.set_flag("c", f"(uint32_t)({t} < {ta})")
        ctx.set_flag("v", f"(({ta} ^ {t}) & ({tb} ^ {t})) >> 31")
    elif m == "sub":
        t = ctx.tmp(f"{ta} - {tb}") if need_t else "0u"
        ctx.set_nz(t)
        ctx.set_flag("c", f"(uint32_t)({ta} >= {tb})")
        ctx.set_flag("v", f"(({ta} ^ {tb}) & ({ta} ^ {t})) >> 31")
    else:  # adc / sbc: AddWithCarry(a, b or ~b, C)
        addend = tb if m == "adc" else f"(uint32_t)~{tb}"
        u = ctx.tmp(f"(uint64_t){ta} + {addend} + {ctx.flag('c')}", "uint64_t")
        t = ctx.tmp(f"(uint32_t){u}") if need_t else "0u"
        ctx.set_nz(t)
        ctx.set_flag("c", f"(uint32_t)({u} >> 32)")
        if m == "adc":
            ctx.set_flag("v", f"(({ta} ^ {t}) & ({tb} ^ {t})) >> 31")
        else:
            ctx.set_flag("v", f"(({ta} ^ {tb}) & ({ta} ^ {t})) >> 31")
    if rd is not None:
        _write(ctx, rd, t)


@lifter(*_SHIFTS, "rrx")
def shift(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    rd = ops[0].n
    m = insn.mnemonic
    if m == "rrx":  # rrx rd, rm
        src, amount = ops[1], None
    elif len(ops) == 3:  # lsl rd, rm, #n / lsl rd, rn, rm
        src, amount = ops[1], ops[2]
    else:  # 16-bit register shifts: lsls rdn, rm
        src, amount = ops[0], ops[1]
    if m == "rrx" or isinstance(amount, Imm):
        n = 1 if m == "rrx" else amount.value
        value, carry = shift_imm(ctx, ctx.r(src.n), m, n)
    else:  # shift by the bottom byte of a register (ARM ARM Shift_C semantics)
        x = ctx.tmp(ctx.r(src.n))
        n = ctx.tmp(f"{ctx.r(amount.n)} & 0xFFu")
        value = f"mt_{m}({x}, {n})"
        carry = f"mt_{m}_c({x}, {n}, {ctx.flag('c', passthrough=True)})"
    if not insn.setflags:
        _write(ctx, rd, value)
        return
    t = ctx.tmp(value)
    ctx.set_nz(t)
    if carry is not None:
        ctx.set_flag("c", carry, passthrough=not isinstance(amount, Imm) and m != "rrx")
    _write(ctx, rd, t)


@lifter("movw", "movt")
def move_wide(ctx: Ctx, insn: Insn) -> None:
    rd, imm = insn.operands[0].n, insn.operands[1].value
    if insn.mnemonic == "movw":
        ctx.set_r(rd, const(imm))
    else:
        ctx.set_r(rd, f"({ctx.r(rd)} & 0xFFFFu) | {const(imm << 16)}")


@lifter("addw", "subw", "adr")
def add_wide(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    rd = ops[0].n
    if insn.mnemonic == "adr":  # ADR T1: Align(PC, 4) + imm
        ctx.set_r(rd, const((ctx.pc & ~3) + ops[1].value))
        return
    rn, imm = ops[1].n, ops[2].value
    sign = 1 if insn.mnemonic == "addw" else -1
    if rn == PC:  # ADR T2/T3
        ctx.set_r(rd, const((ctx.pc & ~3) + sign * imm))
    else:
        ctx.set_r(rd, f"{ctx.r(rn)} {'+' if sign > 0 else '-'} {const(imm)}")
