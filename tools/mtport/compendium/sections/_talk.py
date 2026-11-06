"""The conversation model of the quests section (#223): a dialogue entity's tree as *parts*.

The knowledge base stores a conversation as the script walker found it (``dialogue.nodes``):
nodes, each with every path through the function that runs there. Paths repeat the same lines
under many combinations of quest states, so a page cannot show them one by one. This module
turns the tree into a graph of the lines themselves and cuts that graph into **parts**:

- a line (a ``regMsgNPC``/``regMsgHero`` key) or a menu row (a ``regSelList`` key) is a node;
- a path's lines follow each other; the last line of a path leads to the first line of every
  path of the next node that the answer allows (Accept or Refuse, the chosen row, or simply
  the next box), and whose conditions do not contradict the path's;
- a part is a run of lines with no branch inside: each line but the last has one successor,
  and each line but the first one predecessor. Every menu row starts a part of its own.

So every key a conversation shows is in exactly one part. A part keeps what the paths say
about it: the conditions under which it shows (summarised to what holds on every path that
reaches it, :func:`necessary`), the natives called while it shows (stage directions), where
it leads and what leads to it, and the quest it belongs to (:func:`_owner`).

Everything here is derived from the knowledge base at build time; no game text is stored.
"""

from __future__ import annotations

import re
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from typing import Any

LINE_KEYS = ("npc", "hero", "choice")
# Natives that name a quest by (kind, number) in their first two arguments.
QUEST_CALLS = frozenset(
    {"setQuest", "getReward", "regMsgReward", "regMsgRequire", "removeQuest", "makeEquipItem"}
)
TERM_RE = re.compile(r"^(\w+)\((.*)\)$")
ANSWER_RE = re.compile(r"^answer#(\d+)$")
# The values a condition's term can take, where the game bounds them (getQuestState
# 0x799B1: 0 not started, 1 in progress, 2 done; checkRequire and the inventory checks give
# 0 or 1; getClassUpCount counts advancements 0 to 4).
DOMAINS: dict[str, frozenset[int]] = {
    "getQuestState": frozenset({0, 1, 2}),
    "checkRequire": frozenset({0, 1}),
    "isInvenAvailable": frozenset({0, 1}),
    "getClassUpCount": frozenset({0, 1, 2, 3, 4}),
}
LEVELS = range(1, 100)


def parse_term(term: str) -> tuple[str, tuple[Any, ...]]:
    """``getQuestState(1, 21)`` gives ``("getQuestState", (1, 21))``; ``answer#1`` gives
    ``("answer", (1,))``; anything else ``(term, ())``."""
    m = ANSWER_RE.match(term)
    if m:
        return "answer", (int(m.group(1)),)
    m = TERM_RE.match(term)
    if not m:
        return term, ()
    args: list[Any] = []
    for a in m.group(2).split(","):
        a = a.strip()
        if not a:
            continue
        try:
            args.append(int(a))
        except ValueError:
            args.append(a)
    return m.group(1), tuple(args)


def _ok(op: str, x: int, v: Any) -> bool:
    if not isinstance(v, (int, float)):
        return True
    return {
        "==": x == v,
        "!=": x != v,
        "<": x < v,
        "<=": x <= v,
        ">": x > v,
        ">=": x >= v,
    }.get(op, True)


def domain(term: str) -> frozenset[int] | None:
    native, _ = parse_term(term)
    if native == "getLevel":
        return frozenset(LEVELS)
    return DOMAINS.get(native)


def allowed(conds: Iterable[Mapping[str, Any]]) -> dict[str, frozenset[int]] | None:
    """The values each bounded term may take under one alternative's conditions; ``None``
    when the alternative cannot hold (a term with no value left)."""
    out: dict[str, frozenset[int]] = {}
    for c in conds:
        term = str(c.get("term", ""))
        dom = domain(term)
        if dom is None:
            continue
        cur = out.get(term, dom)
        cur = frozenset(x for x in cur if _ok(str(c.get("op")), x, c.get("value")))
        if not cur:
            return None
        out[term] = cur
    return out


