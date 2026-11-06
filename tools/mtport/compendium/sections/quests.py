"""The quests section of the compendium (#223): a page per quest, the quest lists and every
conversation of the game.

Pages (all under ``quests/``):

- ``<quest id>.html``: one per quest. Infobox (title in English and Korean, chapter, main
  story or side quest, giver and map, level and job), then Prerequisites, Requirements (each
  item with where to get it, each monster with where it lives, meso), Rewards, How to
  complete (numbered steps generated from the knowledge base in the project's words, or the
  hand-written ``docs/compendium/pages/quests/<quest id>.md`` when there is one), Dialogue
  (the conversation parts that belong to the quest, :mod:`._talk`), Follow-up and Notes;
- ``index.html`` (every quest by chapter, in story order), ``chapter-<n>.html`` (a chapter's
  quests with its quest chain diagram, our own SVG of the prerequisite graph),
  ``by-giver.html``, ``by-area.html``, ``by-level.html``;
- ``dialogue/<dialogue id>.html``: one per conversation (``dialogue`` entity): its NPC and
  maps, a table of all its parts with the page each lives on, the parts that belong to no
  quest (greetings, refusals, portal messages), and any other text of the script (quest
  titles of crafting and trading orders, lines no path shows). NPC pages link here with
  ``ctx.ref("dialogue", id)``; ``dialogue/index.html`` lists them all.

Every text key of every conversation appears on exactly one page, in both languages, on an
element with ``data-key`` (``tests/test_compendium_quests.py``): a quest's title and
description in its infobox, a line in the one part that holds it, anything else under
"Other text" of its conversation. A line that a second conversation repeats links to the
first.
"""

from __future__ import annotations

import html
import re
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass, field
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.sections import _talk
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of, slug, verbatim

SECTION_ID = "quests"
BUG_11 = (
    "https://github.com/armaatus/maplethief-port/blob/main/docs/re/original-bugs.md"
    "#11-ok-on-a-quest-pages-a-quest-it-never-stored"
)
STATES = {
    frozenset({0}): "is not started",
    frozenset({1}): "is in progress",
    frozenset({2}): "is done",
    frozenset({0, 1}): "is not done",
    frozenset({1, 2}): "has been accepted",
    frozenset({0, 2}): "is not in progress",
}
KIND_WORDS = {"main": "main quest", "sub": "side quest"}


def quest_path(qid: str) -> str:
    return f"{SECTION_ID}/{slug(qid)}.html"


def dialogue_path(did: str) -> str:
    return f"{SECTION_ID}/dialogue/{slug(did)}.html"


def chapter_path(ch: int) -> str:
    return f"{SECTION_ID}/chapter-{ch}.html"


def chapter_name(ch: int) -> str:
    return "Tutorial (chapter 0)" if ch == 0 else f"Chapter {ch}"


def _refs(v: Any) -> list[Mapping[str, Any]]:
    if isinstance(v, Mapping):
        return [v]
    return [r for r in v or [] if isinstance(r, Mapping) and "kind" in r and "id" in r]


