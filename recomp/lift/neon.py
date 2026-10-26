"""Advanced SIMD (NEON): the forms :mod:`recomp.decode.neon` decodes (#34).

Lane-wise C over ``cpu->d[]``: a D register is one ``uint64_t`` of two 32-bit lanes, a Q
register ``qN`` is ``d[2N]`` and ``d[2N+1]``, lifted as two independent halves (each half of
the result reads only the same half of the sources, so a destination that is also a source
is safe). The f32 lane operations are the ``mt_neon_*`` helpers of vfp.h, which use the
standard FPSCR value of the ARM ARM (round to nearest, flush-to-zero, default NaN).
Registered as ``neon:<mnemonic>`` (``core`` picks them by ``Insn.simd``).
"""

from __future__ import annotations

from recomp.decode.insn import DReg, FPImm, Imm, Insn, QReg
from recomp.lift.core import Ctx, Unsupported, lifter

_REPEAT = {".i8": 8, ".i16": 16, ".i32": 32, ".f32": 32}


def _halves(reg) -> list[int]:
    """The D registers that make up ``reg``."""
    if isinstance(reg, DReg):
        return [reg.n]
    if isinstance(reg, QReg):
        return [2 * reg.n, 2 * reg.n + 1]
    raise Unsupported(f"NEON operand {reg}")


def _each_half(ctx: Ctx, insn: Insn, expr) -> None:
    """``d = expr(sources)`` for each D half of the operands (the first is the destination)."""
    regs = [_halves(op) for op in insn.operands]
    for i, dest in enumerate(regs[0]):
        ctx.emit(f"cpu->d[{dest}] = {expr(*(f'cpu->d[{r[i]}]' for r in regs[1:]))};")


@lifter("neon:vadd", "neon:vsub", "neon:vmul")
def arith(ctx: Ctx, insn: Insn) -> None:
    if insn.dt != ".f32":
        raise Unsupported(insn.text())
    _each_half(ctx, insn, lambda a, b: f"mt_neon_{insn.mnemonic}_f32({a}, {b})")


@lifter("neon:vorr")
def orr(ctx: Ctx, insn: Insn) -> None:
    _each_half(ctx, insn, lambda a, b: a if a == b else f"({a} | {b})")


@lifter("neon:vneg")
def neg(ctx: Ctx, insn: Insn) -> None:  # FPNeg: the sign bits only, NaNs included
    if insn.dt != ".f32":
        raise Unsupported(insn.text())
    _each_half(ctx, insn, lambda a: f"({a} ^ 0x8000000080000000ull)")


@lifter("neon:vcvt")
def convert(ctx: Ctx, insn: Insn) -> None:
    name = insn.dt[1:].replace(".", "_")  # .f32.s32 -> f32_s32
    if name not in ("f32_s32", "f32_u32", "s32_f32", "u32_f32"):
        raise Unsupported(insn.text())
    _each_half(ctx, insn, lambda a: f"mt_neon_vcvt_{name}({a})")


@lifter("neon:vmov")
def move_immediate(ctx: Ctx, insn: Insn) -> None:
    """VMOV (immediate): AdvSIMDExpandImm's element, repeated across the register."""
    dest, imm = insn.operands
    if insn.dt not in _REPEAT or not isinstance(imm, Imm | FPImm):
        raise Unsupported(insn.text())
    width = _REPEAT[insn.dt]
    element = imm.bits if isinstance(imm, FPImm) else imm.value
    value = 0
    for i in range(64 // width):
        value |= element << (i * width)
    for d in _halves(dest):
        ctx.emit(f"cpu->d[{d}] = {value:#x}ull;")
