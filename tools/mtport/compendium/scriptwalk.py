"""Symbolic walker over the shipped scripts' bytecode (issue #217, docs/compendium/kb.md).

The 307 scripts use 24 of Lua 2.4's 67 opcodes: no locals, tables, loops or concatenation
(docs/formats/script-out.md). Every value is a constant, a global or the result of a native,
and every jump goes forward, so each function has finitely many paths. :func:`walk` runs a
function on a symbolic VM and returns every path through it: the conditions it took (as
comparisons of native results with constants), the native calls it made (with their argument
values) and the state it ended in. :func:`dialogue` follows a script's entry point through the
dialogue box: a path that registers a callback (``regCallBack``) and opens the box
(``showMsg``) continues in the callback once the player answers, with ``getEvtIndex`` bound to
the answer, so a whole conversation is one tree.

Semantics are the game's (docs/re/script-vm.md): integer numbers; ``nil`` is false and every
number (0 included) is true; ``EQOP`` and the order operators give 1 or nil; script globals
start as nil on every load except the 65 natives; a script function called from another runs
inline and returns nothing. A native that returns a value gives a :class:`Term`, a symbol for
"what the game answers here"; two reads of the same native with the same arguments give the
same term until a native that changes the game state runs in between (the *epoch*). A branch
on a term splits the path into the cases the conditions so far allow (:class:`Store`), so
contradictory paths (a quest that is both 0 and 1) are never produced.

Paths that meet again at a jump target with the same calls made, the same stack and the same
box state, and that differ only in their conditions on one term, which together allow every
value of it (the two arms of an if), are merged (:func:`_merge_covering`): the merged path
keeps the conditions they share, so it runs exactly when one of them would. A global they
disagree on becomes a conditional value (``Term("ite")``: the
alternatives with their conditions), which splits again where it is read. An NPC-state
function that sets one flag per quest and reads them all at the end then costs one split per
read instead of a doubling per quest.

The answer of a box (``CChatBox::GetAnswer`` ``0x31E11``): mode 1 or 3 (Accept / Refuse) gives
1 for Accept and 0 for Refuse; mode 2 (a list) gives the chosen row's ``regSelList`` value (-1
when there are no rows); mode 0 gives -1. Back closes the box without running the callback
(``CChatBox::Close`` ``0x35E89``), which ends any conversation; :func:`dialogue` lists only the
answers that continue it.

The bytecode is untrusted data: it is decoded by :mod:`mtport.scriptout` and interpreted here,
never executed.
"""

from __future__ import annotations

import heapq
from collections.abc import Iterable, Iterator, Sequence
from dataclasses import dataclass, field, replace
from typing import Any

from mtport import scriptout
from mtport.scriptout_isa import NATIVE_BY_NAME, NATIVES

# Natives whose call cannot change what a later native returns (the getters, the message
# box, the prepared quest): they keep the epoch. Every other native starts a new one.
PURE = frozenset(
    {
        "getLevel",
        "getQuestState",
        "checkRequire",
        "getEvtIndex",
        "getHeroClass",
        "getClassUpCount",
        "checkItemCount",
        "getQuestTitle",
        "isInvenAvailable",
        "getEquipState",
        "regCallBack",
        "regMsgHero",
        "regMsgNPC",
        "regSelList",
        "initMsg",
        "showMsg",
        "setQuest",
        "regRequireItem",
        "regRequireMob",
        "regRequireMeso",
        "regRewardItem",
        "regRewardMeso",
        "regRewardExp",
        "regMsgRequire",
        "regMsgReward",
        "setNPC",
        "setBoss",
        "setNpcNotify",
        "setNpcHide",
        "setNpcDir",
        "showPortalErrMsg",
        "invenFullNotify",
        "updateNPCState",
        "clearQuest",
        "println",
        "print",
    }
)

# The values a native's result can take, where the game bounds them: (low, high) inclusive.
RANGES: dict[str, tuple[int, int]] = {
    "checkRequire": (0, 1),
    "getReward": (0, 1),
    "getItem": (0, 1),
    "getEquipState": (0, 1),
    "isInvenAvailable": (0, 1),
    "getLevel": (1, 255),
    "getQuestState": (0, 255),
    "getHeroClass": (0, 255),
    "getClassUpCount": (0, 255),
    "checkItemCount": (0, 0x7FFF),
}

# Box modes (CChatBox +0x08) and what getEvtIndex gives in each.
MODE_OK, MODE_ACCEPT, MODE_LIST, MODE_ACCEPT2 = 0, 1, 2, 3
NO_ANSWER = -1

MAX_PATHS = 100000  # per walk of one function, and per conversation; more is a problem
# The exact walk's budget: past it a function is walked again with approximate merges.
EXACT_PATHS = 20000  # per walk of one function
EXACT_CONVERSATION = 100000  # per conversation
MAX_DEPTH = 40  # nested callbacks in one conversation


class WalkError(Exception):
    """The script does something the walker does not model (an unresolved path)."""


class TooManyPaths(WalkError):
    """A walk went past its path budget."""


