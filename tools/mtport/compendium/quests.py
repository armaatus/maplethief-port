"""KB stage: what the scripts do (issue #217, docs/compendium/kb.md).

Every shipped script is walked by :mod:`mtport.compendium.scriptwalk` (every path of every
function, conversations followed through their callbacks) and the native calls on the paths
become entities:

- ``script``: one per ``.out`` file, with the chapters it runs in and whether it runs at all;
- ``dialogue``: the conversation tree of an NPC's or a map's script, with the text of every
  key in English and Korean;
- ``quest``: one per ``(chapter, kind, number)`` a script prepares with ``setQuest``, with its
  giver, prerequisites, requirements, rewards and the quests it unlocks;
- ``recipe`` and ``trade``: the item-for-item quests the smiths (``makeEquipItem``) and the
  traders prepare and complete in one conversation;
- ``job_advancement``: ``setClassUp``;
- ``warp``: every ``changeMap``, with its conditions (portal, NPC and map scripts), which also
  completes ``map.exits`` with the NPC and map-script warps;
- ``event``: ``startBoss``, ``runAnimation``, ``initMonCa`` and ``clearAll``;
- the ``npc.opens_shop`` links (``showMarket``).

Chapters: the quest manager's chapter (``quests+4``) picks ``script/chapter<n>/<name>.out``
over ``script/common/<name>.out`` (``0x63191``). Chapter 0 is a new game: the intro and the
tutorial, until ``portal281``'s ``clearAll``. A script runs in chapter ``n`` when an object
on a map the hero can reach in chapter ``n`` names it (:func:`reachable`).
"""

from __future__ import annotations

import json
from collections import defaultdict
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import scriptout
from mtport.compendium import scriptwalk as sw
from mtport.compendium.kb import KB, Sources, natural_key, relation
from mtport.compendium.tables import SKILL_BRANCHES, clean_name, clean_text, item_id

relation("script", "maps", "map", "scripts")
relation("dialogue", "script", "script", "dialogue")
relation("dialogue", "npcs", "npc", "dialogues")
relation("quest", "given_by", "npc", "gives_quests")
relation("quest", "completed_by", "npc", "completes_quests")
relation("quest", "script", "script", "quests")
relation("quest", "needs_items", "item", "needed_by_quests")
relation("quest", "needs_kills", "monster", "hunted_by_quests")
relation("quest", "reward_items", "item", "rewarded_by_quests")
relation("quest", "requires", "quest", "unlocks")
relation("recipe", "crafted_by", "npc", "crafts")
relation("recipe", "result", "item", "crafted_by")
relation("recipe", "materials", "item", "material_of")
relation("trade", "offered_by", "npc", "trades")
relation("trade", "cost", "item", "traded_away_in")
relation("trade", "result", "item", "traded_for_in")
relation("job_advancement", "given_by", "npc", "advancements")
relation("warp", "script", "script", "warps")
relation("warp", "source", "map", "warps_out")
relation("warp", "target", "map", "warps_in")
relation("event", "script", "script", "events")
relation("event", "maps", "map", "events")
relation("npc", "opens_shop", "shop", "opened_by")

CHAPTERS = (0, 1, 2, 3)
# A new game (chapter 0) starts on the intro's map 2-10; its scene puts the hero on the
# tutorial's 2-8 (CStateGame::LoadGame 0x86C1D; docs/ANALYSIS.md, the launch to the first map).
INTRO_MAPS = ("2-10", "2-8")
QUEST_KINDS = {0: "main", 1: "sub"}
# Quest states (getQuestState 0x799B1): what the scripts compare them with.
QUEST_STATES = {0: "not started", 1: "in progress", 2: "done"}
JOB_NAMES = {k: v for k, v in SKILL_BRANCHES.items()}
MESSAGE_NATIVES = ("regMsgNPC", "regMsgHero", "regSelList")
# Natives that only shape the box: left out of a step's "do" list (the box mode is "box").
BOX_NATIVES = frozenset({"initMsg", "showMsg", "regCallBack"})
GETTERS = {"getLevel": "level", "getHeroClass": "job", "getClassUpCount": "advancements"}
REQUIRE = {"regRequireItem": "item", "regRequireMob": "mob", "regRequireMeso": "meso"}
REWARD = {"regRewardItem": "item", "regRewardMeso": "meso", "regRewardExp": "exp"}


# -- walking the files ----------------------------------------------------------------------


def script_id(rel: str) -> str:
    """``chapter1/Alex`` -> ``chapter1-Alex``."""
    return rel.replace("/", "-")


@dataclass
class Walked:
    """One file walked: its conversation trees and the functions no entry point reaches."""

    rel: str  # chapter1/Alex
    script: sw.Script
    trees: dict[str, sw.Node] = field(default_factory=dict)  # "main", "state"
    approximate: list[str] = field(default_factory=list)  # entries walked approximately
    unreached: dict[str, list[sw.Path]] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)

    @property
    def ident(self) -> str:
        return script_id(self.rel)

    def steps(self, entry: str = "main") -> Iterator[sw.Step]:
        tree = self.trees.get(entry)
        if tree is not None:
            yield from tree.walk_steps()

    def all_events(self) -> Iterator[tuple[sw.Event, bool]]:
        """Every event of every walk, with whether an entry point reaches it."""
        for entry in self.trees:
            for st in self.steps(entry):
                for e in st.events:
                    yield e, True
        for paths in self.unreached.values():
            for p in paths:
                for e in p.events:
                    yield e, False


def walk_file(rel: str, path: Path) -> Walked:
    w = Walked(rel, sw.Script(scriptout.load(path), rel))
    reached: set[str] = set()
    for entry, slot in sw.entry_points(w.script):
        try:
            w.trees[entry], exact = sw.conversation(w.script, slot)
        except sw.WalkError as e:
            w.errors.append(str(e))
            continue
        if not exact:
            w.approximate.append(entry)
        for st in w.trees[entry].walk_steps():
            reached |= st.state.calls
    for slot, name in sorted(w.script.names.items()):
        if name in reached:
            continue
        try:
            w.unreached[name] = sw.standalone(w.script, slot)
        except sw.WalkError as e:
            w.errors.append(f"{e} (standalone)")
    return w


