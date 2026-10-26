"""The function lifter: turns a discovered :class:`Function` into one C function (ADR 0014).

Per-class lifters (``alu``, ``mul``, ``ldst``, ``branch``, ``misc``, ``vfp``) register a
function per mnemonic with :func:`lifter` and write C through :class:`Ctx`; they never build
the function frame, labels, IT conditions or flag declarations, which this module does.

The conventions (the full contract is ADR 0014 and ``runtime/include/maplethief/recomp/``):

* Guest registers are ``cpu->r[n]`` / ``cpu->d[n]``; a read of the PC is a constant.
* N, Z, C, V are the C locals ``nf zf cf vf``. A flag is computed only if some instruction
  of the function reads it. No flag may be live into a function or across a call (AAPCS
  leaves them undefined there); :func:`check_flags` proves that per function and reports a
  problem otherwise, so the assumption can never silently produce wrong code.
* Each guest instruction becomes a comment plus a few C statements; an IT or other
  conditional instruction is wrapped in ``if (cond) { ... }``; a branch target gets a label
  ``L_XXXXXXXX:``. An instruction with no translation becomes ``MT_TRAP(MT_TRAP_UNSUPPORTED)``
  at its address, and is reported.
* Lifting runs twice: the first pass records which flags each instruction reads and writes
  (and whether it calls), the second emits only the flag computations someone reads.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Protocol

from recomp.decode.insn import AL, COND_NAMES, Insn
from recomp.discover import Function, JumpTable, falls_through

FLAGS = ("n", "z", "c", "v")
LOCAL = {"n": "nf", "z": "zf", "c": "cf", "v": "vf"}
_COND_FLAGS = {
    0: "z", 1: "z", 2: "c", 3: "c", 4: "n", 5: "n", 6: "v", 7: "v",
    8: "cz", 9: "cz", 10: "nv", 11: "nv", 12: "znv", 13: "znv",
}  # fmt: skip
_COND_EXPR = (  # used only as a whole `if (...)` condition, so no outer parentheses
    "zf", "!zf", "cf", "!cf", "nf", "!nf", "vf", "!vf",
    "cf && !zf", "!cf || zf", "nf == vf", "nf != vf",
    "!zf && nf == vf", "zf || nf != vf",
)  # fmt: skip


class Unsupported(Exception):
    """Raised by a lifter for an operand form it does not translate (yet)."""


class Program(Protocol):
    """What the lifter needs to know about the rest of the program."""

    def const_word(self, addr: int, size: int) -> int | None:
        """The little-endian value at ``addr`` if it is immutable (``__TEXT``), else None."""

    def callee(self, ptr: int) -> str | None:
        """The C name of the lifted function with function pointer ``ptr``, or None."""

    def import_at(self, addr: int) -> tuple[int, str] | None:
        """``(import id, symbol)`` if ``addr`` is an import stub."""


def c_name(ptr: int) -> str:
    """The C name of the guest function with function pointer ``ptr``."""
    return f"f_{ptr:08X}"


def const(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:X}u"


@dataclass(frozen=True)
class LiftOptions:
    """``flags_in`` / ``flags_out``: flags start from / end in ``cpu->cpsr`` (the snippet
    harness checks them; real functions never need it). ``fall_off_returns``: running past
    the last decoded instruction returns instead of trapping (snippets)."""

    flags_in: bool = False
    flags_out: bool = False
    fall_off_returns: bool = False
    comments: bool = True


@dataclass
class _Trace:
    reads: set[str] = field(default_factory=set)
    writes: set[str] = field(default_factory=set)  # unconditional writes only
    kills: bool = False  # a call: every flag is undefined afterwards


@dataclass
class Lifted:
    """The C of one function and what the output stage needs to know about it."""

    ptr: int
    name: str
    source: str
    unsupported: list[tuple[int, str]] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)
    callees: set[str] = field(default_factory=set)
    imports: set[int] = field(default_factory=set)

    @property
    def clean(self) -> bool:
        return not self.unsupported and not self.problems


LIFTERS: dict[str, Callable[[Ctx, Insn], None]] = {}


def lifter(*mnemonics: str):
    """Register the decorated function as the lifter of ``mnemonics``."""

    def register(fn: Callable[[Ctx, Insn], None]):
        for m in mnemonics:
            if m in LIFTERS:
                raise ValueError(f"two lifters for {m}")
            LIFTERS[m] = fn
        return fn

    return register


class Ctx:
    """The emission API of the per-class lifters, valid for the instruction ``insn``."""

    def __init__(self, fn: Function, program: Program, opts: LiftOptions, used: set[str]):
        self.fn = fn
        self.program = program
        self.opts = opts
        self.used = used
        self.lines: list[str] = []
        self.insn: Insn
        self.trace = _Trace()
        self.depth = 1
        self.conditional = False
        self._tmp = 0
        self._block_open = False
        self.callees: set[str] = set()
        self.imports: set[int] = set()

    # -- output ---------------------------------------------------------------------------

    def emit(self, line: str) -> None:
        self.lines.append("    " * self.depth + line)

    def tmp(self, expr: str, ctype: str = "uint32_t") -> str:
        """Declare a temporary for this instruction; returns its name."""
        if not self._block_open:
            self.emit("{")
            self.depth += 1
            self._block_open = True
        name = f"t{self._tmp}"
        self._tmp += 1
        self.emit(f"{ctype} {name} = {expr};")
        return name

    # -- registers, constants, memory -------------------------------------------------------

    @property
    def pc(self) -> int:
        """The value a read of the PC returns (the address + 4 in Thumb, + 8 in ARM)."""
        return self.insn.pc

    def r(self, n: int) -> str:
        return const(self.pc) if n == 15 else f"cpu->r[{n}]"

    def set_r(self, n: int, expr: str) -> None:
        if n == 15:
            raise Unsupported("write to pc outside a branch lifter")
        self.emit(f"cpu->r[{n}] = {expr};")

    def site(self) -> str:
        return const(self.insn.address)

    def ld(self, size: int, addr: str) -> str:
        return f"MT_LD{size * 8}({addr}, {self.site()})"

    def st(self, size: int, addr: str, value: str) -> None:
        self.emit(f"MT_ST{size * 8}({addr}, {value}, {self.site()});")

    def const_word(self, addr: int, size: int) -> int | None:
        return self.program.const_word(addr, size)

    # -- flags ------------------------------------------------------------------------------

    def flag(self, f: str, passthrough: bool = False) -> str:
        """Read flag ``f``. ``passthrough``: the value only flows into the same flag's new
        value (a shift by 0 keeps C), which is not a use of its own."""
        if not passthrough:
            self.trace.reads.add(f)
        return LOCAL[f]

    def set_flag(self, f: str, expr: str, passthrough: bool = False) -> None:
        if not passthrough and not self.conditional:
            self.trace.writes.add(f)
        if f in self.used:
            self.emit(f"{LOCAL[f]} = {expr};")

    def live(self, *flags: str) -> bool:
        """Whether any of ``flags`` is computed in this function (some instruction reads it);
        a lifter skips temporaries that only a dead flag would use."""
        return any(f in self.used for f in flags)

    def set_nz(self, value: str) -> None:
        self.set_flag("n", f"{value} >> 31")
        self.set_flag("z", f"(uint32_t)({value} == 0u)")

    def cond(self, cond: int) -> str:
        for f in _COND_FLAGS[cond]:
            self.flag(f)
        return _COND_EXPR[cond]

    # -- control flow -----------------------------------------------------------------------

    def label(self, addr: int) -> str:
        return f"L_{addr:08X}"

    def _spill_flags(self) -> None:
        if self.opts.flags_out:
            # The harness's view of the flags, not a guest read: after a call they are
            # undefined (AAPCS), and spilling them must not be reported as a problem.
            for f in FLAGS:
                self.flag(f, passthrough=True)
            self.emit(
                "cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | (nf << 31) | (zf << 30) | (cf << 29)"
                " | (vf << 28);"
            )

    def ret(self, target: str) -> None:
        """Return to guest address ``target`` (bx lr, pop {pc}, ...)."""
        self._spill_flags()
        self.emit(f"MT_RETURN({target});")

    def branch(self, target: int) -> None:
        """``b target``: a goto inside the function, otherwise a tail call."""
        if self.fn.contains(target) and target in self.fn.insns:
            self.emit(f"goto {self.label(target)};")
            return
        ptr = target | (1 if self.fn.thumb else 0)
        self._spill_flags()
        imp = self.program.import_at(target)
        if imp is not None:
            self.imports.add(imp[0])
            self.emit(f"MT_CALL_IMPORT({imp[0]}u); /* tail call: {imp[1]} */")
            self.emit("return;")
            return
        name = self.program.callee(ptr)
        if name is not None:
            self.callees.add(name)
            self.emit(f"MT_TAILCALL({name}(cpu));")
        else:
            self.emit(f"MT_JUMP_INDIRECT({const(ptr)}, {self.site()});")

    def call(self, target: int, arm: bool) -> None:
        """``bl`` / ``blx #imm`` to ``target`` (ARM code if ``arm``)."""
        self.trace.kills = True
        ret = const(self.insn.end | (1 if self.fn.thumb else 0))
        self.emit(f"cpu->r[14] = {ret};")
        imp = self.program.import_at(target)
        if imp is not None:
            self.imports.add(imp[0])
            self.emit(f"MT_CALL_IMPORT({imp[0]}u); /* {imp[1]} */")
            return
        ptr = target if arm else target | 1
        name = self.program.callee(ptr)
        if name is not None:
            self.callees.add(name)
            self.emit(f"{name}(cpu);")
        else:
            self.emit(f"MT_CALL_INDIRECT({const(ptr)}, {self.site()});")

    def call_indirect(self, target: str) -> None:
        """``blx rN``."""
        self.trace.kills = True
        t = self.tmp(target)
        self.emit(f"cpu->r[14] = {const(self.insn.end | (1 if self.fn.thumb else 0))};")
        self.emit(f"MT_CALL_INDIRECT({t}, {self.site()});")

    def jump_indirect(self, target: str) -> None:
        """An indirect jump (``bx rN``, ``mov pc, rN``, ``ldr pc``): a return if it goes to
        the address the function was called from, otherwise a tail call through the
        dispatch table."""
        self._spill_flags()
        self.emit(f"MT_JUMP_INDIRECT({target}, {self.site()});")

    def trap(self, kind: str, detail: str = "0u") -> None:
        """``MT_TRAP`` at this instruction; in snippet mode (``flags_out``) the flags are
        stored first, so a test sees them at the trap too."""
        self._spill_flags()
        self.emit(f"MT_TRAP({kind}, {self.site()}, {detail});")

    def jump_table(self, table: JumpTable) -> None:
        """A switch discovery resolved (``tbb``/``tbh``, ``ldr pc``, ``add pc``, a ``mov pc``
        branch table): a goto per case; an index outside the table traps."""
        by_target: dict[int, list[int]] = {}
        for value, target in table.cases:
            by_target.setdefault(target, []).append(value)
        self.emit(f"switch ({self.r(table.index)}) {{")
        for target, values in by_target.items():
            cases = " ".join(f"case {const(v)}:" for v in values)
            self.emit(f"{cases} goto {self.label(target)};")
        self.emit("default: break;")
        self.emit("}")
        self.trap("MT_TRAP_UNREACHABLE", self.r(table.index))


