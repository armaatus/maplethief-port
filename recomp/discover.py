"""Function discovery: every function's instructions, blocks, edges, tables and data (#32).

Recursive descent with our own decoder (ADR 0014); Ghidra and Capstone are cross-checks in
the report and the tests, never inputs. Per function (:func:`discover_function`):

- **Descent** from the function pointer (Thumb bit = mode: Thumb code with ``decode_thumb``,
  the one ARM function, the entry stub, with ``decode_arm``), bounded by the next
  ``LC_FUNCTION_STARTS`` entry (``end``), the IT conditions carried along the fall-through
  path. ``bl`` keeps the instruction set and ``blx #imm`` switches it (from Thumb: to the
  ARM ``__symbolstub1`` stubs): each call is recorded with the callee's pointer, so the
  mode travels with it. ``bx``/``blx rN``
  are dynamic (#35). A ``b`` out of the function is a **tail call**; to an address that is no
  function start it is a problem.
- **Jump tables**: ``tbb``/``tbh [pc, rI]``, ``ldr pc, [rB, rI, lsl #2]`` (absolute entries),
  ``ldr rT, [rB, rI, lsl #2]; add pc, rT`` (entries relative to the ``add``) and
  ``add.w rT, rB, rI, lsl #2; mov pc, rT`` (``branch``: a table of ``b.w`` instructions, the
  SjLj dispatch of Chartboost's clang code), with ``rB`` from ``adr``/``addw rB, pc``. The
  entry count comes from the ``cmp`` that guards the index: the single-predecessor path
  before the table is evaluated with value numbers, so the index is followed through ``mov``
  and stack-slot reloads (the -O0 ``str r0, [sp]; bhi; ldr r1, [sp]; tbb [pc, r1]``) to a
  ``cmp rX, #N`` with a ``bhi``/``bhs`` that skips the table, or a ``bls``/``blo`` that goes
  to it. Every target becomes a label and a successor edge. Once the whole function is
  known the bound is checked again (no new edge into the guard path).
- **Noreturn calls**: a call to :data:`NORETURN_IMPORTS` or to a function proven not to
  return does not fall through (the lifter traps after it).
- **Data**: the words PC-relative loads read (literal pools) and the tables; an instruction
  overlapping data is a problem.
- **Internal entries**: a code address computed inside the function that points into it,
  the SjLj resume address stored into the jump buffer (``mov r4, pc; adds r4, #7`` in the
  game, ``ldr r1, =k; orr r1, #1; add r1, pc`` in Chartboost; #37), is a second way in;
  its code (the landing pads) is discovered from there.

:func:`discover` runs that over the whole image: the seeds (function starts, entry point,
init functions, ObjC IMPs, vtable slots, call and tail-call targets, code pointers in data
and computed in code), the noreturn fixpoint, and ``work/recomp/functions.json``
(:meth:`Discovery.to_json`, schema below). ``recomp.discovery_report`` writes the report with
the Ghidra cross-check.

**functions.json**, schema 1 (``ghidra_common.dumps`` layout: header keys one per line, then
one function per line, sorted by address; addresses are ``"0x"`` + upper-case hex, function
pointers carry the Thumb bit)::

    {"schema": 1, "source": {"path", "arch", "sha256"},
     "counts": {"functions", "thumb", "arm", "failed", "instructions", "blocks", "tables",
                "tail_calls", "noreturn_functions", "noreturn_calls", "entries", ...},
     "noreturn": [{"address": "0xF0E60", "why": "import _abort"}, ...],
     "seeds": [{"ptr": "0x2851", "sources": ["function_starts", "objc"]}, ...],
     "functions": [
      {"ptr": "0x2851", "mode": "thumb", "start": "0x2850",
       "end": "0x28A8",                 # extent: end of the last instruction or data item
       "limit": "0x28A8",               # the next function start (descent bound)
       "instructions": 23, "code_bytes": 88, "noreturn": false,
       "blocks": [{"start": "0x2850", "end": "0x2860", "succ": ["0x2866", "0x2860"],
                   "exit": "cond"}],    # exit: fall branch cond return tail table jump
                                        # noreturn trap off (flow leaves the function)
       "calls": [["0x2858", "0xF0FA8"]],          # site, callee pointer (bit 0 = Thumb)
       "tail_calls": [["0x2870", "0x2A01"]],
       "noreturn_calls": ["0x2860"],
       "tables": [{"site": "0x60D4", "kind": "tbb", "base": "0x60D8", "entry_size": 1,
                   "count": 6, "index": 1, "bound": "0x60C8",   # kind: tbb tbh ldr add
                   "cases": [[0, "0x60E0"], ...]}],   # branch; value of r[index] -> target
       "data": [["0x2890", 4, "literal"], ["0x60D8", 6, "table"]],
       "entries": [["0x58CC", "code address taken at 0x58C4 (sjlj resume)"]],
       "code_refs": [["0x2862", "0x8E29"]],       # code pointers computed in this function
       "data_refs": [["0xD19E2", "0xD1EB0"]],     # addresses of in-function data tables
       "indirect": ["0x2866"],                    # indirect jumps that are not tables
       "problems": []}]}

The input is untrusted data: it is decoded, never executed.
"""

from __future__ import annotations

import bisect
import itertools
import time
from collections.abc import Iterable
from dataclasses import dataclass, field

from recomp.decode import decode_arm, decode_thumb
from recomp.decode.insn import (
    AL,
    CC,
    COMPARES,
    LR,
    LS,
    PC,
    SP,
    DReg,
    Imm,
    Insn,
    Label,
    Mem,
    Reg,
    RegList,
    Shifted,
)

# How an instruction leaves: "next" (falls through only), "branch" (to targets), "call",
# "return", "jump" (indirect), "trap" (stops), "table" (tbb/tbh switch).
CALLS = frozenset({"bl", "blx"})