# -- references and chapters ----------------------------------------------------------------


@dataclass(frozen=True)
class ObjRef:
    """An object of a map that names a script: ``role`` npc, portal or map."""

    name: str
    map: str
    role: str
    npc: str | None = None
    portal: int | None = None


def object_refs(kb: KB) -> list[ObjRef]:
    out = []
    for mid, m in sorted(kb.entities.get("map", {}).items(), key=lambda kv: natural_key(kv[0])):
        if m.get("map_script"):
            out.append(ObjRef(m["map_script"], mid, "map"))
        for p in m.get("portals", []):
            if p["script"]:
                out.append(ObjRef(p["script"], mid, "portal", portal=p["number"]))
        for n in m.get("npcs", []):
            if n.get("script"):
                out.append(ObjRef(n["script"], mid, "npc", npc=n["id"]))
    return out


def resolve(files: dict[str, Path], name: str, chapter: int) -> str | None:
    """The file the game runs for ``name`` in ``chapter``, as ``dir/name`` (None: no file)."""
    for rel in (f"chapter{chapter}/{name}", f"common/{name}"):
        if rel in files:
            return rel
    return None


def warp_targets(w: Walked, chapter_end: bool = False) -> set[str]:
    """The maps a file's walked paths warp to within the chapter, or (``chapter_end``) on the
    paths that end it with ``clearAll`` (where the next chapter starts)."""
    out = set()
    for st in w.steps("main"):
        if ("clearAll" in {e.native for e in st.events}) != chapter_end:
            continue
        for e in st.events:
            if e.native == "changeMap" and all(isinstance(a, int) for a in e.args):
                out.add(f"{e.args[0]}-{e.args[1]}")
    return out


def reachable(
    refs: list[ObjRef], files: dict[str, Path], walked: dict[str, Walked]
) -> tuple[dict[int, set[str]], dict[int, set[str]]]:
    """Per chapter, the maps the hero can reach and the files that run there.

    From the chapter's first maps (chapter 0: :data:`INTRO_MAPS`; chapter n: the maps the
    warps on chapter n - 1's ``clearAll`` paths lead to), every warp of every script named on
    a reached map is followed, whatever its conditions (an upper bound)."""
    by_map: dict[str, list[ObjRef]] = defaultdict(list)
    for r in refs:
        by_map[r.map].append(r)
    maps: dict[int, set[str]] = {}
    runs: dict[int, set[str]] = {}
    start = set(INTRO_MAPS)
    for c in CHAPTERS:
        seen, todo, used = set(start), list(sorted(start)), set()
        while todo:
            m = todo.pop()
            for r in by_map.get(m, []):
                rel = resolve(files, r.name, c)
                if rel is None or rel not in walked:
                    continue
                used.add(rel)
                for t in sorted(warp_targets(walked[rel])):
                    if t not in seen:
                        seen.add(t)
                        todo.append(t)
        maps[c], runs[c] = seen, used
        start = set()
        for rel in sorted(used):
            start |= warp_targets(walked[rel], chapter_end=True)
    return maps, runs


# -- facts from the paths -------------------------------------------------------------------


@dataclass(frozen=True)
class Prep:
    """A quest as ``setQuest`` and the ``regRequire*`` / ``regReward*`` after it prepare it."""

    kind: int
    number: int
    title: str | None
    msg: str | None
    function: str
    requirements: tuple[tuple, ...] = ()
    rewards: tuple[tuple, ...] = ()
    conds: tuple[sw.Cond, ...] = ()  # at the setQuest

    def add(self, e: sw.Event) -> Prep:
        a = e.args
        if e.native in REQUIRE:
            row = (REQUIRE[e.native], *a)
            return Prep(**{**self.__dict__, "requirements": (*self.requirements, row)})
        row = (REWARD[e.native], *a)
        return Prep(**{**self.__dict__, "rewards": (*self.rewards, row)})

    @property
    def key(self) -> tuple[int, int]:
        return (self.kind, self.number)

    def sig(self) -> tuple:
        """What tells two preparations apart (not the path that led to them)."""
        return (self.key, self.title, self.msg, self.function, self.requirements, self.rewards)


Conds = tuple[sw.Cond, ...]


@dataclass
class Facts:
    """What one file's paths do, each fact once (with whether an entry point reaches it)."""

    preps: dict[tuple, tuple[Prep, bool]] = field(default_factory=dict)
    starts: dict[tuple, tuple[Prep, Conds, bool]] = field(default_factory=dict)
    rewards: set[tuple[tuple[int, int], bool]] = field(default_factory=set)
    crafts: set[tuple[int, int]] = field(default_factory=set)
    removes: set[tuple[int, int]] = field(default_factory=set)
    class_ups: dict[tuple, tuple[int | None, int, Conds]] = field(default_factory=dict)
    warps: dict[tuple, tuple[sw.Event, bool, bool]] = field(default_factory=dict)
    shops: dict[tuple, tuple[int, Conds]] = field(default_factory=dict)
    events: dict[tuple, tuple[str, tuple, Conds, bool]] = field(default_factory=dict)
    speakers: set[int] = field(default_factory=set)
    gifts: dict[tuple, tuple[tuple, Conds]] = field(default_factory=dict)
    keys: set[str] = field(default_factory=set)
    # Where each key is first said: (function, offset), for the order of an unused
    # script's text (#225).
    key_at: dict[str, tuple[str, int]] = field(default_factory=dict)
    problems: list[str] = field(default_factory=list)

    def say_at(self, key: str, e: sw.Event) -> None:
        at = (str(e.function), int(e.offset))
        if key not in self.key_at or at < self.key_at[key]:
            self.key_at[key] = at

    def prep(self, p: Prep, reached: bool) -> None:
        old = self.preps.get(p.sig())
        self.preps[p.sig()] = (old[0] if old else p, reached or bool(old and old[1]))


EVENT_NATIVES = {
    "startBoss": "boss",
    "runAnimation": "scene",
    "initMonCa": "carnival",
    "clearAll": "chapter_end",
}


