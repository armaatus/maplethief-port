"""Multiplies and divides (#33). ``muls`` sets N and Z only (C and V are unchanged on
ARMv6 and later); the long forms never set flags in Thumb."""

from __future__ import annotations

from recomp.decode.insn import Insn
from recomp.lift.core import Ctx, lifter


@lifter("mul", "mla", "mls")
def mul(ctx: Ctx, insn: Insn) -> None:
    rd, rn, rm = (op.n for op in insn.operands[:3])
    product = f"{ctx.r(rn)} * {ctx.r(rm)}"
    if insn.mnemonic == "mla":
        value = f"{product} + {ctx.r(insn.operands[3].n)}"
    elif insn.mnemonic == "mls":
        value = f"{ctx.r(insn.operands[3].n)} - {product}"
    else:
        value = product
    if insn.setflags:
        t = ctx.tmp(value)
        ctx.set_nz(t)
        value = t
    ctx.set_r(rd, value)


@lifter("smmul", "smmulr", "smmla", "smmlar", "smmls", "smmlsr")
def smmul(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    rd, rn, rm = ops[0].n, ops[1].n, ops[2].n
    m = insn.mnemonic
    rounding = " + 0x80000000u" if m.endswith("r") else ""
    product = f"mt_smul64({ctx.r(rn)}, {ctx.r(rm)})"
    if m.startswith("smmla"):
        value = f"((uint64_t){ctx.r(ops[3].n)} << 32) + {product}{rounding}"
    elif m.startswith("smmls"):
        value = f"((uint64_t){ctx.r(ops[3].n)} << 32) - {product}{rounding}"
    else:
        value = f"{product}{rounding}"
    ctx.set_r(rd, f"(uint32_t)(({value}) >> 32)")


@lifter("umull", "smull", "umlal", "smlal")
def long_mul(ctx: Ctx, insn: Insn) -> None:
    lo, hi, rn, rm = (op.n for op in insn.operands)
    m = insn.mnemonic
    if m[0] == "s":
        product = f"mt_smul64({ctx.r(rn)}, {ctx.r(rm)})"
    else:
        product = f"(uint64_t){ctx.r(rn)} * {ctx.r(rm)}"
    if m.endswith("lal"):
        acc = f"(((uint64_t){ctx.r(hi)} << 32) | {ctx.r(lo)})"
        product = f"{acc} + {product}"
    t = ctx.tmp(product, "uint64_t")
    ctx.set_r(lo, f"(uint32_t){t}")
    ctx.set_r(hi, f"(uint32_t)({t} >> 32)")


@lifter("sdiv", "udiv")
def div(ctx: Ctx, insn: Insn) -> None:
    rd, rn, rm = (op.n for op in insn.operands)
    a, b = ctx.tmp(ctx.r(rn)), ctx.tmp(ctx.r(rm))
    if insn.mnemonic == "udiv":
        ctx.set_r(rd, f"{b} == 0u ? 0u : {a} / {b}")
    else:  # division by zero gives 0, INT_MIN / -1 gives INT_MIN (ARM ARM)
        ctx.set_r(
            rd,
            f"{b} == 0u ? 0u : ({a} == 0x80000000u && {b} == 0xFFFFFFFFu) ? 0x80000000u"
            f" : (uint32_t)((int32_t){a} / (int32_t){b})",
        )
