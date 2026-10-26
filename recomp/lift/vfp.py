"""VFP (#30, #34; NEON is in neon.py).

Registers are bit patterns: ``mt_s_get``/``mt_s_set`` for s0-s31 (halves of d0-d15),
``cpu->d[n]`` for d0-d31. Arithmetic and conversions go through the ``mt_v*`` helpers of
vfp.h, which implement the iOS floating-point mode (round to nearest, flush-to-zero, default
NaN; ADR 0022) bit-exactly on every host. VMLA and friends are two rounded operations, as on
VFP.
"""

from __future__ import annotations

from recomp.decode.insn import SP, DReg, FPImm, Imm, Insn, Reg, SReg, SysReg
from recomp.lift.core import Ctx, Unsupported, const, lifter
from recomp.lift.ldst import address

_SIGN = {False: "0x80000000u", True: "0x8000000000000000ull"}


def _get(ctx: Ctx, reg) -> str:
    if isinstance(reg, SReg):
        return f"mt_s_get(cpu, {reg.n}u)"
    if isinstance(reg, DReg):
        return f"cpu->d[{reg.n}]"
    raise Unsupported(f"VFP operand {reg}")


def _set(ctx: Ctx, reg, value: str) -> None:
    if isinstance(reg, SReg):
        ctx.emit(f"mt_s_set(cpu, {reg.n}u, {value});")
    elif isinstance(reg, DReg):
        ctx.emit(f"cpu->d[{reg.n}] = {value};")
    else:
        raise Unsupported(f"VFP operand {reg}")


def _width(insn: Insn) -> str:
    if insn.dt not in (".f32", ".f64"):
        raise Unsupported(f"VFP data type {insn.dt}")
    return insn.dt[1:]


@lifter("vadd", "vsub", "vmul", "vdiv")
def arith(ctx: Ctx, insn: Insn) -> None:
    w = _width(insn)
    d, n, m = insn.operands
    _set(ctx, d, f"mt_{insn.mnemonic}_{w}({_get(ctx, n)}, {_get(ctx, m)})")


@lifter("vmla", "vmls", "vnmla", "vnmls", "vnmul")
def multiply_accumulate(ctx: Ctx, insn: Insn) -> None:
    w = _width(insn)
    sign = _SIGN[w == "f64"]
    d, n, m = insn.operands
    product = ctx.tmp(
        f"mt_vmul_{w}({_get(ctx, n)}, {_get(ctx, m)})", "uint64_t" if w == "f64" else "uint32_t"
    )
    acc = _get(ctx, d)
    value = {
        "vmla": f"mt_vadd_{w}({acc}, {product})",
        "vmls": f"mt_vadd_{w}({acc}, {product} ^ {sign})",
        "vnmla": f"mt_vadd_{w}({acc} ^ {sign}, {product} ^ {sign})",
        "vnmls": f"mt_vadd_{w}({acc} ^ {sign}, {product})",
        "vnmul": f"{product} ^ {sign}",
    }[insn.mnemonic]
    _set(ctx, d, value)


@lifter("vsqrt", "vabs", "vneg")
def unary(ctx: Ctx, insn: Insn) -> None:
    w = _width(insn)
    d, m = insn.operands
    x = _get(ctx, m)
    sign = _SIGN[w == "f64"]
    value = {
        "vsqrt": f"mt_vsqrt_{w}({x})",
        "vabs": f"({x} & ~{sign})",
        "vneg": f"({x} ^ {sign})",
    }[insn.mnemonic]
    _set(ctx, d, value)


_CONVERSIONS = frozenset({
    "f64_f32", "f32_f64", "s32_f32", "u32_f32", "s32_f64", "u32_f64", "f32_s32", "f32_u32",
    "f64_s32", "f64_u32",
})  # fmt: skip


@lifter("vcvt")
def convert(ctx: Ctx, insn: Insn) -> None:
    """VCVT between f32 and f64, and to or from 32-bit integers: to an integer rounds toward
    zero and saturates, NaN gives 0 (the mt_vcvt_* helpers of vfp.h). ``vcvtr`` (rounding
    with the FPSCR mode) is not used by the binary and has no lifter."""
    d, m = insn.operands
    name = insn.dt[1:].replace(".", "_")  # .s32.f64 -> s32_f64
    if name not in _CONVERSIONS:
        raise Unsupported(f"vcvt{insn.dt}")
    _set(ctx, d, f"mt_vcvt_{name}({_get(ctx, m)})")


