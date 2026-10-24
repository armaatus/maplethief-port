"""Capstone sweep of the armv7 ``__text``: calls, PC-relative data references, ObjC sends.

Each ``LC_FUNCTION_STARTS`` function (Thumb bit = mode) runs from its start to the next start
(or the end of ``__text``). It is decoded linearly; the words read by ``ldr rX, [pc, #n]`` /
``vldr`` (literal pools) and ``tbb``/``tbh`` jump tables are skipped instead of decoded, and
the sweep is repeated until that set of data is stable (a pool *before* its load would
otherwise be decoded as code first). Then a forward data-flow pass over the function's basic
blocks tracks a few register and stack values, enough to resolve the position-independent
idioms to absolute addresses::

    ldr   r0, [pc, #0x5c]      @ literal: r0 = L - (here + 4)
    add   r0, pc               @ r0 = L               (the address of a slot, e.g. a selref)
    ldr   r1, [r0]             @ load of L            -> Ref(kind="load", target=L)
    movw  r0, #:lower16:x; movt r0, #:upper16:x; add r0, pc     @ the same, Thumb-2 style

Values live in registers and in stack slots (``[sp, #n]``, normalised by ``push``/``sub sp``,
and ``[r7, #n]``): the -O0 game code passes everything through the stack. At a join the
in-state is the intersection of the predecessors' out-states, so a value is only used when
every path that reaches the instruction agrees on it. After a call, r0-r3, r12 and lr are
unknown, except that r0 after an ``objc_msgSend``-family call holds that call's result (so
``[[X alloc] init]`` can be followed). Stores through other pointers are not tracked, so a
callee that writes a local whose address escaped is not seen; that is a known limit.

Output: :class:`Ref` records (``call``/``tail`` to a code address, ``load``/``store`` of a data
address, ``address`` for a PC-relative address that is computed but never dereferenced in the
function) and :class:`Send` records (r0-r3 at each call to a dispatcher stub). The input is
untrusted data; nothing here executes it.
"""

from __future__ import annotations

import bisect
from collections import Counter, deque
from collections.abc import Iterable
from dataclasses import dataclass, field

from mtport import macho

# A tracked value: ("k", n, origins) a known 32-bit number (origins = the sites of the
# ``add rX, pc`` instructions that made it an address; empty for a plain number);
# ("m", a) the word loaded from data address a; ("ret", site) the result of the ObjC send
# at site.
Value = tuple

_MASK = 0xFFFFFFFF
_CALLER_SAVED = ("r0", "r1", "r2", "r3", "ip", "lr")  # Capstone spells r12 "ip"
_FLAGS = ("pc", "cpsr", "itstate", "apsr", "fpscr")
_NONE: frozenset = frozenset()


@dataclass(frozen=True, order=True)
class Ref:
    """A reference from the instruction at ``site`` in ``function`` (a function pointer)."""

    site: int
    function: int
    target: int
    kind: str  # "call", "tail", "load", "store", "address"


@dataclass(frozen=True)
class Send:
    """A call at ``site`` to the dispatcher stub ``stub``; ``regs`` holds r0-r3 (or None)."""

    site: int
    function: int
    stub: int
    regs: tuple[Value | None, Value | None, Value | None, Value | None]


@dataclass
class Scan:
    refs: list[Ref] = field(default_factory=list)
    sends: list[Send] = field(default_factory=list)
    functions: int = 0
    instructions: int = 0
    skipped_bytes: int = 0


@dataclass
class _State:
    regs: dict = field(default_factory=dict)
    stack: dict = field(default_factory=dict)
    sp: int | None = 0  # sp relative to its value at entry; None once unknown

    def copy(self) -> _State:
        return _State(dict(self.regs), dict(self.stack), self.sp)

    def meet(self, other: _State) -> bool:
        """Intersect ``other`` into self; return whether self changed."""
        changed = False
        for d, o in ((self.regs, other.regs), (self.stack, other.stack)):
            for k in [k for k, v in d.items() if o.get(k) != v]:
                v, w = d[k], o.get(k)
                if w is not None and v[0] == w[0] == "k" and v[1] == w[1]:
                    # The same number on both paths: keep it, with both origins.
                    merged = ("k", v[1], v[2] | w[2])
                    if merged != v:
                        d[k] = merged
                        changed = True
                    continue
                del d[k]
                changed = True
        if self.sp != other.sp:
            self.sp = None
            if self.stack:
                self.stack = {k: v for k, v in self.stack.items() if k[0] != "sp"}
            changed = True
        return changed


