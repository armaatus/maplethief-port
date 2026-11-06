"""The walkthrough section of the compendium (#227): the story from a new character to the
ending, chapter by chapter, in parts.

Pages (all under ``walkthrough/``):

- ``index.html``: the chapters and parts with the levels to aim for, and the main story as
  one checklist in an order that satisfies every quest's prerequisites;
- ``chapter-<n>.html`` for chapters 1 to 3 (chapter 1 starts with the tutorial): one section
  per part, each with its goal, route (map to map, generated from the maps' exits), the
  quests of the main story and the optional quests that open there, the level to aim for,
  the shops on the way, the boss (if any) and what carries over to the next part; then the
  chapter's crafting and trades, the point of no return and the story (a spoiler block);
- ``endings.html``: every ending the scripts and scenes have: each chapter end
  (``clearAll``) and the game's ending after the last boss;
- ``checklist.html``: everything that can be missed (quests, bosses, recipes, trades and
  items only a quest gives) with the last point it can be done.

The parts (:data:`PARTS`) name the main-story quests of each part in the order the story
takes them; :func:`story_order` turns them into an order that satisfies every prerequisite
(``quest.requires``) whatever the knowledge base holds, and adds the side quests the story
cannot do without (a chapter end's condition, a prerequisite of a main quest, the only source
of an item a main quest needs). Everything else on the pages is generated from the knowledge
base, or is our own prose in ``docs/compendium/pages/walkthrough/``.

The recommended level of a part is the compendium's estimate, shown as two numbers: the
level the story's own EXP gives by the end of the part (quest rewards, the kills it asks for
and the expected kills for the items it asks for, at the drop's real odds) and the level whose
base accuracy (the level table's ACC) reaches the level of the strongest monster the part
sends the hero to, so that no hit is evaded ([accuracy](mechanics/accuracy.html)).
"""

from __future__ import annotations

import heapq
from collections import deque
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.sections import monsters as mons
from mtport.compendium.sections import npcs as npcs_s
from mtport.compendium.sections import quests as qs
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of

SECTION_ID = "walkthrough"
REPO = "https://github.com/armaatus/maplethief-port/blob/main/docs/"
SCRIPT_VM = REPO + "re/script-vm.md#natives"
SCENES_DOC = REPO + "re/qol.md#which-steps-change-state"
MECHANICS_DOC = REPO + "re/mechanics.md#drops"
YETI_ISSUE = "https://github.com/armaatus/maplethief-port/issues/218"
REVIEW_ISSUE = "https://github.com/armaatus/maplethief-port/issues/228"

# The scenes the last boss's defeat runs (CMob::Die 0x73349 sets the queued scene to 1 for
# mob 69; docs/re/qol.md's scene list): number, builder, steps, speech boxes, and the map the
# scene's end command leads to (scene 1: scene 2 on map 2.11; scene 2: a warp to 2.0).
FINAL_SCENES: tuple[tuple[int, str, int, int, str], ...] = (
    (1, "makeAniEndingZacum", 202, 11, "2-11"),
    (2, "makeAniEndingHenesis", 430, 5, "2-0"),
)
HAZARD_AREA = "10"  # the Abandoned Mine: 300 damage every 2.5 seconds without the suit
HAZARD_SUIT = "equip:73"
RARE = 5.0  # percent: a drop rarer than this is not counted as a source
FIRST_MAP = "2-8"  # the intro scene ends with a warp to 2.8 (docs/re/qol.md, scene 0)


@dataclass(frozen=True)
class Part:
    """One part of a chapter: its anchor, title (our words) and the main-story quests it
    covers, in the order the story takes them."""

    chapter: int
    slug: str
    title: str
    quests: tuple[str, ...]
    # (quest, other): finish the quest only after the other one (a gate on its turn-in map)
    finish_after: tuple[tuple[str, str], ...] = ()


PARTS: tuple[Part, ...] = (
    Part(
        1,
        "tutorial",
        "A new character and the tutorial",
        ("0-main-0", "0-main-1", "0-main-2", "0-main-3", "0-main-4", "0-main-5", "0-main-6"),
    ),
    Part(1, "henesys", "Henesys", ("1-main-7", "1-main-8")),
    Part(
        1, "kerning-city", "Kerning City and the first job advancement", ("1-main-21", "1-main-9")
    ),
    Part(1, "perion", "Perion", ("1-main-10", "1-main-11")),
    Part(1, "bruce", "Back to Henesys", ("1-main-12", "1-main-13", "1-main-14")),
    Part(
        1,
        "roy",
        "Roy's errands and the guard",
        ("1-main-15", "1-main-16", "1-sub-7", "1-sub-8"),
        (("1-main-16", "1-sub-7"), ("1-sub-7", "1-sub-8")),
    ),
    Part(1, "ant-tunnel", "The Ant Tunnel", ("1-main-17", "1-main-18", "1-main-19")),
    Part(1, "to-orbis", "Passage to Orbis", ("1-main-20", "1-sub-11")),
    Part(2, "orbis", "Orbis", ("2-main-0", "2-main-1", "2-main-2")),
    Part(2, "werewolf", "The garden in ruins", ("2-sub-0", "2-sub-1", "2-sub-37", "2-main-3")),
    Part(
        2,
        "medicine",
        "The medicine",
        ("2-main-4", "2-main-5", "2-main-6", "2-sub-2"),
        (("2-main-6", "2-sub-2"),),
    ),
    Part(2, "el-nath", "El Nath", ("2-main-7", "2-main-71", "2-sub-35", "2-sub-36", "2-main-72")),
    Part(2, "yeti", "The valley of the snowmen", ("2-main-8", "2-main-9")),
    Part(3, "magatia", "Magatia", ("3-main-0", "3-sub-0", "3-main-1", "3-main-2")),
    Part(3, "alcadno", "The Alcadno", ("3-sub-58", "3-sub-15", "3-sub-2", "3-sub-4")),
    Part(3, "homunscullo", "Under the library", ("3-main-3", "3-main-4")),
    Part(3, "alcadno-technology", "Back to El Nath", ("3-main-5", "3-main-6")),
    Part(3, "evil-lord", "The altar in the mine", ("3-main-7", "3-main-8")),
)
CHAPTERS = (1, 2, 3)
PREFIX_CHAPTER = {0: 1}  # quests of the tutorial (chapter 0) are walked on chapter 1's page


# --- small helpers ------------------------------------------------------------------------------


def _refs(v: Any) -> list[Mapping[str, Any]]:
    if isinstance(v, Mapping) and "id" in v:
        return [v]
    return [r for r in v or [] if isinstance(r, Mapping) and "id" in r]


def _num(v: Any) -> int:
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 0


def _p(markup: str) -> str:
    return f"<p>{markup}</p>\n"


def mine_ref(ctx: Context) -> str:
    """The hazard area, by the name its return scroll gives it (the area table calls it after
    its town)."""
    return ctx.ref("area", HAZARD_AREA, f"the Abandoned Mine (area {HAZARD_AREA})")


def chapter_path(ch: int) -> str:
    return f"{SECTION_ID}/chapter-{ch}.html"


ENDINGS = f"{SECTION_ID}/endings.html"
CHECKLIST = f"{SECTION_ID}/checklist.html"
INDEX = f"{SECTION_ID}/index.html"


def part_url(ctx: Context, p: Part) -> str:
    return ctx.url(f"{chapter_path(p.chapter)}#{p.slug}")


def part_link(ctx: Context, p: Part, number: bool = True) -> str:
    n = f"Part {part_number(p)}: " if number else ""
    return f'<a href="{part_url(ctx, p)}">{t.esc(n + p.title)}</a>'


def last_chapter(w: Walk) -> int:
    """The chapter of the walkthrough's last part (3 in version 1.5)."""
    return max((p.chapter for p in w.parts), default=max(CHAPTERS))


def part_number(p: Part) -> int:
    return [x for x in PARTS if x.chapter == p.chapter].index(p) + 1


def quest_chapter(q: Entity) -> int:
    return _num(q.get("chapter"))


def page_chapter(q: Entity) -> int:
    ch = quest_chapter(q)
    return PREFIX_CHAPTER.get(ch, ch)


def allowed_states(r: Mapping[str, Any]) -> frozenset[int]:
    """The states (0 not started, 1 in progress, 2 done) a ``requires`` relation allows."""
    op, v = str(r.get("op", "==")), _num(r.get("value"))
    ops = {
        "==": lambda x: x == v,
        "!=": lambda x: x != v,
        ">=": lambda x: x >= v,
        ">": lambda x: x > v,
        "<=": lambda x: x <= v,
        "<": lambda x: x < v,
    }
    f = ops.get(op, ops["=="])
    return frozenset(x for x in (0, 1, 2) if f(x))


def needs_before(r: Mapping[str, Any]) -> bool:
    """Whether a prerequisite must have been accepted before (its state cannot be 0)."""
    return 0 not in allowed_states(r)


def necessary_quests(
    ctx: Context, alternatives: Any, chapters: list[int]
) -> dict[str, frozenset[int]]:
    """The quest states that hold on every alternative of a condition, by quest id."""
    out: dict[str, frozenset[int]] = {}
    for term, vals in qs._talk.necessary(alternatives or []).items():
        name, args = qs._talk.parse_term(term)
        if name == "getQuestState" and len(args) == 2:
            qid = qs.quest_id(ctx, int(args[0]), int(args[1]), chapters)
            if qid is not None:
                out[qid] = frozenset(vals)
    return out


def prereqs(ctx: Context, q: Entity) -> list[str]:
    """The quests ``q`` needs accepted (or done) before it is offered: its ``requires``
    relations whose condition holds on every way to start it and excludes "not started"."""
    nec = necessary_quests(ctx, q.get("prerequisites"), [quest_chapter(q)])
    out = []
    for r in _refs(q.get("requires")):
        rid = str(r["id"])
        if rid == str(q["id"]) or rid in out or ctx.kb.get("quest", rid) is None:
            continue
        if not needs_before(r):
            continue
        if q.get("prerequisites") and (rid not in nec or 0 in nec[rid]):
            continue  # only some ways to start it need that quest
        out.append(rid)
    return out