def check_flags(fn: Function, traces: dict[int, _Trace], flags_in: bool) -> list[str]:
    """Problems where a flag is read but may be undefined: live into the function or
    across a call (a forward must-be-defined analysis over the instruction graph)."""
    entry = set(FLAGS) if flags_in else set()
    defined: dict[int, set[str]] = {fn.start: entry}
    work = [fn.start]
    problems: set[str] = set()
    while work:
        addr = work.pop()
        if addr not in traces:
            continue
        d = defined[addr]
        t = traces[addr]
        for f in sorted(t.reads - d):
            problems.add(f"{addr:#x}: flag {f.upper()} read where it may be undefined")
        out = set() if t.kills else d | t.writes
        for s in fn.succ.get(addr, ()):
            old = defined.get(s)
            new = out if old is None else old & out
            if old is None or new != old:
                defined[s] = new
                work.append(s)
    return sorted(problems)


def _jump_table(ctx: Ctx, insn: Insn) -> None:
    ctx.jump_table(ctx.fn.tables[insn.address])


def _lift_pass(fn: Function, program: Program, opts: LiftOptions, used: set[str]):
    from recomp import lift as _registry  # noqa: F401  (registers the per-class lifters)

    ctx = Ctx(fn, program, opts, used)
    traces: dict[int, _Trace] = {}
    unsupported: list[tuple[int, str]] = []
    insns = fn.ordered()
    for i, insn in enumerate(insns):
        ctx.insn = insn
        ctx.trace = _Trace()
        ctx._tmp = 0
        if insn.address in fn.labels:
            ctx.lines.append(f"{ctx.label(insn.address)}:;")
        if opts.comments:
            ctx.emit(f"/* {insn.address:08X}: {insn.text()} */")
        start = len(ctx.lines)
        try:
            table = fn.tables.get(insn.address)
            key = f"neon:{insn.mnemonic}" if insn.simd else insn.mnemonic  # lift/neon.py
            body = LIFTERS.get(key) if table is None else _jump_table
            if body is None or not insn.known:
                raise Unsupported(insn.text() if insn.known else f"undefined {insn.raw:#x}")
            if insn.cond != AL:
                ctx.emit(f"if ({ctx.cond(insn.cond)}) {{")
                ctx.depth += 1
                ctx.conditional = True
            body(ctx, insn)
        except Unsupported as exc:
            del ctx.lines[start:]
            ctx.depth = 1
            ctx._block_open = False
            unsupported.append((insn.address, str(exc)))
            ctx.trap("MT_TRAP_UNSUPPORTED", const(insn.raw))
        finally:
            while ctx.depth > 1:
                ctx.depth -= 1
                ctx.emit("}")
            ctx._block_open = False
            ctx.conditional = False
        traces[insn.address] = ctx.trace
        nxt = insns[i + 1].address if i + 1 < len(insns) else None
        if falls_through(insn) and nxt != insn.end:
            if insn.address in fn.noreturn_calls:
                ctx.trap("MT_TRAP_UNREACHABLE", const(insn.end))  # the callee returned
            elif opts.fall_off_returns:
                ctx.ret("mt_entry_lr")
            else:
                ctx.trap("MT_TRAP_FELL_OFF", const(insn.end))
    return ctx, traces, unsupported