# -- values ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class Func:
    """A script function, by the global main stores it in (or ``f<slot>``)."""

    name: str
    slot: int


@dataclass(frozen=True)
class NativeFn:
    name: str


@dataclass(frozen=True)
class Term:
    """A value the game decides: a native's result, an answer, or an expression of them.

    ``op`` is ``"call"`` (``name`` the native, ``args`` its argument values, ``epoch``),
    ``"answer"`` (``epoch`` = the number of the box answered), ``"global"`` (an unknown initial
    global, only in a standalone walk), ``"add"``, ``"neg"``, a comparison (``"=="`` ...) of
    two terms, or ``"ite"`` (a merged global: ``args`` = ``((conds, value), ...)``)."""

    op: str
    name: str = ""
    args: tuple = ()
    epoch: int = 0

    def __hash__(self) -> int:  # cached: terms nest deeply (ite, or) and are hashed often
        h = self.__dict__.get("_hash")
        if h is None:
            h = hash((self.op, self.name, self.args, self.epoch))
            object.__setattr__(self, "_hash", h)
        return h

    def __str__(self) -> str:
        t = self.__dict__.get("_text")
        if t is None:
            t = _show_term(self)
            object.__setattr__(self, "_text", t)
        return t


Value = int | str | None | Func | NativeFn | Term

CMP = ("==", "!=", "<", "<=", ">", ">=")
NEGATE = {"==": "!=", "!=": "==", "<": ">=", ">=": "<", ">": "<=", "<=": ">"}
FLIP = {"==": "==", "!=": "!=", "<": ">", ">": "<", "<=": ">=", ">=": "<="}
OPS = {"EQOP": "==", "LTOP": "<", "LEOP": "<=", "GTOP": ">", "GEOP": ">="}


def _holds(op: str, a: int, b: int) -> bool:
    return {
        "==": a == b,
        "!=": a != b,
        "<": a < b,
        "<=": a <= b,
        ">": a > b,
        ">=": a >= b,
    }[op]


def show(v: Any) -> str:
    """A value as text: ``getQuestState(1, 21)``, ``answer#2``, ``"key"``, ``nil``."""
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "1" if v else "nil"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return f'"{v}"'
    if isinstance(v, Func):
        return f"function {v.name}"
    if isinstance(v, NativeFn):
        return f"native {v.name}"
    if isinstance(v, Term):
        return str(v)
    return repr(v)


def _show_term(v: Term) -> str:
    if v.op == "call":
        return f"{v.name}({', '.join(show(a) for a in v.args)})"
    if v.op == "answer":
        return f"answer#{v.epoch}"
    if v.op in ("global", "approximate"):
        return v.name or v.op
    if v.op == "add":
        return f"({show(v.args[0])} + {show(v.args[1])})"
    if v.op == "neg":
        return f"-{show(v.args[0])}"
    if v.op == "ite":
        return (
            "ite("
            + "; ".join(
                f"{' and '.join(str(c) for c in cs) or 'else'}: {show(x)}" for cs, x in v.args
            )
            + ")"
        )
    return f"({show(v.args[0])} {v.op} {show(v.args[1])})"


def as_json(v: Any) -> Any:
    """A value for the KB: constants as themselves, functions and terms as text."""
    if v is None or isinstance(v, (int, str)):
        return v
    return show(v)


# -- conditions -----------------------------------------------------------------------------


@dataclass(frozen=True)
class Cond:
    """One condition a path took: ``term op value`` (``value`` an int), or for an opaque
    boolean term (a comparison of two terms) ``term == 1`` / ``term != 1``."""

    term: Term
    op: str
    value: int

    def __hash__(self) -> int:
        h = self.__dict__.get("_hash")
        if h is None:
            h = hash((self.term, self.op, self.value))
            object.__setattr__(self, "_hash", h)
        return h

    def __str__(self) -> str:
        return f"{show(self.term)} {self.op} {self.value}"

    def json(self) -> dict[str, Any]:
        out: dict[str, Any] = {"term": show(self.term), "op": self.op, "value": self.value}
        t = self.term
        if t.op == "call":
            out["native"] = t.name
            out["args"] = [as_json(a) for a in t.args]
        elif t.op == "answer":
            out["answer"] = t.epoch
        return out


APPROX = Cond(Term("approximate"), "==", 1)