def _event_quests(ctx: Context, ev: Entity) -> list[str]:
    """The quests a chapter end's condition needs accepted or done."""
    out = []
    chapters = [int(c) for c in ev.get("chapters") or [] if isinstance(c, int)]
    for alt in (ev.get("conditions") or [])[:1]:
        for c in alt:
            if c.get("native") != "getQuestState" or len(c.get("args") or []) != 2:
                continue
            if not needs_before(c):
                continue
            k, n = c["args"]
            qid = qs.quest_id(ctx, int(k), int(n), chapters)
            if qid and qid not in out:
                out.append(qid)
    return out


def chapter_ends(ctx: Context) -> list[Entity]:
    evs = [e for e in ctx.kb.entities("event") if e.get("type") == "chapter_end"]
    return sorted(evs, key=lambda e: (min(e.get("chapters") or [99]), str(e["id"])))


def chapter_end_of(ctx: Context, ch: int) -> Entity | None:
    """The used chapter end of quest chapter ``ch`` (the tutorial is chapter 0)."""
    for ev in chapter_ends(ctx):
        if ch in (ev.get("chapters") or []) and ev.get("used", True):
            return ev
    return None


def _only_quest_sources(ctx: Context, iid: str) -> list[str]:
    """The quests rewarding an item that no monster drops (at a usable rate), no shop sells
    and no recipe or trade makes: its only sources."""
    it = ctx.kb.get("item", iid)
    if it is None:
        return []
    if any(float(r.get("percent") or 0) >= RARE for r in _refs(it.get("dropped_by"))):
        return []
    if _refs(it.get("sold_by")) or _refs(it.get("crafted_by")) or _refs(it.get("traded_for_in")):
        return []
    return [str(r["id"]) for r in _refs(it.get("rewarded_by_quests"))]


def story_quests(ctx: Context) -> set[str]:
    """The quests of the main story: every main-story quest, every quest a chapter end needs,
    the quests :data:`PARTS` names (a gate on the way, such as the guard of the Ant Tunnel in
    chapter 1), and, repeatedly, every prerequisite of one of them and every quest that is the
    only source of an item one of them asks for."""
    todo = [str(q["id"]) for q in ctx.kb.entities("quest") if q.get("main_story")]
    todo += [qid for p in PARTS for qid in p.quests if ctx.kb.get("quest", qid) is not None]
    for ev in chapter_ends(ctx):
        todo += _event_quests(ctx, ev)
    out: set[str] = set()
    while todo:
        qid = todo.pop()
        if qid in out:
            continue
        q = ctx.kb.get("quest", qid)
        if q is None:
            continue
        out.add(qid)
        todo += prereqs(ctx, q)
        for r in q.get("requirements") or []:
            if r.get("type") == "item" and isinstance(r.get("item"), str):
                for src in _only_quest_sources(ctx, r["item"]):
                    src_q = ctx.kb.get("quest", src)
                    if src_q is not None and quest_chapter(src_q) == quest_chapter(q):
                        todo.append(src)
    return out


def _rank() -> dict[str, tuple[int, int]]:
    return {qid: (i, j) for i, p in enumerate(PARTS) for j, qid in enumerate(p.quests)}


def story_order(ctx: Context) -> list[str]:
    """The story quests in the order to accept them: a topological order of the prerequisite
    graph, ties broken by :data:`PARTS`, then by chapter, main before side, and number."""
    story = story_quests(ctx)
    rank = _rank()

    def key(qid: str) -> tuple:
        q = ctx.kb.get("quest", qid) or {}
        return (
            quest_chapter(q),
            rank.get(qid, (len(PARTS), 0)),
            0 if q.get("quest_kind") == "main" else 1,
            _num(q.get("number")),
            qid,
        )

    need = {qid: [r for r in prereqs(ctx, ctx.kb.get("quest", qid)) if r in story] for qid in story}
    users: dict[str, list[str]] = {qid: [] for qid in story}
    for qid, rs in need.items():
        for r in rs:
            users[r].append(qid)
    left = {qid: len(rs) for qid, rs in need.items()}
    heap = [(key(qid), qid) for qid, n in left.items() if n == 0]
    heapq.heapify(heap)
    out: list[str] = []
    while heap:
        _, qid = heapq.heappop(heap)
        out.append(qid)
        for u in users[qid]:
            left[u] -= 1
            if left[u] == 0:
                heapq.heappush(heap, (key(u), u))
    # a cycle (none in 1.5): keep what is left in the tie-break order
    out += sorted((qid for qid in story if qid not in out), key=key)
    return out


def order_problems(ctx: Context, order: Sequence[str]) -> list[str]:
    """Why ``order`` is not a walkthrough of the main story: a main-story quest missing, or a
    quest placed before a quest it needs."""
    probs = []
    pos = {qid: i for i, qid in enumerate(order)}
    for q in ctx.kb.entities("quest"):
        if q.get("main_story") and str(q["id"]) not in pos:
            probs.append(f"{q['id']}: main-story quest not in the walkthrough")
    for qid in order:
        q = ctx.kb.get("quest", qid)
        if q is None:
            probs.append(f"{qid}: not a quest")
            continue
        for r in prereqs(ctx, q):
            if r not in pos:
                probs.append(f"{qid}: needs {r}, which the walkthrough leaves out")
            elif pos[r] > pos[qid]:
                probs.append(f"{qid}: needs {r}, which comes later")
    return probs


# --- the model: parts with their quests ----------------------------------------------------------


@dataclass
class Walk:
    """The story as parts: each part's story quests (in order), the side quests that open in
    it, its route stops and its EXP."""

    order: list[str]
    parts: list[Part]
    story: dict[str, list[str]] = field(default_factory=dict)  # part slug -> quests
    side: dict[str, list[str]] = field(default_factory=dict)  # part slug -> side quests
    part_of: dict[str, Part] = field(default_factory=dict)  # quest -> part
    exp_end: dict[str, int] = field(default_factory=dict)  # part slug -> total EXP at its end


def walk(ctx: Context) -> Walk:
    cached = getattr(ctx, "_walkthrough", None)
    if cached is not None:
        return cached
    order = story_order(ctx)
    rank = _rank()
    w = Walk(order, [])
    for qid in order:
        q = ctx.kb.get("quest", qid) or {}
        if qid in rank:
            p = PARTS[rank[qid][0]]
        else:  # not named by a part: the part of its latest prerequisite, else the chapter's
            p = _fallback_part(ctx, w, q)
        w.part_of[qid] = p
        w.story.setdefault(p.slug, []).append(qid)
    w.parts = [p for p in PARTS if w.story.get(p.slug)]
    if not w.parts:
        return _cache(ctx, w)
    story = set(order)
    for q in sorted(qs.quests_in_order(ctx), key=lambda q: str(q["id"])):
        if str(q["id"]) in story:
            continue
        p = _side_part(ctx, w, q)
        if p is not None:
            w.part_of[str(q["id"])] = p
    for q in qs.quests_in_order(ctx):
        p = w.part_of.get(str(q["id"]))
        if p is not None and str(q["id"]) not in story:
            w.side.setdefault(p.slug, []).append(str(q["id"]))
    total = 0
    for p in w.parts:
        for qid in w.story.get(p.slug, []):
            total += quest_exp(ctx, ctx.kb.get("quest", qid) or {})
        w.exp_end[p.slug] = total
    return _cache(ctx, w)


def _cache(ctx: Context, w: Walk) -> Walk:
    ctx._walkthrough = w  # type: ignore[attr-defined]
    return w


def _fallback_part(ctx: Context, w: Walk, q: Entity) -> Part:
    ch = page_chapter(q)
    best = None
    for r in prereqs(ctx, q):
        p = w.part_of.get(r)
        if p is not None and p.chapter == ch:
            if best is None or PARTS.index(p) > PARTS.index(best):
                best = p
    if best is not None:
        return best
    for p in PARTS:
        if p.chapter == ch:
            return p
    return Part(ch, f"chapter-{ch}", f"Chapter {ch}", ())


def _side_part(ctx: Context, w: Walk, q: Entity) -> Part | None:
    """The part in which a side quest opens: the latest part among its prerequisites (a side
    prerequisite counts with the part where it opens), else the first part of its chapter
    whose route passes the giver's map, else the chapter's first part."""
    ch = page_chapter(q)
    chapter_parts = [p for p in w.parts if p.chapter == ch]
    if not chapter_parts:
        return None
    best: Part | None = None
    for r in prereqs(ctx, q):
        p = w.part_of.get(r)
        if p is None:
            rq = ctx.kb.get("quest", r)
            p = _side_part(ctx, w, rq) if rq is not None else None
        if (
            p is not None
            and p.chapter == ch
            and (best is None or PARTS.index(p) > PARTS.index(best))
        ):
            best = p
    if best is not None:
        return best
    giver_maps = {str(g.get("map")) for g in _refs(q.get("given_by")) if g.get("map")}
    for p in chapter_parts:
        if giver_maps & set(route_maps(ctx, w, p)):
            return p
    return chapter_parts[0]


# --- EXP and levels -----------------------------------------------------------------------------


def _drop_source(ctx: Context, iid: str) -> tuple[Entity, float] | None:
    """The best non-boss monster drop of an item: (monster, percent a kill). A drop of at
    least ``RARE`` percent wins; when every source is rarer, the best rare one counts."""
    it = ctx.kb.get("item", iid)
    best = rare = None
    for r in _refs((it or {}).get("dropped_by")):
        mon = ctx.kb.get("monster", str(r["id"]))
        pct = float(r.get("percent") or 0)
        if mon is None or mons.is_boss(mon) or pct <= 0 or not mons.spawns(mon):
            continue
        if pct >= RARE and (best is None or pct > best[1]):
            best = (mon, pct)
        elif pct < RARE and (rare is None or pct > rare[1]):
            rare = (mon, pct)
    return best or rare


