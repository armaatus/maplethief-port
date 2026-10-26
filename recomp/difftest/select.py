"""Which real functions the differential test runs: the leaf functions (#41).

Static criteria, applied in this order to every function of the whole-image discovery (each
one's count is in the report, ``Selection.counts``):

1. ``discovered``: discovery finished it with no problem (``Function.problems`` empty).
2. ``returns``: it is not noreturn and has no noreturn call.
3. ``closed``: no SjLj entry (``entries``), no indirect jump that is not a table
   (``indirect``) and no computed code pointer (``code_refs``): control stays inside
   functions the test knows.
4. ``plain``: no instruction the emulator and the C runtime would handle differently by
   design: ``svc``/``udf``/``bkpt`` (traps), coprocessor and status-register access
   (``mrc``/``mcr``/``mrs``/``msr``/``vmsr``) and the wait hints (``wfi``/``wfe``/``sev``/
   ``yield``).
5. ``clean``: it lifts with no unsupported instruction and no problem, and the C has no trap
   other than a jump table's out-of-range default, and no indirect call or jump.
6. ``leaf``: every call and tail call goes to another selected function (a fixpoint: a
   function that calls an import or a function that fails a criterion drops out, then its
   callers do). So no imports.

"Bounded memory access" is checked when the test runs (``recomp.difftest``): a run must
return within an instruction budget, and an access outside the guest window must fault at
the same address on both sides.

Standard library only (ADR 0014): selecting needs neither Unicorn nor a C compiler.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from recomp.decode.insn import DReg, Mem, QReg, Reg, SReg, VRegList
from recomp.discover import Discovery, Function
from recomp.lift.core import Lifted, LiftOptions, c_name, lift_function

EXCLUDED_MNEMONICS = frozenset(
    {"svc", "udf", "bkpt", "trap", "mrc", "mcr", "mrc2", "mcr2", "mrrc", "mcrr", "mrs", "msr",
     "vmsr", "wfi", "wfe", "sev", "yield", "cps", "setend"}
)  # fmt: skip
CRITERIA = ("discovered", "returns", "closed", "plain", "clean", "leaf")
ALLOWED_TRAP = "MT_TRAP(MT_TRAP_UNREACHABLE"  # a jump table's default (index out of range)


class ImageProgram:
    """:class:`recomp.lift.core.Program` over the image: every function in ``lifted`` is
    callable by name, an import stub is an import (``(0, symbol)``: the id does not matter,
    since a function that calls one is no leaf)."""

    def __init__(self, image, lifted: set[int]) -> None:
        self.image, self.lifted = image, lifted

    def const_word(self, addr: int, size: int) -> int | None:
        return self.image.const_word(addr, size)

    def callee(self, ptr: int) -> str | None:
        return c_name(ptr) if ptr in self.lifted else None

    def import_at(self, addr: int) -> tuple[int, str] | None:
        stub = self.image.stubs.get(addr & ~1)
        return None if stub is None else (0, stub.symbol)


@dataclass
class Leaf:
    """A selected function: its discovery, its production lift and which registers it uses
    (``pointer_regs``: r0-r12 used as a memory base, given heap pointers by the inputs;
    ``vfp``: it touches VFP registers)."""

    fn: Function
    lifted: Lifted
    pointer_regs: frozenset[int]
    vfp: bool

    @property
    def ptr(self) -> int:
        return self.fn.ptr

    @property
    def has_calls(self) -> bool:
        return bool(self.fn.calls or self.fn.tail_calls)


@dataclass
class Selection:
    leaves: dict[int, Leaf]
    counts: dict[str, int] = field(default_factory=dict)  # criterion -> functions passing it
    rejected: dict[int, str] = field(default_factory=dict)  # ptr -> first failed criterion


def _plain(fn: Function) -> bool:
    return not any(i.mnemonic in EXCLUDED_MNEMONICS for i in fn.insns.values())


def _clean(lifted: Lifted) -> bool:
    if not lifted.clean or lifted.imports:
        return False
    src = lifted.source
    if "MT_CALL_INDIRECT" in src or "MT_JUMP_INDIRECT" in src or "MT_SJLJ" in src:
        return False
    return src.count("MT_TRAP(") == src.count(ALLOWED_TRAP)


def pointer_regs(fn: Function) -> frozenset[int]:
    """r0-r12 that some instruction uses as the base of a memory operand (or of ldm/stm)."""
    regs: set[int] = set()
    for insn in fn.insns.values():
        ops = insn.operands
        for op in ops:
            if isinstance(op, Mem) and op.base < 13:
                regs.add(op.base)
        if insn.mnemonic.startswith(("ldm", "stm", "vldm", "vstm")) and ops:
            if isinstance(ops[0], Reg) and ops[0].n < 13:
                regs.add(ops[0].n)
    return frozenset(regs)


def uses_vfp(fn: Function) -> bool:
    for insn in fn.insns.values():
        if insn.mnemonic.startswith("v"):
            return True
        if any(isinstance(op, (SReg, DReg, QReg, VRegList)) for op in insn.operands):
            return True
    return False


def select_leaves(image, discovery: Discovery) -> Selection:
    """The leaf functions of ``image`` (criteria in the module docstring)."""
    fns = discovery.functions
    alive: dict[int, Function] = {}
    rejected: dict[int, str] = {}
    counts = dict.fromkeys(CRITERIA, 0)
    for ptr, fn in sorted(fns.items()):
        checks = (
            ("discovered", not fn.problems),
            ("returns", not fn.noreturn and not fn.noreturn_calls),
            ("closed", not fn.entries and not fn.indirect and not fn.code_refs),
            ("plain", _plain(fn)),
        )
        failed = next((name for name, ok in checks if not ok), None)
        for name, ok in checks:
            if not ok:
                break
            counts[name] += 1
        if failed:
            rejected[ptr] = failed
        else:
            alive[ptr] = fn
    # Lift with every discovered function callable (a call to a rejected one is the leaf
    # criterion's business), drop the unclean ones, then every function that calls something
    # that is not selected, until none is left.
    program = ImageProgram(image, set(fns))
    lifted: dict[int, Lifted] = {}
    for ptr, fn in alive.items():
        lf = lift_function(fn, program, LiftOptions())
        if _clean(lf):
            lifted[ptr] = lf
        else:
            rejected[ptr] = "clean"
    counts["clean"] = len(lifted)
    members = set(lifted)
    while True:
        drop = {
            p
            for p in members
            if any(t not in members for t in (*fns[p].calls.values(), *fns[p].tail_calls.values()))
        }
        if not drop:
            break
        members -= drop
        for p in drop:
            rejected[p] = "leaf"
    counts["leaf"] = len(members)
    leaves = {
        p: Leaf(fns[p], lifted[p], pointer_regs(fns[p]), uses_vfp(fns[p])) for p in sorted(members)
    }
    return Selection(leaves, counts, rejected)


__all__ = [
    "CRITERIA", "EXCLUDED_MNEMONICS", "ImageProgram", "Leaf", "Selection", "pointer_regs",
    "select_leaves", "uses_vfp",
]  # fmt: skip
