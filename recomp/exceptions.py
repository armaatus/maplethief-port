"""SjLj exception contexts and their LSDA tables (#37, ADR 0020).

The binary uses setjmp/longjmp ("SjLj") exception handling: a function with a landing pad
builds a **function context** on its stack, registers it with ``_Unwind_SjLj_Register``,
stores the index of the current call site into it before each call that may throw, and
unregisters it on the way out. The layout (GCC ``unwind-sjlj.c``, LLVM ``SjLjEHPrepare``;
measured from the binary, ``docs/formats/lsda.md``)::

    +0x00 prev          the next outer context (written by _Unwind_SjLj_Register)
    +0x04 call_site     before a call: its 1-based index in the LSDA call-site table, or -1
                        (no landing pad); after a throw: the landing-pad index the dispatch
                        switches on
    +0x08 data[4]       data[0] = the exception (_Unwind_Exception *), data[1] = the selector
    +0x18 personality   ___gxx_personality_sj0 or ___objc_personality_v0
    +0x1C lsda          this function's LSDA in __gcc_except_tab
    +0x20 jbuf[0]       r7 (frame pointer)
    +0x24 jbuf[1]       the resume address inside the function (Thumb bit set)
    +0x28 jbuf[2]       sp

This module has three parts:

- :func:`parse_lsda` reads one LSDA (the GCC/Itanium "language-specific data area" with SjLj
  call-site records: landing-pad index and action, both ULEB128). Its spec is
  ``docs/formats/lsda.md``. The runtime's personality routine (``runtime/src/sjlj.c``) reads
  the same tables from guest memory.
- :func:`find_context` finds a function's context by evaluating its prologue up to the
  ``_Unwind_SjLj_Register`` call with symbolic values (``sp``-relative addresses, constants,
  loads from data): where the context lives, its personality, LSDA, resume address and the
  jump-buffer store the lifter hooks (``recomp.lift.sjlj``).
- :func:`dispatch` runs a function's resume code for one landing-pad index, so the tests can
  show that every index of the LSDA reaches its landing pad and an index past the end
  reaches the dispatch's trap.

``python -m recomp exceptions`` writes ``work/recomp/exceptions.json`` (:func:`report`),
schema 1 (``ghidra_common.dumps`` layout, one context per line, sorted by address)::

    {"schema": 1, "source": {...},
     "counts": {"contexts", "lsdas", "lsda_bytes", "call_sites", "problems",
                "cleanup", "catch", "catch-all", "spec",     # call sites by what they find
                "personality:<import>"},
     "raisers": {"<import>": ["0x<caller ptr>", ...]},   # every import that raises
     "contexts": [
      {"ptr": "0x5839", "frame": 96,          # the context's offset from sp at Register
       "register": "0x58D8", "unregister": ["0x5958"], "resume_calls": ["0x5990"],
       "personality": "___objc_personality_v0", "lsda": "0xE9490", "lsda_size": 8,
       "resume": "0x58CC", "store": "0x58C6",  # the jbuf[1] store
       "call_sites": [[1, 0, "cleanup"], ...], # index, landing pad, what its action does
       "types": ["0x..."],                     # catch types (0 = catch-all)
       "dispatch": {"0": "call 0x5944", "2": "trap 0x59A8"},  # where each landing-pad
                                               # index leads, and the one past the last
       "problems": []}]}

The input is untrusted data: it is parsed, never executed.
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from recomp import REPO
from recomp.decode.insn import (
    AL,
    COMPARES,
    PC,
    SP,
    Imm,
    Insn,
    Mem,
    Reg,
    RegList,
    VRegList,
)
from recomp.discover import CALL_CLOBBERS, Function, is_return, literal, written_regs

EXCEPTIONS_OUT = REPO / "work" / "recomp" / "exceptions.json"

# The function context (offsets from its start).
FC_PREV, FC_CALL_SITE, FC_DATA, FC_PERSONALITY, FC_LSDA, FC_JBUF = 0, 4, 8, 0x18, 0x1C, 0x20
FC_SIZE = FC_JBUF + 12  # jbuf: r7, resume address, sp (LLVM's __builtin_setjmp buffer)

# DWARF exception-header pointer encodings (LSB: format, then application, then indirect).
DW_EH_PE_ABSPTR, DW_EH_PE_ULEB128, DW_EH_PE_UDATA2, DW_EH_PE_UDATA4 = 0x00, 0x01, 0x02, 0x03
DW_EH_PE_SLEB128, DW_EH_PE_SDATA2, DW_EH_PE_SDATA4 = 0x09, 0x0A, 0x0B
DW_EH_PE_PCREL, DW_EH_PE_INDIRECT, DW_EH_PE_OMIT = 0x10, 0x80, 0xFF
_FIXED = {DW_EH_PE_ABSPTR: (4, False), DW_EH_PE_UDATA2: (2, False), DW_EH_PE_UDATA4: (4, False),
          DW_EH_PE_SDATA2: (2, True), DW_EH_PE_SDATA4: (4, True)}  # fmt: skip

# Imports that start or continue an unwind, and the catch helpers.
REGISTER = "__Unwind_SjLj_Register"
UNREGISTER = "__Unwind_SjLj_Unregister"
RESUME = "__Unwind_SjLj_Resume"
RAISERS = (
    "___cxa_throw", "___cxa_rethrow", "__Unwind_SjLj_RaiseException",
    "__Unwind_SjLj_ForcedUnwind", "__Unwind_RaiseException", "__Unwind_Resume",
    RESUME, "_objc_exception_throw", "_objc_exception_rethrow",
)  # fmt: skip
CATCH_HELPERS = (
    "___cxa_begin_catch", "___cxa_end_catch", "___cxa_allocate_exception",
    "___cxa_call_unexpected", "__ZSt9terminatev", "_objc_begin_catch", "_objc_end_catch",
)  # fmt: skip
PERSONALITIES = ("___gxx_personality_sj0", "___objc_personality_v0")


class LsdaError(ValueError):
    """A malformed LSDA (the message names its address)."""


# -- LSDA -------------------------------------------------------------------------------------


@dataclass(frozen=True)
class CallSite:
    """One SjLj call-site record. ``index`` is the 1-based value the function stores into
    ``call_site`` before the call; ``landing_pad`` is the value the personality stores there
    when it installs the landing pad (the index the resume dispatch switches on);
    ``action`` is 1 + the offset of the first action record, 0 for cleanup only."""

    index: int
    landing_pad: int
    action: int


@dataclass(frozen=True)
class Action:
    """An action record at ``offset`` in the action table: ``filter`` > 0 catches type
    ``filter`` of the type table (0 there means catch-all), 0 is a cleanup, < 0 is an
    exception specification at ``-filter - 1`` bytes after the type-table base. ``next``
    is the offset of the next record, None at the end of the chain."""

    offset: int
    filter: int
    next: int | None


@dataclass(frozen=True)
class TypeRef:
    """A type-table entry: ``address`` of the ``type_info`` (0: catch-all), or, if
    ``indirect``, of the pointer that holds it (a non-lazy pointer bound by dyld)."""

    address: int
    indirect: bool = False


@dataclass(frozen=True)
class Lsda:
    address: int
    end: int  # one past the last byte the tables use (alignment padding not included)
    lpstart: int | None
    ttype_encoding: int
    ttype_base: int | None
    call_site_encoding: int
    call_sites: tuple[CallSite, ...]
    actions: dict[int, Action] = field(default_factory=dict)
    types: dict[int, TypeRef] = field(default_factory=dict)  # filter -> entry
    specs: dict[int, tuple[int, ...]] = field(default_factory=dict)  # filter < 0 -> filters

    @property
    def size(self) -> int:
        return self.end - self.address

    def chain(self, site: CallSite) -> tuple[Action, ...]:
        """The action records of ``site``, in the order the personality tries them."""
        out: list[Action] = []
        offset = site.action - 1 if site.action else None
        while offset is not None:
            record = self.actions[offset]
            out.append(record)
            offset = record.next
        return tuple(out)

    def kind(self, site: CallSite) -> str:
        """What a throw at ``site`` finds here: ``cleanup``, ``catch``, ``catch-all``,
        ``spec`` (an exception specification), joined by ``+`` in chain order."""
        parts: list[str] = []
        for a in self.chain(site) or (Action(0, 0, None),):
            if a.filter == 0:
                part = "cleanup"
            elif a.filter < 0:
                part = "spec"
            else:
                part = "catch-all" if self.types[a.filter].address == 0 else "catch"
            if part not in parts:
                parts.append(part)
        return "+".join(parts)


class _Reader:
    def __init__(self, read: Callable[[int, int], bytes], pos: int, where: int) -> None:
        self.read, self.pos, self.where = read, pos, where

    def fail(self, why: str) -> LsdaError:
        return LsdaError(f"LSDA {self.where:#x}: {why} at {self.pos:#x}")

    def u8(self) -> int:
        data = self.read(self.pos, 1)
        if len(data) != 1:
            raise self.fail("truncated")
        self.pos += 1
        return data[0]

    def uleb(self) -> int:
        value = shift = 0
        while True:
            b = self.u8()
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value
            if shift >= 35:
                raise self.fail("ULEB128 longer than 5 bytes")

    def sleb(self) -> int:
        value = shift = 0
        while True:
            b = self.u8()
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    value -= 1 << shift
                return value
            if shift >= 35:
                raise self.fail("SLEB128 longer than 5 bytes")

    def encoded(self, enc: int) -> TypeRef:
        """A pointer in encoding ``enc`` (GCC's read_encoded_value: 0 stays 0)."""
        at = self.pos
        fmt = enc & 0x0F
        if fmt == DW_EH_PE_ULEB128:
            raw = self.uleb()
        elif fmt == DW_EH_PE_SLEB128:
            raw = self.sleb()
        elif fmt in _FIXED:
            size, signed = _FIXED[fmt]
            data = self.read(at, size)
            if len(data) != size:
                raise self.fail("truncated")
            self.pos += size
            raw = int.from_bytes(data, "little", signed=signed)
        else:
            raise self.fail(f"unsupported pointer encoding {enc:#x}")
        if raw == 0:
            return TypeRef(0)
        app = enc & 0x70
        if app == DW_EH_PE_PCREL:
            raw += at
        elif app != 0:
            raise self.fail(f"unsupported pointer application {enc:#x}")
        return TypeRef(raw & 0xFFFFFFFF, bool(enc & DW_EH_PE_INDIRECT))


def _entry_size(enc: int) -> int:
    if enc & 0x0F not in _FIXED:
        raise LsdaError(f"type table encoding {enc:#x} has no fixed size")
    return _FIXED[enc & 0x0F][0]


def parse_lsda(read: Callable[[int, int], bytes], address: int) -> Lsda:
    """Parse the LSDA at guest ``address``; ``read(addr, n)`` returns guest bytes."""
    r = _Reader(read, address, address)
    lpstart_enc = r.u8()
    lpstart = None if lpstart_enc == DW_EH_PE_OMIT else r.encoded(lpstart_enc).address
    ttype_enc = r.u8()
    ttype_base = None
    if ttype_enc != DW_EH_PE_OMIT:
        off = r.uleb()
        ttype_base = r.pos + off
    cs_enc = r.u8()  # SjLj records are ULEB128 whatever this says (GCC and libc++abi agree)
    cs_len = r.uleb()
    cs_end = r.pos + cs_len
    sites: list[CallSite] = []
    while r.pos < cs_end:
        lp = r.uleb()
        action = r.uleb()
        sites.append(CallSite(len(sites) + 1, lp, action))
    if r.pos != cs_end:
        raise r.fail("call-site table overruns its length")
    table = cs_end
    actions: dict[int, Action] = {}
    end = table
    pending = sorted({s.action - 1 for s in sites if s.action})
    while pending:
        offset = pending.pop()
        if offset in actions:
            continue
        r.pos = table + offset
        flt = r.sleb()
        at = r.pos
        disp = r.sleb()
        end = max(end, r.pos)
        nxt = None if disp == 0 else at + disp - table
        if nxt is not None and nxt < 0:
            raise r.fail(f"action record {offset} links before the table")
        actions[offset] = Action(offset, flt, nxt)
        if nxt is not None and nxt not in actions:
            pending.append(nxt)
    types: dict[int, TypeRef] = {}
    specs: dict[int, tuple[int, ...]] = {}
    filters = {a.filter for a in actions.values()}
    if any(f for f in filters) and ttype_base is None:
        raise LsdaError(f"LSDA {address:#x}: filters but no type table")
    if ttype_base is not None:
        size = _entry_size(ttype_enc)
        positive = {f for f in filters if f > 0}
        for f in sorted(filters):
            if f < 0:
                r.pos = ttype_base - f - 1
                allowed: list[int] = []
                while (i := r.uleb()) != 0:
                    allowed.append(i)
                specs[f] = tuple(allowed)
                positive.update(allowed)
                end = max(end, r.pos)
        for f in sorted(positive):
            r.pos = ttype_base - f * size
            if r.pos < table:
                raise r.fail(f"type {f} lies before the action table")
            types[f] = r.encoded(ttype_enc)
        end = max(end, ttype_base)
    return Lsda(address, end, lpstart, ttype_enc, ttype_base, cs_enc, tuple(sites), actions,
                types, specs)  # fmt: skip


# -- symbolic evaluation of the prologue and the resume dispatch ----------------------------

# A value: ("c", n) a constant, ("sp", k) the stack pointer at entry + k, ("ld", a) the word
# loaded from the (writable, so unknown) address a; None: unknown.
Value = tuple[str, int] | None


def _add(a: Value, b: Value) -> Value:
    if a is None or b is None:
        return None
    if a[0] == "c" and b[0] == "c":
        return ("c", (a[1] + b[1]) & 0xFFFFFFFF)
    if a[0] == "sp" and b[0] == "c":
        return ("sp", a[1] + _signed(b[1]))
    if b[0] == "sp" and a[0] == "c":
        return ("sp", b[1] + _signed(a[1]))
    return None


def _signed(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


def _neg(a: Value) -> Value:
    return ("c", -a[1] & 0xFFFFFFFF) if a is not None and a[0] == "c" else None


class _State:
    """Registers and stack slots during an evaluation (straight-line semantics)."""

    def __init__(self, const_word: Callable[[int, int], int | None]) -> None:
        self.const_word = const_word
        self.regs: dict[int, Value] = {SP: ("sp", 0)}
        self.slots: dict[int, Value] = {}  # entry sp + k -> value
        self.flags: tuple[int, int] | None = None  # the operands of the last cmp

    def copy(self) -> _State:
        s = _State(self.const_word)
        s.regs, s.slots, s.flags = dict(self.regs), dict(self.slots), self.flags
        return s

    def get(self, insn: Insn, op) -> Value:
        if isinstance(op, Imm):
            return ("c", op.value & 0xFFFFFFFF)
        if isinstance(op, Reg):
            return ("c", insn.pc) if op.n == PC else self.regs.get(op.n)
        return None

    def load(self, addr: Value, size: int) -> Value:
        if addr is None:
            return None
        if addr[0] == "sp":
            return self.slots.get(addr[1]) if size == 4 else None
        if addr[0] == "c":
            word = self.const_word(addr[1], size)
            return ("c", word) if word is not None else ("ld", addr[1]) if size == 4 else None
        return None

    def store(self, addr: Value, value: Value, size: int) -> None:
        if addr is not None and addr[0] == "sp":
            for k in range(addr[1] - 3, addr[1] + size):
                self.slots.pop(k, None)
            if size == 4:
                self.slots[addr[1]] = value

    def forget(self, regs) -> None:
        for r in regs:
            self.regs.pop(r, None)

    def step(self, insn: Insn) -> None:
        """Apply ``insn`` (any control flow is the caller's business)."""
        m, ops = insn.mnemonic, insn.operands
        written = written_regs(insn)
        if insn.cond != AL:  # conditional: whatever it writes becomes unknown
            self.forget(range(16) if written is None else written)
            if m.startswith("st"):
                self.slots.clear()
            return
        if m in COMPARES or insn.setflags:
            self.flags = None
        if m == "cmp" and len(ops) == 2:
            a, b = self.get(insn, ops[0]), self.get(insn, ops[1])
            if a is not None and b is not None and a[0] == b[0] == "c":
                self.flags = (a[1], b[1])
            return
        if m in ("bl", "blx"):
            self.forget(CALL_CLOBBERS)
            self.flags = None
            return
        if m in ("push", "pop") and isinstance(ops[-1], RegList):
            regs = ops[-1].regs
            sp = self.regs.get(SP)
            if sp is None or sp[0] != "sp":
                self.forget(range(16))
                return
            if m == "push":
                base = sp[1] - 4 * len(regs)
                for i, r in enumerate(regs):
                    self.store(("sp", base + 4 * i), self.regs.get(r), 4)
                self.regs[SP] = ("sp", base)
            else:
                for i, r in enumerate(regs):
                    self.regs[r] = self.slots.get(sp[1] + 4 * i)
                self.regs[SP] = ("sp", sp[1] + 4 * len(regs))
            return
        if m in ("vpush", "vpop") and isinstance(ops[-1], VRegList):
            n = ops[-1].count * (8 if ops[-1].double else 4)
            self.regs[SP] = _add(self.regs.get(SP), ("c", -n if m == "vpush" else n))
            return
        if m in ("ldr", "str") and len(ops) == 2 and isinstance(ops[1], Mem):
            self._ldst(insn, m, ops[0], ops[1])
            return
        rd = ops[0].n if ops and isinstance(ops[0], Reg) else None
        value: Value = None
        if rd is not None:
            srcs = [self.get(insn, op) for op in ops[1:]]
            if m in ("mov", "movw") and len(ops) == 2:
                value = srcs[0]
            elif m == "mvn" and len(ops) == 2 and isinstance(ops[1], Imm):
                value = ("c", ~ops[1].value & 0xFFFFFFFF)
            elif m == "movt" and isinstance(ops[1], Imm):
                old = self.regs.get(rd)
                if old is not None and old[0] == "c":
                    value = ("c", (old[1] & 0xFFFF) | (ops[1].value << 16))
            elif m == "adr":
                value = ("c", ((insn.pc & ~3) + ops[1].value) & 0xFFFFFFFF)
            elif m in ("add", "sub", "addw", "subw", "orr"):
                a, b = (self.regs.get(rd), srcs[0]) if len(ops) == 2 else srcs[:2]
                if len(ops) == 3 and isinstance(ops[1], Reg) and ops[1].n == PC and m != "orr":
                    a = ("c", insn.pc & ~3) if m in ("addw", "subw") else ("c", insn.pc)
                if m in ("sub", "subw"):
                    value = _add(a, _neg(b))
                elif m == "orr":
                    if a is not None and b is not None and a[0] == b[0] == "c":
                        value = ("c", a[1] | b[1])
                else:
                    value = _add(a, b)
        self.forget(range(16) if written is None else written)
        if rd is not None and value is not None and rd != PC:
            self.regs[rd] = value

    def _ldst(self, insn: Insn, m: str, rt_op, mem: Mem) -> None:
        if mem.base == PC:
            lit = literal(insn)
            if m == "ldr" and lit is not None and isinstance(rt_op, Reg):
                self.regs[rt_op.n] = self.load(("c", lit[0]), 4)
            return
        base = self.regs.get(mem.base)
        if mem.index is not None:
            addr = None
        else:
            addr = _add(base, ("c", mem.disp & 0xFFFFFFFF)) if mem.pre else base
        rt = rt_op.n
        if m == "str":
            self.store(addr, self.regs.get(rt) if rt != PC else None, 4)
        else:
            self.regs[rt] = self.load(addr, 4)
        if mem.wback or not mem.pre:
            new = None if mem.index is not None else _add(base, ("c", mem.disp & 0xFFFFFFFF))
            if mem.base != rt or m == "str":
                self.regs[mem.base] = new


# -- the context of one function --------------------------------------------------------------


@dataclass
class Context:
    """The SjLj function context of the function ``ptr`` (see the module docstring)."""

    ptr: int
    register: int | None = None
    unregister: tuple[int, ...] = ()
    resume_calls: tuple[int, ...] = ()
    frame: int | None = None  # offset of the context from sp at the Register call
    personality: str | None = None
    lsda: int | None = None
    resume: int | None = None  # the resume address (Thumb bit clear)
    store: int | None = None  # the instruction that stores jbuf[1]
    jbuf_base: int | None = None  # the store's base register
    state: _State | None = field(default=None, repr=False)  # at the Register call
    fc: int | None = field(default=None, repr=False)  # entry sp + fc is the context
    problems: list[str] = field(default_factory=list)


def _import_calls(fn: Function, image) -> dict[str, list[int]]:
    out: dict[str, list[int]] = {}
    for site, target in sorted(fn.calls.items()):
        imp = image.import_at(target)
        if imp is not None:
            out.setdefault(imp.name, []).append(site)
    return out


def find_context(fn: Function, image) -> Context | None:
    """The function's SjLj context, or None if it never calls ``_Unwind_SjLj_Register``.
    The prologue is evaluated in address order up to the Register call (it is straight
    line code in all 205 functions; branches are stepped over)."""
    calls = _import_calls(fn, image)
    if REGISTER not in calls:
        return None
    ctx = Context(fn.ptr, calls[REGISTER][0], tuple(calls.get(UNREGISTER, ())),
                  tuple(calls.get(RESUME, ())))  # fmt: skip
    if len(calls[REGISTER]) != 1:
        ctx.problems.append(f"{len(calls[REGISTER])} Register calls")
    entries = {e | 1: e for e in fn.entries}
    state = _State(image.const_word)
    stores: list[tuple[int, int, int, int]] = []  # (site, slot, resume, base register)
    for insn in fn.ordered():
        if insn.address == ctx.register:
            break
        if insn.mnemonic == "str" and insn.cond == AL and isinstance(insn.operands[1], Mem):
            mem = insn.operands[1]
            v = state.regs.get(insn.operands[0].n)
            if v is not None and v[0] == "c" and v[1] in entries and mem.index is None:
                addr = _add(state.regs.get(mem.base), ("c", mem.disp & 0xFFFFFFFF))
                if addr is not None and addr[0] == "sp":
                    stores.append((insn.address, addr[1], entries[v[1]], mem.base))
        state.step(insn)
    fc = state.regs.get(0)
    sp = state.regs.get(SP)
    if fc is None or fc[0] != "sp" or sp is None or sp[0] != "sp":
        ctx.problems.append("the context address passed to Register is not on the stack")
        return ctx
    ctx.fc, ctx.frame, ctx.state = fc[1], fc[1] - sp[1], state
    slot = state.slots.get
    pers = slot(fc[1] + FC_PERSONALITY)
    if pers is not None and pers[0] in ("ld", "c") and (imp := image.import_at(pers[1])):
        ctx.personality = imp.name
    else:
        ctx.problems.append(f"personality not found ({pers})")
    lsda = slot(fc[1] + FC_LSDA)
    if lsda is not None and lsda[0] == "c":
        ctx.lsda = lsda[1]
    else:
        ctx.problems.append(f"LSDA not found ({lsda})")
    jb = fc[1] + FC_JBUF
    for site, at, resume, base in stores:
        if at == jb + 4:
            ctx.store, ctx.resume, ctx.jbuf_base = site, resume, base
    if ctx.store is None:
        ctx.problems.append("no store of the resume address into jbuf[1]")
    if slot(jb) is None or slot(jb) != state.regs.get(7):
        ctx.problems.append(f"jbuf[0] is not r7 ({slot(jb)})")
    if slot(jb + 8) is None or slot(jb + 8)[0] != "sp":
        ctx.problems.append(f"jbuf[2] is not sp ({slot(jb + 8)})")
    return ctx


def dispatch(fn: Function, ctx: Context, index: int, limit: int = 400) -> str:
    """Run the resume code of ``fn`` as after a longjmp that installed landing pad
    ``index``: r7 and sp from the jump buffer, ``call_site`` = ``index``, the data words
    unknown. Returns where it ends: ``call 0xADDR`` (the first call: the landing pad
    runs), ``branch 0xADDR`` (a decision on data the dispatch does not know: the landing
    pad's own code), ``return 0xADDR``, ``trap 0xADDR`` (the dispatch rejected the index)
    or ``lost 0xADDR``."""
    assert ctx.state is not None and ctx.fc is not None and ctx.resume is not None
    s = ctx.state.copy()
    jb = ctx.fc + FC_JBUF
    s.regs = {7: s.slots.get(jb), SP: s.slots.get(jb + 8)}
    s.flags = None
    for k in range(ctx.fc + FC_CALL_SITE, ctx.fc + FC_PERSONALITY):
        s.slots.pop(k, None)
    s.slots[ctx.fc + FC_CALL_SITE] = ("c", index & 0xFFFFFFFF)
    addr = ctx.resume
    for _ in range(limit):
        insn = fn.insns.get(addr)
        if insn is None:
            return f"lost 0x{addr:X}"
        m = insn.mnemonic
        if not insn.known or m in ("trap", "udf", "bkpt"):
            return f"trap 0x{addr:X}"
        if m in ("bl", "blx"):
            return f"call 0x{addr:X}"
        if is_return(insn):
            return f"return 0x{addr:X}"
        table = fn.tables.get(addr)
        if table is not None:
            v = s.regs.get(table.index)
            if v is None or v[0] != "c":
                return f"branch 0x{addr:X}"
            target = dict(table.cases).get(v[1])
            if target is None:  # an index the table does not have: the code after it
                return f"trap 0x{addr:X}"
            addr = target
            continue
        if m in ("b", "cbz", "cbnz"):
            taken = _taken(s, insn)
            if taken is None:
                return f"branch 0x{addr:X}"
            addr = insn.operands[-1].target if taken else insn.end
            continue
        if insn.cond != AL and not insn.in_it:
            return f"branch 0x{addr:X}"
        if insn.in_it:
            c = _cond(s, insn.cond)
            if c is None:
                return f"branch 0x{addr:X}"
            if not c:
                addr = insn.end
                continue
            insn = Insn(insn.address, insn.size, insn.raw, m, insn.operands, AL, insn.setflags)
        if written_regs(insn) is not None and PC in (written_regs(insn) or ()):
            return f"lost 0x{addr:X}"
        s.step(insn)
        addr = insn.end
    return f"lost 0x{addr:X}"


def _cond(s: _State, cond: int) -> bool | None:
    if cond == AL:
        return True
    if s.flags is None:
        return None
    a, b = s.flags
    r = (a - b) & 0xFFFFFFFF
    n, z, c = r >> 31, int(r == 0), int(a >= b)
    v = ((a ^ b) & (a ^ r)) >> 31
    return (
        z, not z, c, not c, n, not n, v, not v,
        c and not z, not c or z, n == v, n != v, not z and n == v, z or n != v,
    )[cond]  # fmt: skip


def _taken(s: _State, insn: Insn) -> bool | None:
    if insn.mnemonic in ("cbz", "cbnz"):
        v = s.regs.get(insn.operands[0].n)
        if v is None or v[0] != "c":
            return None
        return (v[1] == 0) == (insn.mnemonic == "cbz")
    return _cond(s, insn.cond)


# -- the whole image ----------------------------------------------------------------------------


def read_image(image) -> Callable[[int, int], bytes]:
    """A guest-memory reader over ``image``'s file-backed segments."""

    def read(addr: int, n: int) -> bytes:
        try:
            return image.macho.read(addr, n)
        except Exception:  # unmapped: the parser reports it as truncated
            return b""

    return read


@dataclass
class Report:
    contexts: list[Context]
    lsdas: dict[int, Lsda]
    raisers: dict[str, list[int]]  # import -> callers (function pointers)
    dispatches: dict[int, dict[int, str]]  # ptr -> landing pad index -> outcome

    def counts(self) -> dict[str, int]:
        c: dict[str, int] = {
            "contexts": len(self.contexts),
            "lsdas": len(self.lsdas),
            "lsda_bytes": sum(x.size for x in self.lsdas.values()),
            "call_sites": sum(len(x.call_sites) for x in self.lsdas.values()),
            "problems": sum(len(x.problems) for x in self.contexts),
        }
        for x in self.lsdas.values():
            for site in x.call_sites:
                for part in x.kind(site).split("+"):
                    c[part] = c.get(part, 0) + 1
        for ctx in self.contexts:
            key = f"personality:{ctx.personality}"
            c[key] = c.get(key, 0) + 1
        return c


def report(image, discovery) -> Report:
    """Every SjLj context of the image, its LSDA and dispatch, and who raises."""
    read = read_image(image)
    contexts: list[Context] = []
    lsdas: dict[int, Lsda] = {}
    dispatches: dict[int, dict[int, str]] = {}
    raisers: dict[str, list[int]] = {}
    for ptr in sorted(discovery.functions):
        fn = discovery.functions[ptr]
        for name, _ in _import_calls(fn, image).items():
            if name in RAISERS:
                raisers.setdefault(name, []).append(ptr)
        ctx = find_context(fn, image)
        if ctx is None:
            continue
        contexts.append(ctx)
        if ctx.lsda is not None:
            try:
                lsdas[ctx.lsda] = lsda = parse_lsda(read, ctx.lsda)
            except LsdaError as exc:
                ctx.problems.append(str(exc))
                continue
            if ctx.resume is not None and ctx.state is not None:
                pads = sorted({s.landing_pad for s in lsda.call_sites})
                dispatches[ptr] = {i: dispatch(fn, ctx, i) for i in [*pads, max(pads) + 1]}
    return Report(contexts, lsdas, raisers, dispatches)


def _h(v: int) -> str:
    return f"0x{v:X}"


def to_json(rep: Report, source: dict) -> str:
    from mtport import ghidra_common as gc

    rows = []
    for ctx in rep.contexts:
        lsda = rep.lsdas.get(ctx.lsda) if ctx.lsda is not None else None
        row = {
            "ptr": _h(ctx.ptr),
            "frame": ctx.frame,
            "register": _h(ctx.register) if ctx.register is not None else None,
            "unregister": [_h(a) for a in ctx.unregister],
            "resume_calls": [_h(a) for a in ctx.resume_calls],
            "personality": ctx.personality,
            "lsda": _h(ctx.lsda) if ctx.lsda is not None else None,
            "lsda_size": lsda.size if lsda else None,
            "resume": _h(ctx.resume) if ctx.resume is not None else None,
            "store": _h(ctx.store) if ctx.store is not None else None,
            "call_sites": [[s.index, s.landing_pad, lsda.kind(s)] for s in lsda.call_sites]
            if lsda
            else [],
            "types": [_h(t.address) for _, t in sorted(lsda.types.items())] if lsda else [],
            "dispatch": {str(k): v for k, v in rep.dispatches.get(ctx.ptr, {}).items()},
            "problems": ctx.problems,
        }
        rows.append(row)
    header = {
        "schema": 1,
        "source": source,
        "counts": rep.counts(),
        "raisers": {k: [_h(p) for p in v] for k, v in sorted(rep.raisers.items())},
    }
    return gc.dumps(header, "contexts", rows)


def write(slice_path: Path, out: Path = EXCEPTIONS_OUT) -> Report:
    from mtport import ghidra_common as gc
    from recomp.discover import discover
    from recomp.loader import Image

    data = Path(slice_path).read_bytes()
    image = Image(data)
    rep = report(image, discover(image))
    gc.write_text(out, to_json(rep, gc.source_info(Path(slice_path), data, REPO)))
    return rep


__all__ = [
    "Action", "CallSite", "Context", "EXCEPTIONS_OUT", "FC_CALL_SITE", "FC_DATA", "FC_JBUF",
    "FC_LSDA", "FC_PERSONALITY", "FC_PREV", "FC_SIZE", "Lsda", "LsdaError", "PERSONALITIES",
    "RAISERS", "Report", "TypeRef", "dispatch", "find_context", "parse_lsda", "read_image",
    "report", "to_json", "write",
]  # fmt: skip