def _hunt_source(ctx: Context, iid: str, ch: int) -> tuple[Entity, float] | None:
    """The monster to hunt for an item in chapter ``ch``: none when a shop of the chapter
    sells it (the walkthrough buys it), else :func:`_drop_source`."""
    if _shop_source(ctx, iid, ch, None) is not None:
        return None
    return _drop_source(ctx, iid)


def quest_exp(ctx: Context, q: Entity) -> int:
    """The EXP a story quest gives when the hero does exactly what it asks: its reward, the
    kills it asks for, and the expected kills for its items (bosses pay no EXP)."""
    total = sum(_num(r.get("amount")) for r in q.get("rewards") or [] if r.get("type") == "exp")
    for r in q.get("requirements") or []:
        if r.get("type") == "mob":
            mon = ctx.kb.get("monster", str(r.get("monster")))
            if mon is not None and not mons.is_boss(mon):
                total += _num(r.get("count")) * _num(mon.get("exp"))
        elif r.get("type") == "item" and isinstance(r.get("item"), str):
            src = _hunt_source(ctx, r["item"], max(quest_chapter(q), 1))
            if src is not None:
                mon, pct = src
                total += round(_num(r.get("count")) * 100 / pct) * _num(mon.get("exp"))
    return total


def _levels(ctx: Context) -> list[Entity]:
    return sorted(
        (e for e in ctx.kb.entities("level") if isinstance(e.get("level"), int)),
        key=lambda e: e["level"],
    )


def level_for_exp(ctx: Context, exp: int) -> int:
    """The level a hero has after ``exp`` EXP in all (``level.exp_total``: EXP to reach it)."""
    best = 1
    for e in _levels(ctx):
        if _num(e.get("exp_total")) <= exp:
            best = e["level"]
    return best


def accuracy_level(ctx: Context, mob_level: int) -> int | None:
    """The lowest level whose base ACC (the level table's) reaches ``mob_level``, so that the
    monster never evades; None when no level does."""
    for e in _levels(ctx):
        if _num(e.get("accuracy")) >= mob_level:
            return e["level"]
    return None


def part_monsters(ctx: Context, w: Walk, p: Part) -> list[str]:
    """The non-boss monsters the part's story quests send the hero to: kills and the best
    drop of each item they ask for."""
    out: list[str] = []
    for qid in w.story.get(p.slug, []):
        q = ctx.kb.get("quest", qid) or {}
        for r in q.get("requirements") or []:
            mid = None
            if r.get("type") == "mob":
                mid = str(r.get("monster"))
            elif r.get("type") == "item" and isinstance(r.get("item"), str):
                src = _hunt_source(ctx, r["item"], max(quest_chapter(q), 1))
                mid = str(src[0]["id"]) if src else None
            mon = ctx.kb.get("monster", mid) if mid else None
            if mon is not None and not mons.is_boss(mon) and mid not in out:
                out.append(mid)
    return out


def part_bosses(ctx: Context, w: Walk, p: Part) -> list[str]:
    out: list[str] = []
    for qid in w.story.get(p.slug, []):
        q = ctx.kb.get("quest", qid) or {}
        for r in q.get("requirements") or []:
            mon = ctx.kb.get("monster", str(r.get("monster"))) if r.get("type") == "mob" else None
            if mon is not None and mons.is_boss(mon) and str(mon["id"]) not in out:
                out.append(str(mon["id"]))
    return out


def part_levels(ctx: Context, w: Walk, p: Part) -> tuple[int, int | None, int]:
    """(story level at the end of the part, no-miss level, strongest monster level)."""
    story = level_for_exp(ctx, w.exp_end.get(p.slug, 0))
    lv = max(
        (_num((ctx.kb.get("monster", m) or {}).get("level")) for m in part_monsters(ctx, w, p)),
        default=0,
    )
    return story, (accuracy_level(ctx, lv) if lv else None), lv


def level_words(story: int, acc: int | None) -> str:
    if acc is None or acc <= story:
        return f"level {story}"
    return f"level {story} to {acc}"


def level_cell(story: int, acc: int | None) -> str:
    if acc is None or acc <= story:
        return f"{story}"
    return f"{story}-{acc}"


def min_level(ctx: Context, q: Entity) -> int:
    return qs.quest_level(q)


# --- the route ----------------------------------------------------------------------------------


def _effective_exits(ctx: Context, mid: str, ch: int) -> list[Mapping[str, Any]]:
    """The exits of a map that the game runs in quest chapter ``ch``: a chapter's own script
    replaces the common one of the same name."""
    mp = ctx.kb.get("map", mid) or {}
    exits = _refs(mp.get("exits"))
    own = {
        str(x.get("script", "")).split("/", 1)[-1]
        for x in exits
        if str(x.get("script", "")).startswith(f"chapter{ch}/")
    }
    out = []
    for x in exits:
        s = str(x.get("script", ""))
        folder, _, name = s.partition("/")
        if folder == f"chapter{ch}" or (folder == "common" and name not in own):
            if ctx.kb.get("map", str(x["id"])) is not None:
                out.append(x)
    return out


def path_between(ctx: Context, a: str, b: str, ch: int) -> list[str] | None:
    """The shortest way from map ``a`` to map ``b`` over the exits of chapter ``ch`` (both
    ends included), or None."""
    if a == b:
        return [a]
    prev: dict[str, str] = {a: a}
    todo = deque([a])
    while todo:
        cur = todo.popleft()
        for x in _effective_exits(ctx, cur, ch):
            nxt = str(x["id"])
            if nxt in prev:
                continue
            prev[nxt] = cur
            if nxt == b:
                out = [b]
                while out[-1] != a:
                    out.append(prev[out[-1]])
                return out[::-1]
            todo.append(nxt)
    return None


def _area(ctx: Context, mid: str | None) -> str | None:
    for a in _refs((ctx.kb.get("map", mid) or {}).get("area") if mid else None):
        return str(a["id"])
    return None


def _hunt_map(ctx: Context, mid: str, near: str | None = None) -> str | None:
    """Where to hunt a monster: the map ``near`` itself when it spawns there, else its map
    with the most spawn points in ``near``'s area, else anywhere (a boss: its boss map)."""
    mon = ctx.kb.get("monster", mid)
    if mon is None:
        return None
    if mons.is_boss(mon):
        bm = mons.boss_maps(ctx, mon)
        return bm[0] if bm else None
    spots = [
        (n, m) for m, n in mons.spawns(mon) if _refs((ctx.kb.get("map", m) or {}).get("entrances"))
    ]
    if not spots:
        return None
    if near is not None and any(m == near for _n, m in spots):
        return near
    area = _area(ctx, near)
    local = [s for s in spots if area is not None and _area(ctx, s[1]) == area]
    pool = local or spots
    pool.sort(key=lambda s: (-s[0], s[1]))
    return pool[0][1]


def finish_after(ctx: Context, w: Walk, p: Part) -> dict[str, str]:
    """Quests of the part to finish only after another one: the part's own pairs (a gate on
    the turn-in map), and every quest asking for an item a later quest of the part rewards."""
    out = dict(p.finish_after)
    ids = w.story.get(p.slug, [])
    for i, qid in enumerate(ids):
        if qid in out:
            continue
        q = ctx.kb.get("quest", qid) or {}
        wanted = {
            r["item"]
            for r in q.get("requirements") or []
            if r.get("type") == "item" and isinstance(r.get("item"), str)
        }
        for later in ids[i + 1 :]:
            lq = ctx.kb.get("quest", later) or {}
            if any(
                r.get("type") == "item" and r.get("item") in wanted for r in lq.get("rewards") or []
            ):
                out[qid] = later
                break
    return out


@dataclass(frozen=True)
class Stop:
    map: str
    why: str  # markup: what the hero does there
    then: str = ""  # the map a script moves the hero to afterwards, if any


def part_stops(ctx: Context, w: Walk, p: Part) -> list[Stop]:
    """The maps the part's story quests send the hero to, in order; a chapter end comes
    last."""
    cached = getattr(ctx, "_walkthrough_stops", None)
    if cached is None:
        cached = {}
        ctx._walkthrough_stops = cached  # type: ignore[attr-defined]
    if p.slug in cached:
        return cached[p.slug]
    stops: list[Stop] = []
    final = final_boss(ctx)[0]

    def add(mid: Any, why: str, then: str = "") -> None:
        if not isinstance(mid, str) or ctx.kb.get("map", mid) is None:
            return
        if stops and stops[-1].map == mid and not stops[-1].then:
            stops[-1] = Stop(mid, stops[-1].why + "; " + why, then)
        else:
            stops.append(Stop(mid, why, then))

    def here() -> str | None:
        return stops[-1].map if stops else None

    ends: list[Entity] = []
    after = finish_after(ctx, w, p)
    pending: dict[str, list[tuple[str, Any, str]]] = {}  # other quest -> finishes waiting

    def finish(qid: str, mid: Any, why: str) -> None:
        if qid in after:
            pending.setdefault(after[qid], []).append((qid, mid, why))
            return
        add(mid, why)
        flush(qid)

    def flush(qid: str) -> None:
        for other, mid, why in pending.pop(qid, []):
            add(mid, why)
            flush(other)

    for qid in w.story.get(p.slug, []):
        q = ctx.kb.get("quest", qid) or {}
        link = ctx.ref("quest", qid)
        for g in qs._uniq_npcs(q.get("given_by"))[:1]:
            add(g.get("map"), f"take {link} from {d.link_or_text(ctx, 'npc', str(g['id']))}")
        for r in q.get("requirements") or []:
            count = t.esc(t.number(_num(r.get("count"))))
            if r.get("type") == "mob":
                mid = str(r.get("monster"))
                then = ""
                if mid == final and ctx.kb.get("map", FINAL_SCENES[-1][4]) is not None:
                    then = FINAL_SCENES[-1][4]
                add(_hunt_map(ctx, mid, here()), f"defeat {count} {ctx.ref('monster', mid)}", then)
            elif r.get("type") == "item" and isinstance(r.get("item"), str):
                shop = _shop_source(ctx, r["item"], p.chapter, here())
                src = None if shop is not None else _drop_source(ctx, r["item"])
                if shop is not None:
                    add(
                        str(shop.get("map")),
                        f"buy {count} {ctx.ref('item', r['item'])}"
                        f" from {d.link_or_text(ctx, 'npc', str(shop['id']))}",
                    )
                elif src is not None:
                    mon = src[0]
                    add(
                        _hunt_map(ctx, str(mon["id"]), here()),
                        f"collect {count} {ctx.ref('item', r['item'])}"
                        f" from {ctx.ref('monster', str(mon['id']))}",
                    )
        enders = qs._uniq_npcs(q.get("completed_by"))
        for g in enders[:1]:
            who = d.link_or_text(ctx, "npc", str(g["id"]))
            finish(qid, g.get("map"), f"finish {link} with {who}")
        if not enders:
            flush(qid)
            ev = qs.chapter_end_for(ctx, q)
            if ev is not None and all(ev["id"] != e["id"] for e in ends):
                ends.append(ev)
    for rest in list(pending.values()):
        for _other, mid, why in rest:
            add(mid, why)
    for ev in ends:
        for m in _refs(ev.get("maps"))[:1]:
            label = ctx.ref("event", str(ev["id"]), "chapter end")
            add(str(m["id"]), f"end the chapter ({label})", _warp_target(ctx, ev) or "")
    cached[p.slug] = stops
    return stops