def facts(w: Walked) -> Facts:
    f = Facts()
    for tree in w.trees.values():
        for step, prep in _with_preps(tree, None):
            _step_facts(w, f, step.events, prep, True)
    for paths in w.unreached.values():
        for p in paths:
            _step_facts(w, f, p.events, None, False)
    f.problems = sorted(set(f.problems))
    return f


def _with_preps(node: sw.Node, prep: Prep | None) -> Iterator[tuple[sw.Step, Prep | None]]:
    """Each step with the quest prepared before it on its conversation."""
    for step in node.steps:
        yield step, prep
        if step.next is not None:
            after = prep
            for e in step.events:
                after = _prep_after(after, e)
            yield from _with_preps(step.next, after)


def _prep_after(prep: Prep | None, e: sw.Event) -> Prep | None:
    if e.native == "setQuest":
        k, n, title, msg = e.args
        if not isinstance(k, int) or not isinstance(n, int):
            return None
        return Prep(
            k,
            n,
            title if isinstance(title, str) else None,
            msg if isinstance(msg, str) else None,
            e.function,
            conds=e.conds,
        )
    if prep is not None and (e.native in REQUIRE or e.native in REWARD) and _ints(e.args):
        return prep.add(e)
    return prep


def _ints(args: Iterable[Any]) -> bool:
    return all(isinstance(a, int) for a in args)


def _step_facts(
    w: Walked, f: Facts, events: list[sw.Event], prep: Prep | None, reached: bool
) -> None:
    natives = {e.native for e in events}
    # A function no entry point reaches is walked without its context: what it cannot
    # resolve there (the globals its caller sets) is not a problem of the script.
    problems = f.problems if reached else []
    dirty = False  # the prepared quest was made or changed on this step
    for e in events:
        where = f"{w.rel}: {e.function} 0x{e.offset:X} {e}"
        if e.native in MESSAGE_NATIVES and isinstance(e.args[0], str):
            f.keys.add(e.args[0])
            f.say_at(e.args[0], e)
        if e.native == "setQuest":
            for a in e.args[2:]:
                if isinstance(a, str):
                    f.keys.add(a)
                    f.say_at(a, e)
            if not _ints(e.args[:2]):
                problems.append(f"{where}: the quest is not a constant")
            if prep is not None and dirty:
                f.prep(prep, reached)
            prep, dirty = _prep_after(prep, e), True
        elif e.native in REQUIRE or e.native in REWARD:
            if prep is None:
                problems.append(f"{where}: no quest is prepared")
            elif not _ints(e.args):
                problems.append(f"{where}: an argument is not a constant")
            prep, dirty = _prep_after(prep, e), True
        elif e.native == "startQuest":
            if prep is None:
                problems.append(f"{where}: startQuest with no quest prepared")
            else:
                f.prep(prep, reached)
                key = (prep.sig(), e.conds)
                old = f.starts.get(key)
                f.starts[key] = (prep, e.conds, reached or bool(old and old[2]))
        elif e.native == "getReward" and _ints(e.args):
            f.rewards.add(((e.args[0], e.args[1]), reached))
        elif e.native == "makeEquipItem" and _ints(e.args):
            f.crafts.add((e.args[0], e.args[1]))
        elif e.native == "removeQuest" and _ints(e.args):
            f.removes.add((e.args[0], e.args[1]))
        elif e.native == "setClassUp":
            job, n = e.args
            if not isinstance(n, int):
                problems.append(f"{where}: the advancement count is not a constant")
            elif reached:
                j = job if isinstance(job, int) else None
                f.class_ups[(j, n, e.conds)] = (j, n, e.conds)
        elif e.native == "changeMap":
            if not _ints(e.args):
                problems.append(f"{where}: the warp target is not a constant")
                continue
            clear = "clearAll" in natives
            key = (e.function, e.offset, e.args, clear, e.conds)
            old = f.warps.get(key)
            f.warps[key] = (e, clear, reached or bool(old and old[2]))
        elif e.native == "showMarket":
            if not _ints(e.args):
                problems.append(f"{where}: the shop is not a constant")
                continue
            f.shops[(e.args[0], e.conds)] = (e.args[0], e.conds)
        elif e.native in EVENT_NATIVES:
            key = (EVENT_NATIVES[e.native], e.args, e.conds)
            old = f.events.get(key)
            f.events[key] = (key[0], e.args, e.conds, reached or bool(old and old[3]))
        elif e.native == "setNPC" and _ints(e.args):
            f.speakers.add(e.args[0])
        elif e.native == "getItem" and _ints(e.args):
            f.gifts[(e.args, e.conds)] = (e.args, e.conds)
    if prep is not None and dirty:
        f.prep(prep, reached)


# -- conditions as data ---------------------------------------------------------------------


def condition(c: sw.Cond) -> dict[str, Any]:
    """A condition for the KB: the native, its arguments, the comparison and a text."""
    out = c.json()
    t = c.term
    if t.op == "approximate":
        out["approximate"] = True
        out["text"] = "(the walk merged paths here: the conditions may be incomplete)"
    elif t.op == "call" and t.name == "getQuestState" and _ints(t.args):
        k, n = t.args
        out["quest"] = [k, n]
        what = f"{QUEST_KINDS.get(k, k)} quest {n}"
        state = QUEST_STATES.get(c.value)
        if c.op in ("==", "!=") and state:
            out["text"] = f"{what} is {'not ' if c.op == '!=' else ''}{state}"
        else:
            out["text"] = f"{what} {c.op} {c.value}"
    elif t.op == "call" and t.name in GETTERS:
        out["text"] = f"{GETTERS[t.name]} {c.op} {c.value}"
    else:
        out["text"] = str(c)
    return out


def _keep(c: sw.Cond, own: tuple[int, int] | None, answers: bool) -> bool:
    t = c.term
    if not answers and sw._mentions_answer(t):
        return False
    if own is not None and t.op == "call" and t.name in ("getQuestState", "checkRequire"):
        return tuple(t.args) != own
    return True