def necessary(alternatives: Iterable[Iterable[Mapping[str, Any]]]) -> dict[str, frozenset[int]]:
    """What holds on every alternative: per bounded term, the union of the values each
    alternative allows, kept when it is not the whole domain. Answer terms are left out (the
    part's links show the answers)."""
    sets: list[dict[str, frozenset[int]]] = []
    for alt in alternatives:
        a = allowed(alt)
        if a is not None:
            sets.append(a)
    if not sets:
        return {}
    terms = set(sets[0])
    for s in sets[1:]:
        terms &= set(s)
    out = {}
    for term in sorted(terms):
        dom = domain(term) or frozenset()
        union = frozenset().union(*(s[term] for s in sets))
        if union != dom:
            out[term] = union
    return out


def compatible(a: Iterable[Iterable[Mapping[str, Any]]], b: Iterable[Iterable[Mapping]]) -> bool:
    """Whether some alternative of ``a`` and some of ``b`` can hold together (answer terms
    and unbounded terms never conflict)."""
    aa = [x for x in (allowed(alt) for alt in a) if x is not None]
    bb = [x for x in (allowed(alt) for alt in b) if x is not None]
    if not aa or not bb:
        return bool(aa) and bool(bb)
    for x in aa:
        for y in bb:
            if all(x[t] & y[t] for t in set(x) & set(y)):
                return True
    return False


@dataclass(frozen=True)
class Line:
    """One line of a part: ``who`` is ``npc``, ``hero`` or ``choice`` (a menu row)."""

    key: str
    who: str
    value: int | None = None  # a menu row's answer value


@dataclass
class Part:
    """A run of lines with no branch inside (the module docstring)."""

    dialogue: str
    number: int
    lines: list[Line]
    when: dict[str, frozenset[int]] = field(default_factory=dict)
    calls: list[tuple[str, tuple[Any, ...]]] = field(default_factory=list)
    nexts: list[tuple[str, int]] = field(default_factory=list)  # (label, part number)
    prevs: list[int] = field(default_factory=list)
    ends: bool = False
    quests: list[tuple[int, int]] = field(default_factory=list)  # (kind, number) by weight
    owner: tuple[int, int] | None = None

    @property
    def anchor(self) -> str:
        return re.sub(r"[^A-Za-z0-9_-]+", "-", f"{self.dialogue}-{self.number}")

    @property
    def menu(self) -> bool:
        return bool(self.lines) and self.lines[0].who == "choice"


@dataclass
class _Path:
    node: str
    items: list[Line]
    calls: dict[int, list[tuple[str, tuple[Any, ...]]]]  # by the index of the line before
    cond: list[list[Mapping[str, Any]]]
    box: int | None
    next: str | None
    quests: list[tuple[int, int]]


def _paths(e: Mapping[str, Any]) -> tuple[dict[str, list[_Path]], dict[str, Mapping]]:
    nodes = e.get("nodes") or {}
    out: dict[str, list[_Path]] = {}
    for nid in nodes:
        rows = []
        for p in nodes[nid].get("paths") or []:
            items: list[Line] = []
            calls: dict[int, list[tuple[str, tuple[Any, ...]]]] = {}
            quests: list[tuple[int, int]] = []
            prepared: tuple[int, int] | None = None
            for a in p.get("do") or []:
                if "call" in a:
                    name = str(a["call"])
                    args = tuple(a.get("args") or ())
                    if name == "startQuest" and prepared is not None:
                        args = prepared  # startQuest takes the prepared quest
                    calls.setdefault(len(items) - 1, []).append((name, args))
                    if (
                        name in QUEST_CALLS
                        and len(args) >= 2
                        and all(isinstance(x, int) for x in args[:2])
                    ):
                        q = (int(args[0]), int(args[1]))
                        quests.append(q)
                        if name == "setQuest":
                            prepared = q
                    continue
                for k in LINE_KEYS:
                    if k in a and isinstance(a[k], str):
                        items.append(Line(a[k], k, a.get("value") if k == "choice" else None))
                        break
            rows.append(
                _Path(
                    nid,
                    items,
                    calls,
                    [list(alt) for alt in p.get("if") or []],
                    p.get("box"),
                    p.get("next"),
                    quests,
                )
            )
        out[nid] = rows
    return out, nodes