def start_map(ctx: Context, w: Walk, p: Part) -> str | None:
    """Where the hero is when the part starts: the last stop of the part before, or where
    the chapter starts."""
    i = w.parts.index(p)
    if i > 0 and w.parts[i - 1].chapter == p.chapter:
        prev = part_stops(ctx, w, w.parts[i - 1])
        return (prev[-1].then or prev[-1].map) if prev else None
    return chapter_start(ctx, p.chapter)


def chapter_start(ctx: Context, ch: int) -> str | None:
    """The map where chapter ``ch`` starts: the target of the warp of the chapter end before
    it (chapter 1: the tutorial's first map, where the intro leaves the hero)."""
    if ch == 1:
        return FIRST_MAP if ctx.kb.get("map", FIRST_MAP) is not None else None
    for wp in ctx.kb.entities("warp"):
        if wp.get("chapter_end") and (ch - 1) in (wp.get("chapters") or []):
            tm = wp.get("target_map")
            if isinstance(tm, str) and ctx.kb.get("map", tm) is not None:
                return tm
    return None


def route_maps(ctx: Context, w: Walk, p: Part) -> list[str]:
    out: list[str] = []
    for s in part_stops(ctx, w, p):
        if s.map not in out:
            out.append(s.map)
    return out


def _quest_chapter_of_part(p: Part) -> list[int]:
    return [0, 1] if p.chapter == 1 else [p.chapter]


def legs(ctx: Context, w: Walk, p: Part) -> list[tuple[Stop, list[str] | None, int]]:
    """Each stop with the way there from the stop before (both ends included; ``None``: no
    walking route) and the quest chapter whose exits it uses."""
    here = start_map(ctx, w, p)
    out: list[tuple[Stop, list[str] | None, int]] = []
    for s in part_stops(ctx, w, p):
        path: list[str] | None = [s.map]
        used = _quest_chapter_of_part(p)[-1]
        if here and here != s.map:
            path = None
            for ch in _quest_chapter_of_part(p)[::-1]:
                path = path_between(ctx, here, s.map, ch)
                if path:
                    used = ch
                    break
        out.append((s, path, used))
        here = s.then or s.map
    return out


def gates(ctx: Context, path: Sequence[str], ch: int) -> list[tuple[str, str, Entity]]:
    """The steps of ``path`` that only a conditional portal makes: (from, to, its warp)."""
    out = []
    for a, b in zip(path, path[1:], strict=False):
        exits = [x for x in _effective_exits(ctx, a, ch) if str(x["id"]) == b]
        if not exits or any(not x.get("gated") for x in exits):
            continue
        for x in exits:
            script = str(x.get("script", "")).replace("/", "-")
            for wp in ctx.kb.entities("warp"):
                if wp.get("target_map") == b and any(
                    str(r["id"]) == script for r in _refs(wp.get("script"))
                ):
                    out.append((a, b, wp))
                    break
            else:
                continue
            break
    return out


def gate_phrase(ctx: Context, a: str, b: str, wp: Entity, ch: int) -> str:
    conds = qs.cond_phrases(ctx, qs._talk.necessary(wp.get("conditions") or []), [ch], None)
    when = f" only when {qs.join(conds)}" if conds else " only under a condition"
    return f"The way from {qs.map_phrase(ctx, a)} to {qs.map_phrase(ctx, b)} opens{when}."


def gate_quests(ctx: Context, w: Walk, p: Part) -> list[str]:
    """The quests the conditional portals on the part's route name."""
    out: list[str] = []
    for _s, path, ch in legs(ctx, w, p):
        for _a, _b, wp in gates(ctx, path or [], ch):
            for qid in necessary_quests(ctx, wp.get("conditions"), [ch]):
                if qid not in out:
                    out.append(qid)
    return out


def route_problems(ctx: Context) -> list[str]:
    """Conditional portals on the route whose quest the walkthrough does not take by then:
    each quest a gate names must be a story quest of the same part or an earlier one."""
    w = walk(ctx)
    probs = []
    for i, p in enumerate(w.parts):
        for qid in gate_quests(ctx, w, p):
            owner = w.part_of.get(qid)
            if qid not in w.order or owner is None or w.parts.index(owner) > i:
                probs.append(f"{p.slug}: a portal on the route needs {qid}, not taken by then")
    return probs


def passed_maps(ctx: Context, w: Walk, p: Part) -> list[str]:
    """Every map the part's route enters, stops and the maps between them."""
    out: list[str] = []
    for s, path, _ch in legs(ctx, w, p):
        for m in (path or []) + [s.map]:
            if m not in out:
                out.append(m)
    return out


def strongest_on_way(ctx: Context, w: Walk, p: Part) -> tuple[str, str] | None:
    """(monster, map) of the strongest non-boss monster on the maps the route passes."""
    best = None
    for m in passed_maps(ctx, w, p):
        for r in _refs((ctx.kb.get("map", m) or {}).get("spawns")):
            mon = ctx.kb.get("monster", str(r["id"]))
            if mon is None or mons.is_boss(mon):
                continue
            lv = _num(mon.get("level"))
            if best is None or lv > best[0]:
                best = (lv, str(mon["id"]), m)
    return (best[1], best[2]) if best else None


def route_table(ctx: Context, w: Walk, p: Part) -> str:
    stops = part_stops(ctx, w, p)
    if not stops:
        return ""
    rows = []
    for s, path, ch in legs(ctx, w, p):
        via = ""
        if path and len(path) > 2:
            via = ", ".join(qs.map_phrase(ctx, m) for m in path[1:-1])
        elif path is None:
            via = t.esc("no walking route (a script moves the hero)")
        why = qs._cap(s.why)
        if s.then and s.then != s.map:
            why += f"; afterwards a script moves you to {qs.map_phrase(ctx, s.then)}"
        why += "."
        for a, b, wp in gates(ctx, path or [], ch):
            why += " " + gate_phrase(ctx, a, b, wp, ch)
        rows.append(
            [
                t.markup_cell(qs.map_phrase(ctx, s.map), name_of(ctx.kb.get("map", s.map) or {})),
                t.markup_cell(via or t.esc("next door")),
                t.markup_cell(why),
            ]
        )
    cols = [t.Column("Go to"), t.Column("Through"), t.Column("To")]
    start = start_map(ctx, w, p)
    lead = (
        _p(
            f"The part starts on {qs.map_phrase(ctx, start)}. The way between stops is the shortest"
            " one over the maps' exits in this chapter."
        )
        if start
        else ""
    )
    return lead + t.table(cols, rows, filter=False)


# --- quest lines --------------------------------------------------------------------------------


def task_phrase(ctx: Context, q: Entity) -> str:
    """What a quest asks for, in a few words (markup)."""
    parts = []
    for r in q.get("requirements") or []:
        if r.get("type") == "mob" and ctx.kb.get("monster", str(r.get("monster"))):
            parts.append(
                f"defeat {t.esc(t.number(_num(r.get('count'))))}"
                f" {ctx.ref('monster', str(r['monster']))}"
            )
        elif r.get("type") == "item" and ctx.kb.get("item", str(r.get("item"))):
            parts.append(
                f"bring {t.esc(t.number(_num(r.get('count'))))} {ctx.ref('item', str(r['item']))}"
            )
        elif r.get("type") == "meso":
            parts.append(f"bring {t.esc(t.number(_num(r.get('amount'))))} meso")
    return qs.join(parts) if parts else "talk"


def _npc_words(ctx: Context, refs: Any) -> str:
    return qs.join([qs.npc_at(ctx, g) for g in qs._uniq_npcs(refs)], "or")


def story_step(ctx: Context, qid: str, p: Part | None = None) -> str:
    """One numbered step of the story: accept, do, finish (second person)."""
    q = ctx.kb.get("quest", qid) or {}
    link = ctx.ref("quest", qid)
    giver = _npc_words(ctx, q.get("given_by"))
    ender = _npc_words(ctx, q.get("completed_by"))
    out = f"Take {link}" + (f" from {giver}" if giver else "") + "."
    task = task_phrase(ctx, q)
    if task != "talk":
        out += f" {qs._cap(task)}."
    lv = min_level(ctx, q)
    if lv > 1:
        out += t.esc(f" It needs level {lv}.")
    later = finish_after(ctx, walk(ctx), p).get(qid) if p is not None else None
    if ender and later and ctx.kb.get("quest", later) is not None:
        out += f" Finish it with {ender} once {ctx.ref('quest', later)} is done."
    elif ender:
        out += f" Finish it with {ender}."
    else:
        ev = qs.chapter_end_for(ctx, q)
        if ev is not None:
            out += (
                f" It is never finished: it stays open until the"
                f" {ctx.ref('event', str(ev['id']), 'chapter end')}."
            )
    rew = qs.reward_summary(q)
    if rew and ender:
        out += t.esc(f" Reward: {rew}.")
    if not q.get("main_story"):
        out += " This side quest is part of the main story's path."
    return out