def conditions(
    sets: Iterable[Iterable[sw.Cond]], own: tuple[int, int] | None = None, answers: bool = False
) -> list[list[dict]]:
    """The condition sets of the paths to one outcome, simplified (:func:`sw.simplify`): one
    list per alternative, ``[[]]`` when the outcome has no condition. Box answers are left out
    unless ``answers``, and so is the state of quest ``own`` (a quest's own state is always
    "not started" where it is offered)."""
    kept = [tuple(c for c in s if _keep(c, own, answers)) for s in sets]
    if not kept:
        return []
    return [[condition(c) for c in alt] for alt in sw.simplify(kept)]


# -- the stage ------------------------------------------------------------------------------


def stage(kb: KB, src: Sources) -> None:
    root = src.app / "script"
    files = {
        f"{p.parent.name}/{p.stem}": p
        for p in sorted(root.glob("*/*.out"), key=lambda p: natural_key(str(p)))
    }
    walked: dict[str, Walked] = {}
    for rel, path in files.items():
        w = walk_file(rel, path)
        walked[rel] = w
        kb.problems.extend(w.errors)
    refs = object_refs(kb)
    for r in refs:
        if all(resolve(files, r.name, c) is None for c in CHAPTERS):
            kb.problems.append(f"map {r.map}: {r.role} script {r.name} has no file")
    maps, runs = reachable(refs, files, walked)
    chapters_of: dict[str, list[int]] = defaultdict(list)
    for c in CHAPTERS:
        for rel in sorted(runs[c]):
            chapters_of[rel].append(c)
    refs_of: dict[str, list[tuple[int, ObjRef]]] = defaultdict(list)
    for c in CHAPTERS:
        for r in refs:
            rel = resolve(files, r.name, c)
            if rel is not None and r.map in maps[c]:
                refs_of[rel].append((c, r))
    named = {r.name for r in refs}
    strings = _strings(src)
    fs = {rel: facts(w) for rel, w in walked.items()}
    for rel in files:
        if chapters_of.get(rel):  # an unused script's slips are its own (script.notes)
            kb.problems.extend(fs[rel].problems)
    ctx = _Ctx(kb, files, walked, fs, chapters_of, refs_of, named, strings, maps, refs)
    for rel in files:
        _script(ctx, rel)
    _quests(ctx)
    for rel in files:
        _warps(ctx, rel)
        _events(ctx, rel)
        if chapters_of.get(rel):
            _dialogue(ctx, rel)
            _shops(ctx, rel)
            _class_ups(ctx, rel)
    _unopened_shops(kb)


@dataclass
class _Ctx:
    kb: KB
    files: dict[str, Path]
    walked: dict[str, Walked]
    facts: dict[str, Facts]
    chapters: dict[str, list[int]]
    refs: dict[str, list[tuple[int, ObjRef]]]
    named: set[str]
    strings: dict[str, dict[str, str]]
    maps: dict[int, set[str]]
    all_refs: list[ObjRef]

    def npcs(self, rel: str, chapter: int | None = None) -> list[tuple[str, str]]:
        """(npc, map) of the objects that run ``rel`` (in ``chapter``), or for a map script
        the NPC it shows as the speaker (``setNPC``)."""
        out = []
        for c, r in self.refs.get(rel, []):
            if chapter is not None and c != chapter:
                continue
            if r.role == "npc" and r.npc is not None:
                out.append((r.npc, r.map))
            elif r.role == "map":
                out += [(str(n), r.map) for n in sorted(self.facts[rel].speakers)]
        return sorted(set(out), key=lambda t: (natural_key(t[0]), natural_key(t[1])))

    def text(self, key: str | None) -> dict[str, str | None]:
        if key is None:
            return {"en": None, "ko": None}
        if not scriptout.is_dialogue_key(key):
            return {"en": key, "ko": key}  # literal text, shown as written
        return {lang: self.strings[lang].get(key) for lang in ("en", "ko")}


def _strings(src: Sources) -> dict[str, dict[str, str]]:
    return {
        lang: scriptout.load_strings(src.app / f"{lang}.lproj" / "Scripts.strings")
        for lang in ("en", "ko")
    }


def _clean(t: dict[str, str | None], fn=clean_text) -> dict[str, str | None]:
    return {k: (fn(v) if isinstance(v, str) else None) for k, v in t.items()}


# -- script ---------------------------------------------------------------------------------


def _script(ctx: _Ctx, rel: str) -> None:
    w = ctx.walked[rel]
    name = rel.split("/", 1)[1]
    chapters = ctx.chapters.get(rel, [])
    roles = sorted({r.role for _, r in ctx.refs.get(rel, [])})
    why = None
    if not chapters:
        why = "no map object names it"
        if name in ctx.named:
            reached = [
                c
                for c in CHAPTERS
                if any(r.name == name and r.map in ctx.maps[c] for r in ctx.all_refs)
            ]
            others = sorted({str(resolve(ctx.files, name, c)) for c in reached})
            why = (
                f"in chapter{'s' if len(reached) > 1 else ''} {', '.join(map(str, reached))}, "
                f"where a map that names it is "
                f"reachable, the game runs {', '.join(others)} instead"
                if reached
                else "no map that names it is reachable in any chapter"
            )
    unused_quests = []
    if not chapters:
        for prep, _ in ctx.facts[rel].preps.values():
            row = {"kind": prep.kind, "number": prep.number, "title_key": prep.title}
            if row not in unused_quests:
                unused_quests.append(row)
    paths = {e: sum(1 for _ in w.steps(e)) for e in w.trees}
    ctx.kb.add(
        "script",
        w.ident,
        {"en": rel, "ko": rel},
        {
            "file": (f"script/{rel}.out", "code"),
            "dir": (rel.split("/", 1)[0], "code"),
            "base_name": (name, "code"),
            # Reachability follows every warp whatever its conditions (an upper bound); only
            # "no map object names it" is certain.
            "used": (bool(chapters), "code" if why == "no map object names it" else "probable"),
            "unused_reason": (why, "code"),
            "chapters": (chapters, "code"),
            "roles": (roles, "code"),
            "functions": ([w.script.func_name(None), *sorted(w.script.names.values())], "code"),
            "unreached_functions": (sorted(w.unreached), "code"),
            "paths": (paths, "code"),
            "approximate_entries": (w.approximate, "code"),
            "unused_quests": (unused_quests, "code"),
            "notes": ([] if chapters else ctx.facts[rel].problems, "code"),
            # An unused script has no dialogue entity: its text is kept here, in the order
            # the script says it (#225).
            "text": (
                []
                if chapters
                else [{"key": k, **_clean(ctx.text(k))} for k in _said_order(w, ctx.facts[rel])],
                "code",
            ),
        },
    )
    seen = set()
    for _, r in ctx.refs.get(rel, []):
        key = (r.map, r.role, r.npc, r.portal)
        if key in seen:
            continue
        seen.add(key)
        ctx.kb.link(
            "script", w.ident, "maps", "map", r.map, role=r.role, npc=r.npc, portal=r.portal
        )


