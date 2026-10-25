"""Function discovery: which instructions belong to a function and how control flows (#32).

The #30 prototype: recursive descent from the function start, bounded by the next function
start, with IT conditions carried along the fall-through path. #32 replaces the bounds with
real extents and adds jump tables (TBB/TBH, ``ldr pc``), tail-call and noreturn detection,
literal-pool marking, the Ghidra cross-check and ``work/recomp/functions.json``; its
:class:`Function` keeps the fields below, which is what the lifter reads.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from recomp.decode import decode_thumb
from recomp.decode.insn import AL, LR, PC, SP, Insn, Mem, Reg, RegList

# How an instruction leaves: "next" (falls through only), "branch" (to targets), "call",
# "return", "jump" (indirect), "trap" (stops), "table" (switch: #32).
CALLS = frozenset({"bl", "blx"})


@dataclass
class Function:
    """``ptr`` is the function pointer (Thumb bit set for Thumb code); the instructions are
    those reachable from the start inside ``[start, end)``. ``labels`` are the addresses
    some branch inside the function targets; ``problems`` lists what discovery could not
    follow (the lifter traps there)."""

    ptr: int
    start: int
    end: int
    insns: dict[int, Insn] = field(default_factory=dict)
    succ: dict[int, tuple[int, ...]] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    problems: list[str] = field(default_factory=list)

    @property
    def thumb(self) -> bool:
        return bool(self.ptr & 1)

    def contains(self, addr: int) -> bool:
        return self.start <= addr < self.end

    def ordered(self) -> list[Insn]:
        return [self.insns[a] for a in sorted(self.insns)]


def writes_pc(insn: Insn) -> bool:
    """Whether the instruction writes the PC (other than as a direct branch)."""
    m, ops = insn.mnemonic, insn.operands
    if m in ("pop", "ldm") and isinstance(ops[-1], RegList):
        return PC in ops[-1].regs
    if m == "ldr" and ops and ops[0] == Reg(PC):
        return True
    if m in ("mov", "add") and ops and ops[0] == Reg(PC):
        return True
    return m in ("bx", "tbb", "tbh")


def is_return(insn: Insn) -> bool:
    """``bx lr``, ``pop {..., pc}``, ``ldm sp!, {..., pc}``, ``ldr pc, [sp], #4``."""
    m, ops = insn.mnemonic, insn.operands
    if m == "bx":
        return ops[0] == Reg(LR)
    if m == "pop":
        return PC in ops[-1].regs
    if m == "ldm":
        return ops[0] == Reg(SP) and PC in ops[-1].regs
    if m == "ldr" and ops[0] == Reg(PC):
        mem = ops[1]
        return isinstance(mem, Mem) and mem.base == SP and not mem.pre and mem.disp == 4
    return False


def flow(insn: Insn) -> tuple[str, tuple[int, ...]]:
    """``(kind, branch targets)``; a conditional instruction also falls through."""
    m = insn.mnemonic
    if not insn.known or m in ("trap", "udf", "bkpt"):
        return "trap", ()
    if m in ("b", "cbz", "cbnz"):
        return "branch", (insn.operands[-1].target,)
    if m in CALLS:
        return "call", ()
    if m in ("tbb", "tbh"):
        return "table", ()
    if writes_pc(insn):
        return ("return" if is_return(insn) else "jump"), ()
    return "next", ()


def falls_through(insn: Insn) -> bool:
    kind, _ = flow(insn)
    if kind in ("next", "call"):
        return True
    if insn.mnemonic in ("cbz", "cbnz"):
        return True
    return insn.cond != AL


def discover_function(code: bytes, base: int, ptr: int, end: int) -> Function:
    """Discover the Thumb function at ``ptr`` from ``code`` (the bytes at guest ``base``),
    not crossing ``end``."""
    start = ptr & ~1
    fn = Function(ptr, start, end)
    if not ptr & 1:
        fn.problems.append(f"{start:#x}: ARM-mode function (decoder: #33)")
        return fn
    work: list[tuple[int, tuple[int, ...]]] = [(start, ())]
    while work:
        addr, pending = work.pop()
        while addr not in fn.insns:
            if not fn.contains(addr):
                fn.problems.append(f"flow leaves the function at {addr:#x}")
                break
            cond = pending[0] if pending else None
            pending = pending[1:]
            insn = decode_thumb(code, addr - base, addr, cond)
            fn.insns[addr] = insn
            kind, targets = flow(insn)
            succ = []
            if insn.mnemonic.startswith("it") and insn.known and cond is None:
                pending = insn.it_conditions()
            for t in targets:
                if fn.contains(t):
                    fn.labels.add(t)
                    succ.append(t)
                    work.append((t, ()))
            if kind == "table":
                fn.problems.append(f"{addr:#x}: {insn.mnemonic} jump table (#32)")
            if not falls_through(insn):
                fn.succ[addr] = tuple(succ)
                break
            succ.append(insn.end)
            fn.succ[addr] = tuple(succ)
            addr = insn.end
    return fn


__all__ = ["Function", "discover_function", "falls_through", "flow", "is_return", "writes_pc"]