def side_table(ctx: Context, w: Walk, ids: list[str]) -> str:
    rows = []
    for qid in ids:
        q = ctx.kb.get("quest", qid) or {}
        lv = min_level(ctx, q)
        rows.append(
            [
                t.markup_cell(ctx.ref("quest", qid), name_of(q)),
                t.markup_cell(_npc_words(ctx, q.get("given_by")) or t.esc("unknown")),
                t.markup_cell(qs._cap(task_phrase(ctx, q))),
                t.Cell(t.esc(str(lv) if lv > 1 else "any"), lv),
                t.markup_cell(t.esc(qs.reward_summary(q) or "none")),
            ]
        )
    cols = [
        t.Column("Quest"),
        t.Column("Giver"),
        t.Column("Task"),
        t.Column("Level", numeric=True),
        t.Column("Reward"),
    ]
    return t.table(cols, rows)


# --- gear: shops on the way ---------------------------------------------------------------------


def chapter_shops(
    ctx: Context, ch: int, maps: Iterable[str]
) -> list[tuple[str, Mapping[str, Any]]]:
    """(shop id, opener) for each shop an NPC opens on one of ``maps`` in chapter ``ch``."""
    maps = list(maps)
    out = []
    for s in ctx.kb.entities("shop"):
        for o in _refs(s.get("opened_by")):
            chs = o.get("chapters") or []
            if o.get("map") in maps and (not chs or ch in chs):
                out.append((str(s["id"]), o))
    out.sort(key=lambda x: (maps.index(str(x[1].get("map"))), x[0]))
    return out


def _shop_source(ctx: Context, iid: str, ch: int, near: str | None) -> Mapping[str, Any] | None:
    """The opener (NPC and map) of a shop that sells the item in chapter ``ch``: the one on
    ``near`` if there is one, else the first by map; None when no shop sells it."""
    it = ctx.kb.get("item", iid) or {}
    sids = {str(r["id"]) for r in _refs(it.get("sold_by")) if r.get("kind", "shop") == "shop"}
    found = []
    for sid in sorted(sids):
        for o in _refs((ctx.kb.get("shop", sid) or {}).get("opened_by")):
            chs = o.get("chapters") or []
            if o.get("map") and o.get("id") is not None and (not chs or ch in chs):
                found.append(o)
    if not found:
        return None
    found.sort(key=lambda o: (o.get("map") != near, str(o.get("map"))))
    return found[0]


def _best_weapon(ctx: Context, sid: str, kind: str, max_level: int) -> str:
    shop = ctx.kb.get("shop", sid) or {}
    best = None
    for r in _refs(shop.get("sells")):
        it = ctx.kb.get("item", str(r["id"]))
        if it is None or it.get("weapon_type") != kind:
            continue
        lv = _num(it.get("req_level"))
        if lv > max_level:
            continue
        att = _num(((it.get("stats") or {}).get("attack") or {}).get("min"))
        k = (lv, att)
        if best is None or k > best[0]:
            best = (k, str(r["id"]), _num(r.get("price")))
    if best is None:
        return ""
    (lv, _att), iid, price = best
    return f"{ctx.ref('item', iid)} ({t.esc(f'level {lv}, {t.number(price)} meso')})"


def shops_table(ctx: Context, ch: int, maps: list[str], max_level: int) -> str:
    rows = []
    for sid, o in chapter_shops(ctx, ch, maps):
        rows.append(
            [
                t.markup_cell(
                    ctx.ref("shop", sid, f"Shop {sid}"), int(sid) if sid.isdigit() else 0
                ),
                t.markup_cell(qs.npc_at(ctx, o)),
                t.markup_cell(_best_weapon(ctx, sid, "claw", max_level) or t.esc("none")),
                t.markup_cell(_best_weapon(ctx, sid, "dagger", max_level) or t.esc("none")),
            ]
        )
    if not rows:
        return ""
    cols = [
        t.Column("Shop"),
        t.Column("Keeper"),
        t.Column("Best claw"),
        t.Column("Best dagger"),
    ]
    return t.table(cols, rows, filter=False)


# --- carry-over ---------------------------------------------------------------------------------


def carry_over(ctx: Context, w: Walk, p: Part) -> list[str]:
    """Items the part's quests give that a later story quest asks for, and job
    advancements that open."""
    out = []
    i = w.parts.index(p)
    later = [qid for x in w.parts[i + 1 :] for qid in w.story.get(x.slug, [])]
    later += [qid for qid in w.story.get(p.slug, [])]
    for qid in w.story.get(p.slug, []):
        q = ctx.kb.get("quest", qid) or {}
        for r in q.get("rewards") or []:
            iid = r.get("item") if r.get("type") == "item" else None
            if not isinstance(iid, str) or ctx.kb.get("item", iid) is None:
                continue
            users = []
            for other in later:
                if other == qid:
                    continue
                oq = ctx.kb.get("quest", other) or {}
                if any(x.get("item") == iid for x in oq.get("requirements") or []):
                    users.append(ctx.ref("quest", other))
            if users:
                out.append(
                    f"Keep the {ctx.ref('item', iid)} from {ctx.ref('quest', qid)}:"
                    f" {qs.join(users)} asks for it."
                )
            elif iid == HAZARD_SUIT:
                out.append(
                    f"Wear the {ctx.ref('item', iid)} from {ctx.ref('quest', qid)} in"
                    f" {mine_ref(ctx)}: without it the mine's heat deals 300"
                    " damage every 2.5 seconds."
                )
    return out


# --- bosses ---------------------------------------------------------------------------------


def boss_block(ctx: Context, mid: str) -> str:
    mon = ctx.kb.get("monster", mid)
    if mon is None:
        return ""
    lv = _num(mon.get("level"))
    hp = _num(mon.get("hp"))
    _lo, hi = mons.contact_damage(lv, 0)
    acc = accuracy_level(ctx, lv)
    where = [mons.where(ctx, m) for m in mons.boss_maps(ctx, mon)]
    facts = (
        f"{ctx.ref('monster', mid)} has {t.esc(t.number(hp))} HP and level"
        f" {t.esc(str(lv))}"
        + (f"; it waits on {qs.join(where)}" if where else "")
        + "."
        + t.esc(
            f" Its touch deals up to {t.number(hi)} damage to a hero with no DEF, and its skill"
            " attacks twice the roll, which cannot be avoided."
        )
    )
    if acc is None:
        facts += t.esc(
            f" No level gives the base ACC of {lv}, so some hits are always evaded:"
            " equipment with ACC narrows the gap."
        )
    else:
        facts += t.esc(f" A hero needs level {acc} for base ACC to match it.")
    out = _p(facts)
    evs = mons.boss_events(ctx, mon)
    if evs and not any(ev.get("used") for ev in evs):
        out += _p(
            "No map object runs this boss's fight script in version 1.5, so how the fight"
            " starts has not been confirmed in play"
            f' (<a href="{YETI_ISSUE}">issue 218</a>, a playthrough check). The quest still asks'
            " for the kill."
        )
    elif evs:
        out += _p(
            "Entering the boss map while the quest's kill is still missing starts the"
            " boss's entrance scene; the fight begins when it ends. Saving is refused on boss"
            " maps."
        )
    strat = ctx.prose(f"{SECTION_ID}/boss-{mid}.md")
    if strat:
        out += strat
    out += _p(f"The monster page has the full boss data: {ctx.ref('monster', mid)}.")
    return out


# --- crafting and trades per chapter --------------------------------------------------------


def _offer_when(ctx: Context, e: Entity, ch: int) -> str:
    alts = npcs_s.offer_conditions(ctx, e, None)
    if not alts or alts == [{}]:
        return "at any time"
    return "when " + npcs_s.alternatives_markup(ctx, ch, alts)


def crafting_section(ctx: Context, ch: int) -> str:
    items = []
    for kind, verb, page in (
        ("recipe", "crafts", npcs_s.CRAFTING),
        ("trade", "trades", npcs_s.TRADES),
    ):
        groups: dict[tuple[str, str], list[Entity]] = {}
        for e in ctx.kb.entities(kind):
            if _num(e.get("chapter")) != ch:
                continue
            who = _refs(e.get("crafted_by") if kind == "recipe" else e.get("offered_by"))
            for o in who[:1]:
                groups.setdefault((str(o["id"]), str(o.get("map"))), []).append(e)
        for (npc, mid), es in sorted(groups.items()):
            levels = [
                _num((ctx.kb.get("item", str(e.get("result_item"))) or {}).get("req_level"))
                for e in es
            ]
            lv = [x for x in levels if x]
            span = f", for levels {min(lv)} to {max(lv)}" if lv and min(lv) != max(lv) else ""
            words = "recipe" if kind == "recipe" else "trade"
            plural = words + ("s" if len(es) != 1 else "")
            when = _offer_when(ctx, es[0], ch)
            items.append(
                f"<li>{d.link_or_text(ctx, 'npc', npc)} in {qs.map_phrase(ctx, mid)} {verb}:"
                f' <a href="{ctx.url(page)}">{t.esc(t.number(len(es)))} {plural}</a>'
                f"{t.esc(span)}, {when}{'' if when.endswith('</ul>') else '.'}</li>"
            )
    if not items:
        return ""
    body = (
        _p(
            "Crafting and trading are open in this chapter as listed. Each recipe and trade is"
            " tied to the chapter: the quest states reset at the chapter end, and a later"
            " chapter has its own list."
        )
        + "<ul>"
        + "".join(items)
        + "</ul>\n"
    )
    return t.section("Crafting and trades", body, "crafting-and-trades")