def _said_order(w: Walked, f: Facts) -> list[str]:
    """``f.keys`` in the order the script says them: main first, then the functions by name,
    each by offset."""
    order = [w.script.func_name(None), *sorted(w.script.names.values())]
    rank = {name: i for i, name in enumerate(order)}

    def key(k: str) -> tuple:
        fn, off = f.key_at.get(k, ("", 1 << 30))
        return (rank.get(fn, len(rank)), off, natural_key(k))

    return sorted(f.keys, key=key)


# -- quests, recipes, trades ----------------------------------------------------------------


def quest_id(chapter: int, kind: int, number: int) -> str:
    """``<chapter>-<main|sub>-<number>``: ``1-main-7``, ``2-sub-21``."""
    return f"{chapter}-{QUEST_KINDS.get(kind, str(kind))}-{number}"


def _transient(f: Facts, key: tuple[int, int]) -> str | None:
    """``recipe`` or ``trade`` for a quest a file prepares, completes and drops in one
    conversation (``removeQuest``), else None."""
    if key not in f.removes:
        return None
    return "recipe" if key in f.crafts else "trade"


def _rows(prep: Prep) -> tuple[list[dict], list[dict]]:
    req, rew = [], []
    for row in prep.requirements:
        if row[0] == "item":
            req.append({"type": "item", "item": item_id(row[1], row[2]), "count": row[3]})
        elif row[0] == "mob":
            req.append({"type": "mob", "monster": str(row[1]), "count": row[2]})
        else:
            req.append({"type": "meso", "amount": row[1]})
    for row in prep.rewards:
        if row[0] == "item":
            rew.append({"type": "item", "item": item_id(row[1], row[2]), "count": row[3]})
        else:
            rew.append({"type": row[0], "amount": row[1]})
    return req, rew


def _quests(ctx: _Ctx) -> None:
    by_id: dict[str, dict[str, Any]] = {}
    recipes: dict[str, dict] = {}
    trades: dict[str, dict] = {}
    for rel in ctx.files:
        chapters = ctx.chapters.get(rel, [])
        f = ctx.facts[rel]
        for prep, reached in f.preps.values():
            kind = _transient(f, prep.key)
            for c in chapters:
                if kind is not None:
                    _transaction(ctx, rel, c, prep, kind, recipes if kind == "recipe" else trades)
                    continue
                q = by_id.setdefault(
                    quest_id(c, *prep.key),
                    {"chapter": c, "key": prep.key, "offers": [], "starts": [], "files": set()},
                )
                q["offers"].append((rel, prep, reached))
                q["files"].add(rel)
        for prep, conds, reached in f.starts.values():
            for c in chapters if _transient(f, prep.key) is None else []:
                q = by_id.get(quest_id(c, *prep.key))
                if q is not None:
                    q["starts"].append((rel, prep, conds, reached))
    completers: dict[str, set[str]] = defaultdict(set)
    for rel in ctx.files:
        f = ctx.facts[rel]
        for key, reached in f.rewards:
            if reached and _transient(f, key) is None:
                for c in ctx.chapters.get(rel, []):
                    completers[quest_id(c, *key)].add(rel)
    for qid in sorted(by_id, key=natural_key):
        _quest(ctx, qid, by_id[qid], sorted(completers.get(qid, set()), key=natural_key))
    for qid in sorted(by_id, key=natural_key):
        _quest_links(ctx, qid)
    for rid in sorted(recipes, key=natural_key):
        _recipe(ctx, rid, recipes[rid])
    for tid in sorted(trades, key=natural_key):
        _trade(ctx, tid, trades[tid])


def _quest(ctx: _Ctx, qid: str, q: dict, completers: list[str]) -> None:
    kb = ctx.kb
    kind, number = q["key"]
    c = q["chapter"]
    offers = sorted(q["offers"], key=lambda o: (not o[2], natural_key(o[0]), str(o[1].sig())))
    first = offers[0][1]
    title = _clean(ctx.text(first.title), clean_name)
    name, conf = title, "code"
    if not (title["en"] and title["ko"]):
        name = {"en": f"Quest {qid}", "ko": f"퀘스트 {qid}"}
        conf = "data"
    variants: list[dict] = []
    for _, prep, _ in offers:
        req, rew = _rows(prep)
        v = {"requirements": req, "rewards": rew}
        if v not in variants:
            variants.append(v)
    reached_starts = [s for s in q["starts"] if s[3]]
    prereq = conditions((conds for _, _, conds, _ in reached_starts), own=q["key"])
    files = sorted(q["files"], key=natural_key)
    kb.add(
        "quest",
        qid,
        name,
        {
            "chapter": (c, "code"),
            "quest_kind": (QUEST_KINDS.get(kind, str(kind)), "code"),
            "kind_no": (kind, "code"),
            "number": (number, "code"),
            "main_story": (kind == 0, "code"),
            "title_key": (first.title, "code"),
            "description_key": (first.msg, "code"),
            "description": (_clean(ctx.text(first.msg)), "code"),
            "scripts": ([script_id(r) for r in files], "code"),
            "functions": (sorted({p.function for _, p, _ in offers}), "code"),
            "requirements": (variants[0]["requirements"], "code"),
            "rewards": (variants[0]["rewards"], "code"),
            "variants": (variants if len(variants) > 1 else [], "code"),
            "prerequisites": (prereq, "code"),
            "started": (bool(reached_starts), "code"),
            "reachable": (any(r for _, _, r in offers), "code"),
            "completion_scripts": ([script_id(r) for r in completers], "code"),
        },
        name_confidence=conf,
    )
    for rel in files:
        kb.link("quest", qid, "script", "script", script_id(rel))
        for npc, m in ctx.npcs(rel, c):
            kb.link("quest", qid, "given_by", "npc", npc, map=m, script=script_id(rel))
    for rel in completers:
        for npc, m in ctx.npcs(rel, c):
            kb.link("quest", qid, "completed_by", "npc", npc, map=m, script=script_id(rel))
    for row in variants[0]["requirements"]:
        if row["type"] == "item":
            kb.link("quest", qid, "needs_items", "item", row["item"], count=row["count"])
        elif row["type"] == "mob":
            kb.link("quest", qid, "needs_kills", "monster", row["monster"], count=row["count"])
    for row in variants[0]["rewards"]:
        if row["type"] == "item":
            kb.link("quest", qid, "reward_items", "item", row["item"], count=row["count"])


