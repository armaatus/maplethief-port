"""Branches, calls, returns and IT (#33; the indirect forms reach the #35 dispatch table).

``b`` inside the function is a goto, to another function a tail call; ``bl``/``blx #imm``
set lr and call the target's C function (or the import, for a stub); ``blx rN`` and
``bx rN`` go through the dispatch table; ``bx lr`` returns. ``it`` emits nothing: the core
wraps the instructions it covers in their conditions.
"""

from __future__ import annotations

import itertools

from recomp.decode.insn import LR, Insn
from recomp.lift.core import Ctx, lifter


@lifter("b")
def b(ctx: Ctx, insn: Insn) -> None:
    ctx.branch(insn.operands[0].target)


@lifter("cbz", "cbnz")
def cbz(ctx: Ctx, insn: Insn) -> None:
    rn, target = insn.operands[0].n, insn.operands[1].target
    test = "==" if insn.mnemonic == "cbz" else "!="
    ctx.emit(f"if ({ctx.r(rn)} {test} 0u) {{")
    ctx.depth += 1
    ctx.branch(target)
    ctx.depth -= 1
    ctx.emit("}")


@lifter("bl")
def bl(ctx: Ctx, insn: Insn) -> None:
    ctx.call(insn.operands[0].target, arm=False)


@lifter("blx")
def blx(ctx: Ctx, insn: Insn) -> None:
    op = insn.operands[0]
    if hasattr(op, "target"):
        ctx.call(op.target, arm=True)
    else:
        ctx.call_indirect(ctx.r(op.n))


@lifter("bx")
def bx(ctx: Ctx, insn: Insn) -> None:
    rm = insn.operands[0].n
    if rm == LR:
        ctx.ret(ctx.r(LR))
    else:
        ctx.jump_indirect(ctx.r(rm))


@lifter(*("it" + "".join(p) for n in range(4) for p in itertools.product("te", repeat=n)))
def it(ctx: Ctx, insn: Insn) -> None:
    """Nothing to emit: the conditions are applied to the instructions it covers."""