# --- endings --------------------------------------------------------------------------------


@dataclass(frozen=True)
class Ending:
    anchor: str
    title: str
    event: str  # the event id: a chapter end, or the last boss's fight
    chapter: int  # the quest chapter it ends


def final_boss(ctx: Context) -> tuple[str, Entity | None, str | None]:
    """(monster id, its boss event, the quest asking for the kill) of the game's last fight:
    the boss the last chapter's last main-story quest with a boss kill asks for."""
    order = story_order(ctx)
    last_ch = max((quest_chapter(ctx.kb.get("quest", q) or {}) for q in order), default=0)
    for qid in reversed(order):
        q = ctx.kb.get("quest", qid) or {}
        if quest_chapter(q) != last_ch or chapter_end_of(ctx, last_ch) is not None:
            continue
        for r in q.get("requirements") or []:
            mon = ctx.kb.get("monster", str(r.get("monster"))) if r.get("type") == "mob" else None
            if mon is not None and mons.is_boss(mon):
                evs = mons.boss_events(ctx, mon)
                return str(mon["id"]), (evs[0] if evs else None), qid
    return "", None, None


def endings(ctx: Context) -> list[Ending]:
    """Every ending the scripts and scenes have: each chapter end, and the game's ending."""
    out = []
    for ev in chapter_ends(ctx):
        chs = [int(c) for c in ev.get("chapters") or [] if isinstance(c, int)]
        ch = chs[0] if chs else -1
        title = "End of the tutorial" if ch == 0 else f"End of chapter {ch}"
        out.append(Ending(f"ending-{t.anchor_id(str(ev['id']))}", title, str(ev["id"]), ch))
    mid, ev, _qid = final_boss(ctx)
    if mid and ev is not None:
        ch = int((ev.get("chapters") or [0])[0])
        out.append(Ending(f"ending-{t.anchor_id(str(ev['id']))}", "The ending", str(ev["id"]), ch))
    return out


def _warp_target(ctx: Context, ev: Entity) -> str | None:
    script = str((_refs(ev.get("script")) or [{"id": ""}])[0]["id"])
    for wp in ctx.kb.entities("warp"):
        if wp.get("chapter_end") and any(str(r["id"]) == script for r in _refs(wp.get("script"))):
            tm = wp.get("target_map")
            if isinstance(tm, str) and ctx.kb.get("map", tm) is not None:
                return tm
    return None


def chapter_end_body(ctx: Context, ev: Entity, ch: int) -> str:
    eid = str(ev["id"])
    conds = qs.cond_phrases(ctx, qs._talk.necessary(ev.get("conditions") or []), [ch], None)
    answers = [
        c
        for alt in (ev.get("conditions") or [])[:1]
        for c in alt
        if str(c.get("term", "")).startswith("answer#")
    ]
    trig = ev.get("trigger") or []
    maps = [qs.map_phrase(ctx, str(m["id"])) for m in _refs(ev.get("maps"))]
    who = ""
    did = eid[: -len("-chapter_end")] if eid.endswith("-chapter_end") else ""
    dia = ctx.kb.get("dialogue", did) if did else None
    if "npc" in trig and dia is not None:
        who = qs._talk_with(ctx, dia)
    parts = []
    how = (
        f"Talk to {who}"
        if who
        else ("Go through the portal" if "portal" in trig else "Trigger the script")
    )
    how += f" on {qs.join(maps, 'or')}" if maps and not who else ""
    when = f" once {qs.join(conds)}" if conds else ""
    parts.append(
        f"<li><strong>What decides it.</strong> {qs._cap(qs.join(conds) or 'nothing')}"
        f" ({ctx.ref('event', eid, 'the chapter end event')}).</li>"
    )
    choice = ""
    if answers:
        n = _num(answers[0].get("value"))
        choice = t.esc(f" and pick the choice that leaves for the next chapter (answer {n})")
    parts.append(f"<li><strong>How to see it.</strong> {how}{when}{choice}.</li>")
    target = _warp_target(ctx, ev)
    nxt = ch + 1
    after = (
        "The script calls clearAll"
        f' (<a href="{SCRIPT_VM}">script VM</a>): every quest state resets and chapter'
        f" {nxt} begins"
    )
    after += f" on {qs.map_phrase(ctx, target)}." if target else "."
    after += (
        " Unfinished side quests of the chapter lose their progress, and the chapter's"
        " recipes and trades give way to the next chapter's. Level, items and meso are not among"
        " what clearAll resets (probable, from the native's description)."
    )
    parts.append(f"<li><strong>What happens after.</strong> {after}</li>")
    out = "<ul>" + "".join(parts) + "</ul>\n"
    nxt_ch = PREFIX_CHAPTER.get(nxt, nxt)
    if nxt_ch in CHAPTERS and nxt != 1:
        out += _p(
            f'The walkthrough goes on with <a href="{ctx.url(chapter_path(nxt_ch))}">'
            f"chapter {nxt}</a>."
        )
    return out


def final_ending_body(ctx: Context) -> str:
    mid, ev, qid = final_boss(ctx)
    if not mid or ev is None:
        return ""
    mon = ctx.kb.get("monster", mid) or {}
    maps = [mons.where(ctx, m) for m in mons.boss_maps(ctx, mon)]
    q = ctx.kb.get("quest", qid or "") or {}
    ender = _npc_words(ctx, q.get("completed_by"))
    follow = [str(r["id"]) for r in _refs(q.get("unlocks")) if ctx.kb.get("quest", str(r["id"]))]
    parts = [
        f"<li><strong>What decides it.</strong> Defeating {ctx.ref('monster', mid)} on"
        f" {qs.join(maps)} while {ctx.ref('quest', qid)} is in progress"
        f" ({ctx.ref('event', str(ev['id']), 'its boss event')}). There is no choice and no"
        " second ending: every playthrough that finishes the story sees the same scenes.</li>",
    ]
    part = walk(ctx).part_of.get(qid or "")
    if part is not None:
        parts.append(
            f'<li><strong>How to see it.</strong> Follow <a href="{part_url(ctx, part)}">part'
            f" {part_number(part)} of chapter {part.chapter}</a> and win the fight.</li>"
        )
    scenes = []
    for i, (n, builder, steps, boxes, mp) in enumerate(FINAL_SCENES):
        where = qs.map_phrase(ctx, mp, number=True) if ctx.kb.get("map", mp) else t.esc(mp)
        last = i == len(FINAL_SCENES) - 1
        scenes.append(
            f"scene {n} (<code>CAniManager::{t.esc(builder)}</code>, {t.esc(t.number(steps))}"
            f" steps, {t.esc(t.number(boxes))} speech boxes), which "
            + (f"ends with a warp to {where}" if last else f"moves the hero to {where} for")
        )
    after = (
        f"The boss's defeat counts the kill and queues {' '.join(scenes)}"
        f' (<a href="{SCENES_DOC}">scene list</a>). The game does not end: the hero is back in'
        " control in town."
    )
    if ender:
        after += f" Return to {ender} to finish {ctx.ref('quest', qid)}."
    if follow:
        after += (
            f" {ctx.ref('quest', follow[0])} opens in the same conversation; it has no"
            " requirements, no rewards and no turn-in, so it stays open for the rest of the game."
        )
    parts.append(f"<li><strong>What happens after.</strong> {after}</li>")
    out = "<ul>" + "".join(parts) + "</ul>\n"
    story = ctx.prose(f"{SECTION_ID}/ending-story.md")
    if story:
        out += (
            '<details class="spoiler"><summary>The ending scenes</summary>\n'
            + story
            + "</details>\n"
        )
    post = ctx.prose(f"{SECTION_ID}/post-game.md")
    if post:
        out += t.section("After the ending", post, "after-the-ending", level=3)
    return out


def endings_page(ctx: Context) -> Page:
    body = [
        '<p class="lead">The game has one ending, after the last boss of chapter'
        f" {last_chapter(walk(ctx))}, and a"
        " chapter end between chapters, where the quest states reset. This page says what"
        " decides each one, how to see it and what happens after.</p>\n"
    ]
    prose = ctx.prose(f"{SECTION_ID}/endings.md")
    if prose:
        body.append(prose)
    rows = []
    for e in endings(ctx):
        rows.append(f'<li><a href="#{e.anchor}">{t.esc(e.title)}</a></li>')
    body.append('<ul class="see-also">' + "".join(rows) + "</ul>\n")
    for e in endings(ctx):
        ev = ctx.kb.get("event", e.event) or {}
        if ev.get("type") == "chapter_end":
            inner = chapter_end_body(ctx, ev, e.chapter)
        else:
            inner = final_ending_body(ctx)
        body.append(t.section(e.title, inner, e.anchor))
    body.append(
        t.section(
            "Notes",
            "<ul><li>The scene runner can also end a scene by returning to the main menu"
            " (its command 28 with the map argument set to minus one), but no scene of version"
            f' 1.5 does (<a href="{SCENES_DOC}">scene list</a>).</li>'
            "<li>Every ending above has been read from the scripts and the scene data; a"
            f' playthrough of each is listed for the <a href="{REVIEW_ISSUE}">accuracy'
            " review</a>.</li></ul>\n",
            "notes",
        )
    )
    return Page(ENDINGS, "Endings", "\n".join(body), description="Every ending and chapter end")


# --- chapter pages --------------------------------------------------------------------------


