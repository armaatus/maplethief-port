"""Bit fields, extends, byte reversal, CLZ, hints, barriers and traps (#33)."""

from __future__ import annotations

from recomp.decode.insn import Insn, Shifted
from recomp.lift.core import Ctx, const, lifter


@lifter("nop", "yield", "wfe", "wfi", "sev", "dmb", "dsb", "isb")
def hint(ctx: Ctx, insn: Insn) -> None:
    """No effect: the guest is single-threaded on one core (ANALYSIS.md: all 66 dmb are in
    Chartboost's atomic property accessors), so a barrier orders nothing extra."""


@lifter("trap", "udf", "bkpt", "svc")
def trap(ctx: Ctx, insn: Insn) -> None:
    detail = const(insn.operands[0].value) if insn.operands else "0xFEu"
    ctx.trap("MT_TRAP_UDF", detail)


@lifter("ubfx", "sbfx")
def bitfield_extract(ctx: Ctx, insn: Insn) -> None:
    rd, rn = insn.operands[0].n, insn.operands[1].n
    lsb, width = insn.operands[2].value, insn.operands[3].value
    mask = (1 << width) - 1
    field = f"(({ctx.r(rn)} >> {lsb}) & {const(mask)})"
    if insn.mnemonic == "sbfx" and width < 32:
        sign = 1 << (width - 1)
        field = f"(({field} ^ {const(sign)}) - {const(sign)})"
    ctx.set_r(rd, field)


@lifter("bfi", "bfc")
def bitfield_insert(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    rd = ops[0].n
    lsb, width = (
        (ops[1].value, ops[2].value) if insn.mnemonic == "bfc" else (ops[2].value, ops[3].value)
    )
    mask = ((1 << width) - 1) << lsb
    keep = f"({ctx.r(rd)} & {const(~mask)})"
    if insn.mnemonic == "bfc":
        ctx.set_r(rd, keep)
    else:
        ctx.set_r(rd, f"{keep} | (({ctx.r(ops[1].n)} << {lsb}) & {const(mask)})")


def _rotated(ctx: Ctx, op) -> str:
    if isinstance(op, Shifted):
        return f"mt_ror({ctx.r(op.n)}, {op.amount}u)"
    return ctx.r(op.n)


_EXTEND = {
    "uxtb": "({x} & 0xFFu)",
    "uxth": "({x} & 0xFFFFu)",
    "sxtb": "mt_sx8({x})",
    "sxth": "mt_sx16({x})",
}


@lifter(*_EXTEND, *(m.replace("xt", "xta") for m in _EXTEND))
def extend(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    m = insn.mnemonic
    if "xta" in m:
        value = _EXTEND[m.replace("xta", "xt")].format(x=_rotated(ctx, ops[2]))
        ctx.set_r(ops[0].n, f"{ctx.r(ops[1].n)} + {value}")
    else:
        ctx.set_r(ops[0].n, _EXTEND[m].format(x=_rotated(ctx, ops[1])))


@lifter("clz", "rev", "rev16", "revsh", "rbit")
def bits(ctx: Ctx, insn: Insn) -> None:
    rd, rm = insn.operands[0].n, insn.operands[1].n
    x = ctx.r(rm)
    m = insn.mnemonic
    if m == "revsh":
        value = f"mt_sx16(mt_rev16({x}))"
    elif m == "rbit":
        t = ctx.tmp(x)
        for shift, mask in ((1, 0x55555555), (2, 0x33333333), (4, 0x0F0F0F0F)):
            ctx.emit(
                f"{t} = (({t} >> {shift}) & {const(mask)}) | (({t} & {const(mask)}) << {shift});"
            )
        value = f"mt_rev({t})"
    else:
        value = f"mt_{m}({x})"
    ctx.set_r(rd, value)