@dataclass(frozen=True)
class Store:
    """The conditions of a path, per term, and the allowed values of the answer terms."""

    conds: tuple[Cond, ...] = ()
    allowed: tuple[tuple[Term, frozenset[int]], ...] = ()

    def _of(self, term: Term) -> tuple[list[tuple[str, int]], frozenset[int] | None]:
        cs = [(c.op, c.value) for c in self.conds if c.term == term]
        allow = None
        for t, vals in self.allowed:
            if t == term:
                allow = vals if allow is None else allow & vals
        lo_hi = _range(term)
        if lo_hi is not None:
            cs += [(">=", lo_hi[0]), ("<=", lo_hi[1])]
        return cs, allow

    def feasible(self, term: Term, extra: tuple[str, int] | None = None) -> bool:
        cs, allow = self._of(term)
        if extra is not None:
            cs.append(extra)
        if allow is not None:
            candidates: set[int] = set(allow)
        else:
            consts = {c for _, c in cs} or {0}
            candidates = {c + d for c in consts for d in (-1, 0, 1)}
        return any(all(_holds(op, x, c) for op, c in cs) for x in candidates)

    def add(self, cond: Cond) -> Store:
        return replace(self, conds=(*self.conds, cond))

    def allow(self, term: Term, values: frozenset[int]) -> Store:
        return replace(self, allowed=(*self.allowed, (term, values)))

    def value(self, term: Term) -> int | None:
        """The one value the conditions leave, if they leave exactly one."""
        cs, allow = self._of(term)
        if allow is not None:
            left = [x for x in allow if all(_holds(op, x, c) for op, c in cs)]
            return left[0] if len(left) == 1 else None
        eq = [c for op, c in cs if op == "=="]
        return eq[0] if eq else None


def _range(term: Term) -> tuple[int, int] | None:
    if term.op == "call":
        return RANGES.get(term.name)
    if term.op in CMP:
        return (0, 1)
    return None


def _points(term: Term, conds: Iterable[Cond]) -> list[int]:
    """Values of ``term`` that stand for all others: the truth of a comparison with the
    constants only changes at a constant, so the constants and their neighbours do."""
    consts = {c.value for c in conds} or {0}
    pts = {c + d for c in consts for d in (-1, 0, 1)} | {min(consts) - 2, max(consts) + 2}
    lo_hi = _range(term)
    if lo_hi is not None:
        pts = {p for p in pts if lo_hi[0] <= p <= lo_hi[1]} | set(lo_hi)
    return sorted(pts)


def _covers(term: Term, groups: list[list[Cond]]) -> bool:
    """Whether every value of ``term`` satisfies at least one of the condition groups."""
    every = [c for g in groups for c in g]
    return all(
        any(all(_holds(c.op, x, c.value) for c in g) for g in groups) for x in _points(term, every)
    )


def simplify(alternatives: Iterable[Sequence[Cond]]) -> list[tuple[Cond, ...]]:
    """A smaller list of condition sets that holds exactly when one of ``alternatives``
    does. Two rules, to a fixed point: a set that contains another is dropped; sets that are
    the same but for their conditions on one term become their common rest when those
    conditions together allow every value of the term, or the rest and one condition when
    they allow exactly what that condition does (``level < 30`` or ``level >= 30``, each
    with ``level >= 10``: ``level >= 10``). Paths split on every condition they meet, so the paths
    to one outcome often differ only in conditions that do not matter to it."""
    alts = _reduce({_tighten(a) for a in alternatives})
    return [tuple(sorted(a, key=_cond_key)) for a in alternatives_order(alts)]


def _reduce(alts: set[frozenset[Cond]]) -> set[frozenset[Cond]]:
    """:func:`simplify`'s rules, to a fixed point."""
    while True:
        alts = _minimal(alts)
        # Deterministic order (the result depends on which merges are made first).
        groups: dict[tuple, list[list[Cond]]] = {}
        for a in alternatives_order(alts):
            for term in sorted({c.term for c in a}, key=show):
                rest = frozenset(c for c in a if c.term != term)
                groups.setdefault((term, rest), []).append([c for c in a if c.term == term])
        new, used = set(alts), set()
        for (term, rest), gs in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            if len(gs) == 1 and len(gs[0]) == 1:
                continue
            members = {rest | frozenset(g) for g in gs}
            if members & used:
                continue
            union = _union(term, gs)
            if union is None or members == {rest | union}:
                continue
            new = (new - members) | {rest | union}
            used |= members
        if new == alts:
            return alts
        alts = new


def _union(term: Term, groups: list[list[Cond]]) -> frozenset[Cond] | None:
    """The condition groups on ``term`` taken together, when that is no condition (they
    allow every value) or one of their conditions; else None."""
    every = [c for g in groups for c in g]
    pts = _points(term, every)
    union = [any(all(_holds(c.op, x, c.value) for c in g) for g in groups) for x in pts]
    if all(union):
        return frozenset()
    for c in sorted(set(every), key=_cond_key):
        if all(_holds(c.op, x, c.value) == u for x, u in zip(pts, union, strict=True)):
            return frozenset({c})
    return None


def _minimal(alts: set[frozenset[Cond]]) -> set[frozenset[Cond]]:
    """The sets no other set is a subset of."""
    out: list[frozenset[Cond]] = []
    for a in sorted(alts, key=len):
        if not any(b <= a for b in out):
            out.append(a)
    return set(out)


def _tighten(conds: Iterable[Cond]) -> frozenset[Cond]:
    """One set of conditions without the ones another of the set implies: next to
    ``term == c`` the other conditions on ``term`` say nothing more."""
    conds = frozenset(conds)
    eq = {c.term: c for c in conds if c.op == "=="}
    return frozenset(c for c in conds if c.term not in eq or c is eq[c.term])


def _cond_key(c: Cond) -> tuple:
    return (show(c.term), c.op, c.value)