def _quest_links(ctx: _Ctx, qid: str) -> None:
    """``requires``: the other quests whose state a prerequisite reads (the inverse,
    ``unlocks``, lists the follow-ups)."""
    q = ctx.kb.get("quest", qid)
    seen = set()
    for alt in q["prerequisites"]:
        for cond in alt:
            if "quest" not in cond:
                continue
            target = quest_id(q["chapter"], *cond["quest"])
            key = (target, cond["op"], cond["value"])
            if target == qid or key in seen or ctx.kb.get("quest", target) is None:
                continue
            seen.add(key)
            ctx.kb.link(
                "quest", qid, "requires", "quest", target, op=cond["op"], value=cond["value"]
            )


def _transaction(ctx: _Ctx, rel: str, c: int, prep: Prep, kind: str, out: dict) -> None:
    req, rew = _rows(prep)
    items = [r for r in rew if r["type"] == "item"]
    costs = [r for r in req if r["type"] == "item"]
    if not items:
        ctx.kb.problems.append(f"{rel}: {kind} {prep.key} in {prep.function} gives no item")
        return
    if kind == "recipe":
        base = f"{c}-{items[0]['item']}"
    else:
        base = f"{c}-{costs[0]['item'] if costs else 'meso'}-{items[0]['item']}"
    entry = {"chapter": c, "rel": rel, "prep": prep, "requirements": req, "rewards": rew}
    ident, n = base, 1
    while ident in out:
        old = out[ident]
        if (old["rel"], old["requirements"], old["rewards"]) == (rel, req, rew):
            return
        n += 1
        ident = f"{base}-{n}"
    out[ident] = entry


def _recipe(ctx: _Ctx, rid: str, r: dict) -> None:
    kb = ctx.kb
    result = next(x for x in r["rewards"] if x["type"] == "item")
    item = kb.get("item", result["item"])
    name = dict(item["name"]) if item else {"en": f"Recipe {rid}", "ko": f"제작 {rid}"}
    kb.add(
        "recipe",
        rid,
        name,
        {
            "chapter": (r["chapter"], "code"),
            "quest": (list(r["prep"].key), "code"),
            "title_key": (r["prep"].title, "code"),
            "title": (_clean(ctx.text(r["prep"].title), clean_name), "code"),
            "function": (r["prep"].function, "code"),
            "script": (script_id(r["rel"]), "code"),
            "material_items": ([x for x in r["requirements"] if x["type"] == "item"], "code"),
            "meso": (sum(x["amount"] for x in r["requirements"] if x["type"] == "meso"), "code"),
            "result_item": (result["item"], "code"),
            "result_count": (result["count"], "code"),
            "other_rewards": ([x for x in r["rewards"] if x is not result], "code"),
        },
    )
    kb.link("recipe", rid, "result", "item", result["item"], count=result["count"])
    for x in r["requirements"]:
        if x["type"] == "item":
            kb.link("recipe", rid, "materials", "item", x["item"], count=x["count"])
    for npc, m in ctx.npcs(r["rel"], r["chapter"]):
        kb.link("recipe", rid, "crafted_by", "npc", npc, map=m)


def _trade(ctx: _Ctx, tid: str, t: dict) -> None:
    kb = ctx.kb
    result = next(x for x in t["rewards"] if x["type"] == "item")
    item = kb.get("item", result["item"])
    name = dict(item["name"]) if item else {"en": f"Trade {tid}", "ko": f"교환 {tid}"}
    kb.add(
        "trade",
        tid,
        name,
        {
            "chapter": (t["chapter"], "code"),
            "quest": (list(t["prep"].key), "code"),
            "title_key": (t["prep"].title, "code"),
            "title": (_clean(ctx.text(t["prep"].title), clean_name), "code"),
            "function": (t["prep"].function, "code"),
            "script": (script_id(t["rel"]), "code"),
            "cost_items": ([x for x in t["requirements"] if x["type"] == "item"], "code"),
            "cost_meso": (
                sum(x["amount"] for x in t["requirements"] if x["type"] == "meso"),
                "code",
            ),
            "result_items": ([x for x in t["rewards"] if x["type"] == "item"], "code"),
            "result_other": ([x for x in t["rewards"] if x["type"] != "item"], "code"),
        },
    )
    for x in t["rewards"]:
        if x["type"] == "item":
            kb.link("trade", tid, "result", "item", x["item"], count=x["count"])
    for x in t["requirements"]:
        if x["type"] == "item":
            kb.link("trade", tid, "cost", "item", x["item"], count=x["count"])
    for npc, m in ctx.npcs(t["rel"], t["chapter"]):
        kb.link("trade", tid, "offered_by", "npc", npc, map=m)


# -- job advancement, shops -----------------------------------------------------------------


def _class_ups(ctx: _Ctx, rel: str) -> None:
    groups: dict[tuple, list] = defaultdict(list)
    for job, n, conds in ctx.facts[rel].class_ups.values():
        groups[(job, n)].append(conds)
    for (job, n), alts in sorted(
        groups.items(), key=lambda kv: (kv[0][1], kv[0][0] is None, kv[0][0] or 0)
    ):
        for c in ctx.chapters[rel]:
            ident = f"{c}-{n}-{job if job is not None else 'same'}"
            job_name = JOB_NAMES.get(job) if job is not None else None
            en = f"Advancement {n}: {job_name}" if job_name else f"Advancement {n}"
            ctx.kb.add(
                "job_advancement",
                ident,
                {"en": en, "ko": f"{n}차 전직"},
                {
                    "chapter": (c, "code"),
                    "advancement": (n, "code"),
                    "job": (job, "code"),
                    "job_name": (job_name, "probable"),
                    "keeps_job": (job is None, "code"),
                    "script": (script_id(rel), "code"),
                    "conditions": (conditions(alts), "code"),
                },
                name_confidence="data",
            )
            for npc, m in ctx.npcs(rel, c):
                ctx.kb.link("job_advancement", ident, "given_by", "npc", npc, map=m)