def _answers(q: _Path, box_index: int, dom: Iterable[int]) -> set[int]:
    """The answer values (of box ``box_index``) under which path ``q`` runs."""
    out: set[int] = set()
    term = f"answer#{box_index}"
    for alt in q.cond:
        vals = set(dom)
        for c in alt:
            if c.get("term") == term:
                vals = {x for x in vals if _ok(str(c.get("op")), x, c.get("value"))}
        out |= vals
    return out


BUTTONS = {1: {1: "Accept", 0: "Refuse"}, 3: {1: "First button", 0: "Second button"}}


@dataclass
class Talk:
    """One conversation as parts, numbered from 1 in the order a reader meets them."""

    dialogue: str
    parts: list[Part]
    by_key: dict[str, int]  # line key -> part number

    def part(self, number: int) -> Part:
        return self.parts[number - 1]


def build(e: Mapping[str, Any]) -> Talk:
    """The parts of dialogue entity ``e``."""
    did = str(e["id"])
    by_node, nodes = _paths(e)

    def successors(p: _Path, seen: frozenset[int] = frozenset()) -> list[tuple[str, Line, _Path]]:
        """``(label, first line)`` of what follows path ``p``; silent paths pass through."""
        if p.next is None or p.next not in nodes:
            return []
        node = nodes[p.next]
        idx = node.get("answer")
        dom = [int(x) for x in node.get("answers") or [-1] if isinstance(x, int)]
        out: list[tuple[str, Line, _Path]] = []
        rows = [it for it in p.items if it.who == "choice"]
        for q in by_node.get(p.next, []):
            if not compatible(p.cond, q.cond):
                continue
            vals = _answers(q, int(idx), dom) if isinstance(idx, int) else set(dom)
            if not vals:
                continue
            if p.box == 2 and rows:
                label = "choice"  # rows lead on themselves (see edges below)
            elif p.box in BUTTONS:
                names = [BUTTONS[p.box][v] for v in (1, 0) if v in vals and v in BUTTONS[p.box]]
                label = " or ".join(names)
            else:
                label = ""
            firsts = _first_lines(q, seen | {id(q)})
            for f in firsts:
                out.append((label, f, q))
        return out

    def _first_lines(q: _Path, seen: frozenset[int]) -> list[Line]:
        speech = [it for it in q.items if it.who != "choice"]
        if speech:
            return [speech[0]]
        rows = [it for it in q.items if it.who == "choice"]
        if rows:
            return rows
        if len(seen) > 64:
            return []
        return [f for _, f, _q in successors(q, seen)]

    # The line graph.
    succ: dict[str, list[tuple[str, str]]] = {}  # key -> [(label, key)]
    pred: dict[str, set[str]] = {}
    order: list[str] = []
    lines: dict[str, Line] = {}
    calls: dict[str, list[tuple[str, tuple[Any, ...]]]] = {}
    when: dict[str, list[list[Mapping[str, Any]]]] = {}
    votes: dict[str, dict[tuple[int, int], int]] = {}
    ends: set[str] = set()
    targeted: set[str] = set()  # menu rows that lead somewhere on some path

    def see(line: Line) -> None:
        if line.key not in lines:
            lines[line.key] = line
            order.append(line.key)
            succ[line.key] = []
            pred.setdefault(line.key, set())

    def edge(a: str, b: str, label: str) -> None:
        if (label, b) not in succ[a]:
            succ[a].append((label, b))
        pred.setdefault(b, set()).add(a)

    root = e.get("tree")
    node_order = ([root] if root in by_node else []) + [n for n in by_node if n != root]
    # Visit nodes from the root along `next`, so the numbering follows the conversation.
    visited: list[str] = []
    stack = list(reversed(node_order[:1]))
    seen_nodes: set[str] = set()
    while stack:
        n = stack.pop()
        if n in seen_nodes or n not in by_node:
            continue
        seen_nodes.add(n)
        visited.append(n)
        nxt = [p.next for p in by_node[n] if p.next]
        for m in reversed(list(dict.fromkeys(nxt))):
            if m not in seen_nodes:
                stack.append(m)
    visited += [n for n in node_order if n not in seen_nodes]

    # startQuest takes the quest the conversation prepared, often on an earlier path.
    for n in visited:
        for p in by_node[n]:
            prepared = [c[1][:2] for cs in p.calls.values() for c in cs if c[0] == "setQuest"]
            if not prepared or len(prepared[-1]) < 2:
                continue
            q = (int(prepared[-1][0]), int(prepared[-1][1]))
            todo, seen_p = [p], set()
            while todo:
                x = todo.pop()
                for _lab, _f, y in successors(x):
                    if id(y) in seen_p:
                        continue
                    seen_p.add(id(y))
                    for i, cs in y.calls.items():
                        y.calls[i] = [
                            ("startQuest", q) if c == ("startQuest", ()) else c for c in cs
                        ]
                    if any(c == ("startQuest", q) for cs in y.calls.values() for c in cs):
                        if q not in y.quests:
                            y.quests.append(q)
                    elif not y.items:
                        todo.append(y)

    for n in visited:
        for p in by_node[n]:
            for it in p.items:
                see(it)
                when.setdefault(it.key, []).extend(p.cond)
                v = votes.setdefault(it.key, {})
                for q in p.quests:
                    v[q] = v.get(q, 0) + 1
            for i, cs in p.calls.items():
                if not p.items:
                    continue
                k = p.items[max(i, 0)].key
                for c in cs:
                    if c not in calls.setdefault(k, []):
                        calls[k].append(c)
    for n in visited:
        for p in by_node[n]:
            speech = [it for it in p.items if it.who != "choice"]
            rows = [it for it in p.items if it.who == "choice"]
            for a, b in zip(speech, speech[1:], strict=False):
                edge(a.key, b.key, "")
            after = successors(p)
            if rows:
                for r in rows:
                    if speech:
                        edge(speech[-1].key, r.key, "")
                    targets = [
                        (f, q)
                        for _lab, f, q in after
                        if r.value is None
                        or r.value
                        in _answers(
                            q,
                            int(nodes[p.next].get("answer") or 0),
                            [r.value],
                        )
                    ]
                    for f, _q in targets:
                        see(f)
                        edge(r.key, f.key, "")
                        targeted.add(r.key)
            elif speech:
                last = speech[-1].key
                for lab, f, _q in after:
                    see(f)
                    edge(last, f.key, lab)
                if not after:
                    ends.add(last)

    ends |= {k for k, ln in lines.items() if ln.who == "choice" and k not in targeted}

    # Parts: maximal runs without a branch inside.
    def head(k: str) -> bool:
        line = lines[k]
        ps = pred.get(k, set())
        if line.who == "choice" or len(ps) != 1:
            return True
        (p,) = tuple(ps)
        return len(succ[p]) != 1 or bool(succ[p][0][0]) or p in ends

    part_of: dict[str, int] = {}
    parts: list[Part] = []

    def run(start: str) -> None:
        seq = [start]
        k = start
        while (
            len(succ[k]) == 1
            and not succ[k][0][0]
            and k not in ends
            and not head(succ[k][0][1])
            and succ[k][0][1] not in part_of
            and succ[k][0][1] not in seq
        ):
            k = succ[k][0][1]
            seq.append(k)
        part = Part(did, len(parts) + 1, [lines[x] for x in seq])
        for x in seq:
            part_of[x] = part.number
        parts.append(part)

    for k in order:
        if k not in part_of and head(k):
            run(k)
    for k in order:  # lines on a cycle with no head
        if k not in part_of:
            run(k)

    for part in parts:
        first, last = part.lines[0].key, part.lines[-1].key
        part.when = necessary(when.get(first, []))
        seen_calls: list[tuple[str, tuple[Any, ...]]] = []
        tally: dict[tuple[int, int], int] = {}
        for ln in part.lines:
            for c in calls.get(ln.key, []):
                if c not in seen_calls:
                    seen_calls.append(c)
            for q, w in votes.get(ln.key, {}).items():
                tally[q] = tally.get(q, 0) + w
        part.calls = seen_calls
        part.quests = [q for q, _ in sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))]
        nexts: list[tuple[str, int]] = []
        for lab, b in succ[last]:
            t = (lab, part_of[b])
            if t not in nexts:
                nexts.append(t)
        part.nexts = nexts
        part.ends = last in ends
    for part in parts:
        for _lab, n in part.nexts:
            target = parts[n - 1]
            if part.number not in target.prevs:
                target.prevs.append(part.number)
    _owner(parts)
    return Talk(did, parts, part_of)