@lifter("vmov")
def move(ctx: Ctx, insn: Insn) -> None:
    ops = insn.operands
    if len(ops) == 2 and isinstance(ops[1], FPImm):
        _set(ctx, ops[0], f"{ops[1].bits:#x}{'ull' if ops[1].double else 'u'}")
    elif len(ops) == 2 and isinstance(ops[0], Reg):  # vmov rt, sn
        ctx.set_r(ops[0].n, _get(ctx, ops[1]))
    elif len(ops) == 2 and isinstance(ops[1], Reg):  # vmov sn, rt
        _set(ctx, ops[0], ctx.r(ops[1].n))
    elif len(ops) == 2:
        _set(ctx, ops[0], _get(ctx, ops[1]))
    elif len(ops) == 3 and isinstance(ops[2], DReg):  # vmov rt, rt2, dm
        t = ctx.tmp(_get(ctx, ops[2]), "uint64_t")
        ctx.set_r(ops[0].n, f"(uint32_t){t}")
        ctx.set_r(ops[1].n, f"(uint32_t)({t} >> 32)")
    elif len(ops) == 3:  # vmov dm, rt, rt2
        _set(ctx, ops[0], f"((uint64_t){ctx.r(ops[2].n)} << 32) | {ctx.r(ops[1].n)}")
    elif isinstance(ops[0], Reg):  # vmov rt, rt2, sm, sm1
        lo, hi = ctx.tmp(_get(ctx, ops[2])), ctx.tmp(_get(ctx, ops[3]))
        ctx.set_r(ops[0].n, lo)
        ctx.set_r(ops[1].n, hi)
    else:  # vmov sm, sm1, rt, rt2
        lo, hi = ctx.tmp(ctx.r(ops[2].n)), ctx.tmp(ctx.r(ops[3].n))
        _set(ctx, ops[0], lo)
        _set(ctx, ops[1], hi)


@lifter("vcmp", "vcmpe")
def compare(ctx: Ctx, insn: Insn) -> None:
    """Sets FPSCR.NZCV. The exception (cumulative) flags of FPSCR are not modelled: the
    binary never reads FPSCR other than through vmrs APSR_nzcv."""
    w = _width(insn)
    a, b = insn.operands
    rhs = "0u" if isinstance(b, Imm) else _get(ctx, b)
    if isinstance(b, Imm) and w == "f64":
        rhs = "0ull"
    ctx.emit(f"cpu->fpscr = (cpu->fpscr & 0x0FFFFFFFu) | mt_vcmp_{w}({_get(ctx, a)}, {rhs});")


@lifter("vmrs")
def vmrs(ctx: Ctx, insn: Insn) -> None:
    dest = insn.operands[0]
    if isinstance(dest, SysReg):  # APSR_nzcv: copy FPSCR.NZCV to the flags
        t = ctx.tmp("cpu->fpscr") if ctx.live(*"nzcv") else "0u"
        for bit, f in zip((31, 30, 29, 28), "nzcv", strict=True):
            ctx.set_flag(f, f"({t} >> {bit}) & 1u")
    else:
        ctx.set_r(dest.n, "cpu->fpscr")


@lifter("vldr", "vstr")
def load_store(ctx: Ctx, insn: Insn) -> None:
    reg, mem = insn.operands
    size = 8 if isinstance(reg, DReg) else 4
    addr, _, fixed = address(ctx, mem)
    if insn.mnemonic == "vstr":
        if size == 8:
            ctx.emit(f"MT_ST64({addr}, {_get(ctx, reg)}, {ctx.site()});")
        else:
            ctx.st(4, addr, _get(ctx, reg))
        return
    folded = ctx.const_word(fixed, size) if fixed is not None else None
    if folded is not None:
        value = f"{folded:#x}ull" if size == 8 else const(folded)
    else:
        value = ctx.ld(size, addr)
    _set(ctx, reg, value)


@lifter("vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb")
def load_store_multiple(ctx: Ctx, insn: Insn) -> None:
    m = insn.mnemonic
    if m in ("vpush", "vpop"):
        base, regs, wback = SP, insn.operands[0], True
        load, before = m == "vpop", m == "vpush"
    else:
        base, regs, wback = insn.operands[0].n, insn.operands[1], insn.wback
        load, before = m.startswith("vldm"), m.endswith("db")
    step = 8 if regs.double else 4
    total = step * regs.count
    b = ctx.tmp(ctx.r(base))
    start = ctx.tmp(f"{b} - {const(total)}") if before else b
    for i in range(regs.count):
        reg = DReg(regs.first + i) if regs.double else SReg(regs.first + i)
        addr = f"{start} + {const(step * i)}" if i else start
        if load:
            _set(ctx, reg, ctx.ld(step, addr))
        else:
            ctx.emit(f"MT_ST{step * 8}({addr}, {_get(ctx, reg)}, {ctx.site()});")
    if wback:
        ctx.set_r(base, f"{b} {'-' if before else '+'} {const(total)}")