# Imports that never return. A call to one of them ends the path (the lifter traps after it).
NORETURN_IMPORTS = frozenset(
    {
        "_abort",
        "_exit",
        "__exit",
        "___assert_rtn",
        "___stack_chk_fail",
        "__ZSt9terminatev",  # std::terminate()
        "___cxa_throw",
        "___cxa_rethrow",
        "___cxa_bad_cast",
        "___cxa_bad_typeid",
        "___cxa_pure_virtual",
        "__Unwind_SjLj_Resume",
        "__Unwind_Resume",
        "_objc_exception_throw",
        "_objc_exception_rethrow",
        "_longjmp",
        "_siglongjmp",
        "_pthread_exit",
    }
)

# Registers a call may change (AAPCS caller-saved, plus lr).
CALL_CLOBBERS = frozenset({0, 1, 2, 3, 12, LR})
MAX_TABLE = 4096  # entries; a larger bound is not a switch
_WALK = 64  # instructions to walk back from a table to its bound

_LOAD_SIZE = {
    "ldr": 4, "ldrb": 1, "ldrsb": 1, "ldrh": 2, "ldrsh": 2, "ldrd": 8, "vldr": 0,
}  # fmt: skip


@dataclass(frozen=True)
class JumpTable:
    """A resolved switch at ``site``. ``kind``: ``tbb``/``tbh`` (offsets from ``site + 4``,
    halved), ``ldr`` (``ldr pc``: absolute pointers) or ``add`` (``add pc, rT``: offsets
    from ``site + 4``). ``cases`` maps the value of ``r[index]`` at ``site`` to the target:
    the entry number for ``tbb``/``tbh``/``ldr``, the loaded entry for ``add``. ``bound``
    is the address of the ``cmp`` that limits the index."""

    site: int
    kind: str
    base: int
    entry_size: int
    count: int
    index: int
    bound: int
    cases: tuple[tuple[int, int], ...]

    @property
    def targets(self) -> tuple[int, ...]:
        return tuple(sorted({t for _, t in self.cases}))

    @property
    def size(self) -> int:
        """Bytes of the table, padded to a halfword (the code after it is aligned)."""
        n = self.count * self.entry_size
        return n + (n & 1)


@dataclass
class Function:
    """``ptr`` is the function pointer (Thumb bit set for Thumb code); the instructions are
    those reachable from the start (and :attr:`entries`) inside ``[start, end)``.
    ``labels`` are the addresses some branch or table inside the function targets;
    ``succ`` maps each instruction to its successors inside the function (the fall-through
    last); ``problems`` lists what discovery could not follow (the lifter traps there).

    The rest is #32's: ``tables`` (by site), ``data`` (address -> (size, kind)), ``calls``
    and ``tail_calls`` (site -> target pointer), ``noreturn_calls`` (sites that do not fall
    through), ``entries`` (address -> why), ``code_refs`` (site -> code pointer computed
    there), ``data_refs`` (site -> PC-relative address inside the function that is not code:
    a constant table read through a register), ``indirect`` (indirect jumps that are not
    tables), ``noreturn`` (set by :func:`discover`)."""

    ptr: int
    start: int
    end: int
    insns: dict[int, Insn] = field(default_factory=dict)
    succ: dict[int, tuple[int, ...]] = field(default_factory=dict)
    labels: set[int] = field(default_factory=set)
    problems: list[str] = field(default_factory=list)
    tables: dict[int, JumpTable] = field(default_factory=dict)
    data: dict[int, tuple[int, str]] = field(default_factory=dict)
    calls: dict[int, int] = field(default_factory=dict)
    tail_calls: dict[int, int] = field(default_factory=dict)
    noreturn_calls: set[int] = field(default_factory=set)
    entries: dict[int, str] = field(default_factory=dict)
    code_refs: dict[int, int] = field(default_factory=dict)
    data_refs: dict[int, int] = field(default_factory=dict)
    indirect: set[int] = field(default_factory=set)
    noreturn: bool = False

    @property
    def thumb(self) -> bool:
        return bool(self.ptr & 1)

    @property
    def mode(self) -> str:
        return "thumb" if self.thumb else "arm"

    def contains(self, addr: int) -> bool:
        return self.start <= addr < self.end

    def ordered(self) -> list[Insn]:
        return [self.insns[a] for a in sorted(self.insns)]

    @property
    def extent(self) -> int:
        """The end of the last instruction or data item (``start`` if there are none)."""
        ends = [i.end for i in self.insns.values()]
        ends += [a + size for a, (size, _) in self.data.items() if self.contains(a)]
        return max(ends, default=self.start)

    @property
    def code_bytes(self) -> int:
        return sum(i.size for i in self.insns.values())

    def returns(self, noreturn: Iterable[int] = ()) -> bool:
        """Whether some path may return to the caller: a return, an indirect jump, a tail
        call to a function not in ``noreturn`` (plain addresses), or flow leaving the
        function."""
        if self.indirect or any("leaves" in p for p in self.problems):
            return True
        nr = noreturn if isinstance(noreturn, (set, frozenset)) else set(noreturn)
        if any(t & ~1 not in nr for t in self.tail_calls.values()):
            return True
        return any(is_return(i) for i in self.insns.values())

    def blocks(self) -> list[Block]:
        """Basic blocks in address order. A block starts at the entry, an internal entry, a
        label, after an instruction that does not just fall through, or after a gap."""
        addrs = sorted(self.insns)
        leaders = {self.start} | self.labels | set(self.entries)
        for a in addrs:
            insn = self.insns[a]
            if self.succ.get(a) != (insn.end,):
                leaders.add(insn.end)
        out: list[Block] = []
        first = None
        for k, a in enumerate(addrs):
            if first is None:
                first = a
            insn = self.insns[a]
            nxt = addrs[k + 1] if k + 1 < len(addrs) else None
            if nxt is None or nxt in leaders or nxt != insn.end:
                out.append(Block(first, insn.end, self.succ.get(a, ()), self._exit(insn)))
                first = None
        return out

    def _exit(self, insn: Insn) -> str:
        a = insn.address
        succ = self.succ.get(a, ())
        if a in self.tables:
            return "table"
        if a in self.noreturn_calls:
            return "noreturn"
        kind, targets = flow(insn)
        if kind == "branch":
            if a in self.tail_calls:
                return "tail" if insn.cond == AL else "cond"
            if insn.cond != AL or insn.mnemonic in ("cbz", "cbnz"):
                return "cond"
            return "branch"
        if kind == "return":
            return "return" if insn.cond == AL else "cond"
        if kind == "jump":
            return "jump" if insn.cond == AL else "cond"
        if kind == "trap":
            return "trap"
        if falls_through(insn) and insn.end not in succ:
            return "off"
        return "fall"