def _cs(thumb: bool):
    import capstone

    md = capstone.Cs(
        capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM
    )
    md.detail = True
    return md


class Scanner:
    """Sweeps the functions of one image. ``dispatchers`` are the stub addresses of the
    ``objc_msgSend`` family; a :class:`Send` is recorded at each call to one of them."""

    def __init__(self, image: macho.MachO, dispatchers: Iterable[int] = ()) -> None:
        from capstone import arm

        self.m = image
        self.arm = arm
        self.dispatchers = frozenset(dispatchers)
        self._md = {True: _cs(True), False: _cs(False)}
        self.text = image.section("__TEXT", "__text")
        if self.text is None:
            raise macho.MachOError("no __TEXT,__text section")
        self._code = image.read(self.text.addr, self.text.size)

    # -- functions -----------------------------------------------------------------------

    def functions(self) -> list[tuple[int, int, int]]:
        """``(function pointer, start, end)`` for each LC_FUNCTION_STARTS entry in __text."""
        starts = sorted(a for a in self.m.function_starts if self.text.contains(a & ~1))
        out = []
        for i, ptr in enumerate(starts):
            end = (starts[i + 1] & ~1) if i + 1 < len(starts) else self.text.end
            out.append((ptr, ptr & ~1, end))
        return out

    def scan(self) -> Scan:
        result = Scan()
        for ptr, start, end in self.functions():
            insns, tables, skipped = self.decode(start, end, bool(ptr & 1))
            result.functions += 1
            result.instructions += len(insns)
            result.skipped_bytes += skipped
            self.analyse(ptr, start, end, insns, tables, result)
        result.refs.sort()
        result.sends.sort(key=lambda s: s.site)
        return result

    def _pc(self, insn, thumb: bool, align: bool = False) -> int:
        if thumb:
            pc = insn.address + 4
            return pc & ~3 if align else pc
        return insn.address + 8

    # -- decoding ------------------------------------------------------------------------

    def decode(self, start: int, end: int, thumb: bool):
        """Decode ``[start, end)``: ``(instructions, {tbb/tbh site: targets}, bytes skipped)``."""
        known: dict[int, int] = {}
        for _ in range(8):
            insns, tables, found, skipped = self._sweep(start, end, thumb, known)
            if found == known:
                break
            known = found
        return insns, tables, skipped

    def _sweep(self, start: int, end: int, thumb: bool, known: dict[int, int]):
        a = self.arm
        md = self._md[thumb]
        step = 2 if thumb else 4
        skip = dict(known)  # address -> size, of data to skip in this sweep
        found: dict[int, int] = {}  # data referenced by the instructions decoded now
        tables: dict[int, list[int]] = {}
        insns = []
        skipped = 0
        pos = start
        last_cmp: int | None = None

        def data(addr: int, size: int) -> None:
            if start <= addr < end:
                found[addr] = max(size, found.get(addr, 0))
                if addr >= pos:
                    skip[addr] = max(size, skip.get(addr, 0))

        while pos < end:
            if pos in skip:
                skipped += skip[pos]
                pos += skip[pos]
                continue
            stop = min((x for x in skip if x > pos), default=end)
            decoded = False
            for insn in md.disasm(self._code[pos - self.text.addr : stop - self.text.addr], pos):
                decoded = True
                insns.append(insn)
                pos = insn.address + insn.size
                ins = insn.id
                ops = insn.operands
                cmp = None
                if ins == a.ARM_INS_CMP and len(ops) == 2 and ops[1].type == a.ARM_OP_IMM:
                    cmp = ops[1].imm
                for op in ops:
                    if (
                        op.type == a.ARM_OP_MEM
                        and op.mem.base == a.ARM_REG_PC
                        and op.mem.index == 0
                    ):
                        wide = ins == a.ARM_INS_LDRD or (
                            ins == a.ARM_INS_VLDR and insn.op_str.startswith("d")
                        )
                        data(self._pc(insn, thumb, align=True) + op.mem.disp, 8 if wide else 4)
                if ins in (a.ARM_INS_TBB, a.ARM_INS_TBH) and thumb:
                    # -O0 switch: cmp rX, #n; bhi default; tbb [pc, rX] -> n + 1 entries.
                    base = insn.address + 4
                    width = 1 if ins == a.ARM_INS_TBB else 2
                    count = (last_cmp + 1) if last_cmp is not None else 0
                    size = count * width
                    targets = []
                    if count and base + size <= end:
                        raw = self._code[base - self.text.addr : base - self.text.addr + size]
                        for i in range(count):
                            entry = int.from_bytes(raw[i * width : (i + 1) * width], "little")
                            targets.append(base + 2 * entry)
                        data(base, size + (size & 1))
                    tables[insn.address] = targets
                last_cmp = cmp
                if pos in skip or ins in (a.ARM_INS_TBB, a.ARM_INS_TBH):
                    break
            if not decoded:
                # Capstone stopped at an undecodable unit (data or padding): skip it.
                skipped += step
                pos += step
        return insns, tables, found, skipped

    # -- control flow --------------------------------------------------------------------

    def _flow(self, insn, start: int, end: int, tables) -> tuple[bool, list[int], bool]:
        """``(falls through, in-function successors, ends a block)`` of one instruction."""
        a = self.arm
        ins, ops = insn.id, insn.operands
        conditional = insn.cc not in (a.ARM_CC_AL, a.ARM_CC_INVALID)
        if insn.address in tables:
            return False, [t for t in tables[insn.address] if start <= t < end], True
        if ins in (a.ARM_INS_B, a.ARM_INS_CBZ, a.ARM_INS_CBNZ) and ops:
            target = ops[-1].imm if ops[-1].type == a.ARM_OP_IMM else None
            inside = [target] if target is not None and start <= target < end else []
            through = conditional or ins != a.ARM_INS_B
            return through, inside, True
        if ins in (a.ARM_INS_BL, a.ARM_INS_BLX) and not (
            ins == a.ARM_INS_BLX and ops and ops[0].type == a.ARM_OP_REG
        ):
            return True, [], False
        if ins == a.ARM_INS_BLX:
            return True, [], False
        _, written = insn.regs_access()
        if ins == a.ARM_INS_BX or a.ARM_REG_PC in written:
            return conditional, [], True
        return True, [], False

    def analyse(self, ptr: int, start: int, end: int, insns, tables, out: Scan) -> None:
        if not insns:
            return
        index = {insn.address: i for i, insn in enumerate(insns)}
        flows = [self._flow(insn, start, end, tables) for insn in insns]
        leaders = {0}
        for i, (_, targets, ends) in enumerate(flows):
            leaders.update(index[t] for t in targets if t in index)
            if ends and i + 1 < len(insns):
                leaders.add(i + 1)
        order = sorted(leaders)
        bounds = [
            (b, order[k + 1] if k + 1 < len(order) else len(insns)) for k, b in enumerate(order)
        ]
        block_at = {b: k for k, (b, _) in enumerate(bounds)}
        succs: list[list[int]] = []
        has_pred = [False] * len(bounds)
        for _, hi in bounds:
            through, targets, _ = flows[hi - 1]
            s = [block_at[index[t]] for t in targets if t in index]
            if through and hi < len(insns):
                s.append(block_at[hi])
            succs.append(s)
            for k in s:
                has_pred[k] = True

        # Blocks no edge reaches (SjLj landing pads, targets of computed jumps) start with
        # nothing known but the function's usual stack depth, once everything reachable from
        # the entry has been seen; otherwise their unknown sp would wipe the stack slots of
        # every block they join.
        states: list[_State | None] = [None] * len(bounds)
        states[0] = _State()
        thumb = bool(ptr & 1)
        self._solve(states, [0], bounds, succs, insns, thumb)
        orphans = [k for k in range(len(bounds)) if states[k] is None and not has_pred[k]]
        if orphans:
            depths = Counter(st.sp for st in states if st is not None and st.sp is not None)
            sp = depths.most_common(1)[0][0] if depths else None
            for k in orphans:
                states[k] = _State(sp=sp)
            self._solve(states, orphans, bounds, succs, insns, thumb)

        rec = _Recorder(ptr)
        for k, (lo, hi) in enumerate(bounds):
            state = (states[k] or _State()).copy()
            for insn in insns[lo:hi]:
                self._step(state, insn, thumb, rec)
        rec.finish(out)

    def _solve(self, states, seeds, bounds, succs, insns, thumb) -> None:
        work = deque(seeds)
        queued = set(work)
        while work:
            k = work.popleft()
            queued.discard(k)
            state = states[k].copy()
            lo, hi = bounds[k]
            for insn in insns[lo:hi]:
                self._step(state, insn, thumb, None)
            for s in succs[k]:
                if states[s] is None:
                    states[s] = state.copy()
                elif not states[s].meet(state):
                    continue
                if s not in queued:
                    work.append(s)
                    queued.add(s)

    # -- transfer function ---------------------------------------------------------------

    def _step(self, st: _State, insn, thumb: bool, rec: _Recorder | None) -> None:
        a = self.arm
        ins, ops = insn.id, insn.operands
        conditional = insn.cc not in (a.ARM_CC_AL, a.ARM_CC_INVALID)
        regs, stack = st.regs, st.stack

        def known(reg: int) -> Value | None:
            v = regs.get(insn.reg_name(reg))
            return v if v is not None and v[0] == "k" else None

        def slot(base: str | None, disp: int):
            if base == "sp":
                return None if st.sp is None else ("sp", st.sp + disp)
            if base == "r7":
                return ("r7", disp)
            return None

        def set_reg(name: str, value: Value | None) -> None:
            if value is None or conditional:
                regs.pop(name, None)
            else:
                regs[name] = value

        # Calls and branches.
        if (
            ins in (a.ARM_INS_BL, a.ARM_INS_BLX, a.ARM_INS_B)
            and ops
            and ops[-1].type == a.ARM_OP_IMM
        ):
            target = ops[-1].imm
            if ins == a.ARM_INS_B:
                if rec and not self.text.contains(target):
                    rec.ref(insn.address, target, "tail")
                return
            if rec:
                rec.ref(insn.address, target, "call")
                if target in self.dispatchers:
                    rec.send(insn.address, target, tuple(regs.get(f"r{i}") for i in range(4)))
            for name in _CALLER_SAVED:
                regs.pop(name, None)
            if target in self.dispatchers:
                regs["r0"] = ("ret", insn.address)
            return

        # Memory accesses.
        mems = [op for op in ops if op.type == a.ARM_OP_MEM]
        store = insn.mnemonic.startswith(("st", "vst", "push"))
        if mems:
            mem = mems[0].mem
            base = insn.reg_name(mem.base) if mem.base else None
            word = ins in (a.ARM_INS_LDR, a.ARM_INS_STR) and len(ops) == 2
            rt = insn.reg_name(ops[0].reg) if word and ops[0].type == a.ARM_OP_REG else None
            simple = mem.index == 0 and not insn.writeback
            if base == "pc" and simple and ins == a.ARM_INS_LDR and rt not in (None, "pc"):
                try:
                    set_reg(rt, ("k", self.m.u32(self._pc(insn, thumb, True) + mem.disp), _NONE))
                except macho.MachOError:
                    set_reg(rt, None)
                return
            if base == "sp" and insn.writeback and mem.index == 0 and word:
                # str rX, [sp, #-4]! / ldr rX, [sp], #4: a one-register push / pop.
                delta = ops[2].imm if len(ops) == 3 and ops[2].type == a.ARM_OP_IMM else mem.disp
                if len(ops) == 3 and ops[2].subtracted:
                    delta = -delta
                if rt is not None:
                    regs.pop(rt, None)
                if st.sp is not None:
                    st.sp += delta
                return
            if base in ("sp", "r7"):
                if not simple or (store and not word):
                    # strd/vstr/stm...: forget the slots the store may cover.
                    first = slot(base, mem.disp & ~3) if simple else None
                    if first is None:
                        for k in [k for k in stack if k[0] == base]:
                            del stack[k]
                    else:
                        for off in (0, 4):
                            stack.pop((first[0], first[1] + off), None)
                    self._clobber(st, insn, conditional)
                    return
                s = slot(base, mem.disp)
                if word and rt not in (None, "pc", "sp"):
                    if store:
                        value = regs.get(rt)
                        if s is not None:
                            if value is None or conditional:
                                stack.pop(s, None)
                            else:
                                stack[s] = value
                        return
                    set_reg(rt, stack.get(s) if s is not None else None)
                    return
            elif simple and base not in (None, "pc"):
                value = known(mem.base)
                if value is not None:
                    target = (value[1] + mem.disp) & _MASK
                    if self.m.section_at(target) is not None:
                        if rec:
                            rec.ref(insn.address, target, "store" if store else "load", value[2])
                        if word and not store and rt not in (None, "pc", "sp"):
                            set_reg(rt, ("m", target))
                            return
            self._clobber(st, insn, conditional)
            return

        # Address arithmetic and moves.
        if ins in (a.ARM_INS_ADD, a.ARM_INS_ADR, a.ARM_INS_ADDW) and ops:
            rd = insn.reg_name(ops[0].reg)
            if rd not in ("sp", "pc"):
                if ins == a.ARM_INS_ADR:
                    vals = [self._pc(insn, thumb, align=True), ops[1].imm]
                    pc_used, origin = True, _NONE
                else:
                    vals, pc_used, origin = [], False, _NONE
                    for op in ops[1:] if len(ops) > 2 else ops:
                        if op.type == a.ARM_OP_IMM:
                            vals.append(op.imm)
                        elif op.type == a.ARM_OP_REG and op.reg == a.ARM_REG_PC:
                            vals.append(self._pc(insn, thumb, align=ins == a.ARM_INS_ADDW))
                            pc_used = True
                        elif op.type == a.ARM_OP_REG and op.shift.type == 0:
                            v = known(op.reg)
                            vals.append(None if v is None else v[1])
                            if v is not None:
                                origin = origin | v[2]
                        else:
                            vals.append(None)
                if vals and all(v is not None for v in vals):
                    total = sum(vals) & _MASK
                    if pc_used and self.m.section_at(total) is not None:
                        origin = frozenset({insn.address})
                        if rec:
                            rec.computed(insn.address, total)
                    set_reg(rd, ("k", total, origin))
                else:
                    set_reg(rd, None)
                return
        elif ins in (a.ARM_INS_MOV, a.ARM_INS_MOVW) and len(ops) == 2:
            rd = insn.reg_name(ops[0].reg)
            if rd not in ("sp", "pc"):
                src = ops[1]
                if src.type == a.ARM_OP_IMM:
                    set_reg(rd, ("k", src.imm & _MASK, _NONE))
                elif src.type == a.ARM_OP_REG and src.shift.type == 0:
                    set_reg(rd, regs.get(insn.reg_name(src.reg)))
                else:
                    set_reg(rd, None)
                return
        elif ins == a.ARM_INS_MOVT and len(ops) == 2:
            rd = insn.reg_name(ops[0].reg)
            low = known(ops[0].reg)
            value = None if low is None else ((low[1] & 0xFFFF) | (ops[1].imm << 16))
            set_reg(rd, None if value is None else ("k", value, _NONE))
            return
        elif ins in (a.ARM_INS_PUSH, a.ARM_INS_POP):
            if st.sp is not None:
                st.sp += (4 if ins == a.ARM_INS_POP else -4) * len(ops)
            if ins == a.ARM_INS_POP:
                for op in ops:
                    regs.pop(insn.reg_name(op.reg), None)
            return
        elif (
            ins in (a.ARM_INS_SUB, a.ARM_INS_ADD, a.ARM_INS_SUBW, a.ARM_INS_ADDW)
            and len(ops) in (2, 3)
            and insn.reg_name(ops[0].reg) == "sp"
            and (len(ops) == 2 or insn.reg_name(ops[1].reg) == "sp")
            and ops[-1].type == a.ARM_OP_IMM
            and not conditional
        ):
            if st.sp is not None:
                add = ins in (a.ARM_INS_ADD, a.ARM_INS_ADDW)
                st.sp += ops[-1].imm if add else -ops[-1].imm
            return
        self._clobber(st, insn, conditional)

    def _clobber(self, st: _State, insn, conditional: bool) -> None:
        """Forget every register the instruction writes (and the stack if it moves sp)."""
        _, written = insn.regs_access()
        for r in written:
            name = insn.reg_name(r)
            if name == "sp":
                st.sp = None
                st.stack = {k: v for k, v in st.stack.items() if k[0] != "sp"}
            elif name not in _FLAGS:
                st.regs.pop(name, None)