def alternatives_order(alts: Iterable[frozenset[Cond]]) -> list[frozenset[Cond]]:
    return sorted(alts, key=lambda a: (len(a), [_cond_key(c) for c in sorted(a, key=_cond_key)]))


# -- events and paths -----------------------------------------------------------------------


@dataclass(frozen=True)
class Event:
    """One call of a native on a path: its name, argument values and where it is. Getters
    (natives that only return a value) are not events: they live on in the conditions."""

    native: str
    args: tuple[Value, ...]
    function: str
    offset: int
    conds: tuple[Cond, ...] = ()  # the path's conditions when the call is made

    def __str__(self) -> str:
        return f"{self.native}({', '.join(show(a) for a in self.args)})"

    def json(self) -> dict[str, Any]:
        return {"native": self.native, "args": [as_json(a) for a in self.args]}


@dataclass(frozen=True)
class Choice:
    key: str | None
    value: Value


@dataclass
class State:
    globals: dict[str, Value]
    store: Store = field(default_factory=Store)
    events: list[Event] = field(default_factory=list)
    callback: Func | None = None
    choices: list[Choice] = field(default_factory=list)
    shows: list[tuple[Value, tuple[Choice, ...]]] = field(default_factory=list)
    epoch: int = 0
    answers: int = 0  # boxes answered so far on this conversation
    calls: frozenset[str] = frozenset()  # script functions entered (for coverage)

    def fork(self) -> State:
        return State(
            self.globals.__class__(self.globals),
            self.store,
            list(self.events),
            self.callback,
            list(self.choices),
            list(self.shows),
            self.epoch,
            self.answers,
            self.calls,
        )


@dataclass
class Path:
    """One complete run of a function: from its start to its RETCODE0."""

    state: State
    conds: tuple[Cond, ...]  # the conditions this run added (not the ones before it)

    @property
    def events(self) -> list[Event]:
        return self.state.events


@dataclass
class _Frame:
    name: str
    code: tuple[scriptout.Instruction, ...]
    index: dict[int, int]
    joins: frozenset[int]
    pc: int
    base: int
    nres: int


def initial_globals() -> dict[str, Value]:
    return {n.name: NativeFn(n.name) for n in NATIVES}


class Script:
    """A decoded script with its functions by name and slot."""

    def __init__(self, chunk: scriptout.Chunk, name: str | None = None) -> None:
        self.chunk = chunk
        self.name = name or chunk.name
        names = chunk.function_names()
        self.by_slot: dict[int, scriptout.Function] = {
            f.slot: f for f in chunk.subfunctions if f.slot is not None
        }
        self.names: dict[int, str] = {s: names.get(s, f"f{s}") for s in self.by_slot}
        self.by_name: dict[str, int] = {n: s for s, n in self.names.items()}
        self._code: dict[int | None, tuple] = {}

    def function(self, slot: int | None) -> scriptout.Function:
        return self.chunk.main if slot is None else self.by_slot[slot]

    def func_name(self, slot: int | None) -> str:
        return "main" if slot is None else self.names[slot]

    def code(
        self, slot: int | None
    ) -> tuple[tuple[scriptout.Instruction, ...], dict[int, int], frozenset[int]]:
        """The function's instructions, offset -> index, and the indexes jumps land on."""
        if slot not in self._code:
            code = self.chunk.instructions(self.function(slot))
            for ins in code:
                if ins.opcode is None:
                    raise WalkError(
                        f"{self.name}: unknown opcode 0x{ins.raw[0]:02X} at 0x{ins.offset:X}"
                    )
                if ins.name in ("UPJMP", "IFFUPJMP"):
                    raise WalkError(f"{self.name}: backward jump at 0x{ins.offset:X}")
            index = {ins.offset: k for k, ins in enumerate(code)}
            joins = frozenset(index[i.target] for i in code if i.target is not None)
            self._code[slot] = (code, index, joins)
        return self._code[slot]


# -- the walk -------------------------------------------------------------------------------


def walk(
    script: Script,
    slot: int | None,
    state: State | None = None,
    limit: int = MAX_PATHS,
    exact: bool = True,
) -> list[Path]:
    """Every path of function ``slot`` (None: main) of ``script``, from ``state`` (default:
    a fresh load, globals nil but the natives). Not ``exact``: paths that cannot merge
    exactly merge anyway, with :data:`APPROX` among their conditions."""
    if state is None:
        state = State(initial_globals())
    start = len(state.store.conds)
    out: list[Path] = []
    for st in _run(script, slot, state, exact, limit):
        out.append(Path(st, st.store.conds[start:]))
        if len(out) > limit:
            raise TooManyPaths(
                f"{script.name}: more than {limit} paths from {script.func_name(slot)}"
            )
    return out