@dataclass(frozen=True)
class Block:
    """``[start, end)`` of one basic block, its successors and how it ends."""

    start: int
    end: int
    succ: tuple[int, ...]
    exit: str


# -- instruction properties -------------------------------------------------------------------


def writes_pc(insn: Insn) -> bool:
    """Whether the instruction writes the PC (other than as a direct branch)."""
    m, ops = insn.mnemonic, insn.operands
    if m in ("pop", "ldm", "ldmdb") and isinstance(ops[-1], RegList):
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


def is_simd(insn: Insn) -> bool:
    """An Advanced SIMD (NEON) encoding the decoder does not know yet (#34): 32 bits, in the
    data-processing space ``111x 1111`` or the element load/store space ``1111 1001 xxx0``.
    None of them writes the PC, so discovery steps over them; the lifter traps there."""
    if insn.known or insn.size != 4:
        return False
    if insn.arm:
        return False
    hw1 = insn.raw >> 16
    return hw1 & 0xEF00 == 0xEF00 or hw1 & 0xFF10 == 0xF900


def flow(insn: Insn) -> tuple[str, tuple[int, ...]]:
    """``(kind, branch targets)``; a conditional instruction also falls through."""
    m = insn.mnemonic
    if is_simd(insn):
        return "next", ()
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


def call_target(insn: Insn) -> int | None:
    """The callee pointer of ``bl``/``blx #imm`` (bit 0 = Thumb), None for ``blx rN``.
    ``bl`` keeps the instruction set, ``blx #imm`` switches it."""
    op = insn.operands[0] if insn.operands else None
    if not isinstance(op, Label):
        return None
    thumb = (insn.mnemonic == "bl") != insn.arm
    return op.target | 1 if thumb else op.target


def literal(insn: Insn) -> tuple[int, int] | None:
    """``(address, size)`` of the word a PC-relative load reads (a literal pool entry)."""
    size = _LOAD_SIZE.get(insn.mnemonic)
    if size is None:
        return None
    mem = insn.operands[-1]
    if not isinstance(mem, Mem) or mem.base != PC or mem.index is not None:
        return None
    if size == 0:  # vldr: the width of the register
        size = 8 if isinstance(insn.operands[0], DReg) else 4
    return (insn.pc & ~3) + mem.disp, size


def written_regs(insn: Insn) -> frozenset[int] | None:
    """Core registers the instruction may write (None: any, e.g. unknown encodings)."""
    m, ops = insn.mnemonic, insn.operands
    if not insn.known:
        return None
    if m in CALLS:
        return CALL_CLOBBERS
    out: set[int] = set()
    for op in ops:
        if isinstance(op, Mem) and (op.wback or not op.pre):
            out.add(op.base)
    if insn.wback and ops and isinstance(ops[0], Reg):
        out.add(ops[0].n)
    if m in ("push", "vpush"):
        return frozenset({SP})
    if m in ("pop", "ldm", "ldmdb") and ops and isinstance(ops[-1], RegList):
        out.update(ops[-1].regs)
        if m == "pop":
            out.add(SP)
        return frozenset(out)
    if m.startswith(("st", "vst", "it")) or m in COMPARES or m in (
        "b", "cbz", "cbnz", "bx", "tbb", "tbh", "nop", "dmb", "dsb", "isb", "pld", "trap",
    ):  # fmt: skip
        return frozenset(out)
    if m.startswith("v") and m not in ("vmov", "vmrs"):
        return frozenset(out)
    regs = [op.n for op in ops if isinstance(op, Reg)]
    if m in ("ldrd", "umull", "smull", "umlal", "smlal") or (m == "vmov" and len(regs) == 2):
        out.update(regs[:2])
    elif ops and isinstance(ops[0], Reg):
        out.add(ops[0].n)
    return frozenset(out)


def sets_flags(insn: Insn) -> bool:
    m = insn.mnemonic
    return insn.setflags or m in COMPARES or m in CALLS or m == "vmrs" or not insn.known


# -- one function -------------------------------------------------------------------------------