class _Recorder:
    """Collects one function's references during the final pass."""

    def __init__(self, ptr: int) -> None:
        self.ptr = ptr
        self.refs: list[Ref] = []
        self.sends: list[Send] = []
        self.addresses: dict[int, int] = {}
        self.used: set[int] = set()

    def ref(self, site: int, target: int, kind: str, origins: frozenset = frozenset()) -> None:
        self.refs.append(Ref(site, self.ptr, target, kind))
        self.used.update(origins)

    def send(self, site: int, stub: int, regs) -> None:
        self.sends.append(Send(site, self.ptr, stub, regs))

    def computed(self, site: int, target: int) -> None:
        self.addresses[site] = target

    def finish(self, out: Scan) -> None:
        out.refs.extend(self.refs)
        out.sends.extend(self.sends)
        for site, target in self.addresses.items():
            if site not in self.used:
                out.refs.append(Ref(site, self.ptr, target, "address"))


def scan(image: macho.MachO, dispatchers: Iterable[int] = ()) -> Scan:
    return Scanner(image, dispatchers).scan()


@dataclass(frozen=True, order=True)
class Insn:
    """One instruction found by :func:`find_instructions`."""

    site: int
    function: int  # function pointer (Thumb bit = mode)
    mnemonic: str  # Capstone's mnemonic without condition/width suffixes, e.g. "ldrex"
    text: str  # the full disassembly, e.g. "ldrex r1, [r0]"