def _shops(ctx: _Ctx, rel: str) -> None:
    """``npc.opens_shop``: one link per NPC, shop, map and script, with the chapters in which
    the game runs that script there."""
    shops = sorted({n for n, _ in ctx.facts[rel].shops.values()})
    chapters: dict[tuple, list[int]] = defaultdict(list)
    for c in ctx.chapters[rel]:
        for npc, m in ctx.npcs(rel, c):
            for shop in shops:
                chapters[(npc, shop, m)].append(c)
    for (npc, shop, m), cs in chapters.items():
        ctx.kb.link(
            "npc", npc, "opens_shop", "shop", str(shop), map=m, script=script_id(rel), chapters=cs
        )


# The shop screen (0x4628D) has one caller, showMarket (0x998D1, the call at 0x99900).
NO_OPENER = "no script opens it: showMarket is the only way to the shop screen"


def _unopened_shops(kb: KB) -> None:
    """``shop.unopened_reason``: why a shop no NPC opens is never seen (null for the others)."""
    for ident, shop in kb.entities.get("shop", {}).items():
        kb.set(
            "shop", ident, "unopened_reason", None if shop.get("opened_by") else NO_OPENER, "code"
        )


# -- warps and events -----------------------------------------------------------------------


def _sources(ctx: _Ctx, rel: str) -> list[ObjRef]:
    out: list[ObjRef] = []
    for _, r in ctx.refs.get(rel, []):
        if r not in out:
            out.append(r)
    return out


def _warps(ctx: _Ctx, rel: str) -> None:
    kb = ctx.kb
    w = ctx.walked[rel]
    order = {name: k for k, name in enumerate(w.script.names.values())}
    groups: dict[tuple, dict] = {}
    for e, clear, reached in ctx.facts[rel].warps.values():
        g = groups.setdefault(
            (e.function, e.offset, e.args),
            {"conds": [], "clear": False, "reached": False, "event": e},
        )
        g["conds"].append(e.conds)
        g["clear"] |= clear
        g["reached"] |= reached
    keys = sorted(groups, key=lambda k: (k[0] != "main", order.get(k[0], -1), k[1], k[2]))
    chapters = ctx.chapters.get(rel, [])
    for n, key in enumerate(keys, 1):
        g = groups[key]
        a, m, p = key[2]
        ident = f"{w.ident}-{n}"
        target = f"{a}-{m}"
        sources = _sources(ctx, rel)
        conds = conditions(g["conds"], answers=True)
        kb.add(
            "warp",
            ident,
            {"en": f"{rel} to {target}", "ko": f"{rel} → {target}"},
            {
                "used": (bool(chapters) and g["reached"], "code"),
                "chapters": (chapters, "code"),
                "function": (key[0], "code"),
                "offset": (key[1], "code"),
                "target_map": (target, "code"),
                "target_portal": (p, "code"),
                "conditions": (conds, "code"),
                "conditional": (any(conds), "code"),
                "chapter_end": (g["clear"], "code"),
                "from": (
                    [
                        {"map": r.map, "role": r.role, "portal": r.portal, "npc": r.npc}
                        for r in sources
                    ],
                    "code",
                ),
            },
            name_confidence="data",
        )
        kb.link("warp", ident, "script", "script", w.ident)
        if kb.link("warp", ident, "target", "map", target, portal=p):
            portals = {x["number"] for x in kb.get("map", target)["portals"]}
            if p not in portals and g["reached"] and chapters:
                kb.get("warp", ident)["confidence"]["target_portal"] = "probable"
        seen = set()
        for r in sources:
            if r.map not in seen:
                seen.add(r.map)
                kb.link("warp", ident, "source", "map", r.map, role=r.role, portal=r.portal)
            if r.role != "portal" and g["reached"] and kb.get("map", target) is not None:
                # NPC and map-script warps complete the portal graph of the world stage.
                link = dict(portal=None, target_portal=p, script=rel, gated=any(conds))
                here = kb.get("map", r.map)
                if not any(
                    x == {"kind": "map", "id": target, **link} for x in here.get("exits", [])
                ):
                    kb.link("map", r.map, "exits", "map", target, **link)


def _events(ctx: _Ctx, rel: str) -> None:
    kb = ctx.kb
    w = ctx.walked[rel]
    groups: dict[tuple, dict] = {}
    for kind, args, conds, reached in ctx.facts[rel].events.values():
        g = groups.setdefault((kind, args), {"conds": [], "reached": False})
        g["conds"].append(conds)
        g["reached"] |= reached
    chapters = ctx.chapters.get(rel, [])
    for (kind, args), g in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        ident = f"{w.ident}-{kind}" + "".join(f"-{a}" for a in args)
        sources = _sources(ctx, rel)
        label = {
            "boss": "boss",
            "scene": "scene",
            "carnival": "Monster Carnival",
            "chapter_end": "end of chapter",
        }[kind]
        ko = {
            "boss": "보스",
            "scene": "장면",
            "carnival": "몬스터 카니발",
            "chapter_end": "챕터 끝",
        }[kind]
        suffix = f" {args[0]}" if args else ""
        kb.add(
            "event",
            ident,
            {"en": f"{rel}: {label}{suffix}", "ko": f"{rel}: {ko}{suffix}"},
            {
                "type": (kind, "code"),
                "args": (list(args), "code"),
                "used": (bool(chapters) and g["reached"], "code"),
                "chapters": (chapters, "code"),
                "conditions": (conditions(g["conds"], answers=True), "code"),
                "trigger": (sorted({r.role for r in sources}), "code"),
            },
            name_confidence="data",
        )
        kb.link("event", ident, "script", "script", w.ident)
        for m in sorted({r.map for r in sources}, key=natural_key):
            kb.link("event", ident, "maps", "map", m)