class _Descent:
    def __init__(self, fn: Function, code: bytes, base: int, starts, noreturn) -> None:
        self.fn, self.code, self.base = fn, code, base
        self.starts = starts
        self.noreturn = noreturn
        self.work: list[tuple[int, tuple[int, ...]]] = [(fn.start, ())]
        self.pending: list[int] = []  # tbb/tbh/indirect jump sites to resolve
        self._preds: dict[int, list[int]] | None = None

    def run(self) -> None:
        fn = self.fn
        while True:
            while True:
                self.drain()
                if not self.pending:
                    break
                self._preds = None
                sites, self.pending = self.pending, []
                for site in sites:
                    self.resolve(site)
            if not self.find_code_refs():
                break
        self._preds = None
        for site, table in list(fn.tables.items()):  # the whole function is known now
            again = self.table_at(fn.insns[site])
            if not isinstance(again, JumpTable) or again != table:
                why = again if isinstance(again, str) else "bound changed"
                fn.problems.append(f"{site:#x}: {table.kind} bound not stable: {why}")
        self.check_data()

    # -- descent ----------------------------------------------------------------------------

    def drain(self) -> None:
        fn = self.fn
        while self.work:
            addr, pending = self.work.pop()
            while addr not in fn.insns:
                if not fn.contains(addr):
                    fn.problems.append(f"flow leaves the function at {addr:#x}")
                    break
                cond = pending[0] if pending else None
                pending = pending[1:]
                if fn.thumb:
                    insn = decode_thumb(self.code, addr - self.base, addr, cond)
                else:
                    insn = decode_arm(self.code, addr - self.base, addr)
                fn.insns[addr] = insn
                self.note(insn)
                kind, targets = flow(insn)
                succ: list[int] = []
                if insn.mnemonic.startswith("it") and insn.known and cond is None:
                    pending = insn.it_conditions()
                for t in targets:
                    if fn.contains(t):
                        self.goto(t)
                        succ.append(t)
                    else:
                        self.tail_call(addr, t)
                if kind == "call":
                    self.call(insn)
                elif kind in ("table", "jump"):
                    self.pending.append(addr)
                if not falls_through(insn) or addr in fn.noreturn_calls:
                    fn.succ[addr] = tuple(succ)
                    break
                succ.append(insn.end)
                fn.succ[addr] = tuple(succ)
                addr = insn.end

    def goto(self, target: int) -> None:
        self.fn.labels.add(target)
        self.work.append((target, ()))

    def tail_call(self, site: int, target: int) -> None:
        fn = self.fn
        fn.tail_calls[site] = target | (1 if fn.thumb else 0)
        if self.starts is not None and target not in self.starts:
            fn.problems.append(f"{site:#x}: branch to {target:#x}, outside the function and "
                               "not a function start")  # fmt: skip

    def call(self, insn: Insn) -> None:
        target = call_target(insn)
        if target is None:
            return
        self.fn.calls[insn.address] = target
        if target & ~1 in self.noreturn and insn.cond == AL:
            self.fn.noreturn_calls.add(insn.address)

    def note(self, insn: Insn) -> None:
        """Literal pools: the words PC-relative loads read."""
        fn = self.fn
        lit = literal(insn)
        if lit is not None:
            addr, size = lit
            if fn.contains(addr):
                old = fn.data.get(addr, (0, "literal"))
                fn.data[addr] = (max(size, old[0]), old[1])

    # -- jump tables ------------------------------------------------------------------------

    def preds(self) -> dict[int, list[int]]:
        if self._preds is None:
            preds: dict[int, list[int]] = {}
            for a, ss in self.fn.succ.items():
                for s in ss:
                    preds.setdefault(s, []).append(a)
            for e in self.fn.entries:
                preds.setdefault(e, []).append(-1)  # a second way in: not a single pred
            self._preds = preds
        return self._preds

    def resolve(self, site: int) -> None:
        fn = self.fn
        insn = fn.insns[site]
        table = self.table_at(insn)
        if isinstance(table, JumpTable):
            fn.tables[site] = table
            if table.kind != "branch":  # a branch table is code: one b.w per case
                fn.data[table.base] = (table.size, "table")
            fn.succ[site] = table.targets
            for t in table.targets:
                self.goto(t)
        elif insn.mnemonic in ("tbb", "tbh"):
            fn.problems.append(f"{site:#x}: {insn.mnemonic} jump table not resolved: {table}")
        elif not is_return(insn):
            fn.indirect.add(site)

    def table_at(self, insn: Insn) -> JumpTable | str:
        """The table of the switch instruction ``insn``, or why there is none."""
        m, ops = insn.mnemonic, insn.operands
        site = insn.address
        if m in ("tbb", "tbh"):
            mem = ops[0]
            if mem.base != PC or mem.index is None:
                return f"table base {mem.text()} is not pc"
            return self.make_table(insn, m, site + 4, 1 if m == "tbb" else 2, mem.index, site)
        if m == "ldr" and ops[0] == Reg(PC) and isinstance(ops[1], Mem):
            mem = ops[1]
            if mem.index is None or mem.shift != 2 or mem.disp or not mem.pre or mem.wback:
                return "not a table load"
            base = self.constant(site, mem.base)
            if base is None:
                return f"table base {Reg(mem.base).text()} is not a constant"
            return self.make_table(insn, "ldr", base, 4, mem.index, site)
        if m in ("mov", "add") and len(ops) == 2 and ops[0] == Reg(PC):
            if not isinstance(ops[1], Reg):
                return "not a table"
            d = self.definition(site, ops[1].n)
            if (
                m == "mov"
                and d is not None
                and d.mnemonic == "add"
                and len(d.operands) == 3
                and isinstance(d.operands[2], Shifted)
                and d.operands[2].kind == "lsl"
                and d.operands[2].amount == 2
                and not d.setflags
            ):
                # addw rB, pc, #k; add.w rT, rB, rI, lsl #2; mov pc, rT (LLVM's SjLj
                # dispatch): a table of 4-byte branches at rB, switched on rI.
                index = d.operands[2].n
                if not self.unchanged(site, d.address, index):
                    return f"index r{index} changes before the jump"
                base = self.constant(d.address, d.operands[1].n)
                if base is None:
                    return "branch table base is not a constant"
                return self.make_table(insn, "branch", base, 4, index, d.address)
        if m == "add" and len(ops) == 2 and ops[0] == Reg(PC) and isinstance(ops[1], Reg):
            load = self.definition(site, ops[1].n)
            if load is None or load.mnemonic != "ldr" or load.cond != AL:
                return "the offset is not loaded from a table"
            mem = load.operands[1]
            if not isinstance(mem, Mem) or mem.index is None or mem.shift != 2 or mem.disp:
                return "the offset is not loaded from a table"
            if not mem.pre or mem.wback:
                return "the offset is not loaded from a table"
            base = self.constant(load.address, mem.base)
            if base is None:
                return f"table base {Reg(mem.base).text()} is not a constant"
            return self.make_table(insn, "add", base, 4, mem.index, load.address, ops[1].n)
        return "not a table"

    def make_table(
        self,
        insn: Insn,
        kind: str,
        base: int,
        size: int,
        index: int,
        use: int,
        switch_reg: int | None = None,
    ) -> JumpTable | str:
        fn = self.fn
        found = self.bound(use, index)
        if isinstance(found, str):
            return found
        count, cmp_at = found
        if not 0 < count <= MAX_TABLE:
            return f"bound {count} out of range"
        if not (fn.contains(base) and base + count * size <= fn.end):
            return f"table {base:#x} + {count} x {size} leaves the function"
        off = base - self.base
        raw = self.code[off : off + count * size]
        entries = [int.from_bytes(raw[i * size : (i + 1) * size], "little") for i in range(count)]
        if kind == "branch":
            cases = [(i, base + 4 * i) for i in range(count)]
            return JumpTable(insn.address, kind, base, size, count, index, cmp_at, tuple(cases))
        cases = []
        for i, e in enumerate(entries):
            if kind in ("tbb", "tbh"):
                value, target = i, insn.address + 4 + 2 * e
            elif kind == "ldr":
                if not e & 1:
                    return f"entry {i} ({e:#x}) is not a Thumb address"
                value, target = i, e & ~1
            else:
                value, target = e, (insn.address + 4 + e) & 0xFFFFFFFE
            if not fn.contains(target):
                return f"entry {i} target {target:#x} is outside the function"
            if base <= target < base + count * size:
                return f"entry {i} target {target:#x} is inside the table"
            cases.append((value, target))
        if kind == "add":
            cases = sorted(set(cases))
            if len({v for v, _ in cases}) != len(cases):
                return "two targets for one offset"
        reg = index if switch_reg is None else switch_reg
        return JumpTable(insn.address, kind, base, size, count, reg, cmp_at, tuple(cases))

    def chain(self, addr: int):
        """Walk back from ``addr`` along single-predecessor edges: yields
        ``(instruction, taken)``, ``taken`` = reached through its branch, not by falling
        through. Stops at a join, an entry or after :data:`_WALK` steps."""
        preds = self.preds()
        for _ in range(_WALK):
            ps = preds.get(addr, ())
            if len(ps) != 1 or ps[0] < 0:
                return
            insn = self.fn.insns[ps[0]]
            yield insn, insn.end != addr
            addr = insn.address

    def definition(self, addr: int, reg: int) -> Insn | None:
        """The instruction before ``addr`` (on its single-predecessor chain) that last wrote
        ``reg``, if it is unconditional."""
        for insn, _ in self.chain(addr):
            w = written_regs(insn)
            if w is None or reg in w:
                return insn if insn.cond == AL and not insn.in_it else None
        return None

    def unchanged(self, addr: int, since: int, reg: int) -> bool:
        """Whether nothing on the chain from ``since`` (exclusive) to ``addr`` writes
        ``reg``."""
        for insn, _ in self.chain(addr):
            if insn.address == since:
                return True
            w = written_regs(insn)
            if w is None or reg in w:
                return False
        return False

    def constant(self, addr: int, reg: int) -> int | None:
        """The value of ``reg`` at ``addr`` if an ``adr``/``addw rX, pc, #k`` set it."""
        if reg == PC:
            return None
        d = self.definition(addr, reg)
        if d is None:
            return None
        ops = d.operands
        if d.mnemonic == "adr" and isinstance(ops[1], Imm):
            return (d.pc & ~3) + ops[1].value
        if d.mnemonic in ("add", "addw", "sub", "subw") and len(ops) == 3:
            if ops[1] == Reg(PC) and isinstance(ops[2], Imm):
                k = ops[2].value if d.mnemonic.startswith("add") else -ops[2].value
                return (d.pc & ~3) + k
        return None

    def bound(self, use: int, index: int) -> tuple[int, int] | str:
        """``(entry count, cmp address)`` for the index register ``index`` read at ``use``.

        The single-predecessor path before ``use`` is evaluated forward with symbolic
        values: registers and stack slots (``[sp, #k]``, ``[r7, #k]``) hold value numbers,
        copied by ``mov``, ``ldr`` and ``str`` of a slot, fresh after any other write. A
        ``cmp rX, #N`` followed on the path by the ``ls``/``lo`` side of a branch bounds
        rX's value; the bound of the value the index holds at ``use`` is the answer."""
        path = list(self.chain(use))[::-1]
        sym: dict[tuple, int] = {}
        fresh = itertools.count()

        def get(loc: tuple) -> int:
            if loc not in sym:
                sym[loc] = next(fresh)
            return sym[loc]

        flags: tuple[int, int, int] | None = None  # (value, N, cmp address)
        limits: dict[int, tuple[int, int]] = {}
        for insn, taken in path:
            m, ops = insn.mnemonic, insn.operands
            if m == "b" and insn.cond != AL:
                if flags is not None:
                    cond = insn.cond if taken else insn.cond ^ 1
                    value, n, at = flags
                    if cond == LS:
                        limits[value] = (n + 1, at)
                    elif cond == CC:
                        limits[value] = (n, at)
                continue
            if m == "cmp" and insn.cond == AL and isinstance(ops[1], Imm):
                flags = (get(("r", ops[0].n)), ops[1].value, insn.address)
                continue
            if sets_flags(insn):
                flags = None
            mem = ops[-1] if ops and isinstance(ops[-1], Mem) else None
            slot = _slot(mem) if mem is not None else None
            if insn.cond == AL and m == "mov" and len(ops) == 2 and isinstance(ops[1], Reg):
                sym[("r", ops[0].n)] = get(("r", ops[1].n))
                continue
            if insn.cond == AL and m == "ldr" and slot is not None and len(ops) == 2:
                sym[("r", ops[0].n)] = get(("m", *slot))
                continue
            if insn.cond == AL and m == "str" and slot is not None and len(ops) == 2:
                sym[("m", *slot)] = get(("r", ops[0].n))
                continue
            if mem is not None and m.startswith(("st", "vst")):
                # Any other store: forget the slots it may overlap.
                for loc in [k for k in sym if k[0] == "m" and k[1] == mem.base]:
                    if mem.index is not None or abs(loc[2] - mem.disp) < 8:
                        del sym[loc]
            w = written_regs(insn)
            if w is None:
                sym = {k: v for k, v in sym.items() if k[0] == "m"}
                continue
            for r in w:
                sym[("r", r)] = next(fresh)
                if r in (SP, 7):
                    sym = {k: v for k, v in sym.items() if not (k[0] == "m" and k[1] == r)}
        found = limits.get(get(("r", index)))
        return found if found is not None else "no cmp bounds the index"

    # -- data and code references ----------------------------------------------------------

    def check_data(self) -> None:
        fn = self.fn
        halfwords = set()
        for a, insn in fn.insns.items():
            halfwords.add(a)
            if insn.size == 4:
                halfwords.add(a + 2)
        for a, (size, kind) in sorted(fn.data.items()):
            for h in range(a & ~1, a + size, 2):
                if h in halfwords:
                    fn.problems.append(f"{h:#x}: instruction overlaps {kind} data at {a:#x}")
                    break

    def find_code_refs(self) -> bool:
        """Code pointers computed in the function: a PC-relative value (``add rX, pc`` on a
        ``movw``/``movt`` or literal constant, ``adr``/``addw rX, pc``, ``mov rX, pc``),
        possibly adjusted by ``add``/``adds``/``orr`` with an immediate, that ends up odd
        (Thumb); even ones inside the function are ``data_refs``. Straight-line
        tracking, reset at labels and entries. Callbacks and block invoke functions point at
        other functions (``code_refs``, seeds); a pointer *into* this function is the SjLj
        resume address (``mov r4, pc; adds r4, #7`` in the game code, ``ldr r1, =k;
        orr r1, #1; add r1, pc`` in Chartboost's), a second way in: it becomes an entry.
        Returns whether a new entry was found (its code is discovered next)."""
        fn = self.fn
        known: dict[int, tuple[int, bool]] = {}  # reg -> (value, PC-relative)
        refs: dict[int, int] = {}
        for insn in fn.ordered():
            a, m, ops = insn.address, insn.mnemonic, insn.operands
            if a in fn.labels or a in fn.entries:
                known.clear()
            value: tuple[int, bool] | None = None
            rd = ops[0].n if ops and isinstance(ops[0], Reg) else None
            if insn.cond == AL and rd is not None and rd != PC:
                imm = ops[-1].value if isinstance(ops[-1], Imm) else None
                src = ops[1].n if len(ops) == 3 and isinstance(ops[1], Reg) else rd
                if m == "movw" and imm is not None:
                    value = (imm, False)
                elif m == "movt" and rd in known and imm is not None:
                    value = ((known[rd][0] & 0xFFFF) | (imm << 16), False)
                elif m == "ldr" and (lit := literal(insn)) is not None and lit[1] == 4:
                    off = lit[0] - self.base
                    if 0 <= off <= len(self.code) - 4:
                        value = (int.from_bytes(self.code[off : off + 4], "little"), False)
                elif m == "mov" and len(ops) == 2 and ops[1] == Reg(PC):
                    value = (insn.pc, True)
                elif m == "adr" and imm is not None:
                    value = (((insn.pc & ~3) + imm) & 0xFFFFFFFF, True)
                elif (
                    m in ("addw", "subw")
                    and len(ops) == 3
                    and ops[1] == Reg(PC)
                    and imm is not None
                ):
                    k = imm if m == "addw" else -imm
                    value = (((insn.pc & ~3) + k) & 0xFFFFFFFF, True)
                elif m == "add" and len(ops) == 2 and ops[1] == Reg(PC) and rd in known:
                    value = ((known[rd][0] + insn.pc) & 0xFFFFFFFF, True)
                elif m in ("add", "orr") and imm is not None and src in known:
                    v, pcrel = known[src]
                    value = ((v + imm if m == "add" else v | imm) & 0xFFFFFFFF, pcrel)
                if value is not None and value[1]:
                    refs[a] = value[0]
            w = written_regs(insn)
            for r in list(known) if w is None else w:
                known.pop(r, None)
            if value is not None:
                known[rd] = value
        found = False
        fn.code_refs = {}
        fn.data_refs = {}
        for site, ref in sorted(refs.items()):
            target = ref & ~1
            if not ref & 1:
                if fn.contains(ref):  # e.g. a float table read through the address
                    fn.data_refs[site] = ref
                continue
            if fn.contains(target) and target != fn.start:
                if target not in fn.entries and not any(
                    d <= target < d + size for d, (size, _) in fn.data.items()
                ):
                    # No label: no branch goes there; #37 emits the resume hook and label.
                    fn.entries[target] = f"code address taken at {site:#x} (sjlj resume)"
                    self.work.append((target, ()))
                    found = True
                continue
            fn.code_refs[site] = ref
        return found


