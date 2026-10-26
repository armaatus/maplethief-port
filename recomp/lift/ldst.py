"""Loads and stores: single, dual and multiple, push/pop, literal pools (#33).

A PC-relative load from ``__TEXT`` (a literal pool) is folded into a constant: the image is
mapped at its link-time address and ``__TEXT`` never changes (ADR 0014). A load into the PC
is a return (``ldr pc, [sp], #4``, ``pop {..., pc}``) or an indirect jump.
"""

from __future__ import annotations

from recomp.decode.insn import PC, SP, Insn, Mem
from recomp.discover import is_return
from recomp.lift.core import Ctx, Unsupported, const, lifter

_SIZE = {"b": 1, "h": 2, "": 4}


def _kind(mnemonic: str) -> tuple[bool, int, bool]:
    """``(load, size, signed)`` of ldr/ldrb/ldrsh/str/strb/..., ignoring a trailing t."""
    m = mnemonic[:-1] if mnemonic.endswith("t") and len(mnemonic) > 3 else mnemonic
    load = m.startswith("ldr")
    rest = m[3:]
    signed = rest.startswith("s")
    return load, _SIZE[rest[1:] if signed else rest], signed


def address(ctx: Ctx, mem: Mem) -> tuple[str, str | None, int | None]:
    """``(address expression, writeback value or None, constant address or None)``."""
    if mem.base == PC:
        if mem.index is not None or mem.wback:
            raise Unsupported("pc-relative register offset")
        addr = (ctx.pc & ~3) + mem.disp
        return const(addr), None, addr
    base = ctx.tmp(ctx.r(mem.base))
    if mem.index is not None:
        idx = ctx.r(mem.index) + (f" << {mem.shift}" if mem.shift else "")
        offset = f"{base} {'-' if mem.subtract else '+'} ({idx})"
    elif mem.disp:
        offset = f"{base} {'-' if mem.disp < 0 else '+'} {const(abs(mem.disp))}"
    else:
        offset = base
    addr = offset if mem.pre else base
    wb = offset if mem.wback or not mem.pre else None
    return addr, wb, None


def _load_expr(ctx: Ctx, size: int, signed: bool, addr: str, fixed: int | None) -> str:
    folded = ctx.const_word(fixed, size) if fixed is not None else None
    if folded is not None:
        value = const(folded)
        if signed:
            value = f"mt_sx{size * 8}({value})"
        return value
    value = ctx.ld(size, addr)
    return f"mt_sx{size * 8}({value})" if signed else value


@lifter(
    *(
        p + s + t
        for p in ("ldr", "str")
        for s in ("", "b", "h", "sb", "sh")
        for t in ("", "t")
        if not (p == "str" and s.startswith("s"))
    )
)
def single(ctx: Ctx, insn: Insn) -> None:
    load, size, signed = _kind(insn.mnemonic)
    rt = insn.operands[0].n
    mem = insn.operands[1]
    addr, wb, fixed = address(ctx, mem)
    if not load:
        ctx.st(size, addr, ctx.r(rt))
        if wb is not None:
            ctx.set_r(mem.base, wb)
        return
    value = ctx.tmp(_load_expr(ctx, size, signed, addr, fixed))
    if wb is not None:
        ctx.set_r(mem.base, wb)
    if rt == PC:
        if is_return(insn):
            ctx.ret(value)
        else:
            ctx.jump_indirect(value)
    else:
        ctx.set_r(rt, value)


@lifter("ldrd", "strd")
def dual(ctx: Ctx, insn: Insn) -> None:
    rt, rt2 = insn.operands[0].n, insn.operands[1].n
    mem = insn.operands[2]
    addr, wb, fixed = address(ctx, mem)
    a = ctx.tmp(addr)
    if insn.mnemonic == "strd":
        ctx.st(4, a, ctx.r(rt))
        ctx.st(4, f"{a} + 4u", ctx.r(rt2))
        if wb is not None:
            ctx.set_r(mem.base, wb)
        return
    lo = ctx.tmp(_load_expr(ctx, 4, False, a, fixed))
    hi = ctx.tmp(_load_expr(ctx, 4, False, f"{a} + 4u", None if fixed is None else fixed + 4))
    if wb is not None:
        ctx.set_r(mem.base, wb)
    ctx.set_r(rt, lo)
    ctx.set_r(rt2, hi)


@lifter("push", "pop", "ldm", "stm", "ldmdb", "stmdb")
def multiple(ctx: Ctx, insn: Insn) -> None:
    m = insn.mnemonic
    if m in ("push", "pop"):
        base, regs, wback = SP, insn.operands[0].regs, True
        load, before = m == "pop", m == "push"
    else:
        base, regs, wback = insn.operands[0].n, insn.operands[1].regs, insn.wback
        load, before = m.startswith("ldm"), m.endswith("db")
    count = len(regs)
    b = ctx.tmp(ctx.r(base))
    start = ctx.tmp(f"{b} - {const(4 * count)}") if before else b
    final = f"{b} - {const(4 * count)}" if before else f"{b} + {const(4 * count)}"
    if not load:
        for i, r in enumerate(regs):
            ctx.st(4, f"{start} + {const(4 * i)}" if i else start, ctx.r(r))
        if wback:
            ctx.set_r(base, final)
        return
    values = [ctx.tmp(ctx.ld(4, f"{start} + {const(4 * i)}" if i else start)) for i in range(count)]
    if wback and base not in regs:
        ctx.set_r(base, final)
    for r, v in zip(regs, values, strict=True):
        if r != PC:
            ctx.set_r(r, v)
    if PC in regs:
        if is_return(insn):
            ctx.ret(values[-1])
        else:
            ctx.jump_indirect(values[-1])