def part_section(ctx: Context, w: Walk, p: Part) -> str:
    out = []
    goal = ctx.prose(f"{SECTION_ID}/parts/{p.slug}.md")
    if goal:
        out.append(t.section("Goal", goal, f"{p.slug}-goal", level=3))
    route = route_table(ctx, w, p)
    if route:
        out.append(t.section("Route", route, f"{p.slug}-route", level=3))
    steps = w.story.get(p.slug, [])
    qbody = (
        '<ol class="steps">'
        + "".join(f"<li>{story_step(ctx, q, p)}</li>" for q in steps)
        + "</ol>\n"
    )
    side = w.side.get(p.slug, [])
    if side:
        qbody += _p(
            "Optional quests that open in this part (their own pages have every step). Each is"
            " lost at the chapter end if it is not finished by then."
        )
        qbody += side_table(ctx, w, side)
    out.append(t.section("Quests", qbody, f"{p.slug}-quests", level=3))
    story_lv, acc_lv, mob_lv = part_levels(ctx, w, p)
    lv_body = (
        f"Aim for {t.esc(level_words(story_lv, acc_lv))}. The story's own EXP brings a hero"
        f" who does only what the main story asks to level {t.esc(str(story_lv))} by the end"
        " of this part"
    )
    if mob_lv:
        lv_body += t.esc(
            f"; the strongest monster it sends you to is level {mob_lv}, and base ACC reaches"
            + (f" that at level {acc_lv}" if acc_lv else " that at no level")
        )
    lv_body += "."
    gates = [(qid, min_level(ctx, ctx.kb.get("quest", qid) or {})) for qid in steps]
    gates = [(qid, lv) for qid, lv in gates if lv > 1]
    lv_out = _p(lv_body)
    way = strongest_on_way(ctx, w, p)
    if way is not None:
        wmon, wmap = way
        wlv = _num((ctx.kb.get("monster", wmon) or {}).get("level"))
        if wlv > mob_lv:
            lv_out += _p(
                f"On the way, the strongest monster is {ctx.ref('monster', wmon)} (level"
                f" {t.esc(str(wlv))}) on {qs.map_phrase(ctx, wmap)}; the route only passes it."
            )
    if any(_area(ctx, m) == HAZARD_AREA for m in passed_maps(ctx, w, p)):
        suit = ctx.kb.get("item", HAZARD_SUIT)
        if suit is not None and ctx.kb.get("area", HAZARD_AREA) is not None:
            req = _num(suit.get("req_level"))
            need = t.esc(f" It needs level {req} to wear.") if req > 1 else ""
            lv_out += _p(
                f"<strong>Wear the {ctx.ref('item', HAZARD_SUIT)}.</strong> In"
                f" {mine_ref(ctx)} the heat deals 300 damage every 2.5 seconds to"
                f' a hero without it (<a href="{ctx.url("mechanics/accuracy.html")}">hazard'
                f" maps</a>).{need}"
            )
    if gates:
        lv_out += _p(
            "Level gates: "
            + qs.join(
                [f"{ctx.ref('quest', qid)} needs level {t.esc(str(lv))}" for qid, lv in gates]
            )
            + "."
        )
    gear_lv = max(
        [story_lv]
        + [
            part_levels(ctx, w, x)[1] or 0
            for x in w.parts[: w.parts.index(p) + 1]
            if x.chapter == p.chapter
        ]
    )
    shops = shops_table(ctx, p.chapter, route_maps(ctx, w, p), gear_lv)
    if shops:
        lv_out += (
            _p(
                "Shops on the way, with the strongest claw and dagger each sells up to level"
                f" {t.esc(str(gear_lv))}:"
            )
            + shops
        )
    out.append(t.section("Level and gear", lv_out, f"{p.slug}-level", level=3))
    for mid in part_bosses(ctx, w, p):
        out.append(t.section("Boss", boss_block(ctx, mid), f"{p.slug}-boss-{mid}", level=3))
    carry = carry_over(ctx, w, p)
    last = (
        w.parts.index(p) == len(w.parts) - 1 or w.parts[w.parts.index(p) + 1].chapter != p.chapter
    )
    if last:
        end = chapter_end_of(ctx, p.chapter)
        if end is not None:
            carry.append(
                f'<strong>Point of no return.</strong> The <a href="{ctx.url(ENDINGS)}#ending-'
                f'{t.anchor_id(str(end["id"]))}">chapter end</a> resets every quest state. Finish'
                f' the side quests you want first (<a href="#before-the-chapter-ends">what is'
                " lost</a>)."
            )
    if carry:
        out.append(
            t.section(
                "What carries over",
                "<ul>" + "".join(f"<li>{c}</li>" for c in carry) + "</ul>\n",
                f"{p.slug}-carry",
                level=3,
            )
        )
    heading = f"Part {part_number(p)}: {p.title}"
    return t.section(heading, "".join(out), p.slug)


def missable_section(ctx: Context, w: Walk, ch: int) -> str:
    end = chapter_end_of(ctx, ch)
    side = [qid for p in w.parts if p.chapter == ch for qid in w.side.get(p.slug, [])]
    if end is None:
        body = _p(
            f"No chapter end follows chapter {ch}: its quests stay as they are after the ending, so"
            " nothing here is lost."
        )
    else:
        body = _p(
            f"The {ctx.ref('event', str(end['id']), 'chapter end')} resets every quest state."
            f" These {t.esc(t.number(len(side)))} side quests of the chapter are lost if they are"
            ' not finished by then (the <a href="'
            + ctx.url(CHECKLIST)
            + '">checklist</a> has them all):'
        )
        body += side_table(ctx, w, side)
    return t.section("Before the chapter ends", body, "before-the-chapter-ends")


def chapter_infobox(ctx: Context, w: Walk, ch: int) -> str:
    parts = [p for p in w.parts if p.chapter == ch]
    areas: list[str] = []
    for p in parts:
        for m in route_maps(ctx, w, p):
            for a in _refs((ctx.kb.get("map", m) or {}).get("area")):
                if str(a["id"]) not in areas:
                    areas.append(str(a["id"]))
    lvs = [part_levels(ctx, w, p) for p in parts]
    lo = min((x[0] for x in lvs), default=1)
    hi = max((max(x[0], x[1] or 0) for x in lvs), default=1)
    mains = [q for p in parts for q in w.story.get(p.slug, [])]
    bosses = [m for p in parts for m in part_bosses(ctx, w, p)]
    rows = [
        ("Chapter", t.esc(str(ch))),
        ("Areas", ", ".join(ctx.ref("area", a) for a in areas if ctx.kb.get("area", a))),
        ("Level range", t.esc(f"Lv. {lo}-{hi}")),
        ("Story quests", t.esc(t.number(len(mains)))),
        ("Bosses", ", ".join(ctx.ref("monster", m) for m in bosses)),
    ]
    return t.infobox(t.esc(f"Chapter {ch}"), rows)


def chapter_page(ctx: Context, w: Walk, ch: int) -> Page:
    parts = [p for p in w.parts if p.chapter == ch]
    body = [chapter_infobox(ctx, w, ch)]
    intro = ctx.prose(f"{SECTION_ID}/chapter-{ch}.md")
    body.append(
        f'<p class="lead">Chapter {ch} of the walkthrough, in {t.esc(t.number(len(parts)))} parts.'
        + (" It starts with a new character and the tutorial." if ch == 1 else "")
        + "</p>\n"
    )
    if intro:
        body.append(t.section("Overview", intro, "overview"))
    body.append(
        t.section(
            "Parts",
            "<ol>"
            + "".join(f"<li>{part_link(ctx, p, number=False)}</li>" for p in parts)
            + "</ol>\n",
            "parts",
        )
    )
    for p in parts:
        body.append(part_section(ctx, w, p))
    craft = crafting_section(ctx, ch)
    if craft:
        body.append(craft)
    body.append(missable_section(ctx, w, ch))
    story = ctx.prose(f"{SECTION_ID}/chapter-{ch}-story.md")
    if story:
        body.append(
            t.section(
                "Story",
                f'<details class="spoiler"><summary>Story of chapter {ch}</summary>\n'
                f"{story}</details>\n",
                "story",
            )
        )
    nav = []
    for other in (ch - 1, ch + 1):
        if other in CHAPTERS and any(p.chapter == other for p in w.parts):
            nav.append(f'<a href="{ctx.url(chapter_path(other))}">Chapter {other}</a>')
    nav.append(f'<a href="{ctx.url(ENDINGS)}">Endings</a>')
    nav.append(f'<a href="{ctx.url(CHECKLIST)}">Checklist</a>')
    nav.append(f'<a href="{ctx.url(qs.chapter_path(ch))}">Quest chain of chapter {ch}</a>')
    body.append(t.see_also(nav))
    return Page(
        chapter_path(ch),
        f"Chapter {ch}",
        "\n".join(body),
        description=f"Walkthrough of chapter {ch}",
    )


# --- checklist ------------------------------------------------------------------------------


def last_chance(ctx: Context, ch: int) -> str:
    end = chapter_end_of(ctx, ch)
    if end is None:
        return t.esc("never lost")
    label = "the end of the tutorial" if ch == 0 else f"the end of chapter {ch}"
    return f'<a href="{ctx.url(ENDINGS)}#ending-{t.anchor_id(str(end["id"]))}">{t.esc(label)}</a>'


def _chapter_cell(ch: int) -> t.Cell:
    return t.Cell(t.esc("tutorial" if ch == 0 else str(ch)), ch)