def _slot(mem: Mem) -> tuple[int, int] | None:
    """``(base, disp)`` of a plain ``[sp, #k]`` / ``[r7, #k]`` stack slot."""
    if mem.base in (SP, 7) and mem.index is None and mem.pre and not mem.wback:
        return mem.base, mem.disp
    return None


def discover_function(
    code: bytes,
    base: int,
    ptr: int,
    end: int,
    *,
    starts: Iterable[int] | None = None,
    noreturn: Iterable[int] = (),
) -> Function:
    """Discover the function at ``ptr`` from ``code`` (the bytes at guest ``base``), not
    crossing ``end``. ``starts``: the function start addresses (Thumb bit clear); a branch
    out of the function to anything else is a problem (None: any branch out is a tail
    call). ``noreturn``: call targets (Thumb bit clear) that never return."""
    start = ptr & ~1
    fn = Function(ptr, start, end)
    starts_set = None if starts is None else _as_set(starts)
    _Descent(fn, code, base, starts_set, _as_set(noreturn)).run()
    return fn


def _as_set(values: Iterable[int]) -> set[int] | frozenset[int]:
    return values if isinstance(values, (set, frozenset)) else set(values)


# -- the whole image ----------------------------------------------------------------------------

SEED_SOURCES = (
    "function_starts", "entry", "init", "objc", "vtable", "call", "tail", "data", "code",
)  # fmt: skip
# Seeds from these sources are discovered as functions even outside LC_FUNCTION_STARTS; the
# weak ones ("data": an odd word in a data section that points into __text, "code": an
# address computed in code) are only reported, since a data word can look like a pointer.
STRONG_SOURCES = frozenset({"function_starts", "entry", "init", "objc", "vtable", "call", "tail"})