@dataclass
class _Live:
    """One path being walked: its state, call frames and stack."""

    st: State
    frames: list[_Frame]
    stack: list[Value]

    def position(self) -> tuple[int, ...]:
        """Order of the walk: callers at their CALLFUNC, the callee at its pc. Every
        instruction moves a path forward in this order (all jumps in the scripts go forward)."""
        return (*(f.pc - 1 for f in self.frames[:-1]), self.frames[-1].pc)

    def shape(self) -> tuple:
        """What two paths must share to merge: everything but their globals and conditions."""
        st = self.st
        return (
            tuple((f.name, f.pc, f.base, f.nres) for f in self.frames),
            tuple(self.stack),
            tuple(st.events),
            st.callback,
            tuple(st.choices),
            tuple(st.shows),
            st.epoch,
            st.answers,
            st.store.allowed,
        )


def _enter(script: Script, slot: int | None, base: int, nres: int) -> _Frame:
    code, index, joins = script.code(slot)
    return _Frame(script.func_name(slot), code, index, joins, 0, base, nres)


def _run(
    script: Script, slot: int | None, state: State, exact: bool = True, limit: int = MAX_PATHS
) -> Iterator[State]:
    """The paths in walk order; paths at the same jump target merge first. More than
    ``limit`` paths being walked at once is :class:`TooManyPaths`."""
    st = state.fork()
    st.calls = st.calls | {script.func_name(slot)}
    base = len(st.store.conds)
    heap: list[tuple[tuple[int, ...], int, _Live]] = []
    seq = 0

    def push(live: _Live) -> None:
        nonlocal seq
        heapq.heappush(heap, (live.position(), seq, live))
        seq += 1

    push(_Live(st, [_enter(script, slot, 0, 0)], []))
    while heap:
        pos, _, live = heapq.heappop(heap)
        group = [live]
        while heap and heap[0][0] == pos:
            group.append(heapq.heappop(heap)[2])
        for one in _merge_group(group, base, exact):
            for item in _advance(script, one):
                if isinstance(item, State):
                    yield item
                else:
                    push(item)
        if len(heap) > limit:
            raise TooManyPaths(
                f"{script.name}: more than {limit} paths in {script.func_name(slot)}"
            )


def _merge_group(group: list[_Live], base: int, exact: bool = True) -> list[_Live]:
    if len(group) == 1:
        return group
    by_shape: dict[tuple, list[_Live]] = {}
    for live in group:
        by_shape.setdefault(live.shape(), []).append(live)
    out: list[_Live] = []
    for lives in by_shape.values():
        lives = _merge_covering(lives, base)
        out += lives if exact or len(lives) == 1 else [_merge_approx(lives)]
    return out


def _merge_approx(lives: list[_Live]) -> _Live:
    """One path for ``lives`` whatever their conditions: the ones they share, and
    :data:`APPROX`. Globals they disagree on become an ``ite`` (exact)."""
    first = lives[0].st.store.conds
    n = min(len(x.st.store.conds) for x in lives)
    for k in range(n):
        if any(x.st.store.conds[k] != first[k] for x in lives[1:]):
            n = k
            break
    rests = [x.st.store.conds[n:] for x in lives]
    st0 = lives[0].st
    g = st0.globals.__class__(st0.globals)
    for name in set().union(*(x.st.globals for x in lives)):
        vals = [x.st.globals.get(name) for x in lives]
        if any(v != vals[0] for v in vals[1:]):
            alts = sum((_alternatives(r, v) for r, v in zip(rests, vals, strict=True)), ())
            g[name] = Term("ite", args=alts)
    st = st0.fork()
    st.globals = g
    kept = first[:n]
    st.store = replace(st0.store, conds=kept if APPROX in kept else (*kept, APPROX))
    for x in lives[1:]:
        st.calls = st.calls | x.st.calls
    return _Live(st, lives[0].frames, lives[0].stack)


def _merge_covering(lives: list[_Live], base: int) -> list[_Live]:
    """:func:`simplify`'s rule on paths: paths that differ only in their conditions on one
    term, which together allow every value of it (the arms of an if that join again), become
    one path with the conditions they share (:func:`_merge`). To a fixed point, merging the
    groups that do not overlap in each pass. Paths are never merged otherwise, so a path's
    conditions always say exactly when it runs. Conditions before ``base`` (the walk's
    starting state) are shared by all and kept."""
    while len(lives) > 1:
        groups: dict[tuple, list[int]] = {}
        for i, x in enumerate(lives):
            own = x.st.store.conds[base:]
            for term in dict.fromkeys(c.term for c in own):
                rest = frozenset(c for c in own if c.term != term)
                groups.setdefault((term, rest), []).append(i)
        used: set[int] = set()
        merged: list[_Live] = []
        for (term, _), idx in groups.items():
            if len(idx) < 2 or used.intersection(idx):
                continue
            gs = [[c for c in lives[i].st.store.conds[base:] if c.term == term] for i in idx]
            if _covers(term, gs):
                merged.append(_merge([lives[i] for i in idx], term, base))
                used.update(idx)
        if not merged:
            break
        lives = merged + [x for i, x in enumerate(lives) if i not in used]
    return lives


def _alternatives(rest: tuple[Cond, ...], v: Value) -> tuple[tuple[tuple[Cond, ...], Value], ...]:
    if isinstance(v, Term) and v.op == "ite":
        return tuple((rest + c, x) for c, x in v.args)
    return ((rest, v),)