def lift_function(
    fn: Function,
    program: Program,
    opts: LiftOptions | None = None,
    name: str | None = None,
    comment: str | None = None,
) -> Lifted:
    """Lift one discovered function to C (``void f_XXXXXXXX(mt_cpu *cpu) { ... }``)."""
    opts = opts or LiftOptions()
    _, traces, _ = _lift_pass(fn, program, opts, set(FLAGS))
    used = set().union(*(t.reads for t in traces.values())) if traces else set()
    if opts.flags_out:
        used = set(FLAGS)
    ctx, _, unsupported = _lift_pass(fn, program, opts, used)
    problems = list(fn.problems) + check_flags(fn, traces, opts.flags_in)
    name = name or c_name(fn.ptr)
    head = [f"/* {comment.replace('*/', '* /')} */"] if comment else []
    head += [f"void {name}(mt_cpu *MT_RESTRICT cpu)", "{", "    MT_FN_ENTER();"]
    for f in FLAGS:
        if f in used:
            bit = 31 - FLAGS.index(f)
            init = f"(cpu->cpsr >> {bit}) & 1u" if opts.flags_in else "0u"
            head.append(f"    uint32_t {LOCAL[f]} = {init};")
    for addr, why in unsupported:
        problems.append(f"{addr:#x}: unsupported: {why}")
    source = "\n".join(head + ctx.lines + ["}", ""])
    return Lifted(fn.ptr, name, source, unsupported, problems, ctx.callees, ctx.imports)


__all__ = [
    "COND_NAMES", "Ctx", "LIFTERS", "LiftOptions", "Lifted", "Program", "Unsupported",
    "c_name", "check_flags", "const", "lift_function", "lifter",
]  # fmt: skip