def checklist_page(ctx: Context, w: Walk) -> Page:
    story = set(w.order)
    body = [
        '<p class="lead">Everything the game lets a player miss, with the last point it can be'
        " done. A chapter end resets every quest state, so the end of a quest's chapter is its"
        f" last chance; chapter {last_chapter(w)} has no chapter end.</p>\n"
    ]
    prose = ctx.prose(f"{SECTION_ID}/checklist.md")
    if prose:
        body.append(prose)
    # quests
    rows = []
    for q in qs.quests_in_order(ctx):
        qid = str(q["id"])
        p = w.part_of.get(qid)
        rows.append(
            [
                t.markup_cell(ctx.ref("quest", qid), name_of(q)),
                _chapter_cell(quest_chapter(q)),
                t.text_cell("Story" if qid in story else "Side"),
                t.markup_cell(
                    part_link(ctx, p) if p else t.esc("not on the story's path"),
                    PARTS.index(p) if p in PARTS else 99,
                ),
                t.markup_cell(
                    t.esc("not missable (story)")
                    if qid in story
                    else last_chance(ctx, quest_chapter(q))
                ),
            ]
        )
    cols = [
        t.Column("Quest"),
        t.Column("Chapter", numeric=True),
        t.Column("Path"),
        t.Column("Opens in"),
        t.Column("Last chance"),
    ]
    body.append(t.section("Quests", t.table(cols, rows, filter=True), "quests"))
    # bosses
    rows = []
    for mon in ctx.kb.entities("monster"):
        if not mons.is_boss(mon):
            continue
        mid = str(mon["id"])
        hunts = [
            str(r["id"])
            for r in _refs(mon.get("hunted_by_quests"))
            if ctx.kb.get("quest", str(r["id"]))
        ]
        if not hunts:
            continue
        qid = hunts[0]
        q = ctx.kb.get("quest", qid) or {}
        req = qid in story
        rows.append(
            [
                t.markup_cell(ctx.ref("monster", mid), name_of(mon)),
                t.markup_cell(", ".join(mons.where(ctx, m) for m in mons.boss_maps(ctx, mon))),
                t.markup_cell(ctx.ref("quest", qid), name_of(q)),
                t.text_cell("Story" if req else "Side"),
                t.markup_cell(
                    t.esc("not missable (story)") if req else last_chance(ctx, quest_chapter(q))
                ),
            ]
        )
    cols = [
        t.Column("Boss"),
        t.Column("Where"),
        t.Column("Quest"),
        t.Column("Path"),
        t.Column("Last chance"),
    ]
    body.append(t.section("Bosses", t.table(cols, rows, filter=False), "bosses"))
    # recipes and trades
    rows = []
    for kind, words in (("recipe", "Recipe"), ("trade", "Trade")):
        es = ctx.kb.entities(kind)
        later: dict[str, set[int]] = {}
        for e in es:
            later.setdefault(
                str(e.get("result_item") if kind == "recipe" else _trade_key(e)), set()
            ).add(_num(e.get("chapter")))
        for e in es:
            ch = _num(e.get("chapter"))
            key = str(e.get("result_item") if kind == "recipe" else _trade_key(e))
            again = sorted(c for c in later.get(key, set()) if c > ch)
            who = _refs(e.get("crafted_by") if kind == "recipe" else e.get("offered_by"))
            res = _refs(e.get("result"))
            res_m = d.link_or_text(ctx, "item", str(res[0]["id"])) if res else t.esc(name_of(e))
            last = t.esc(f"comes back in chapter {again[0]}") if again else last_chance(ctx, ch)
            rows.append(
                [
                    t.markup_cell(ctx.ref(kind, str(e["id"]), words), words),
                    t.markup_cell(res_m, name_of(e)),
                    t.markup_cell(qs.join([qs.npc_at(ctx, o) for o in who[:1]])),
                    _chapter_cell(ch),
                    t.markup_cell(last, 0 if again else 1),
                ]
            )
    cols = [
        t.Column("Kind"),
        t.Column("Result"),
        t.Column("Where"),
        t.Column("Chapter", numeric=True),
        t.Column("Last chance"),
    ]
    body.append(
        t.section(
            "Recipes and trades",
            _p(
                "A recipe or trade belongs to its chapter. One that the next chapter offers again"
                " (same result, same crafter or trader) is not lost at the chapter end."
            )
            + t.table(cols, rows, filter=True),
            "recipes-and-trades",
        )
    )
    # items only quests give
    rows = []
    for it in ctx.kb.entities("item"):
        iid = str(it["id"])
        srcs = _only_quest_sources(ctx, iid)
        gifts = _refs(it.get("given_by_npcs"))
        if not srcs and not gifts:
            continue
        if not srcs and (
            any(float(r.get("percent") or 0) > 0 for r in _refs(it.get("dropped_by")))
            or _refs(it.get("sold_by"))
            or _refs(it.get("crafted_by"))
            or _refs(it.get("traded_for_in"))
            or _refs(it.get("rewarded_by_quests"))
        ):
            continue
        chs = []
        where = []
        for s in srcs:
            q = ctx.kb.get("quest", s)
            if q is not None:
                chs.append(quest_chapter(q))
                where.append(ctx.ref("quest", s))
        for g in gifts:
            where.append(qs.npc_at(ctx, g))
            ch_g = [c for c in (g.get("chapters") or []) if isinstance(c, int)]
            chs += ch_g
        on_story = any(s in story for s in srcs)
        last_ch = max(chs) if chs else 3
        rows.append(
            [
                t.markup_cell(ctx.ref("item", iid), name_of(it)),
                t.markup_cell(qs.join(where, "or")),
                t.text_cell("Story" if on_story else "Side"),
                t.markup_cell(
                    t.esc("not missable (story)") if on_story else last_chance(ctx, last_ch)
                ),
            ]
        )
    cols = [t.Column("Item"), t.Column("Only from"), t.Column("Path"), t.Column("Last chance")]
    body.append(
        t.section(
            "Items only a quest or an NPC gives",
            _p(
                "No monster drops these, no shop sells them and no recipe or trade makes them:"
                " each comes from one quest's reward or one NPC's gift. A drop rarer than"
                f" {t.esc(t.number(RARE))}% a kill does not count as a source here."
            )
            + t.table(cols, rows, filter=True),
            "items",
        )
    )
    return Page(
        CHECKLIST, "Checklist", "\n".join(body), description="Everything that can be missed"
    )


def _trade_key(e: Entity) -> str:
    who = ",".join(str(o["id"]) for o in _refs(e.get("offered_by")))
    cost = ",".join(str(c["id"]) for c in _refs(e.get("cost")))
    res = ",".join(str(c["id"]) for c in _refs(e.get("result")))
    return f"{who}:{cost}:{res}"


# --- index ------------------------------------------------------------------------------------


def index_page(ctx: Context, w: Walk) -> Page:
    body = [
        '<p class="lead">The story of MapleStory Thief Edition from a new character to the'
        " ending, in three chapters and their parts. Each part names its goal, the way there,"
        " the quests, the level to aim for, the shops on the way and the boss.</p>\n"
    ]
    prose = ctx.prose(f"{SECTION_ID}/index.md")
    if prose:
        body.append(prose)
    rows = []
    for p in w.parts:
        story_lv, acc_lv, _ = part_levels(ctx, w, p)
        bosses = part_bosses(ctx, w, p)
        maps = route_maps(ctx, w, p)
        areas: list[str] = []
        for m in maps:
            for a in _refs((ctx.kb.get("map", m) or {}).get("area")):
                if str(a["id"]) not in areas and ctx.kb.get("area", str(a["id"])):
                    areas.append(str(a["id"]))
        rows.append(
            [
                t.markup_cell(part_link(ctx, p), PARTS.index(p)),
                t.num_cell(p.chapter),
                t.markup_cell(", ".join(ctx.ref("area", a) for a in areas)),
                t.num_cell(len(w.story.get(p.slug, []))),
                t.Cell(t.esc(level_cell(story_lv, acc_lv)), story_lv),
                t.markup_cell(", ".join(ctx.ref("monster", m) for m in bosses) or t.esc("none")),
            ]
        )
    cols = [
        t.Column("Part"),
        t.Column("Chapter", numeric=True),
        t.Column("Areas"),
        t.Column("Story quests", numeric=True),
        t.Column("Level", "Story level to no-miss level", numeric=True),
        t.Column("Boss"),
    ]
    body.append(
        t.section(
            "Chapters and parts",
            "<ul>"
            + "".join(
                f'<li><a href="{ctx.url(chapter_path(ch))}">Chapter {ch}</a></li>'
                for ch in CHAPTERS
                if any(p.chapter == ch for p in w.parts)
            )
            + f'<li><a href="{ctx.url(ENDINGS)}">Endings</a></li>'
            + f'<li><a href="{ctx.url(CHECKLIST)}">'
            + "Checklist of everything that can be missed</a></li>"
            + "</ul>\n"
            + _p(
                "The level column is the compendium's estimate: the first number is the level a"
                " hero who does only the main story reaches by the end of the part, the second"
                " the level whose base ACC matches the strongest monster the part sends the hero"
                ' to (<a href="' + ctx.url("mechanics/accuracy.html") + '">accuracy</a>).'
            )
            + t.table(cols, rows, filter=False),
            "chapters-and-parts",
        )
    )
    items = []
    for qid in w.order:
        q = ctx.kb.get("quest", qid) or {}
        p = w.part_of.get(qid)
        extra = "" if q.get("main_story") else " (side quest the story needs)"
        items.append(
            f"<li>{ctx.ref('quest', qid)}{t.esc(extra)}"
            + (f", {part_link(ctx, p)}" if p else "")
            + "</li>"
        )
    body.append(
        t.section(
            "Main story checklist",
            _p(
                f"The {t.esc(t.number(len(w.order)))} quests of the main story in an order that"
                " meets every quest's prerequisites: each quest's giver offers it once every"
                " quest above it that it needs has been accepted or done."
            )
            + '<ol class="checklist">'
            + "".join(items)
            + "</ol>\n",
            "main-story",
        )
    )
    return Page(INDEX, "Walkthrough", "\n".join(body), description="The story, chapter by chapter")


def pages(ctx: Context) -> Iterator[Page]:
    w = walk(ctx)
    yield index_page(ctx, w)
    for ch in CHAPTERS:
        if any(p.chapter == ch for p in w.parts):
            yield chapter_page(ctx, w, ch)
    yield endings_page(ctx)
    yield checklist_page(ctx, w)


SECTION = Section(
    SECTION_ID,
    "Walkthrough",
    60,
    (),
    pages,
    blurb="The story from a new character to the ending, part by part.",
    image="npc:25",
)