def _merge(lives: list[_Live], term: Term, base: int) -> _Live:
    """One path for ``lives``, which differ only in their conditions on ``term``: those are
    dropped; a global they disagree on becomes an ``ite`` of their values under them."""

    def on(x: _Live) -> tuple[Cond, ...]:
        return tuple(c for c in x.st.store.conds[base:] if c.term == term)

    first = lives[0].st
    g = first.globals.__class__(first.globals)
    for name in set().union(*(x.st.globals for x in lives)):
        vals = [x.st.globals.get(name) for x in lives]
        if any(v != vals[0] for v in vals[1:]):
            alts = sum((_alternatives(on(x), v) for x, v in zip(lives, vals, strict=True)), ())
            g[name] = Term("ite", args=alts)
    st = first.fork()
    st.globals = g
    conds = first.store.conds
    kept = tuple(c for c in conds[base:] if c.term != term)
    st.store = replace(first.store, conds=conds[:base] + kept)
    for x in lives[1:]:
        st.calls = st.calls | x.st.calls
    return _Live(st, lives[0].frames, lives[0].stack)


def _fork(live: _Live, store: Store | None = None) -> _Live:
    st = live.st.fork()
    if store is not None:
        st.store = store
    return _Live(st, [replace(f) for f in live.frames], [*live.stack])


def _assume(store: Store, conds: tuple[Cond, ...]) -> Store | None:
    """``store`` with ``conds`` added, or None when they contradict it."""
    for c in conds:
        if not store.feasible(c.term, (c.op, c.value)):
            return None
        store = store.add(c)
    return store


def _advance(script: Script, live: _Live) -> list[_Live | State]:
    """Run one path until it ends (a State), forks, or reaches a jump target (_Live)."""
    st, frames, stack = live.st, live.frames, live.stack
    first = True
    while True:
        fr = frames[-1]
        if fr.pc >= len(fr.code):
            raise WalkError(f"{script.name}: {fr.name} runs off its end")
        if not first and fr.pc in fr.joins:
            return [live]
        first = False
        ins = fr.code[fr.pc]
        fr.pc += 1
        op = ins.name
        where = f"{script.name}: {fr.name} 0x{ins.offset:X} {op}"
        if op in ("PUSH0", "PUSH1", "PUSH2"):
            stack.append(int(op[-1]))
        elif op in ("PUSHBYTE", "PUSHWORD"):
            stack.append(ins.operands[0])
        elif op == "PUSHNIL":
            stack.append(None)
        elif op == "PUSHSTRING":
            stack.append(ins.names[0])
        elif op == "PUSHFUNCTION":
            s = ins.operands[0]
            stack.append(Func(script.names.get(s, f"f{s}"), s))
        elif op == "PUSHGLOBAL":
            name = ins.names[0]
            v = st.globals.get(name)
            if isinstance(v, Term) and v.op == "ite":
                return _split(live, name, v, where)
            stack.append(v)
        elif op == "STOREGLOBAL":
            st.globals[ins.names[0]] = _pop(stack, where)
        elif op == "ADJUST0":
            del stack[fr.base :]
        elif op == "POP":
            _pop(stack, where)
        elif op in OPS:
            b = _pop(stack, where)
            a = _pop(stack, where)
            stack.append(_compare(OPS[op], a, b, where))
        elif op == "ADDOP":
            b = _pop(stack, where)
            a = _pop(stack, where)
            if isinstance(a, int) and isinstance(b, int):
                stack.append(a + b)
            elif _numeric(a) and _numeric(b):
                stack.append(Term("add", args=(a, b)))
            else:
                raise WalkError(f"{where}: {show(a)} + {show(b)}")
        elif op == "MINUSOP":
            a = _pop(stack, where)
            if isinstance(a, int):
                stack.append(-a)
            elif _numeric(a):
                stack.append(Term("neg", args=(a,)))
            else:
                raise WalkError(f"{where}: -{show(a)}")
        elif op == "JMP":
            fr.pc = fr.index[ins.target]
        elif op in ("IFFJMP", "ONTJMP", "ONFJMP"):
            x = _pop(stack, where)
            forks: list[_Live | State] = []
            for truth, store in _branches(x, st.store):
                # IFFJMP pops and jumps on nil; ONTJMP / ONFJMP keep the value and jump on
                # non-nil / nil (the compiler's POP after them drops it on the fall-through).
                jump = truth if op == "ONTJMP" else not truth
                f = _fork(live, store)
                if jump:
                    f.frames[-1].pc = f.frames[-1].index[ins.target]
                if op != "IFFJMP":
                    f.stack.append(_concrete(x, truth))
                forks.append(f)
            if not forks:
                raise WalkError(f"{where}: no branch is possible")
            return forks
        elif op == "CALLFUNC":
            nargs, nres = ins.operands
            if nres == scriptout.MULT_RET:
                raise WalkError(f"{where}: all-results call")
            args = [_pop(stack, where) for _ in range(nargs)][::-1]
            fn = _pop(stack, where)
            if isinstance(fn, NativeFn):
                results = _native(st, fn.name, tuple(args), fr.name, ins.offset, where)
                stack.extend((results + [None] * nres)[:nres])
            elif isinstance(fn, Func):
                if fn.slot not in script.by_slot:
                    raise WalkError(f"{where}: no function in slot {fn.slot}")
                if any(f.name == fn.name for f in frames):
                    raise WalkError(f"{where}: recursive call of {fn.name}")
                st.calls = st.calls | {fn.name}
                frames.append(_enter(script, fn.slot, len(stack), nres))
            else:
                raise WalkError(f"{where}: call of {show(fn)}")
        elif op == "RETCODE0":
            done = frames.pop()
            del stack[done.base :]
            if not frames:
                return [st]
            stack.extend([None] * done.nres)
        else:
            raise WalkError(f"{where}: opcode not modelled")