# -- dialogue -------------------------------------------------------------------------------


def brief(c: sw.Cond) -> dict[str, Any]:
    """A condition in a dialogue tree: the term as text, the comparison and the value."""
    return {"term": str(c.term), "op": c.op, "value": c.value}


def _do(events: Iterable[sw.Event]) -> list[dict[str, Any]]:
    do: list[dict[str, Any]] = []
    for e in events:
        if e.native in BOX_NATIVES:
            continue
        a = e.args
        if e.native == "regMsgNPC":
            do.append({"npc": sw.as_json(a[0])})
        elif e.native == "regMsgHero":
            do.append({"hero": sw.as_json(a[0]), "face": sw.as_json(a[1])})
        elif e.native == "regSelList":
            do.append({"choice": sw.as_json(a[0]), "value": sw.as_json(a[1])})
        else:
            do.append({"call": e.native, "args": [sw.as_json(x) for x in a]})
    return do


def _grouped(
    rows: Iterable[tuple[dict[str, Any], tuple[sw.Cond, ...]]],
) -> list[dict[str, Any]]:
    """Rows that do the same thing become one, its ``if`` the simplified alternatives of
    their conditions (``[[]]``: always)."""
    groups: dict[str, tuple[dict[str, Any], list]] = {}
    for body, conds in rows:
        key = json.dumps(body, sort_keys=True, ensure_ascii=False)
        groups.setdefault(key, (body, []))[1].append(conds)
    out = []
    for body, sets in groups.values():
        out.append({"if": [[brief(c) for c in alt] for alt in sw.simplify(sets)], **body})
    return out


class Nodes:
    """The nodes of a conversation, each distinct node once: a callback reached from many
    paths with the same paths of its own is stored once and named by its id (``n1``, ...)."""

    def __init__(self) -> None:
        self.nodes: dict[str, dict[str, Any]] = {}
        self._ids: dict[str, str] = {}

    def add(self, node: sw.Node) -> str:
        """The id of ``node``: per path of the node's function, its conditions (``if``), what
        it says and does (``do``), the box it opens and the node after the answer."""
        rows = []
        for s in node.steps:
            body = {
                "function": s.function,
                "do": _do(s.events),
                "box": sw.as_json(s.show[0]) if s.show else None,
                "next": self.add(s.next) if s.next is not None else None,
                "loop": s.loop,
            }
            rows.append((body, s.conds))
        data = {
            "function": node.function,
            "answer": node.answer,
            "answers": list(node.answers) if node.answers is not None else None,
            "paths": _grouped(rows),
        }
        key = json.dumps(data, sort_keys=True, ensure_ascii=False)
        if key not in self._ids:
            ident = f"n{len(self._ids) + 1}"
            self._ids[key] = ident
            self.nodes[ident] = data
        return self._ids[key]


def state_json(node: sw.Node) -> list[dict[str, Any]]:
    """The NPC-state pass: the quest icon (``notify``) and visibility (``hide``) it sets, with
    the conditions for each."""
    rows = []
    for s in node.steps:
        body: dict[str, Any] = {"notify": None, "hide": None}
        for e in s.events:
            if e.native == "setNpcNotify":
                body["notify"] = sw.as_json(e.args[0])
            elif e.native == "setNpcHide":
                body["hide"] = sw.as_json(e.args[0])
        rows.append((body, s.conds))
    return _grouped(rows)


def _dialogue(ctx: _Ctx, rel: str) -> None:
    kb = ctx.kb
    w = ctx.walked[rel]
    sources = [r for r in _sources(ctx, rel) if r.role in ("npc", "map")]
    if not sources:
        return
    npcs = ctx.npcs(rel)
    names_ = [kb.get("npc", n)["name"] for n, _ in npcs if kb.get("npc", n)]
    where_en = rel.split("/", 1)[0].replace("chapter", "chapter ")
    where_ko = "공통" if rel.startswith("common") else rel[7:] + "장"
    who_en = " / ".join(dict.fromkeys(n["en"] for n in names_)) or rel.split("/", 1)[1]
    who_ko = " / ".join(dict.fromkeys(n["ko"] for n in names_)) or rel.split("/", 1)[1]
    keys = sorted(ctx.facts[rel].keys)
    text = {k: _clean(ctx.text(k)) for k in keys}
    missing = [k for k, t in text.items() if not (t["en"] and t["ko"])]
    nodes = Nodes()
    root = nodes.add(w.trees["main"]) if "main" in w.trees else None
    kb.add(
        "dialogue",
        w.ident,
        {"en": f"{who_en} ({where_en})", "ko": f"{who_ko} ({where_ko})"},
        {
            "chapters": (ctx.chapters[rel], "code"),
            "roles": (sorted({r.role for r in sources}), "code"),
            "tree": (root, "code"),
            "nodes": (nodes.nodes, "code"),
            "state": (state_json(w.trees["state"]) if "state" in w.trees else [], "code"),
            "text": (text, "code"),
            "missing_text": (missing, "code"),
        },
        name_confidence="data",
    )
    kb.link("dialogue", w.ident, "script", "script", w.ident)
    for npc, m in npcs:
        kb.link("dialogue", w.ident, "npcs", "npc", npc, map=m)


def notes(entities: dict[str, dict[str, Any]]) -> list[str]:
    """Build-log notes: unused scripts, dialogue keys without both languages, quests that no
    reachable path starts."""
    out = []
    for ident in sorted(entities.get("script", {}), key=natural_key):
        e = entities["script"][ident]
        if not e["used"]:
            out.append(f"script {ident} is unused: {e['unused_reason']}")
    for ident in sorted(entities.get("dialogue", {}), key=natural_key):
        for k in entities["dialogue"][ident]["missing_text"]:
            out.append(f"dialogue {ident}: key {k} lacks a language")
    for ident in sorted(entities.get("quest", {}), key=natural_key):
        q = entities["quest"][ident]
        if not q["started"]:
            out.append(f"quest {ident} is never started by a reachable path")
    return out