@dataclass
class Discovery:
    """The functions of one image, by function pointer, and how they were found."""

    functions: dict[int, Function]
    seeds: dict[int, set[str]]
    noreturn: dict[int, str]  # plain address -> why
    starts: tuple[int, ...]
    seconds: float = 0.0
    _index: tuple[list[int], list[int]] = field(default=([], []), repr=False, compare=False)

    def function(self, ptr: int) -> Function | None:
        return self.functions.get(ptr)

    def containing(self, addr: int) -> Function | None:
        """The function whose ``[start, end)`` holds ``addr``."""
        if len(self._index[0]) != len(self.functions):
            ptrs = sorted(self.functions, key=lambda p: p & ~1)
            self._index = ([p & ~1 for p in ptrs], ptrs)
        starts, ptrs = self._index
        i = bisect.bisect_right(starts, addr) - 1
        if i < 0:
            return None
        fn = self.functions[ptrs[i]]
        return fn if fn.contains(addr) else None

    def failures(self) -> list[tuple[int, list[str]]]:
        """Every function with a problem, and its problems."""
        return [(p, f.problems) for p, f in sorted(self.functions.items()) if f.problems]

    def outside_starts(self) -> dict[int, set[str]]:
        """Seeds that are not ``LC_FUNCTION_STARTS`` entries, with their sources."""
        starts = set(self.starts)
        return {p: s for p, s in sorted(self.seeds.items()) if p not in starts}

    def counts(self) -> dict[str, int]:
        fns = list(self.functions.values())
        return {
            "functions": len(fns),
            "thumb": sum(f.thumb for f in fns),
            "arm": sum(not f.thumb for f in fns),
            "failed": sum(bool(f.problems) for f in fns),
            "instructions": sum(len(f.insns) for f in fns),
            "code_bytes": sum(f.code_bytes for f in fns),
            "blocks": sum(len(f.blocks()) for f in fns),
            "tables": sum(len(f.tables) for f in fns),
            "tbb": sum(t.kind == "tbb" for f in fns for t in f.tables.values()),
            "tbh": sum(t.kind == "tbh" for f in fns for t in f.tables.values()),
            "table_targets": sum(len(t.targets) for f in fns for t in f.tables.values()),
            "tail_calls": sum(len(f.tail_calls) for f in fns),
            "calls": sum(len(f.calls) for f in fns),
            "noreturn_functions": sum(f.noreturn for f in fns),
            "noreturn_calls": sum(len(f.noreturn_calls) for f in fns),
            "entries": sum(len(f.entries) for f in fns),
            "literal_bytes": sum(s for f in fns for s, k in f.data.values() if k == "literal"),
            "table_bytes": sum(s for f in fns for s, k in f.data.values() if k == "table"),
            "indirect_jumps": sum(len(f.indirect) for f in fns),
            "code_refs": sum(len(f.code_refs) for f in fns),
            "seeds": len(self.seeds),
            "seeds_outside_starts": len(self.outside_starts()),
        }

    def to_json(self, source: dict) -> str:
        """``work/recomp/functions.json`` (schema in the module docstring)."""
        from mtport import ghidra_common as gc

        header = {
            "schema": 1,
            "source": source,
            "counts": self.counts(),
            "noreturn": [{"address": _h(a), "why": w} for a, w in sorted(self.noreturn.items())],
            "seeds": [
                {"ptr": _h(p), "sources": sorted(s, key=SEED_SOURCES.index)}
                for p, s in sorted(self.seeds.items())
                if s != {"function_starts"}
            ],
        }
        records = [_function_json(self.functions[p]) for p in sorted(self.functions)]
        return gc.dumps(header, "functions", records)