def _topic(key: str) -> str:
    return key.rsplit(".", 1)[0] if "." in key else key


def _when_quests(p: Part) -> list[tuple[int, int]]:
    out = []
    for term in p.when:
        name, args = parse_term(term)
        if name in ("getQuestState", "checkRequire") and len(args) == 2:
            q = (int(args[0]), int(args[1]))
            if q not in out:
                out.append(q)
    return out


def _majority(tally: Mapping[tuple[int, int], int]) -> tuple[int, int] | None:
    if not tally:
        return None
    best, n = sorted(tally.items(), key=lambda kv: (-kv[1], kv[0]))[0]
    return best if n * 2 > sum(tally.values()) else None


def _owner(parts: list[Part]) -> None:
    """Each part's quest, by the first rule that decides:

    1. the quest the natives called while it shows name most (``setQuest``, ``getReward``,
       ``regMsgReward``...);
    2. the one quest its conditions name (a quest state or ``checkRequire`` that holds on
       every path to it), not counting quests that must be done when others are named;
    3. the quest most parts of its topic belong to, by a majority (the topic is the keys'
       shared prefix, as the script's author grouped the lines);
    4. the quest of the parts it leads to, or else of the parts that lead to it, when they
       all agree.

    A part no rule decides is general dialogue (``owner`` None)."""
    for p in parts:
        p.owner = p.quests[0] if p.quests else None
        if p.owner is None:
            qs = _when_quests(p)
            if len(qs) > 1:  # a quest that must be done is a prerequisite, not the subject
                qs = [q for q in qs if p.when.get(f"getQuestState({q[0]}, {q[1]})") != {2}]
            if len(qs) == 1:
                p.owner = qs[0]
    topics: dict[str, dict[tuple[int, int], int]] = {}
    for p in parts:
        if p.owner is not None:
            for ln in p.lines:
                t = topics.setdefault(_topic(ln.key), {})
                t[p.owner] = t.get(p.owner, 0) + 1
    for p in parts:
        if p.owner is None:
            tally: dict[tuple[int, int], int] = {}
            for ln in p.lines:
                for q, w in topics.get(_topic(ln.key), {}).items():
                    tally[q] = tally.get(q, 0) + w
            p.owner = _majority(tally)
    for _ in range(4):
        for p in parts:
            if p.owner is not None:
                continue
            for group in ([n for _l, n in p.nexts], p.prevs):
                owners = {parts[n - 1].owner for n in group if n != p.number}
                if len(owners) == 1 and None not in owners:
                    p.owner = owners.pop()
                    break