def _num(v: Any) -> int:
    return int(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 0


# --- the model of the whole section -----------------------------------------------------------


@dataclass
class Model:
    """Every conversation as parts, where each part and each text key lives, and the quests
    in story order. Built once per context."""

    talks: dict[str, _talk.Talk] = field(default_factory=dict)
    home: dict[tuple[str, int], str] = field(default_factory=dict)  # part -> page path
    owner: dict[tuple[str, int], str | None] = field(default_factory=dict)  # part -> quest
    keys: dict[str, tuple[str, str]] = field(default_factory=dict)  # key -> (page, anchor)
    key_where: dict[str, str] = field(default_factory=dict)  # key -> "part 3 of <id>" words
    other: dict[str, list[str]] = field(default_factory=dict)  # dialogue -> other keys
    quest_parts: dict[str, list[tuple[str, int]]] = field(default_factory=dict)
    gifts: dict[str, list[tuple[str, int]]] = field(default_factory=dict)  # item -> parts
    chapter_ends: dict[int, str] = field(default_factory=dict)  # chapter -> event id


def quest_id(ctx: Context, kind: int, number: int, chapters: Iterable[int]) -> str | None:
    """The quest (kind 0 main, 1 side; number) a script running in ``chapters`` means."""
    for ch in chapters:
        qid = f"{ch}-{'main' if kind == 0 else 'sub'}-{number}"
        if ctx.kb.get("quest", qid) is not None:
            return qid
    return None


def _chapters(e: Entity) -> list[int]:
    return [int(c) for c in e.get("chapters") or [] if isinstance(c, int)]


def model(ctx: Context) -> Model:
    cached = getattr(ctx, "_quests_model", None)
    if cached is not None:
        return cached
    m = Model()
    for ev in ctx.kb.entities("event"):
        if ev.get("type") == "chapter_end":
            for ch in _chapters(ev):
                m.chapter_ends.setdefault(ch, str(ev["id"]))
    for q in quests_in_order(ctx):
        qid = str(q["id"])
        page = quest_path(qid)
        for k in (q.get("title_key"), q.get("description_key")):
            if isinstance(k, str) and k and k not in m.keys:
                m.keys[k] = (page, "")
                m.key_where[k] = f"quest:{qid}"
        m.quest_parts[qid] = []
    for e in ctx.kb.entities("dialogue"):
        did = str(e["id"])
        talk = _talk.build(e)
        m.talks[did] = talk
        chapters = _chapters(e) or [0]
        for p in talk.parts:
            qid = quest_id(ctx, *p.owner, chapters) if p.owner else None
            m.owner[(did, p.number)] = qid
            m.home[(did, p.number)] = quest_path(qid) if qid else dialogue_path(did)
            if qid:
                m.quest_parts[qid].append((did, p.number))
            for ln in p.lines:
                if ln.key not in m.keys:
                    m.keys[ln.key] = (m.home[(did, p.number)], p.anchor)
                    m.key_where[ln.key] = f"part:{did}:{p.number}"
            for name, args in p.calls:
                ints = [a for a in args if isinstance(a, int)]
                if name == "getItem" and len(ints) >= 3:
                    m.gifts.setdefault(item_id(ints[0], ints[1]), []).append((did, p.number))
    for e in ctx.kb.entities("dialogue"):
        did = str(e["id"])
        rest = [k for k in sorted(e.get("text") or {}) if k not in m.keys]
        for k in rest:
            m.keys[k] = (dialogue_path(did), "other-text")
        m.other[did] = rest
    ctx._quests_model = m  # type: ignore[attr-defined]
    return m


def quests_in_order(ctx: Context) -> list[Entity]:
    """Story order: by chapter, main quests before side quests, then by number."""
    qs = list(ctx.kb.entities("quest"))
    return sorted(
        qs,
        key=lambda q: (
            _num(q.get("chapter")),
            0 if q.get("quest_kind") == "main" else 1,
            _num(q.get("number")),
            str(q["id"]),
        ),
    )


# --- small phrases ----------------------------------------------------------------------------


def _shared_name(ctx: Context, mp: Entity) -> bool:
    """Whether another map has the same English name (the game falls back to a town's name
    for maps past the end of a name table)."""
    counts = getattr(ctx, "_quests_map_names", None)
    if counts is None:
        counts = {}
        for e in ctx.kb.entities("map"):
            counts[name_of(e)] = counts.get(name_of(e), 0) + 1
        ctx._quests_map_names = counts  # type: ignore[attr-defined]
    return counts.get(name_of(mp), 0) > 1


def map_phrase(ctx: Context, mid: str, number: bool = False) -> str:
    """``Kerning City Village`` linked, with its area when the area's name differs, and with
    ``number`` its map number (``map 4.10``) too."""
    mp = ctx.kb.get("map", mid)
    if mp is None:
        return t.esc(f"map {mid.replace('-', '.')}")
    extra = []
    for a in _refs(mp.get("area")):
        area = ctx.kb.get("area", str(a["id"]))
        if area is not None and name_of(area) not in name_of(mp):
            extra.append(ctx.ref("area", str(a["id"])))
    if number or _shared_name(ctx, mp):
        extra.append(t.esc(f"map {mid.replace('-', '.')}"))
    return ctx.ref("map", mid) + (f" ({', '.join(extra)})" if extra else "")


def maps_phrases(ctx: Context, mids: list[str]) -> list[str]:
    """:func:`map_phrase` of each map, with the map number where two share a name."""
    names = [name_of(ctx.kb.get("map", x) or {"id": x}) for x in mids]
    out = []
    for x, n in zip(mids, names, strict=True):
        out.append(map_phrase(ctx, x, number=names.count(n) > 1))
    return out


def npc_at(ctx: Context, ref: Mapping[str, Any]) -> str:
    """An NPC with the map it stands on in that reference (``npc`` relations carry ``map``),
    and the tile of the one meant when the NPC stands on that map more than once (the
    reference's ``script`` names the object's script)."""
    who = d.link_or_text(ctx, "npc", str(ref["id"]))
    mid = ref.get("map")
    if not mid:
        return who
    out = f"{who} in {map_phrase(ctx, str(mid))}"
    npc = ctx.kb.get("npc", str(ref["id"]))
    spots = [r for r in _refs(npc.get("stands_on") if npc else None) if r["id"] == mid]
    script = str(ref.get("script") or "")
    base = script.split("-", 1)[1] if "-" in script else script
    if len(spots) > 1:
        for r in spots:
            tile = r.get("tile")
            if r.get("script") == base and isinstance(tile, list) and len(tile) == 2:
                out += t.esc(f" at tile {tile[0]}, {tile[1]}")
    return out


def join(parts: list[str], word: str = "and") -> str:
    parts = [p for p in parts if p]
    if len(parts) <= 2:
        return f" {word} ".join(parts)
    return ", ".join(parts[:-1]) + f", {word} " + parts[-1]


def quest_link(ctx: Context, qid: str | None, here: str | None, fallback: str) -> str:
    if qid is None:
        name, args = _talk.parse_term(fallback)
        orders = _orders(ctx)
        if len(args) == 2 and tuple(args) in orders:
            return orders[tuple(args)]
        if len(args) == 2 and all(isinstance(a, int) for a in args):
            kind = "main" if args[0] == 0 else "side"
            return f"{kind} quest {args[1]} (which no script of the chapter prepares)"
        return f"<code>{t.esc(fallback)}</code>"
    if qid == here:
        return "this quest"
    return ctx.ref("quest", qid)


def _orders(ctx: Context) -> dict[tuple[int, int], str]:
    """The transient quests of crafting and trading (``recipe.quest``, ``trade.quest``)."""
    cached = getattr(ctx, "_quests_orders", None)
    if cached is not None:
        return cached
    out: dict[tuple[int, int], str] = {}
    for kind, words in (("recipe", "the crafting order"), ("trade", "the trade order")):
        for e in ctx.kb.entities(kind):
            q = e.get("quest")
            if isinstance(q, list) and len(q) == 2 and all(isinstance(x, int) for x in q):
                out.setdefault((q[0], q[1]), words)
    ctx._quests_orders = out  # type: ignore[attr-defined]
    return out


def level_phrase(vals: frozenset[int]) -> str:
    lo, hi = min(vals), max(vals)
    if hi >= 99:
        return f"Robin is level {lo} or higher"
    if lo <= 1:
        return f"Robin is below level {hi + 1}"
    return f"Robin is level {lo} to {hi}"


def cond_phrases(
    ctx: Context, when: Mapping[str, frozenset[int]], chapters: list[int], here: str | None
) -> list[str]:
    """Each necessary condition in words (markup)."""
    out = []
    for term in sorted(when):
        vals = frozenset(when[term])
        name, args = _talk.parse_term(term)
        if name == "getQuestState" and len(args) == 2:
            qid = quest_id(ctx, int(args[0]), int(args[1]), chapters)
            state = STATES.get(vals, "is in state " + join([str(v) for v in sorted(vals)], "or"))
            out.append(f"{quest_link(ctx, qid, here, term)} {state}")
        elif name == "checkRequire" and len(args) == 2:
            qid = quest_id(ctx, int(args[0]), int(args[1]), chapters)
            what = quest_link(ctx, qid, here, term)
            met = "are met" if vals == {1} else "are not met"
            out.append(f"the requirements of {what} {met}")
        elif name == "getLevel":
            out.append(t.esc(level_phrase(vals)))
        elif name == "getClassUpCount":
            lo, hi = min(vals), max(vals)
            if vals == {0}:
                out.append("before the first job advancement")
            elif hi >= 4:
                n = "one job advancement" if lo == 1 else f"{lo} job advancements"
                out.append(f"after at least {n}")
            else:
                out.append(
                    t.esc(f"with {join([str(v) for v in sorted(vals)], 'or')} job advancements")
                )
        elif name == "isInvenAvailable":
            out.append("the inventory has room" if vals == {1} else "the inventory is full")
        else:
            out.append(
                f"<code>{t.esc(term)}</code> is {t.esc(join([str(v) for v in sorted(vals)], 'or'))}"
            )
    return out


def item_id(kind: int, number: int) -> str:
    return f"{'equip' if kind == 0 else 'etc'}:{number}"


def stage_phrases(
    ctx: Context, part: _talk.Part, did: str, chapters: list[int], here: str | None
) -> list[str]:
    """What the natives called during a part do, in words (markup)."""
    out: list[str] = []
    calls = [c for c, _ in part.calls]
    for name, args in part.calls:
        ints = [a for a in args if isinstance(a, int)]
        q = quest_id(ctx, ints[0], ints[1], chapters) if len(ints) >= 2 else None
        text = ""
        if name == "setQuest" and q:
            text = f"Offers {quest_link(ctx, q, here, '')}."
        elif name == "startQuest" and q:
            text = f"{_cap(quest_link(ctx, q, here, ''))} starts."
        elif name == "getReward" and q:
            text = f"{_cap(quest_link(ctx, q, here, ''))} ends and gives its rewards."
        elif name == "regMsgReward" and q and "getReward" not in calls:
            text = f"Shows the rewards of {quest_link(ctx, q, here, '')}."
        elif name == "getItem" and len(ints) >= 3:
            iid = item_id(ints[0], ints[1])
            if ctx.kb.get("item", iid) is not None:
                text = f"Robin receives {t.esc(t.number(ints[2]))} {ctx.ref('item', iid)}."
        elif name == "showMarket" and ints:
            if ctx.kb.get("shop", str(ints[0])) is not None:
                text = f"Opens {ctx.ref('shop', str(ints[0]))}."
        elif name == "changeMap" and len(ints) >= 2:
            mid = f"{ints[0]}-{ints[1]}"
            if ctx.kb.get("map", mid) is not None:
                text = f"Moves Robin to {ctx.ref('map', mid)}."
        elif name == "setClassUp" and len(ints) >= 2:
            ja = _job_advancement(ctx, ints, chapters)
            text = f"Gives {ctx.ref('job_advancement', ja, 'a job advancement')}." if ja else ""
            text = text or "Gives a job advancement."
        elif name == "makeEquipItem":
            text = "Makes the equipment, with random stats."
        elif name == "invenFullNotify":
            text = "Says the inventory is full."
        elif name in ("startBoss", "runAnimation", "clearAll", "initMonCa"):
            kind = {
                "startBoss": "boss",
                "runAnimation": "scene",
                "clearAll": "chapter_end",
                "initMonCa": "carnival",
            }[name]
            eid = "-".join([did, kind] + [str(a) for a in ints])
            words = {
                "boss": "The boss fight starts",
                "scene": "A scene plays",
                "chapter_end": "The chapter ends, and every quest state is reset",
                "carnival": "Monster Carnival starts",
            }[kind]
            if ctx.kb.get("event", eid) is not None:
                text = f"{words} ({ctx.ref('event', eid, 'event')})."
            else:
                text = f"{words}."
        if text and text not in out:
            out.append(text)
    return out


def _job_advancement(ctx: Context, ints: list[int], chapters: list[int]) -> str | None:
    job, n = ints[0], ints[1]
    for ch in chapters:
        for jid in (f"{ch}-{n}-{job}", f"{ch}-{n}-same"):
            if ctx.kb.get("job_advancement", jid) is not None:
                return jid
    return None


def _cap(markup: str) -> str:
    """Capitalise a phrase that starts with plain text ("this quest" gives "This quest")."""
    return markup[:1].upper() + markup[1:] if markup[:1].isalpha() else markup


# --- conversations -------------------------------------------------------------------------------

TOGGLE = (
    '<p class="lang-toggle" hidden>Show: '
    '<button type="button" data-lang="en">English</button> '
    '<button type="button" data-lang="ko">Korean</button> '
    '<button type="button" data-lang="both" aria-pressed="true">Both</button></p>'
)


def dialogue_block(inner: str, label: str) -> str:
    """A block of conversation with one English/Korean toggle (the site script's)."""
    return (
        f'<div class="dialogue talk" data-show="both" role="group" aria-label="{t.esc(label)}">'
        f"\n{TOGGLE}\n{inner}</div>\n"
    )


def text_spans(text: Mapping[str, Any] | None, key: str) -> str:
    """The English and Korean text of ``key``; a missing language says so in our words."""
    en = text.get("en") if isinstance(text, Mapping) else None
    ko = text.get("ko") if isinstance(text, Mapping) else None
    out = (
        verbatim(str(en), "en", "span", "en")
        if en
        else f'<span class="en"><code>{t.esc(key)}</code> (no English text)</span>'
    )
    if ko:
        out += verbatim(str(ko), "ko", "span", "ko")
    else:
        out += '<span class="ko muted">The game has no Korean text for this line.</span>'
    return out


def speaker(ctx: Context, e: Entity, who: str) -> str:
    if who == "hero":
        return "Robin"
    if who == "choice":
        return "Choice"
    npcs = _refs(e.get("npcs"))
    if npcs:
        return d.link_or_text(ctx, "npc", str(npcs[0]["id"]))
    return "Message"


def part_url(ctx: Context, m: Model, did: str, number: int) -> str:
    """A link marker to part ``number`` of conversation ``did``, on whichever page holds it."""
    return ctx.url(m.home[(did, number)] + "#" + m.talks[did].part(number).anchor)


def chapter_link(ctx: Context, ch: int) -> str:
    return f'<a href="{ctx.url(chapter_path(ch))}">{t.esc(chapter_name(ch))}</a>'


def _key_label(key: str) -> str:
    """The part of a text key that names its use (``Script.function.3`` gives ``function``)."""
    return key.rsplit(".", 2)[-2] if key.count(".") >= 2 else key


def part_link(ctx: Context, m: Model, did: str, number: int) -> str:
    talk = m.talks[did]
    p = talk.part(number)
    label = f"part {number}"
    return f'<a href="{ctx.url(m.home[(did, number)] + "#" + p.anchor)}">{t.esc(label)}</a>'


def _again(ctx: Context, m: Model, key: str, home: tuple[str, str]) -> str:
    """A line shown elsewhere: a link to where it is, in our words."""
    url = ctx.url(home[0] + ("#" + home[1] if home[1] else ""))
    where = m.key_where.get(key, "")
    if where.startswith("part:"):
        _, did, n = where.split(":")
        e = ctx.kb.get("dialogue", did)
        conv = t.esc(name_of(e)) if e is not None else "another conversation"
        return f'Says the line of <a href="{url}">part {n} of {conv}</a> again'
    return f'Says a line shown <a href="{url}">elsewhere</a> again'


def render_part(ctx: Context, m: Model, e: Entity, part: _talk.Part, page: str) -> str:
    did = str(e["id"])
    chapters = _chapters(e) or [0]
    here = m.owner.get((did, part.number))
    text = e.get("text") or {}
    head = [f'<span class="part-no">Part {part.number}</span>']
    conds = cond_phrases(ctx, part.when, chapters, here)
    if conds:
        head.append(f'<span class="cond">When {join(conds)}.</span>')
    items = []
    for ln in part.lines:
        who = f'<span class="who">{speaker(ctx, e, ln.who)}</span>'
        cls = ' class="choice"' if ln.who == "choice" else ""
        home = m.keys.get(ln.key)
        if home is not None and home != (page, part.anchor):
            items.append(f'<li class="again">{who}{_again(ctx, m, ln.key, home)}</li>')
            continue
        items.append(
            f'<li{cls} data-key="{t.esc(ln.key)}">{who}{text_spans(text.get(ln.key), ln.key)}</li>'
        )
    stage = stage_phrases(ctx, part, did, chapters, here)
    leads = []
    groups: dict[str, list[str]] = {}
    for lab, n in part.nexts:
        groups.setdefault(lab, []).append(part_link(ctx, m, did, n))
    for lab, links in groups.items():
        what = "Choosing it leads to" if part.menu else (f"{lab} leads to" if lab else "Then")
        leads.append(f"{what} {join(links, 'or')}.")
    if part.ends:
        leads.append(
            "The conversation can end here." if part.nexts else "The conversation ends here."
        )
    if part.prevs:
        links = [part_link(ctx, m, did, n) for n in part.prevs]
        leads.append(f"Follows {join(links, 'or')}.")
    out = [
        f'<div class="part" id="{t.esc(part.anchor)}">',
        f'<p class="part-head">{" ".join(head)}</p>',
    ]
    out.append("<ol>" + "".join(items) + "</ol>")
    if stage:
        out.append(f'<p class="stage">{" ".join(stage)}</p>')
    if leads:
        out.append(f'<p class="leads">{" ".join(leads)}</p>')
    out.append("</div>")
    return "\n".join(out) + "\n"


# --- quest pages -------------------------------------------------------------------------------


RARE = 5.0  # percent: a drop rarer than this comes after shops, recipes and trades


def _item_sources(ctx: Context, iid: str, limit: int = 3) -> list[str]:
    """:func:`_sources` with good drops first, then shops, recipes, trades, quest rewards
    and conversations, then drops rarer than :data:`RARE` percent."""
    good, other, rare = _sources(ctx, iid, limit)
    return good + other + rare


def _sources(ctx: Context, iid: str, limit: int = 3) -> tuple[list[str], list[str], list[str]]:
    """Where to get an item, best first: drops (with real odds and maps), shops (keeper and
    map), recipes, trades and quest rewards. Each entry is a phrase (markup)."""
    it = ctx.kb.get("item", iid)
    if it is None:
        return [], [], []
    good: list[str] = []
    rare: list[str] = []
    out: list[str] = []
    drops = sorted(
        _refs(it.get("dropped_by")), key=lambda r: (-float(r.get("percent") or 0), str(r["id"]))
    )
    for r in drops[:limit]:
        mon = ctx.kb.get("monster", str(r["id"]))
        where = _monster_maps(ctx, str(r["id"]), 2) if mon is not None else ""
        pct = t.percent_text(float(r.get("percent") or 0))
        phrase = f"{ctx.ref('monster', str(r['id']))} drops it ({t.esc(pct)} a kill)"
        to = good if float(r.get("percent") or 0) >= RARE else rare
        to.append(phrase + (f" on {where}" if where else ""))
    if len(drops) > limit:
        rest = len(drops) - limit
        rare.append(f"{rest} other monster{'s' if rest > 1 else ''} drop it")
    for r in _refs(it.get("sold_by")):
        shop = ctx.kb.get("shop", str(r["id"]))
        if shop is None:
            continue
        price = f" for {t.esc(t.number(_num(r.get('price'))))} meso" if r.get("price") else ""
        openers = [npc_at(ctx, o) + _chapter_note(o) for o in _refs(shop.get("opened_by"))]
        if openers:
            keeper = f" ({join(openers, 'or')})"
            out.append(f"{ctx.ref('shop', str(r['id']))} sells it{price}{keeper}")
        else:
            rare.append(
                f"{ctx.ref('shop', str(r['id']))} lists it{price}, but no NPC opens that shop"
            )
    for r in _refs(it.get("crafted_by")):
        rec = ctx.kb.get("recipe", str(r["id"]))
        if rec is None:
            continue
        who = [npc_at(ctx, o) for o in _refs(rec.get("crafted_by"))]
        out.append(
            f"{ctx.ref('recipe', str(r['id']), 'a recipe')} makes it"
            + (f" ({join(who, 'or')})" if who else "")
        )
    for r in _refs(it.get("traded_for_in")):
        tr = ctx.kb.get("trade", str(r["id"]))
        if tr is None:
            continue
        who = [npc_at(ctx, o) for o in _refs(tr.get("offered_by"))]
        out.append(
            f"{ctx.ref('trade', str(r['id']), 'a trade')} gives it"
            + (f" ({join(who, 'or')})" if who else "")
        )
    for r in _refs(it.get("rewarded_by_quests")):
        out.append(f"the quest {ctx.ref('quest', str(r['id']))} gives it as a reward")
    m = model(ctx)
    seen: list[str] = []
    for did, n in m.gifts.get(iid, []):
        e = ctx.kb.get("dialogue", did)
        if e is None:
            continue
        q = m.owner.get((did, n))
        where = f"while {ctx.ref('quest', q)} goes on" if q else "in conversation"
        phrase = f"{_talk_with(ctx, e)} hands it over {where}"
        link = f' (<a href="{part_url(ctx, m, did, n)}">part {n}</a>)'
        if phrase not in seen:
            seen.append(phrase)
            out.append(phrase + link)
    return good, out, rare


def _chapter_note(ref: Mapping[str, Any]) -> str:
    chs = [c for c in ref.get("chapters") or [] if isinstance(c, int)]
    if not chs or chs == [1, 2, 3]:
        return ""
    if len(chs) == 1:
        return f", chapter {chs[0]}"
    return ", chapters " + join([str(c) for c in chs])


def _monster_maps(ctx: Context, mid: str, limit: int = 3) -> str:
    mon = ctx.kb.get("monster", mid)
    if mon is None:
        return ""
    spots = sorted(_refs(mon.get("spawns_on")), key=lambda r: (-_num(r.get("count")), str(r["id"])))
    seen: list[str] = []
    for r in spots:
        if str(r["id"]) not in seen:
            seen.append(str(r["id"]))
    shown = maps_phrases(ctx, seen[:limit])
    if len(seen) > limit:
        rest = len(seen) - limit
        shown.append(f"{rest} other map{'s' if rest > 1 else ''}")
    return join(shown)


def requirement_rows(ctx: Context, reqs: list[Mapping[str, Any]]) -> tuple[str, str, str]:
    """The tables of items and monsters a quest needs, and its meso, as markup."""
    items, mobs, meso = [], [], []
    for r in reqs:
        if r.get("type") == "item" and isinstance(r.get("item"), str):
            iid = r["item"]
            src = _item_sources(ctx, iid)
            where = (
                '<ul class="sources">' + "".join(f"<li>{_cap(x)}.</li>" for x in src) + "</ul>"
                if src
                else ("No drop, shop, recipe, trade or quest reward in the game's data gives it.")
            )
            items.append(
                [
                    d.entry_cell(ctx, "item", iid),
                    t.num_cell(_num(r.get("count"))),
                    t.markup_cell(where),
                ]
            )
        elif r.get("type") == "mob" and r.get("monster") is not None:
            mid = str(r["monster"])
            where = _monster_maps(ctx, mid, 6)
            mobs.append(
                [
                    d.entry_cell(ctx, "monster", mid),
                    t.num_cell(_num(r.get("count"))),
                    t.markup_cell(where or "No map spawns it."),
                ]
            )
        elif r.get("type") == "meso":
            meso.append(_num(r.get("amount")))
    it = (
        t.table(
            [t.Column("Item"), t.Column("Count", numeric=True), t.Column("Where to get it")], items
        )
        if items
        else ""
    )
    mo = (
        t.table(
            [t.Column("Monster"), t.Column("Count", numeric=True), t.Column("Where it lives")], mobs
        )
        if mobs
        else ""
    )
    me = f"<p>{t.esc(t.number(sum(meso)))} meso.</p>\n" if meso else ""
    return it, mo, me


def reward_table(ctx: Context, rewards: list[Mapping[str, Any]]) -> str:
    rows = []
    for r in rewards:
        typ = r.get("type")
        if typ == "exp":
            rows.append([t.markup_cell("EXP"), t.num_cell(_num(r.get("amount")))])
        elif typ == "meso":
            rows.append([t.markup_cell("Meso"), t.num_cell(_num(r.get("amount")), "meso")])
        elif typ == "item" and isinstance(r.get("item"), str):
            rows.append([d.entry_cell(ctx, "item", r["item"]), t.num_cell(_num(r.get("count")))])
    if not rows:
        return ""
    return t.table([t.Column("Reward"), t.Column("Amount", numeric=True)], rows)


def reward_summary(q: Entity) -> str:
    parts = []
    for r in q.get("rewards") or []:
        if r.get("type") == "exp":
            parts.append(f"{t.number(_num(r.get('amount')))} EXP")
        elif r.get("type") == "meso":
            parts.append(f"{t.number(_num(r.get('amount')))} meso")
    n = sum(1 for r in q.get("rewards") or [] if r.get("type") == "item")
    if n:
        parts.append(f"{n} item" + ("s" if n > 1 else ""))
    return ", ".join(parts)


def quest_level(q: Entity) -> int:
    nec = _talk.necessary(q.get("prerequisites") or [])
    lv = nec.get("getLevel()")
    return min(lv) if lv else 0


def quest_job(q: Entity) -> str:
    nec = _talk.necessary(q.get("prerequisites") or [])
    cu = nec.get("getClassUpCount()")
    if not cu:
        return ""
    lo = min(cu)
    if cu == {0}:
        return "Before the first job advancement"
    return (
        f"After {lo} job advancement" + ("s" if lo > 1 else "")
        if max(cu) >= 4
        else (f"{join([str(v) for v in sorted(cu)], 'or')} job advancements")
    )


def _prereq_terms(q: Entity) -> list[list[Mapping[str, Any]]]:
    return [list(alt) for alt in q.get("prerequisites") or []]


def prerequisite_markup(ctx: Context, q: Entity) -> tuple[list[str], list[str]]:
    """The conditions to start ``q`` (phrases, from what holds on every way to start it) and
    the items starting it hands over."""
    qid = str(q["id"])
    chapters = [_num(q.get("chapter"))]
    alts = _prereq_terms(q)
    nec = _talk.necessary(alts)
    phrases = cond_phrases(ctx, nec, chapters, qid)
    gifts = []
    for alt in alts[:1]:
        for c in alt:
            name, args = _talk.parse_term(str(c.get("term", "")))
            if name == "getItem" and len(args) >= 3:
                iid = item_id(int(args[0]), int(args[1]))
                if ctx.kb.get("item", iid) is not None:
                    gifts.append(f"{t.esc(t.number(int(args[2])))} {ctx.ref('item', iid)}")
    return phrases, gifts


def chained(ctx: Context, q: Entity, other: str) -> bool:
    """Whether ``q`` is offered in the conversation that finishes ``other``: a script that
    gives ``q`` also completes ``other``. Its "``other`` is in progress" condition is then
    read before the reward, so finishing ``other`` is what opens ``q``."""
    o = ctx.kb.get("quest", other)
    if o is None:
        return False
    gives = {str(g.get("script")) for g in _refs(q.get("given_by"))}
    ends = {str(g.get("script")) for g in _refs(o.get("completed_by"))}
    return bool(gives & ends)


def _open_prereqs(ctx: Context, q: Entity) -> list[str]:
    """The quests that must be in progress when ``q`` is offered."""
    out = []
    nec = _talk.necessary(_prereq_terms(q))
    for term, vals in sorted(nec.items()):
        name, args = _talk.parse_term(term)
        if name == "getQuestState" and len(args) == 2 and vals == {1}:
            o = quest_id(ctx, int(args[0]), int(args[1]), [_num(q.get("chapter"))])
            if o and o != str(q["id"]):
                out.append(o)
    return out


def _ancestors(ctx: Context, qid: str) -> set[str]:
    """Every quest ``qid`` needs, directly or through other quests (``quest.requires``)."""
    out: set[str] = set()
    todo = [qid]
    while todo:
        q = ctx.kb.get("quest", todo.pop())
        for r in _refs(q.get("requires") if q else None):
            if str(r["id"]) not in out:
                out.add(str(r["id"]))
                todo.append(str(r["id"]))
    return out


def _implied(ctx: Context, nec: Mapping[str, frozenset[int]], chapters: list[int]) -> set[str]:
    """Quest-state conditions a step can leave out: a quest that may be done (its allowed
    states include 2) and that another named quest needs, directly or not, is done by the
    time that one is."""
    named: dict[str, str] = {}
    for term in nec:
        name, args = _talk.parse_term(term)
        if name == "getQuestState" and len(args) == 2:
            q = quest_id(ctx, int(args[0]), int(args[1]), chapters)
            if q:
                named[term] = q
    out = set()
    for term, q in named.items():
        if 2 not in nec[term]:
            continue
        for other_term, other in named.items():
            if other_term != term and q in _ancestors(ctx, other):
                out.add(term)
    return out


def _portal_phrase(ctx: Context, mid: str, script_id: str) -> str:
    """A map, with the tile and destination of its portal whose script is ``script_id``."""
    out = map_phrase(ctx, mid)
    mp = ctx.kb.get("map", mid)
    base = script_id.split("-", 1)[1] if "-" in script_id else script_id
    for p in (mp or {}).get("portals") or []:
        if not isinstance(p, Mapping) or p.get("script") != base:
            continue
        tile = p.get("tile")
        if isinstance(tile, list) and len(tile) == 2:
            out += t.esc(f" (the portal at tile {tile[0]}, {tile[1]}")
            dest = [x.get("map") for x in p.get("targets") or [] if isinstance(x, Mapping)]
            dest = [x for x in dest if isinstance(x, str) and ctx.kb.get("map", x) is not None]
            out += f", to {map_phrase(ctx, dest[0])})" if dest else ")"
    return out


def before_steps(ctx: Context, q: Entity) -> str:
    """The first step: what to do before the giver offers the quest, in the second person."""
    qid = str(q["id"])
    chapters = [_num(q.get("chapter"))]
    nec = _talk.necessary(_prereq_terms(q))
    implied = _implied(ctx, nec, chapters)
    todo: list[str] = []
    for term in sorted(nec):
        vals = frozenset(nec[term])
        name, args = _talk.parse_term(term)
        if term in implied:
            continue
        if name == "getLevel" and max(vals) >= 99:
            todo.append(f"reach level {min(vals)}")
        elif name == "getQuestState" and len(args) == 2:
            other = quest_id(ctx, int(args[0]), int(args[1]), chapters)
            if other is None or other == qid:
                continue
            link = ctx.ref("quest", other)
            if vals == {2}:
                todo.append(f"finish {link}")
            elif vals == {1} and chained(ctx, q, other):
                continue  # offered as the other quest ends: the giver step says so
            elif vals == {1}:
                todo.append(f"accept {link} and leave it unfinished")
            elif vals == {1, 2}:
                todo.append(f"accept {link}")
            elif vals == {0}:
                todo.append(f"leave {link} unaccepted")
            elif vals == {0, 1}:
                todo.append(f"do not finish {link} yet")
        elif name == "getClassUpCount":
            lo = min(vals)
            if vals == {0}:
                todo.append("do not take a job advancement yet")
            elif max(vals) >= 4:
                todo.append(
                    f"take {'your first job advancement' if lo == 1 else f'{lo} job advancements'}"
                )
        elif name == "getLevel":
            todo.append(f"stay {level_phrase(vals).replace('Robin is ', '')}")
    if not todo:
        return ""
    return f"First, {join(todo)}."


def how_to_complete(ctx: Context, q: Entity) -> str:
    """Numbered steps in the second person, generated from the knowledge base (every step
    links), or the hand-written prose file when there is one."""
    qid = str(q["id"])
    prose = ctx.prose(f"{SECTION_ID}/{slug(qid)}.md")
    if prose:
        return prose
    steps: list[str] = []
    _pre, gifts = prerequisite_markup(ctx, q)
    first = before_steps(ctx, q)
    if first:
        steps.append(first)
    givers = _refs(q.get("given_by"))
    talk = [npc_at(ctx, g) for g in _uniq_npcs(givers)]
    after = [ctx.ref("quest", o) for o in _open_prereqs(ctx, q) if chained(ctx, q, o)]
    if talk:
        line = f"Talk to {join(talk, 'or')} and accept the quest."
        if after:
            line = (
                f"Finish {join(after)} with {join(talk, 'or')}, who offers this quest in the"
                " same conversation; accept it."
            )
        if gifts:
            line += f" You receive {join(gifts)}; keep room in your inventory."
        steps.append(line)
    reqs = list(q.get("requirements") or [])
    for r in reqs:
        if r.get("type") == "item" and isinstance(r.get("item"), str):
            iid = r["item"]
            src = _item_sources(ctx, iid, 1)
            s = f"Collect {t.esc(t.number(_num(r.get('count'))))} {ctx.ref('item', iid)}."
            if src:
                s += f" {_cap(src[0])}."
            steps.append(s)
        elif r.get("type") == "mob" and r.get("monster") is not None:
            mid = str(r["monster"])
            where = _monster_maps(ctx, mid, 2)
            s = f"Defeat {t.esc(t.number(_num(r.get('count'))))} {ctx.ref('monster', mid)}"
            steps.append(s + (f" on {where}." if where else "."))
        elif r.get("type") == "meso":
            steps.append(f"Have {t.esc(t.number(_num(r.get('amount'))))} meso with you.")
    enders = _refs(q.get("completed_by"))
    if enders:
        back = [npc_at(ctx, g) for g in _uniq_npcs(enders)]
        same = {str(g["id"]) for g in enders} <= {str(g["id"]) for g in givers}
        same_spot = {str(g.get("script")) for g in enders} <= {str(g.get("script")) for g in givers}
        if not reqs and same and same_spot:
            steps.append(f"Talk to {join(back, 'or')} again to finish the quest.")
        elif same and same_spot:
            steps.append(f"Return to {join(back, 'or')} to finish the quest.")
        else:
            steps.append(f"Go to {join(back, 'or')} to finish the quest.")
    else:
        steps.append(_no_turn_in_step(ctx, q))
    return '<ol class="steps">' + "".join(f"<li>{s}</li>" for s in steps) + "</ol>\n"


def chapter_end_for(ctx: Context, q: Entity) -> Entity | None:
    """The chapter-end event (``clearAll``) whose conditions need ``q`` in progress or done."""
    qid = str(q["id"])
    for ev in ctx.kb.entities("event"):
        if ev.get("type") != "chapter_end":
            continue
        for alt in ev.get("conditions") or []:
            for c in alt:
                name, args = _talk.parse_term(str(c.get("term", "")))
                if name == "getQuestState" and len(args) == 2:
                    if quest_id(ctx, int(args[0]), int(args[1]), _chapters(ev)) == qid:
                        return ev
    return None


def _no_turn_in_step(ctx: Context, q: Entity) -> str:
    """The last step of a quest no script completes: the chapter end it leads to, or why
    nothing follows."""
    ev = chapter_end_for(ctx, q)
    if ev is not None:
        eid = str(ev["id"])
        did = eid[: -len("-chapter_end")]
        m = model(ctx)
        talk = m.talks.get(did)
        at = ""
        if talk is not None:
            ends = [p for p in talk.parts if any(c[0] == "clearAll" for c in p.calls)]
            if ends:
                n = ends[0].number
                at = f' (<a href="{part_url(ctx, m, did, n)}">part {n}</a>)'
        others = cond_phrases(
            ctx, _talk.necessary(ev.get("conditions") or []), _chapters(ev), str(q["id"])
        )
        others = [o for o in others if not o.startswith("this quest")]
        when = f" once {join(others)}" if others else ""
        e = ctx.kb.get("dialogue", did)
        end = (
            f": the chapter ends ({ctx.ref('event', eid, 'chapter end')}) with this quest"
            " still open, and its listed rewards are never given."
        )
        if "portal" in (ev.get("trigger") or []):
            maps = [_portal_phrase(ctx, str(r["id"]), did) for r in _refs(ev.get("maps"))]
            where = f" on {join(maps, 'or')}" if maps else ""
            return f"Go through the portal{where}{when}{end}"
        who = _talk_with(ctx, e) if e is not None else "the NPC"
        return f"Talk to {who}{when}{at}{end}"
    if not q.get("rewards") and not q.get("requirements") and not _refs(q.get("unlocks")):
        return (
            "Nothing more is needed: the quest has no requirements, no rewards and no turn-in,"
            " and no later quest follows it."
        )
    return (
        "No conversation finishes this quest: no script calls getReward for it, so its"
        " rewards are never given (see Notes)."
    )


def quest_infobox(ctx: Context, m: Model, q: Entity) -> str:
    qid = str(q["id"])
    page = quest_path(qid)
    tk, dk = q.get("title_key"), q.get("description_key")
    mine = isinstance(tk, str) and m.keys.get(tk) == (page, "")
    attr_t = f' data-key="{t.esc(tk)}"' if mine else ""
    head = f"<span{attr_t}>{verbatim(name_of(q, 'en'), 'en')}</span>"
    sub = verbatim(name_of(q, "ko"), "ko")
    rows = [
        ("Type", "Main quest" if q.get("quest_kind") == "main" else "Side quest"),
        ("Story", "Main story" if q.get("main_story") else "Side story"),
        (
            "Chapter",
            chapter_link(ctx, _num(q.get("chapter"))),
        ),
        ("Giver", join([npc_at(ctx, g) for g in _uniq_npcs(q.get("given_by"))], "or")),
        ("Ends with", join([npc_at(ctx, g) for g in _uniq_npcs(q.get("completed_by"))], "or")),
        ("Level", t.esc(f"Lv. {quest_level(q)}") if quest_level(q) else ""),
        ("Job", t.esc(quest_job(q))),
        ("Rewards", t.esc(reward_summary(q))),
        ("ID", f"<code>{t.esc(qid)}</code>"),
    ]
    foot = ""
    desc = q.get("description")
    if isinstance(desc, Mapping) and (desc.get("en") or desc.get("ko")):
        attr = (
            f' data-key="{t.esc(dk)}"'
            if isinstance(dk, str) and m.keys.get(dk) == (page, "")
            else ""
        )
        foot = (
            f'<div class="ib-descs"{attr}>'
            + "".join(
                f'<p class="ib-desc">{verbatim(str(desc[k]), k)}</p>'
                for k in ("en", "ko")
                if desc.get(k)
            )
            + "</div>"
        )
    return t.infobox(head, rows, subtitle_markup=sub, footer_markup=foot)


def _uniq_npcs(refs: Any) -> list[Mapping[str, Any]]:
    out, seen = [], set()
    for r in _refs(refs):
        k = (str(r["id"]), r.get("map"))
        if k not in seen:
            seen.add(k)
            out.append(r)
    return out


def quest_lead(ctx: Context, q: Entity) -> str:
    kind = KIND_WORDS.get(str(q.get("quest_kind")), "quest")
    ch = _num(q.get("chapter"))
    where = "the tutorial (chapter 0)" if ch == 0 else f"chapter {ch}"
    givers = [npc_at(ctx, g) for g in _uniq_npcs(q.get("given_by"))]
    story = " of the main story" if q.get("main_story") and kind != "main quest" else ""
    out = f"{ctx.name('quest', str(q['id']))} is a {kind}{story} of {where}"
    out += f", given by {join(givers, 'or')}." if givers else "."
    return f'<p class="lead">{out}</p>\n'


def quest_page(ctx: Context, m: Model, q: Entity) -> Page:
    qid = str(q["id"])
    page = quest_path(qid)
    chapters = [_num(q.get("chapter"))]
    body = [quest_infobox(ctx, m, q), quest_lead(ctx, q)]
    # Prerequisites
    pre, gifts = prerequisite_markup(ctx, q)
    alts = _prereq_terms(q)
    pre_parts = []
    if pre:
        pre_parts.append(f"<p>The quest is offered when {join(pre)}.</p>")
    elif alts == [[]] or not alts:
        pre_parts.append("<p>None: the quest is offered from the start of its chapter.</p>")
    if len(alts) > 1:
        lis = []
        for alt in alts:
            ph = cond_phrases(ctx, _talk.necessary([alt]), chapters, qid)
            lis.append(f"<li>{_cap(join(ph)) or 'Always'}</li>")
        pre_parts.append(
            "<p>Its giver offers it in any of these cases:</p><ul>" + "".join(lis) + "</ul>"
        )
    ch_pre = [ctx.ref("quest", o) for o in _open_prereqs(ctx, q) if chained(ctx, q, o)]
    if ch_pre:
        pre_parts.append(
            f"<p>The conversation that finishes {join(ch_pre)} offers this quest right after"
            " the reward, so in practice finishing it is what opens this quest.</p>"
        )
    if gifts:
        pre_parts.append(f"<p>Accepting it gives Robin {join(gifts)}.</p>")
    body.append(t.section("Prerequisites", "\n".join(pre_parts) + "\n", "prerequisites"))
    # Requirements
    it, mo, me = requirement_rows(ctx, list(q.get("requirements") or []))
    req = ""
    if it:
        req += '<h3 id="items">Items</h3>\n' + it
    if mo:
        req += '<h3 id="monsters">Monsters</h3>\n' + mo
    if me:
        req += '<h3 id="meso">Meso</h3>\n' + me
    variants = [v for v in q.get("variants") or [] if isinstance(v, Mapping)][1:]
    for i, v in enumerate(variants, 2):
        vi, vm, ve = requirement_rows(ctx, list(v.get("requirements") or []))
        vr = reward_table(ctx, list(v.get("rewards") or []))
        req += (
            f'<h3 id="variant-{i}">Variant {i}</h3>\n<p>Another conversation offers the quest'
            " with these requirements and rewards.</p>\n"
            + (vi + vm + ve or "<p>No requirements.</p>\n")
            + vr
        )
    body.append(
        t.section(
            "Requirements",
            req or "<p>None: finishing the quest needs no items, kills or meso.</p>\n",
            "requirements",
        )
    )
    rw = reward_table(ctx, list(q.get("rewards") or []))
    body.append(t.section("Rewards", rw or "<p>None.</p>\n", "rewards"))
    body.append(t.section("How to complete", how_to_complete(ctx, q), "how-to-complete"))
    # Dialogue
    parts_by: dict[str, list[int]] = {}
    for did, n in m.quest_parts.get(qid, []):
        parts_by.setdefault(did, []).append(n)
    order = [str(g.get("script")) for g in _refs(q.get("given_by")) + _refs(q.get("completed_by"))]
    dids = sorted(parts_by, key=lambda x: (order.index(x) if x in order else len(order), x))
    dia = []
    for did in dids:
        e = ctx.kb.get("dialogue", did)
        if e is None:
            continue
        inner = "".join(render_part(ctx, m, e, m.talks[did].part(n), page) for n in parts_by[did])
        dia.append(
            f'<h3 id="{t.esc(t.anchor_id("talk-" + did))}">With {_talk_with(ctx, e)}</h3>\n'
            f'<p class="muted">From {ctx.ref("dialogue", did, "the whole conversation")}.</p>\n'
            + dialogue_block(inner, "Dialogue")
        )
    if dia:
        intro = (
            "<p>The conversation parts of this quest, in the order the script reaches them."
            " Each part names the conditions under which it shows and where it leads.</p>\n"
        )
        body.append(t.section("Dialogue", intro + "".join(dia), "dialogue"))
    # Follow-up
    fol = []
    for r in _refs(q.get("unlocks")):
        state = {1: "accepted", 2: "done"}.get(_num(r.get("value")), "")
        how = f" once this quest is {state}" if r.get("op") in ("==", ">=") and state else ""
        nxt = ctx.kb.get("quest", str(r["id"]))
        if state == "accepted" and nxt is not None and chained(ctx, nxt, qid):
            how = " as this quest ends, in the same conversation"
        fol.append(f"<li>{ctx.ref('quest', str(r['id']))} opens{how}.</li>")
    if fol:
        body.append(t.section("Follow-up", "<ul>" + "".join(fol) + "</ul>\n", "follow-up"))
    notes = quest_notes(ctx, m, q)
    if notes:
        body.append(
            t.section(
                "Notes", "<ul>" + "".join(f"<li>{n}</li>" for n in notes) + "</ul>\n", "notes"
            )
        )
    title = name_of(q)
    return Page(
        path=page,
        title=title,
        heading=ctx.name("quest", qid),
        title_verbatim=True,
        body="\n".join(body),
        crumbs=[(chapter_name(_num(q.get("chapter"))), chapter_path(_num(q.get("chapter"))))],
        description=f"Quest: {title}",
    ).documents("quest", qid)


def _talk_with(ctx: Context, e: Entity) -> str:
    npcs = _uniq_npcs(e.get("npcs"))
    if npcs:
        return join([npc_at(ctx, n) for n in npcs], "or")
    return "the map script"


def quest_notes(ctx: Context, m: Model, q: Entity) -> list[str]:
    out = []
    ch = _num(q.get("chapter"))
    if q.get("quest_kind") == "sub" and ch in m.chapter_ends:
        ev = m.chapter_ends[ch]
        out.append(
            f"The end of the chapter ({ctx.ref('event', ev, 'the chapter end')}) resets every"
            " quest state. Finish this side quest before then: unfinished, its progress is lost."
        )
    if not _refs(q.get("completed_by")):
        out.append(
            "No script calls getReward for this quest, so the rewards it lists are never"
            " given; the quest stays in progress until the chapter ends."
            if ch in m.chapter_ends
            else "No script calls getReward for this quest, so the rewards it lists are never"
            " given."
        )
    for s in sorted({str(g.get("script")) for g in _refs(q.get("given_by"))}):
        sc = ctx.kb.get("script", s)
        if sc is not None and sc.get("approximate_entries"):
            out.append(
                f"The quest icon of {ctx.ref('dialogue', s, 'this conversation')} comes from an"
                " approximate walk of its script (the knowledge base merged paths that do not"
                " cover every case), so when the icon shows may be incomplete; the conversation"
                " itself is exact."
            )
    if len(_refs(q.get("given_by"))) > 1:
        out.append("More than one NPC offers this quest; the giver depends on the chapter's state.")
    return out


# --- conversation pages ----------------------------------------------------------------------


def dialogue_page(ctx: Context, m: Model, e: Entity) -> Page:
    did = str(e["id"])
    page = dialogue_path(did)
    talk = m.talks[did]
    chapters = _chapters(e)
    text = e.get("text") or {}
    rows = [
        ("Speaker", _talk_with(ctx, e)),
        ("Chapters", t.esc(join([str(c) for c in chapters])) if chapters else ""),
        ("Runs from", t.esc(join([str(r) for r in e.get("roles") or []]))),
        ("Script", d.link_or_text(ctx, "script", did)),
        ("Parts", t.esc(t.number(len(talk.parts)))),
        ("Lines", t.esc(t.number(len(text)))),
        ("ID", f"<code>{t.esc(did)}</code>"),
    ]
    body = [t.infobox(ctx.name("dialogue", did), rows)]
    lead = f"{ctx.name('dialogue', did)} is the conversation of {_talk_with(ctx, e)}"
    lead += (
        f" in chapter {chapters[0]}."
        if len(chapters) == 1
        else (f" in chapters {join([str(c) for c in chapters])}." if chapters else ".")
    )
    body.append(f'<p class="lead">{lead}</p>\n')
    quests = sorted({q for q in (m.owner.get((did, p.number)) for p in talk.parts) if q})
    if quests:
        body.append(
            "<p>Its quest parts are on the quest pages: "
            f"{join([ctx.ref('quest', q) for q in quests])}."
            " The parts that belong to no quest are below.</p>\n"
        )
    # The map of parts.
    if talk.parts:
        cols = [
            t.Column("Part", numeric=True),
            t.Column("Kind"),
            t.Column("Lines", numeric=True),
            t.Column("Shows when"),
            t.Column("On"),
        ]
        trs = []
        chs = chapters or [0]
        for p in talk.parts:
            q = m.owner.get((did, p.number))
            on = ctx.ref("quest", q) if q else "this page"
            kind = "Menu row" if p.menu else "Speech"
            when = join(cond_phrases(ctx, p.when, chs, None)) or "Always"
            link = f'<a href="{part_url(ctx, m, did, p.number)}">{p.number}</a>'
            trs.append(
                [
                    t.Cell(link, p.number),
                    t.text_cell(kind),
                    t.num_cell(len(p.lines)),
                    t.markup_cell(_cap(when)),
                    t.markup_cell(on),
                ]
            )
        body.append(t.section("Parts", t.table(cols, trs), "parts"))
    general = [p for p in talk.parts if m.owner.get((did, p.number)) is None]
    if general:
        inner = "".join(render_part(ctx, m, e, p, page) for p in general)
        body.append(
            t.section(
                "General dialogue",
                "<p>Lines that belong to no quest: greetings, refusals and messages.</p>\n"
                + dialogue_block(inner, "General dialogue"),
                "general",
            )
        )
    rest = m.other.get(did, [])
    if rest:
        items = "".join(
            f'<li data-key="{t.esc(k)}">'
            f'<span class="who"><code>{t.esc(_key_label(k))}</code></span>'
            f"{text_spans(text.get(k), k)}</li>"
            for k in rest
        )
        body.append(
            t.section(
                "Other text",
                "<p>Text of this script that no conversation line shows: the titles and"
                " descriptions of crafting and trading orders, and keys no path reaches.</p>\n"
                + dialogue_block(f"<ol>{items}</ol>", "Other text"),
                "other-text",
            )
        )
    missing = [k for k in e.get("missing_text") or [] if isinstance(k, str)]
    sc = ctx.kb.get("script", did)
    notes = []
    if missing:
        notes.append(
            f"{len(missing)} key{'s' if len(missing) > 1 else ''} of this script lack a language"
            " in the game's string tables; the line says which."
        )
    if sc is not None and sc.get("approximate_entries"):
        notes.append(
            "The NPC-state pass of this script (the quest icon) was walked with approximate"
            " merges, so the conditions of its icon may be incomplete; the conversation parts"
            " are exact."
        )
    if notes:
        body.append(
            t.section(
                "Notes", "<ul>" + "".join(f"<li>{n}</li>" for n in notes) + "</ul>\n", "notes"
            )
        )
    return Page(
        path=page,
        title=name_of(e),
        heading=ctx.name("dialogue", did),
        title_verbatim=True,
        body="\n".join(body),
        crumbs=[("Conversations", f"{SECTION_ID}/dialogue/index.html")],
        description=f"Conversation: {name_of(e)}",
    ).documents("dialogue", did)


def _short(s: str, n: int = 70) -> str:
    s = " ".join(s.split())
    return s if len(s) <= n else s[: n - 3].rstrip() + "..."


# --- lists -------------------------------------------------------------------------------------


def quest_table(
    ctx: Context, qs: list[Entity], chapter_col: bool = False, level_first: bool = False
) -> str:
    cols = [t.Column("Quest"), t.Column("Korean"), t.Column("Type")]
    if chapter_col:
        cols.append(t.Column("Chapter", numeric=True))
    cols += [t.Column("Giver"), t.Column("Level", numeric=True), t.Column("Rewards")]
    rows = []
    for q in qs:
        qid = str(q["id"])
        cells = [
            t.markup_cell(ctx.ref("quest", qid), name_of(q)),
            t.Cell(verbatim(name_of(q, "ko"), "ko"), name_of(q, "ko")),
            t.text_cell("Main" if q.get("quest_kind") == "main" else "Side"),
        ]
        if chapter_col:
            cells.append(t.num_cell(_num(q.get("chapter"))))
        givers = _uniq_npcs(q.get("given_by"))
        cells.append(t.markup_cell(join([npc_at(ctx, g) for g in givers], "or")))
        lv = quest_level(q)
        cells.append(t.Cell(t.esc(str(lv)) if lv else "", lv))
        cells.append(t.text_cell(reward_summary(q)))
        rows.append(cells)
    return t.table(cols, rows)


def chain_svg(ctx: Context, qs: list[Entity], title: str) -> tuple[str, list[Entity]]:
    """The chapter's quest chain as our own SVG, from the prerequisite graph: each quest a box
    linked to its page, an arrow from each quest to the quests it opens (dashed when accepting
    it is enough, solid when it must be done or the next is offered as it ends), rows by depth.
    Returns the SVG and the quests
    with no link in the chain (listed beside it)."""
    ids = {str(q["id"]) for q in qs}
    by = {str(q["id"]): q for q in qs}
    edges: list[tuple[str, str, bool]] = []
    for q in qs:
        for r in _refs(q.get("requires")):
            src = str(r["id"])
            if src in ids and src != str(q["id"]):
                done = _num(r.get("value")) == 2 or chained(ctx, q, src)
                e = (src, str(q["id"]), done)
                if e not in edges:
                    edges.append(e)
    edges = _reduce(edges)
    linked = [str(q["id"]) for q in qs if any(str(q["id"]) in (a, b) for a, b, _ in edges)]
    alone = [q for q in qs if str(q["id"]) not in linked]
    if not linked:
        return "", alone
    parents: dict[str, list[str]] = {x: [] for x in linked}
    for a, b, _ in edges:
        parents[b].append(a)
    depth: dict[str, int] = {}

    def dep(x: str, stack: frozenset[str] = frozenset()) -> int:
        if x in depth:
            return depth[x]
        if x in stack:
            return 0
        v = 1 + max((dep(p, stack | {x}) for p in parents[x]), default=-1)
        depth[x] = v
        return v

    for x in linked:
        dep(x)
    rows: dict[int, list[str]] = {}
    for x in linked:
        rows.setdefault(depth[x], []).append(x)
    pos: dict[str, tuple[float, float]] = {}
    W, H, GX, GY, PAD = 176, 46, 16, 34, 8
    ncol = max(len(v) for v in rows.values())
    width = PAD * 2 + ncol * W + (ncol - 1) * GX
    order_key = {str(q["id"]): i for i, q in enumerate(qs)}
    for r in sorted(rows):
        xs = rows[r]
        if r:
            xs.sort(
                key=lambda x: (
                    sum(pos[p][0] for p in parents[x] if p in pos)
                    / max(1, len([p for p in parents[x] if p in pos])),
                    order_key[x],
                )
            )
        else:
            xs.sort(key=lambda x: order_key[x])
        rw = len(xs) * W + (len(xs) - 1) * GX
        x0 = (width - rw) / 2
        for i, x in enumerate(xs):
            pos[x] = (x0 + i * (W + GX), PAD + r * (H + GY))
    height = PAD * 2 + len(rows) * H + (len(rows) - 1) * GY
    out = [
        f'<svg class="chain-svg" viewBox="0 0 {width:.0f} {height:.0f}" width="{width:.0f}"'
        f' height="{height:.0f}" role="img" aria-label="{t.esc(title)}">'
        f"<title>{t.esc(title)}</title>",
        '<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7"'
        ' markerHeight="7" orient="auto-start-reverse"><path d="M0,0L10,5L0,10z"'
        ' fill="currentColor"/></marker></defs>',
        '<g class="edges" fill="none" stroke="currentColor" stroke-width="1.5">',
    ]
    for a, b, done in edges:
        (ax, ay), (bx, by_) = pos[a], pos[b]
        x1, y1, x2, y2 = ax + W / 2, ay + H, bx + W / 2, by_
        if by_ <= ay:  # an edge back up the rows: draw it from the side
            x1, y1, x2, y2 = ax + W, ay + H / 2, bx + W, by_ + H / 2
            path = (
                f"M{x1:.0f},{y1:.0f}C{x1 + 40:.0f},{y1:.0f}"
                f" {x2 + 40:.0f},{y2:.0f} {x2:.0f},{y2:.0f}"
            )
        else:
            my = (y1 + y2) / 2
            path = f"M{x1:.0f},{y1:.0f}C{x1:.0f},{my:.0f} {x2:.0f},{my:.0f} {x2:.0f},{y2 - 1:.0f}"
        dash = "" if done else ' stroke-dasharray="5 4"'
        out.append(f'<path d="{path}"{dash} marker-end="url(#arrow)"/>')
    out.append("</g>")
    for x in linked:
        q = by[x]
        px, py = pos[x]
        main = q.get("quest_kind") == "main"
        lines = _wrap(name_of(q), 26)
        ty = py + H / 2 - (len(lines) - 1) * 7 + 4
        tspans = "".join(
            f'<tspan x="{px + W / 2:.0f}" y="{ty + i * 14:.0f}">{t.esc(s)}</tspan>'
            for i, s in enumerate(lines)
        )
        cls = "node main" if main else "node"
        out.append(
            f'<a href="{ctx.href("quest", x)}" class="{cls}"><rect x="{px:.0f}" y="{py:.0f}"'
            f' width="{W}" height="{H}" rx="8"/><text data-verbatim lang="en"'
            f' text-anchor="middle" font-size="12">{tspans}</text></a>'
        )
    out.append("</svg>")
    fig = (
        '<figure class="chain"><div class="chain-wrap">' + "".join(out) + "</div>"
        "<figcaption>Each arrow runs from a quest to a quest it opens: solid when the first"
        " must be finished (or its giver offers the next one as it ends), dashed when"
        " accepting it is enough. Main quests have a heavier border.</figcaption></figure>\n"
    )
    return fig, alone


def _reduce(edges: list[tuple[str, str, bool]]) -> list[tuple[str, str, bool]]:
    """Drop an arrow a to c when c is reachable from a through other arrows (the chain still
    shows the order, with fewer lines to follow)."""
    succ: dict[str, set[str]] = {}
    for a, b, _ in edges:
        succ.setdefault(a, set()).add(b)

    def reach(a: str, c: str, skip: tuple[str, str]) -> bool:
        todo, seen = [a], {a}
        while todo:
            x = todo.pop()
            for y in succ.get(x, ()):
                if (x, y) == skip or y in seen:
                    continue
                if y == c:
                    return True
                seen.add(y)
                todo.append(y)
        return False

    return [e for e in edges if not reach(e[0], e[1], (e[0], e[1]))]


def _wrap(s: str, n: int) -> list[str]:
    words, lines, cur = s.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > n:
            lines.append(cur)
            cur = w
        else:
            cur = f"{cur} {w}".strip()
    if cur:
        lines.append(cur)
    if len(lines) > 2:
        lines = [lines[0], _short(" ".join(lines[1:]), n)]
    return lines or [s]


def index_page(ctx: Context, m: Model) -> Page:
    qs = quests_in_order(ctx)
    by_ch: dict[int, list[Entity]] = {}
    for q in qs:
        by_ch.setdefault(_num(q.get("chapter")), []).append(q)
    mains = sum(1 for q in qs if q.get("quest_kind") == "main")
    body = [
        f'<p class="lead">The game has {t.esc(t.number(len(qs)))} quests: {t.esc(t.number(mains))}'
        f" main quests and {t.esc(t.number(len(qs) - mains))} side quests, over the tutorial and"
        " three chapters. Each quest page says who gives it, what it needs and how to finish"
        " it, and holds its conversation in English and Korean.</p>\n",
        '<ul class="see-also">'
        + "".join(f"<li>{chapter_link(ctx, ch)}: quest chain</li>" for ch in sorted(by_ch))
        + f'<li><a href="{ctx.url(SECTION_ID + "/by-giver.html")}">Quests by giver</a></li>'
        + f'<li><a href="{ctx.url(SECTION_ID + "/by-area.html")}">Quests by area</a></li>'
        + f'<li><a href="{ctx.url(SECTION_ID + "/by-level.html")}">Quests by level</a></li>'
        + f'<li><a href="{ctx.url(SECTION_ID + "/dialogue/index.html")}">Conversations</a></li>'
        + "</ul>\n",
    ]
    prose = ctx.prose(f"{SECTION_ID}/index.md")
    if prose:
        body.append(prose)
    for ch in sorted(by_ch):
        body.append(
            t.section(
                chapter_name(ch),
                quest_table(ctx, by_ch[ch]),
                f"chapter-{ch}",
            )
        )
    body.append(
        t.section(
            "Notes",
            "<ul><li>The end of each chapter resets every quest state; a side quest left"
            " unfinished loses its progress. The quest pages of the affected side quests say"
            " so.</li>"
            "<li>In the original game a quest's page in the popup menu opens only by a touch."
            " Opening it with OK, which the port's controller and keyboard send, showed a quest"
            f' the game never stored (<a href="{BUG_11}">original bug 11</a>); the port fixes'
            " it.</li></ul>\n",
            "notes",
        )
    )
    return Page(
        f"{SECTION_ID}/index.html", "Quests", "\n".join(body), description="Every quest of the game"
    )


def chapter_page(ctx: Context, ch: int, qs: list[Entity]) -> Page:
    svg, alone = chain_svg(ctx, qs, f"Quest chain of {chapter_name(ch).lower()}")
    mains = [q for q in qs if q.get("quest_kind") == "main"]
    subs = [q for q in qs if q.get("quest_kind") != "main"]
    body = [
        f'<p class="lead">{t.esc(chapter_name(ch))} has {t.esc(t.number(len(mains)))} main'
        f" quest{'s' if len(mains) != 1 else ''} and {t.esc(t.number(len(subs)))} side"
        f" quest{'s' if len(subs) != 1 else ''}.</p>\n"
    ]
    if svg:
        extra = ""
        if alone:
            extra = (
                "<p>Quests that neither need nor open another quest: "
                + join([ctx.ref("quest", str(q["id"])) for q in alone])
                + ".</p>\n"
            )
        body.append(t.section("Quest chain", svg + extra, "chain"))
    if mains:
        body.append(
            t.section(
                "Main quests",
                "<ol>"
                + "".join(f"<li>{ctx.ref('quest', str(q['id']))}</li>" for q in mains)
                + "</ol>\n"
                + quest_table(ctx, mains),
                "main-quests",
            )
        )
    if subs:
        body.append(t.section("Side quests", quest_table(ctx, subs), "side-quests"))
    return Page(chapter_path(ch), f"Quests of {chapter_name(ch).lower()}", "\n".join(body))


def by_giver_page(ctx: Context) -> Page:
    rows = []
    for q in quests_in_order(ctx):
        for g in _uniq_npcs(q.get("given_by")):
            npc = ctx.kb.get("npc", str(g["id"]))
            rows.append(
                [
                    d.entry_cell(ctx, "npc", str(g["id"])),
                    t.markup_cell(map_phrase(ctx, str(g["map"])) if g.get("map") else ""),
                    t.markup_cell(ctx.ref("quest", str(q["id"])), name_of(q)),
                    t.num_cell(_num(q.get("chapter"))),
                    t.text_cell("Main" if q.get("quest_kind") == "main" else "Side"),
                ]
            )
            del npc
    rows.sort(key=lambda r: (str(r[0].key), r[3].key))
    cols = [
        t.Column("Giver"),
        t.Column("Where"),
        t.Column("Quest"),
        t.Column("Chapter", numeric=True),
        t.Column("Type"),
    ]
    body = (
        '<p class="lead">Every quest under the NPC who offers it, with the map where the NPC'
        " stands.</p>\n" + t.table(cols, rows, filter=True)
    )
    return Page(f"{SECTION_ID}/by-giver.html", "Quests by giver", body)


def by_area_page(ctx: Context) -> Page:
    by_area: dict[str, list[Entity]] = {}
    for q in quests_in_order(ctx):
        areas = []
        for g in _uniq_npcs(q.get("given_by")):
            mp = ctx.kb.get("map", str(g.get("map")))
            for a in _refs(mp.get("area") if mp else None):
                if str(a["id"]) not in areas:
                    areas.append(str(a["id"]))
        for a in areas or ["?"]:
            by_area.setdefault(a, []).append(q)
    body = ['<p class="lead">Every quest under the area where its giver stands.</p>\n']
    for a in sorted(by_area, key=lambda x: (x == "?", int(x) if x.isdigit() else 0)):
        if a == "?":
            title, head = "No area", "No area"
        else:
            area = ctx.kb.get("area", a)
            title = name_of(area) if area else f"Area {a}"
            head = ctx.ref("area", a) if area else t.esc(title)
        sec = (
            f'<section aria-labelledby="area-{t.esc(a)}">\n<h2 id="area-{t.esc(a)}">{head}</h2>\n'
            + quest_table(ctx, by_area[a], chapter_col=True)
            + "</section>\n"
        )
        body.append(sec)
    return Page(f"{SECTION_ID}/by-area.html", "Quests by area", "\n".join(body))


def by_level_page(ctx: Context) -> Page:
    qs = sorted(quests_in_order(ctx), key=lambda q: (quest_level(q), _num(q.get("chapter"))))
    body = (
        '<p class="lead">Every quest by the level its giver asks for, lowest first. Quests'
        " with no level in their conditions come first.</p>\n"
        + quest_table(ctx, qs, chapter_col=True)
    )
    return Page(f"{SECTION_ID}/by-level.html", "Quests by level", body)


def dialogue_index(ctx: Context, m: Model) -> Page:
    cols = [
        t.Column("Conversation"),
        t.Column("Speaker"),
        t.Column("Chapters"),
        t.Column("Parts", numeric=True),
        t.Column("Quests", numeric=True),
    ]
    rows = []
    for e in ctx.kb.entities("dialogue"):
        did = str(e["id"])
        talk = m.talks[did]
        nq = len({m.owner.get((did, p.number)) for p in talk.parts} - {None})
        rows.append(
            [
                t.markup_cell(ctx.ref("dialogue", did), name_of(e)),
                t.markup_cell(_talk_with(ctx, e)),
                t.text_cell(", ".join(str(c) for c in _chapters(e))),
                t.num_cell(len(talk.parts)),
                t.num_cell(nq),
            ]
        )
    body = (
        '<p class="lead">Every conversation the game runs: one per NPC or map script, per'
        " chapter directory. Quest lines are on the quest pages; each conversation's page"
        " holds the rest and a table of where every part is.</p>\n" + t.table(cols, rows)
    )
    return Page(f"{SECTION_ID}/dialogue/index.html", "Conversations", body)


def pages(ctx: Context) -> Iterator[Page]:
    m = model(ctx)
    yield index_page(ctx, m)
    by_ch: dict[int, list[Entity]] = {}
    for q in quests_in_order(ctx):
        by_ch.setdefault(_num(q.get("chapter")), []).append(q)
        yield quest_page(ctx, m, q)
    for ch in sorted(by_ch):
        yield chapter_page(ctx, ch, by_ch[ch])
    yield by_giver_page(ctx)
    yield by_area_page(ctx)
    yield by_level_page(ctx)
    yield dialogue_index(ctx, m)
    for e in ctx.kb.entities("dialogue"):
        yield dialogue_page(ctx, m, e)


SECTION = Section(
    SECTION_ID,
    "Quests",
    50,
    ("quest", "dialogue"),
    pages,
    blurb="Every quest, how to finish it, and every conversation.",
    image="npc:14",
)


# --- checks ------------------------------------------------------------------------------------

_KEY_RE = re.compile(r'data-key="([^"]*)"')


def key_problems(files: Mapping[str, bytes], kb: Any) -> list[str]:
    """Every text key of every conversation must sit on exactly one page (one ``data-key``
    element on the whole site) with its English and Korean text on that page, or the note
    that a language is missing. One line per problem."""
    where: dict[str, list[str]] = {}
    for path in sorted(files):
        if path.endswith(".html"):
            for k in _KEY_RE.findall(files[path].decode("utf-8")):
                where.setdefault(html.unescape(k), []).append(path)
    problems = []
    for e in kb.entities("dialogue"):
        for key, tx in sorted((e.get("text") or {}).items()):
            pages = where.get(key, [])
            if len(pages) != 1:
                problems.append(f"{e['id']}: key {key} is on {len(pages)} places: {pages}")
                continue
            page = files[pages[0]].decode("utf-8")
            for lang in ("en", "ko"):
                value = tx.get(lang) if isinstance(tx, Mapping) else None
                if value and t.esc(str(value)) not in page:
                    problems.append(f"{e['id']}: key {key}: {lang} text not on {pages[0]}")
                if not value and "no Korean text" not in page and "no English text" not in page:
                    problems.append(f"{e['id']}: key {key}: missing {lang} not noted")
    return problems