def _h(value: int) -> str:
    return f"0x{value:X}"


def _function_json(fn: Function) -> dict:
    return {
        "ptr": _h(fn.ptr),
        "mode": fn.mode,
        "start": _h(fn.start),
        "end": _h(fn.extent),
        "limit": _h(fn.end),
        "instructions": len(fn.insns),
        "code_bytes": fn.code_bytes,
        "noreturn": fn.noreturn,
        "blocks": [
            {
                "start": _h(b.start),
                "end": _h(b.end),
                "succ": [_h(s) for s in b.succ],
                "exit": b.exit,
            }
            for b in fn.blocks()
        ],
        "calls": [[_h(a), _h(t)] for a, t in sorted(fn.calls.items())],
        "tail_calls": [[_h(a), _h(t)] for a, t in sorted(fn.tail_calls.items())],
        "noreturn_calls": [_h(a) for a in sorted(fn.noreturn_calls)],
        "tables": [
            {
                "site": _h(t.site),
                "kind": t.kind,
                "base": _h(t.base),
                "entry_size": t.entry_size,
                "count": t.count,
                "index": t.index,
                "bound": _h(t.bound),
                "cases": [[v, _h(target)] for v, target in t.cases],
            }
            for _, t in sorted(fn.tables.items())
        ],
        "data": [[_h(a), size, kind] for a, (size, kind) in sorted(fn.data.items())],
        "entries": [[_h(a), why] for a, why in sorted(fn.entries.items())],
        "code_refs": [[_h(a), _h(v)] for a, v in sorted(fn.code_refs.items())],
        "data_refs": [[_h(a), _h(v)] for a, v in sorted(fn.data_refs.items())],
        "indirect": [_h(a) for a in sorted(fn.indirect)],
        "problems": fn.problems,
    }