def _base_mnemonic(mnemonic: str, cc: int, arm) -> str:
    m = mnemonic.split(".", 1)[0]
    if cc not in (arm.ARM_CC_AL, arm.ARM_CC_INVALID) and len(m) > 2:
        m = m[:-2]  # "strexeq" -> "strex"
    return m


def find_instructions(image: macho.MachO, mnemonics: Iterable[str]) -> list[Insn]:
    """Every instruction whose mnemonic is in ``mnemonics``, decoded the same way :func:`scan`
    decodes (per ``LC_FUNCTION_STARTS`` function, literal pools and jump tables skipped)."""
    wanted = frozenset(mnemonics)
    scanner = Scanner(image)
    out: list[Insn] = []
    for ptr, start, end in scanner.functions():
        insns, _tables, _skipped = scanner.decode(start, end, bool(ptr & 1))
        for insn in insns:
            m = _base_mnemonic(insn.mnemonic, insn.cc, scanner.arm)
            if m in wanted:
                text = f"{insn.mnemonic} {insn.op_str}".strip()
                out.append(Insn(insn.address, ptr, m, text))
    return sorted(out)


class FunctionIndex:
    """Maps a code address to the function pointer whose start precedes it."""

    def __init__(self, starts: Iterable[int]) -> None:
        self.starts = sorted(starts, key=lambda s: s & ~1)
        self._plain = [s & ~1 for s in self.starts]

    def of(self, addr: int) -> int | None:
        i = bisect.bisect_right(self._plain, addr & ~1) - 1
        return self.starts[i] if i >= 0 else None