def _split(live: _Live, name: str, v: Term, where: str) -> list[_Live | State]:
    """A read of a merged global: one path per alternative its conditions allow, each with
    the value pushed (the PUSHGLOBAL is done)."""
    out: list[_Live | State] = []
    for conds, x in v.args:
        store = _assume(live.st.store, conds)
        if store is None:
            continue
        f = _fork(live, store)
        f.st.globals[name] = x
        f.stack.append(x)
        out.append(f)
    if not out:
        raise WalkError(f"{where}: no alternative of {name} is possible")
    return out


def _pop(stack: list[Value], where: str) -> Value:
    if not stack:
        raise WalkError(f"{where}: stack underflow")
    return stack.pop()


def _numeric(v: Value) -> bool:
    if isinstance(v, int):
        return True
    return isinstance(v, Term) and v.op not in CMP and v.name != "getQuestTitle"


def _concrete(x: Value, truth: bool) -> Value:
    """The value an and/or keeps on the stack once its truth is known."""
    if isinstance(x, Term) and x.op in CMP:
        return 1 if truth else None
    return x


def _compare(op: str, a: Value, b: Value, where: str) -> Value:
    if isinstance(a, int) and isinstance(b, int):
        return 1 if _holds(op, a, b) else None
    if isinstance(a, str) and isinstance(b, str) and op != "==":
        return 1 if _holds(op, (a > b) - (a < b), 0) else None
    if op == "==":
        if not isinstance(a, Term) and not isinstance(b, Term):
            return 1 if a == b else None
        if a is None or b is None:
            return None  # a native's result is never nil
        if a == b:
            return 1
    if isinstance(a, Term) and isinstance(b, int):
        return Term(op, args=(a, b))
    if isinstance(b, Term) and isinstance(a, int):
        return Term(FLIP[op], args=(b, a))
    if isinstance(a, Term) and isinstance(b, Term):
        return Term(op, args=(a, b))
    raise WalkError(f"{where}: {show(a)} {op} {show(b)}")


def _branches(x: Value, store: Store) -> list[tuple[bool, Store | None]]:
    """The truth values ``x`` can have under ``store``, each with the store that results."""
    if not isinstance(x, Term):
        return [(x is not None, None)]
    if x.op not in CMP:
        return [(True, None)]  # a number is never nil
    a, b = x.args
    if isinstance(b, int):
        out = []
        for truth, op in ((True, x.op), (False, NEGATE[x.op])):
            if store.feasible(a, (op, b)):
                out.append((truth, store.add(Cond(a, op, b))))
        return out
    out = []  # two terms: the comparison itself is the condition
    for truth, op in ((True, "=="), (False, "!=")):
        if store.feasible(x, (op, 1)):
            out.append((truth, store.add(Cond(x, op, 1))))
    return out


def _native(
    st: State, name: str, args: tuple[Value, ...], func: str, offset: int, where: str
) -> list[Value]:
    native = NATIVE_BY_NAME[name]
    if len(args) != len(native.params):
        raise WalkError(
            f"{where}: {name} with {len(args)} arguments, it reads {len(native.params)}"
        )
    if native.returns is None or name not in PURE:
        st.events.append(Event(name, args, func, offset, st.store.conds))
    if name == "regCallBack":
        (f,) = args
        if not isinstance(f, Func):
            raise WalkError(f"{where}: regCallBack({show(f)})")
        st.callback = f
    elif name == "initMsg":
        st.choices = []
    elif name == "regSelList":
        st.choices.append(Choice(args[0] if isinstance(args[0], str) else None, args[1]))
    elif name == "showMsg":
        st.shows.append((args[0], tuple(st.choices)))
    result: list[Value] = []
    if native.returns is not None:
        if name == "getEvtIndex":
            result = [Term("answer", epoch=st.answers)]
        else:
            result = [Term("call", name, args, st.epoch)]
    if name not in PURE:
        st.epoch += 1
    return result


# -- conversations --------------------------------------------------------------------------


@dataclass
class Step:
    """One path of one function in a conversation, and what follows it."""

    function: str
    conds: tuple[Cond, ...]
    events: list[Event]
    show: tuple[Value, tuple[Choice, ...]] | None  # the box it opened last, if any
    state: State
    next: Node | None = None  # the callback's paths, after the answer
    loop: str | None = None  # the callback repeats an earlier point of this conversation

    def walk_steps(self) -> Iterator[Step]:
        yield self
        if self.next is not None:
            yield from self.next.walk_steps()