def collect_seeds(image) -> dict[int, set[str]]:
    """Function pointers from everything but the code itself: ``LC_FUNCTION_STARTS``, the
    entry point, the init functions, ObjC IMPs (``mtport.objc``), vtable slots
    (``mtport.rtti_analysis``) and odd data words that point into ``__text`` (global block
    invoke functions, callbacks in tables). Call, tail-call and code-computed targets are
    added by :func:`discover`."""
    from mtport import objc
    from mtport import rtti_analysis as ra

    seeds: dict[int, set[str]] = {}

    def add(ptr: int, source: str) -> None:
        seeds.setdefault(ptr, set()).add(source)

    for p in image.function_starts:
        add(p, "function_starts")
    if image.entry_point is not None:
        add(image.entry_point, "entry")
    for p in image.init_functions:
        add(p, "init")
    for imp in objc.imp_names(objc.parse(image.macho)):
        add(imp, "objc")
    rimg = ra.Image(image.macho)
    for vt in ra.find_vtables(rimg, ra.find_typeinfos(image.macho)):
        for slot in vt.slots:
            if slot.target:
                add(slot.target, "vtable")
    text = image.text
    for _, value in ra.data_words(image.macho):
        if value & 1 and text.addr <= value < text.end:
            add(value, "data")
    return seeds


def noreturn_imports(image) -> dict[int, str]:
    """Stub address -> ``import <name>`` for the stubs of :data:`NORETURN_IMPORTS`."""
    return {a: f"import {s.symbol}" for a, s in image.stubs.items() if s.symbol in NORETURN_IMPORTS}


def discover(image) -> Discovery:
    """Discover every function of ``image`` (a :class:`recomp.loader.Image`)."""
    t0 = time.perf_counter()
    seeds = collect_seeds(image)
    starts = tuple(image.function_starts)
    # A tail call may also go to an import stub: the ARM entry stub ends in ``b _exit``.
    start_addrs = {p & ~1 for p in starts} | set(image.stubs)
    text = image.text
    noreturn = noreturn_imports(image)
    nr: set[int] = set(noreturn)

    def run(ptr: int) -> Function:
        return discover_function(
            image.code, image.text_base, ptr, image.function_end(ptr),
            starts=start_addrs, noreturn=nr,
        )  # fmt: skip

    functions: dict[int, Function] = {}
    todo = sorted(p for p, s in seeds.items() if s & STRONG_SOURCES and text.contains(p & ~1))
    while todo:
        for p in todo:
            functions[p] = run(p)
        # Call and tail-call targets are seeds too; a new one outside the starts is analysed.
        todo = []
        for fn in list(functions.values()):
            for kind, targets in (("call", fn.calls.values()), ("tail", fn.tail_calls.values())):
                for t in targets:
                    if text.contains(t & ~1):
                        seeds.setdefault(t, set()).add(kind)
                        if t not in functions and t not in todo:
                            todo.append(t)
    # Noreturn fixpoint: a function none of whose paths returns is itself noreturn; its
    # callers are discovered again (the call no longer falls through).
    callers: dict[int, set[int]] = {}
    for p, fn in functions.items():
        for t in fn.calls.values():
            callers.setdefault(t & ~1, set()).add(p)
    while True:
        new = {p for p, f in functions.items() if p & ~1 not in nr and not f.returns(nr)}
        if not new:
            break
        for p in new:
            nr.add(p & ~1)
            noreturn[p & ~1] = "function never returns"
        for c in sorted({c for p in new for c in callers.get(p & ~1, ())}):
            functions[c] = run(c)
            for t in functions[c].calls.values():
                callers.setdefault(t & ~1, set()).add(c)
    for p, fn in functions.items():
        fn.noreturn = p & ~1 in nr
        for ref in fn.code_refs.values():
            if text.contains(ref & ~1):
                seeds.setdefault(ref, set()).add("code")
    return Discovery(functions, seeds, noreturn, starts, time.perf_counter() - t0)


__all__ = [
    "Block", "Discovery", "Function", "JumpTable", "NORETURN_IMPORTS", "call_target",
    "collect_seeds", "discover", "discover_function", "falls_through", "flow", "is_return",
    "is_simd", "literal", "written_regs", "writes_pc",
]  # fmt: skip