@dataclass
class Node:
    """The paths of one function run at one point of a conversation."""

    function: str
    answer: int | None  # the answer term these paths branch on (None: the entry)
    answers: tuple[int, ...] | None  # the values that answer can take
    steps: list[Step]

    def walk_steps(self) -> Iterator[Step]:
        for s in self.steps:
            yield from s.walk_steps()


def answers_of(mode: Value, choices: Sequence[Choice]) -> frozenset[int] | None:
    """The values ``getEvtIndex`` can give after a box of ``mode`` (None: not known)."""
    if mode in (MODE_ACCEPT, MODE_ACCEPT2):
        return frozenset({0, 1})
    if mode == MODE_LIST:
        vals = {c.value for c in choices}
        if not vals:
            return frozenset({NO_ANSWER})
        if all(isinstance(v, int) for v in vals):
            return frozenset(vals)  # type: ignore[arg-type]
        return None
    if mode == MODE_OK:
        return frozenset({NO_ANSWER})
    return None


def dialogue(
    script: Script, slot: int | None, state: State | None = None, exact: bool = True
) -> Node:
    """The conversation tree from function ``slot`` (None: main)."""
    budget = EXACT_CONVERSATION if exact else MAX_PATHS
    return _node(script, slot, state, None, None, (), [budget], exact)


def conversation(script: Script, slot: int | None) -> tuple[Node, bool]:
    """The conversation tree, exact when it fits :data:`EXACT_CONVERSATION`, else walked
    again with approximate merges; and whether it is exact."""
    try:
        return dialogue(script, slot), True
    except TooManyPaths:
        return dialogue(script, slot, exact=False), False


def _node(
    script: Script,
    slot: int | None,
    state: State | None,
    answer: int | None,
    answers: frozenset[int] | None,
    seen: tuple[tuple, ...],
    budget: list[int],
    exact: bool = True,
) -> Node:
    name = script.func_name(slot)
    if len(seen) > MAX_DEPTH:
        raise WalkError(f"{script.name}: conversation deeper than {MAX_DEPTH} boxes at {name}")
    start = len(state.events) if state is not None else 0
    paths = walk(script, slot, state, EXACT_PATHS if exact else MAX_PATHS, exact)
    budget[0] -= len(paths)
    if budget[0] < 0:
        limit = EXACT_CONVERSATION if exact else MAX_PATHS
        raise TooManyPaths(f"{script.name}: more than {limit} paths in one conversation")
    node = Node(name, answer, tuple(sorted(answers)) if answers is not None else None, [])
    for p in paths:
        st = p.state
        show_ = st.shows[-1] if st.shows else None
        step = Step(name, p.conds, st.events[start:], show_, st)
        node.steps.append(step)
        if st.callback is None:
            continue
        if show_ is None:
            raise WalkError(f"{script.name}: {name} registers {st.callback.name} but opens no box")
        cb = st.callback
        nxt = st.fork()
        nxt.callback = None
        nxt.shows = []
        nxt.answers += 1  # getEvtIndex in the callback reads this answer
        n = nxt.answers
        vals = answers_of(*show_)
        if vals is not None:
            nxt.store = nxt.store.allow(Term("answer", epoch=n), vals)
        key = (cb.name, _point(nxt))
        if key in seen:
            step.loop = cb.name
            continue
        step.next = _node(script, cb.slot, nxt, n, vals, (*seen, key), budget, exact)
    return node


def _point(st: State) -> tuple:
    """A conversation point, for loop detection: the globals and the non-answer conditions."""
    conds = tuple(c for c in st.store.conds if not _mentions_answer(c.term))
    return (
        tuple(sorted((k, show(v)) for k, v in st.globals.items() if not isinstance(v, NativeFn))),
        conds,
        st.epoch,
    )


def _mentions_answer(t: Any) -> bool:
    if isinstance(t, Term):
        return t.op == "answer" or any(_mentions_answer(a) for a in t.args)
    return False


def entry_points(script: Script) -> list[tuple[str, int | None]]:
    """The functions the game runs directly: ``("main", None)`` and, when the file defines
    functions, ``("state", 0)`` (the NPC-state pass runs slot 0 without main)."""
    out: list[tuple[str, int | None]] = [("main", None)]
    if 0 in script.by_slot:
        out.append(("state", 0))
    return out


class _FreeGlobals(dict):
    """Globals where an unset name reads as a free term instead of nil."""

    def get(self, key: str, default: Value = None) -> Value:  # type: ignore[override]
        if key in self:
            return self[key]
        return Term("global", key)


def standalone(script: Script, slot: int) -> list[Path]:
    """Every path of one function on its own (for a function no entry point reaches): the
    script's functions are defined as main defines them, other globals are free terms."""
    g = _FreeGlobals(initial_globals())
    for s, name in script.names.items():
        g[name] = Func(name, s)
    try:
        return walk(script, slot, State(g), EXACT_PATHS)
    except TooManyPaths:
        return walk(script, slot, State(g), exact=False)
