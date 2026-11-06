"""The World section of the compendium (#221): the atlas, the areas, the maps and the events.

Pages:

- ``world/index.html``: the atlas (our own drawing of the areas and how they connect, from the
  warp graph), the areas, the area connections per chapter, the towns and the maps no warp
  reaches, with the reason;
- ``world/maps.html``: every map (sortable by area and monster level), the towns, the boss
  maps and the Monster Carnival rooms;
- ``world/map.html`` and ``world/worldmap.json``: the interactive world map (#232,
  :mod:`mtport.compendium.sections._worldmap`);
- ``world/routes.html``: the shortest way between the main towns (precomputed);
- ``world/warps.html``: every ``changeMap`` of every script (warps are rows with anchors);
- ``world/events.html`` and ``event/<id>.html``: boss starts, scenes, Monster Carnival and
  chapter ends;
- ``area/<id>.html``: an area with a diagram of its maps, its level range, towns and shops;
- ``map/<id>.html``: a map with its render and overlay toggle, infobox, monsters, NPCs (and
  what they do there), quests, connections (each portal, where it leads and when), arrivals,
  hidden portals, events and notes.

The **warp graph** is the knowledge base's used warps (``warp.source`` to ``warp.target``),
every ``changeMap`` of a script that an object on a reached map names, whatever its
conditions. The start is the intro's map 2-10 and the tutorial's 2-8 (``kb.md``, Chapters):
the intro scene hands over to the tutorial in code, not through a warp.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.sections import _worldmap
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of, slug, verbatim

START_MAPS = ("2-10", "2-8")
CHAPTERS = (0, 1, 2, 3)
QUEST_STATES = {0: "not started", 1: "in progress", 2: "done"}
NO_SAVE_WORDS = {
    "tutorial": "the tutorial",
    "boss": "a boss map",
    "climb": "a climbing stage",
    "Monster Carnival": "a Monster Carnival room",
}
EVENT_TYPES = {
    "boss": "Boss fight",
    "scene": "Scene",
    "carnival": "Carnival round",
    "chapter_end": "Chapter end",
}
TRIGGERS = {"map": "entering the map", "npc": "talking to an NPC", "portal": "taking a portal"}


# --- the warp graph ---------------------------------------------------------------------------


def _ids(rows: Any) -> list[str]:
    if isinstance(rows, Mapping):
        rows = [rows]
    return [str(r["id"]) for r in rows or [] if isinstance(r, Mapping) and "id" in r]


def map_id_key(mid: str) -> tuple[int, int]:
    a, _, m = str(mid).partition("-")
    try:
        return int(a), int(m or 0)
    except ValueError:
        return (10**6, 0)


def warp_edges(kb) -> list[tuple[str, str, Entity]]:
    """(source map, target map, warp) for every used warp, in a stable order."""
    out = []
    for w in kb.entities("warp"):
        if not w.get("used"):
            continue
        tgt = w.get("target_map")
        if not isinstance(tgt, str) or kb.get("map", tgt) is None:
            continue
        for src in _ids(w.get("source")):
            if kb.get("map", src) is not None:
                out.append((src, tgt, w))
    return sorted(out, key=lambda e: (map_id_key(e[0]), map_id_key(e[1]), str(e[2]["id"])))


def adjacency(kb, chapter: int | None = None) -> dict[str, list[str]]:
    adj: dict[str, list[str]] = {}
    for src, tgt, w in warp_edges(kb):
        if chapter is not None and chapter not in (w.get("chapters") or []):
            continue
        lst = adj.setdefault(src, [])
        if tgt not in lst:
            lst.append(tgt)
    for k in adj:
        adj[k].sort(key=map_id_key)
    return adj


def start_maps(kb) -> list[str]:
    got = [m for m in START_MAPS if kb.get("map", m) is not None]
    if got:
        return got
    maps = sorted((str(e["id"]) for e in kb.entities("map")), key=map_id_key)
    return maps[:1]


def reached_from(kb, start: Iterable[str], chapter: int | None = None) -> dict[str, list[str]]:
    """Every map reachable from ``start`` with its shortest path (a list of map ids)."""
    adj = adjacency(kb, chapter)
    paths: dict[str, list[str]] = {}
    q: deque[str] = deque()
    for s in start:
        if s not in paths:
            paths[s] = [s]
            q.append(s)
    while q:
        a = q.popleft()
        for b in adj.get(a, []):
            if b not in paths:
                paths[b] = paths[a] + [b]
                q.append(b)
    return paths


def unreached_reasons(kb, reached: Iterable[str]) -> dict[str, str]:
    """Each map the warp graph does not reach, with the reason in words (our own)."""
    reached = set(reached)
    into: dict[str, list[Entity]] = {}
    for w in kb.entities("warp"):
        tgt = w.get("target_map")
        if isinstance(tgt, str):
            into.setdefault(tgt, []).append(w)
    out: dict[str, str] = {}
    for e in kb.entities("map"):
        mid = str(e["id"])
        if mid in reached:
            continue
        srcs = sorted(
            {s for w in into.get(mid, []) for s in _ids(w.get("source"))}
            | set(_ids(e.get("entrances"))),
            key=map_id_key,
        )
        unused = [w for w in into.get(mid, []) if not w.get("used")]
        parts = []
        if e.get("no_save") == "Monster Carnival":
            parts.append("it is a Monster Carnival room")
        if srcs:
            parts.append(
                "it is entered only from "
                + ", ".join(f"map {s.replace('-', '.')}" for s in srcs)
                + ", which no warp reaches either"
            )
        elif unused:
            scripts = sorted({str(s) for w in unused for s in _ids(w.get("script"))})
            reasons = sorted(
                {
                    str((kb.get("script", s) or {}).get("unused_reason") or "")
                    for s in scripts
                    if kb.get("script", s) is not None
                }
                - {""}
            )
            names = ", ".join(s.replace("-", "/", 1) for s in scripts)
            why = f" ({'; '.join(reasons)})" if reasons else ""
            parts.append(f"only unused scripts warp here: {names}{why}")
        else:
            parts.append("no portal, NPC or script of the game leads here")
        out[mid] = "; ".join(parts)
    return out


# --- conditions in words ----------------------------------------------------------------------


def _quest_ref(ctx: Context, kind_no: int, number: int, chapters: Sequence[int]) -> str:
    word = "main" if kind_no == 0 else "sub"
    for c in list(chapters) + list(CHAPTERS):
        qid = f"{c}-{word}-{number}"
        if ctx.kb.get("quest", qid) is not None:
            return ctx.ref("quest", qid)
    return t.esc(f"{word} quest {number}")


def _item_ref(ctx: Context, kind_no: int, number: int) -> str:
    iid = f"{'equip' if kind_no == 0 else 'etc'}:{number}"
    return ctx.ref("item", iid) if ctx.kb.get("item", iid) is not None else t.esc(iid)


def _cmp(op: str, value: int) -> str:
    return {
        ">=": f"{value} or more",
        ">": f"more than {value}",
        "<=": f"{value} or less",
        "<": f"less than {value}",
        "==": f"{value}",
        "!=": f"not {value}",
    }.get(op, f"{op} {value}")


def condition_markup(ctx: Context, c: Mapping[str, Any], chapters: Sequence[int] = ()) -> str:
    """One condition in our words, with links to the quest or item it names."""
    native = c.get("native")
    op, value = str(c.get("op", "")), c.get("value")
    args = c.get("args") or []
    if native == "getQuestState" and len(args) == 2:
        q = _quest_ref(ctx, int(args[0]), int(args[1]), chapters)
        if op == "==" and value in QUEST_STATES:
            return f"{q} is {QUEST_STATES[value]}"
        if op == "!=" and value in QUEST_STATES:
            return f"{q} is not {QUEST_STATES[value]}"
        if op == ">=" and value == 1:
            return f"{q} is in progress or done"
        if op == ">=" and value == 2:
            return f"{q} is done"
        if op == "<" and value == 2:
            return f"{q} is not done"
        return f"{q}: state {t.esc(_cmp(op, value))}"
    if native == "checkRequire" and len(args) == 2:
        q = _quest_ref(ctx, int(args[0]), int(args[1]), chapters)
        if op == "==" and value == 1:
            return f"the requirements of {q} are met"
        if op == "==" and value == 0:
            return f"the requirements of {q} are not met yet"
    if native == "checkItemCount" and len(args) == 2:
        item = _item_ref(ctx, int(args[0]), int(args[1]))
        return f"carries {t.esc(_cmp(op, value))} of {item}"
    if native == "isInvenAvailable" and len(args) == 2 and op == "==" and value == 1:
        inv = "equipment" if args[0] == 0 else "other items"
        n = int(args[1])
        return t.esc(f"{n} free slot{'s' if n != 1 else ''} in the {inv} inventory")
    if native == "getLevel":
        return t.esc(f"level {_cmp(op, value)}")
    if "answer" in c:
        return t.esc(f"answer {value} is picked in the conversation")
    return f"<code>{t.esc(str(c.get('text') or c.get('term') or '?'))}</code>"


def conditions_markup(ctx: Context, alts: Any, chapters: Sequence[int] = ()) -> str:
    """A list of alternatives: ``always``, ``never`` or the conditions joined with "and",
    the alternatives with "or"."""
    if not isinstance(alts, list):
        return ""
    if not alts:
        return "never"
    if any(not a for a in alts):
        return "always"
    parts = [
        " and ".join(condition_markup(ctx, c, chapters) for c in a if isinstance(c, Mapping))
        for a in alts
    ]
    if len(parts) == 1:
        return parts[0]
    return "either " + "; or ".join(f"({p})" if " and " in p else p for p in parts)


def chapters_text(ch: Any) -> str:
    ch = sorted(int(c) for c in ch or [])
    if not ch:
        return "none"
    if ch == list(CHAPTERS):
        return "all"
    if len(ch) > 2 and ch == list(range(ch[0], ch[-1] + 1)):
        return f"{ch[0]} to {ch[-1]}"
    return ", ".join(str(c) for c in ch)


# --- small helpers ----------------------------------------------------------------------------


def dotted(mid: str) -> str:
    return str(mid).replace("-", ".")


def map_link(ctx: Context, mid: str, with_number: bool = True) -> str:
    if ctx.kb.get("map", mid) is None:
        return t.esc(f"map {dotted(mid)}")
    num = f' <span class="muted">({t.esc(dotted(mid))})</span>' if with_number else ""
    return ctx.ref("map", mid) + num


def area_of(kb, mid: str) -> str | None:
    e = kb.get("map", mid)
    ids = _ids(e.get("area")) if e is not None else []
    return ids[0] if ids else None


def map_type(e: Entity) -> str:
    ns = e.get("no_save")
    if ns == "Monster Carnival":
        return "Monster Carnival room"
    if e.get("boss_map"):
        return "boss map"
    if ns == "tutorial":
        return "tutorial"
    if e.get("map_no") == 0:
        return "town"
    return "field"


def bgm_text(kb, e: Entity) -> str:
    """The map's music as the game picks it (``CStateGame::PlayMapBGM``): its sound id and
    which track of the area it is."""
    bgm = e.get("bgm")
    if not isinstance(bgm, Mapping) or bgm.get("sound") is None:
        return ""
    sound = bgm["sound"]
    area = kb.get("area", area_of(kb, str(e["id"])) or "")
    aname = name_of(area) if area is not None else ""
    if e.get("boss_map"):
        role = "the boss music"
    elif area is not None and not bgm.get("resume") and sound == area.get("town_bgm"):
        role = f"the town music of {aname}"
    elif area is not None and sound == area.get("field_bgm"):
        role = f"the field music of {aname}"
    else:
        role = ""
    how = "restarts on entering" if not bgm.get("resume") else "resumes where it stopped"
    return f"track {sound}" + (f", {role}" if role else "") + f" ({how})"


def bgm_markup(kb, e: Entity) -> str:
    """:func:`bgm_text` with the area name marked as game text."""
    text = bgm_text(kb, e)
    area = kb.get("area", area_of(kb, str(e["id"])) or "")
    if area is None or not text:
        return t.esc(text)
    an = name_of(area)
    head, sep, tail = text.partition(an)
    if not sep:
        return t.esc(text)
    return t.esc(head) + verbatim(an) + t.esc(tail)


def save_text(e: Entity) -> str:
    ns = e.get("no_save")
    if not ns:
        return "yes"
    return f"no ({NO_SAVE_WORDS.get(ns, ns)})"


def monster_levels(kb, map_ids: Iterable[str]) -> tuple[int, int] | None:
    lv = []
    for mid in map_ids:
        e = kb.get("map", mid)
        for mon in _ids(e.get("spawns") if e is not None else None):
            m = kb.get("monster", mon)
            if m is not None and isinstance(m.get("level"), int) and m["level"] > 0:
                lv.append(m["level"])
    return (min(lv), max(lv)) if lv else None


def level_text(r: tuple[int, int] | None) -> str:
    if r is None:
        return ""
    return f"{r[0]}" if r[0] == r[1] else f"{r[0]}-{r[1]}"


def main_towns(kb) -> list[str]:
    """Map 0 of the first area of each world spot: the towns the route table joins."""
    seen: dict[Any, str] = {}
    for a in sorted(kb.entities("area"), key=lambda a: map_id_key(str(a["id"]))):
        ws = a.get("world_spot", a["id"])
        mid = f"{a['id']}-0"
        if ws not in seen and kb.get("map", mid) is not None:
            seen[ws] = mid
    return sorted(seen.values(), key=map_id_key)


def warp_anchor(wid: str) -> str:
    return t.anchor_id(f"warp-{wid}")


def event_path(eid: str) -> str:
    return f"event/{slug(eid)}.html"


# --- diagrams (our own drawing, inline SVG) ---------------------------------------------------


@dataclass
class Node:
    id: str
    label: str  # plain text, ours
    href: str = ""  # a link marker
    title: str = ""  # game text for the tooltip
    cls: str = ""
    sub: str = ""  # a second line (game text)


@dataclass
class Edge:
    a: str
    b: str
    dashed: bool = False
    label: str = ""


@dataclass
class Layout:
    pos: dict[str, tuple[int, int]] = field(default_factory=dict)  # node -> (col, row)


def layered(nodes: Sequence[str], edges: Sequence[tuple[str, str]], root: str) -> Layout:
    """Columns by breadth-first distance from ``root`` over the undirected edges; nodes the
    root does not reach get columns of their own after the rest, in the same way."""
    adj: dict[str, list[str]] = {n: [] for n in nodes}
    for a, b in edges:
        if a in adj and b in adj and a != b:
            if b not in adj[a]:
                adj[a].append(b)
            if a not in adj[b]:
                adj[b].append(a)
    for n in adj:
        adj[n].sort(key=lambda x: list(nodes).index(x))
    depth: dict[str, int] = {}
    base = 0
    order: list[str] = []
    for r in [root] + [n for n in nodes if n != root]:
        if r in depth or r not in adj:
            continue
        depth[r] = base
        q = deque([r])
        while q:
            a = q.popleft()
            order.append(a)
            for b in adj[a]:
                if b not in depth:
                    depth[b] = depth[a] + 1
                    q.append(b)
        base = max(depth.values()) + 1
    lay = Layout()
    rows: dict[int, int] = {}
    for n in order:
        c = depth[n]
        lay.pos[n] = (c, rows.get(c, 0))
        rows[c] = rows.get(c, 0) + 1
    return lay


def svg_diagram(
    nodes: Sequence[Node],
    edges: Sequence[Edge],
    root: str,
    title: str,
    node_w: int = 64,
    node_h: int = 30,
    gap_x: int = 40,
    gap_y: int = 22,
) -> str:
    """A node-and-line diagram as inline SVG that follows the theme (stylesheet classes
    ``wd-*``). Nodes link to their pages; their game names are in ``<title>`` tooltips."""
    if not nodes:
        return ""
    ids = [n.id for n in nodes]
    lay = layered(ids, [(e.a, e.b) for e in edges], root)
    cols = max(c for c, _ in lay.pos.values()) + 1
    rows = max(r for _, r in lay.pos.values()) + 1
    pad = 8
    width = pad * 2 + cols * node_w + (cols - 1) * gap_x
    height = pad * 2 + rows * node_h + (rows - 1) * gap_y

    def xy(n: str) -> tuple[float, float]:
        c, r = lay.pos[n]
        return pad + c * (node_w + gap_x) + node_w / 2, pad + r * (node_h + gap_y) + node_h / 2

    lines = []
    seen = set()
    for e in edges:
        if e.a not in lay.pos or e.b not in lay.pos or e.a == e.b:
            continue
        key = tuple(sorted((e.a, e.b))) + (e.dashed,)
        if key in seen:
            continue
        seen.add(key)
        (x1, y1), (x2, y2) = xy(e.a), xy(e.b)
        half = node_w / 2
        if lay.pos[e.a][0] == lay.pos[e.b][0]:
            x1, x2 = x1 + half, x2 + half
        elif lay.pos[e.a][0] < lay.pos[e.b][0]:
            x1, x2 = x1 + half, x2 - half
        else:
            x1, x2 = x1 - half, x2 + half
        dash = ' class="wd-edge wd-dashed"' if e.dashed else ' class="wd-edge"'
        if lay.pos[e.a][0] == lay.pos[e.b][0]:  # same column: bow out to the right
            bow = 14 + 6 * abs(lay.pos[e.a][1] - lay.pos[e.b][1])
            path = (
                f"M{x1:.0f},{y1:.0f} C{x1 + bow:.0f},{y1:.0f}"
                f" {x2 + bow:.0f},{y2:.0f} {x2:.0f},{y2:.0f}"
            )
        else:
            mx = (x1 + x2) / 2
            path = f"M{x1:.0f},{y1:.0f} C{mx:.0f},{y1:.0f} {mx:.0f},{y2:.0f} {x2:.0f},{y2:.0f}"
        lines.append(f'<path{dash} d="{path}"/>')
    boxes = []
    for n in nodes:
        if n.id not in lay.pos:
            continue
        cx, cy = xy(n.id)
        x, y = cx - node_w / 2, cy - node_h / 2
        tip = f'<title data-verbatim="">{t.esc(n.title)}</title>' if n.title else ""
        cls = "wd-node" + (f" {n.cls}" if n.cls else "")
        ty = cy + 4 if not n.sub else cy - 2
        inner = (
            f'{tip}<rect x="{x:.0f}" y="{y:.0f}" width="{node_w}" height="{node_h}" rx="5"/>'
            f'<text x="{cx:.0f}" y="{ty:.0f}" text-anchor="middle">{t.esc(n.label)}</text>'
        )
        if n.sub:
            inner += (
                f'<text class="wd-sub" x="{cx:.0f}" y="{cy + 11:.0f}" text-anchor="middle"'
                f' data-verbatim="">{t.esc(n.sub)}</text>'
            )
        if n.href:
            boxes.append(f'<a href="{n.href}" class="{cls}">{inner}</a>')
        else:
            boxes.append(f'<g class="{cls}">{inner}</g>')
    return (
        f'<figure class="world-diagram"><svg viewBox="0 0 {width} {height}"'
        f' style="max-width: {width}px" role="img" aria-label="{t.esc(title)}">'
        f"<title>{t.esc(title)}</title>"
        + "".join(lines)
        + "".join(boxes)
        + f"</svg><figcaption>{t.esc(title)}</figcaption></figure>\n"
    )


# --- shared data for the pages ----------------------------------------------------------------


@dataclass
class World:
    reached: dict[str, list[str]]
    unreached: dict[str, str]
    towns: list[str]


def world_facts(kb) -> World:
    reached = reached_from(kb, start_maps(kb))
    return World(reached, unreached_reasons(kb, reached), main_towns(kb))


def _warps_by_source(kb) -> dict[tuple[str, str, Any], list[Entity]]:
    """(map, role, portal) -> warps: the objects that run each warp."""
    out: dict[tuple[str, str, Any], list[Entity]] = {}
    for w in kb.entities("warp"):
        for s in w.get("source") or []:
            if isinstance(s, Mapping):
                out.setdefault((str(s["id"]), str(s.get("role")), s.get("portal")), []).append(w)
    return out


def _warp_from_npc(w: Entity, mid: str) -> list[str]:
    return sorted(
        {
            str(f.get("npc"))
            for f in w.get("from") or []
            if isinstance(f, Mapping) and f.get("map") == mid and f.get("npc") is not None
        }
    )


# --- map pages --------------------------------------------------------------------------------


def _target_markup(ctx: Context, w: Entity | None, tmap: str, tportal: Any) -> str:
    out = map_link(ctx, tmap)
    if tportal is not None:
        out += t.esc(f", portal {tportal}")
    if w is not None and w.get("chapter_end"):
        out += " " + t.esc("(ends the chapter)")
    return out


def _when_markup(ctx: Context, w: Entity | None, gated: bool) -> tuple[str, str]:
    """(markup, sort key) of when a warp runs."""
    if w is None:
        return ("conditional (the condition is not known)", "c") if gated else ("always", "a")
    if not w.get("conditional"):
        return "always", "a"
    cond = conditions_markup(ctx, w.get("conditions"), w.get("chapters") or [])
    return f"<strong>conditional</strong>: {cond}", "c"


def connections(ctx: Context, e: Entity, by_source) -> tuple[str, list[str]]:
    """The Connections table of a map and the warp ids it anchors."""
    mid = str(e["id"])
    cols = [
        t.Column("From"),
        t.Column("Leads to"),
        t.Column("When"),
        t.Column("Chapters"),
    ]
    rows, ids, anchored = [], [], []
    for p in sorted(e.get("portals") or [], key=lambda p: p.get("number", 0)):
        num = p.get("number")
        what = f"Portal {num}" + (" (hidden)" if p.get("hidden") else "")
        tile = p.get("tile") or []
        where = t.esc(what) + (
            f' <span class="muted">{t.esc(f"x {tile[0]}, y {tile[1]}")}</span>'
            if len(tile) == 2
            else ""
        )
        warps = by_source.get((mid, "portal", num), [])
        targets = p.get("targets") or []
        if not targets:
            rows.append(
                [
                    t.markup_cell(where, num),
                    t.markup_cell(
                        t.esc(
                            "nowhere: the portal names no script"
                            if not p.get("script")
                            else "nowhere: its script calls no constant changeMap"
                        )
                    ),
                    t.markup_cell(""),
                    t.markup_cell(""),
                ]
            )
            ids.append(None)
            continue
        for tg in targets:
            sid = str(tg.get("script") or "").replace("/", "-")
            w = next((x for x in warps if sid in _ids(x.get("script"))), None)
            if w is None:  # a warp of a script the game never runs here (an unreached map)
                w = next(
                    (
                        x
                        for x in ctx.kb.entities("warp")
                        if sid in _ids(x.get("script")) and x.get("target_map") == tg.get("map")
                    ),
                    None,
                )
            when, key = _when_markup(ctx, w, bool(tg.get("gated")))
            ch = chapters_text(w.get("chapters")) if w is not None else ""
            rows.append(
                [
                    t.markup_cell(where, num),
                    t.markup_cell(
                        _target_markup(ctx, w, str(tg.get("map")), tg.get("portal")),
                        dotted(str(tg.get("map"))),
                    ),
                    t.markup_cell(when, key),
                    t.markup_cell(t.esc(ch)),
                ]
            )
            wid = str(w["id"]) if w is not None else None
            if wid and wid not in anchored:
                anchored.append(wid)
                ids.append(warp_anchor(wid))
            else:
                ids.append(None)
    # NPC and map-script warps
    for role in ("npc", "map"):
        for w in sorted(by_source.get((mid, role, None), []), key=lambda w: str(w["id"])):
            if role == "npc":
                npcs = _warp_from_npc(w, mid)
                who = ", ".join(d.link_or_text(ctx, "npc", n) for n in npcs) or "an NPC"
                where = f"NPC {who}"
            else:
                where = t.esc("the map's own script")
            when, key = _when_markup(ctx, w, True)
            rows.append(
                [
                    t.markup_cell(where, role),
                    t.markup_cell(
                        _target_markup(ctx, w, str(w.get("target_map")), w.get("target_portal")),
                        dotted(str(w.get("target_map"))),
                    ),
                    t.markup_cell(when, key),
                    t.markup_cell(t.esc(chapters_text(w.get("chapters")))),
                ]
            )
            wid = str(w["id"])
            if wid not in anchored:
                anchored.append(wid)
                ids.append(warp_anchor(wid))
            else:
                ids.append(None)
    if not rows:
        return "", []
    row_ids = [i or "" for i in ids]
    # htmlpage.table wants an id per row or none: give the rest an id of their own
    row_ids = [i or t.anchor_id(f"conn-{mid}-{n}") for n, i in enumerate(row_ids)]
    return t.table(cols, rows, row_ids), anchored


def arrivals(ctx: Context, e: Entity, kb) -> str:
    mid = str(e["id"])
    rows = []
    seen = set()
    for w in kb.entities("warp"):
        if w.get("target_map") != mid or not w.get("used"):
            continue
        for src in _ids(w.get("source")):
            key = (src, str(w["id"]))
            if key in seen:
                continue
            seen.add(key)
            when, k = _when_markup(ctx, w, bool(w.get("conditional")))
            how = []
            for s in w.get("source") or []:
                if s.get("id") != src:
                    continue
                if s.get("role") == "portal":
                    how.append(t.esc(f"portal {s.get('portal')}"))
                elif s.get("role") == "npc":
                    npcs = _warp_from_npc(w, src)
                    how.append(
                        "NPC " + ", ".join(d.link_or_text(ctx, "npc", n) for n in npcs)
                        if npcs
                        else "an NPC"
                    )
                else:
                    how.append(t.esc("the map's script"))
            rows.append(
                [
                    t.markup_cell(map_link(ctx, src), dotted(src)),
                    t.markup_cell(", ".join(how)),
                    t.markup_cell(
                        t.esc(f"portal {w.get('target_portal')}")
                        if w.get("target_portal") is not None
                        else ""
                    ),
                    t.markup_cell(when, k),
                ]
            )
    if not rows:
        return ""
    rows.sort(key=lambda r: (map_id_key(str(r[0].key)), str(r[1].key)))
    cols = [t.Column("From map"), t.Column("By"), t.Column("Arrives at"), t.Column("When")]
    return t.table(cols, rows)


def _npc_roles(ctx: Context, npc: Entity, mid: str) -> str:
    """What the NPC does on this map: shops, quests, crafting, trades, job advancements."""
    parts = []

    def here(rel: str) -> list[str]:
        out = []
        for r in npc.get(rel) or []:
            if isinstance(r, Mapping) and r.get("map") == mid and r["id"] not in out:
                out.append(str(r["id"]))
        return out

    shops = here("opens_shop")
    if shops:
        links = ", ".join(d.link_or_text(ctx, "shop", s) for s in shops)
        chapters = sorted(
            {
                c
                for r in npc.get("opens_shop") or []
                if r.get("map") == mid
                for c in r.get("chapters") or []
            }
        )
        ch = chapters_text(chapters)
        parts.append(links + (f" (chapters {t.esc(ch)})" if ch != "all" else ""))
    warps = [
        w
        for w in ctx.kb.entities("warp")
        if w.get("used")
        and any(
            isinstance(f, Mapping) and f.get("map") == mid and str(f.get("npc")) == str(npc["id"])
            for f in w.get("from") or []
        )
    ]
    dests = sorted({str(w.get("target_map")) for w in warps}, key=map_id_key)
    if dests:
        parts.append(
            "sends the hero to "
            + ", ".join(map_link(ctx, x, False) for x in dests)
            + ' (<a href="#connections">when</a>)'
        )
    for rel, word in (
        ("gives_quests", "gives"),
        ("completes_quests", "completes"),
    ):
        n = len(here(rel))
        if n:
            parts.append(t.esc(f"{word} {n} quest{'s' if n != 1 else ''}"))
    for rel, word in (("crafts", "crafting"), ("trades", "trades"), ("advancements", "job")):
        n = len(here(rel))
        if n:
            label = {"crafting": "recipes", "trades": "trades", "job": "job advancements"}[word]
            parts.append(t.esc(f"{n} {label if n != 1 else label[:-1]}"))
    return "; ".join(parts)


def npcs_table(ctx: Context, e: Entity) -> str:
    mid = str(e["id"])
    rows = []
    refs = [r for r in e.get("npcs") or [] if isinstance(r, Mapping)]
    boxes = d.any_thumb(ctx, (("npc", r["id"]) for r in refs))
    for r in refs:
        npc = ctx.kb.get("npc", str(r["id"]))
        tile = r.get("tile") or []
        roles = _npc_roles(ctx, npc, mid) if npc is not None else ""
        rows.append(
            [
                d.entry_cell(ctx, "npc", str(r["id"]), placeholder=boxes),
                t.markup_cell(
                    t.esc(f"x {tile[0]}, y {tile[1]}") if len(tile) == 2 else "",
                    (tile[0] * 1000 + tile[1]) if len(tile) == 2 else 0,
                ),
                t.markup_cell(
                    roles
                    or ('<span class="muted">decoration</span>' if not r.get("script") else "")
                ),
            ]
        )
    if not rows:
        return ""
    return t.table([t.Column("NPC"), t.Column("Position"), t.Column("Here")], rows)


def monsters_table(ctx: Context, e: Entity) -> str:
    refs = [r for r in e.get("spawns") or [] if isinstance(r, Mapping)]
    if not refs:
        return ""
    boxes = d.any_thumb(ctx, (("monster", r["id"]) for r in refs))
    rows = []
    for r in refs:
        m = ctx.kb.get("monster", str(r["id"])) or {}
        lv = m.get("level")
        rows.append(
            [
                d.entry_cell(ctx, "monster", str(r["id"]), placeholder=boxes),
                t.num_cell(lv) if isinstance(lv, int) else t.Cell("", -1),
                t.num_cell(int(r.get("count") or 0)),
            ]
        )
    cols = [
        t.Column("Monster"),
        t.Column("Level", numeric=True),
        t.Column("Spawn points", numeric=True),
    ]
    return t.table(cols, rows)


def quests_here(ctx: Context, e: Entity) -> str:
    mid = str(e["id"])
    rows: dict[tuple[str, str], list] = {}
    for r in e.get("npcs") or []:
        npc = ctx.kb.get("npc", str(r.get("id")))
        if npc is None:
            continue
        for rel, word in (("gives_quests", "gives"), ("completes_quests", "completes")):
            for q in npc.get(rel) or []:
                if not isinstance(q, Mapping) or q.get("map") != mid:
                    continue
                qid = str(q["id"])
                quest = ctx.kb.get("quest", qid)
                if quest is None:
                    continue
                key = (qid, str(npc["id"]))
                if key not in rows:
                    rows[key] = [quest, npc, []]
                if word not in rows[key][2]:
                    rows[key][2].append(word)
    if not rows:
        return ""
    body = []
    for (qid, nid), (quest, npc, words) in sorted(
        rows.items(), key=lambda kv: (kv[1][0].get("chapter", 0), kv[0])
    ):
        body.append(
            [
                t.markup_cell(ctx.ref("quest", qid), name_of(quest)),
                t.num_cell(quest.get("chapter", 0))
                if isinstance(quest.get("chapter"), int)
                else t.Cell("", -1),
                t.markup_cell(d.link_or_text(ctx, "npc", nid), name_of(npc)),
                t.markup_cell(t.esc(" and ".join(words))),
            ]
        )
    cols = [
        t.Column("Quest"),
        t.Column("Chapter", numeric=True),
        t.Column("NPC"),
        t.Column("The NPC"),
    ]
    return t.table(cols, body)


def map_view(ctx: Context, mid: str) -> str:
    """The render with a toggle for the overlay (portals, NPCs, spawn points)."""
    render = ctx.entity_images("map", mid, ("render",))
    if not render:
        return ""
    r = render[0]
    over = ctx.entity_images("map", mid, ("overlay",))
    big = ctx.entity_images("map", mid, ("render2x",))
    box = t.anchor_id(f"ov-{mid}")
    top = ""
    toggle = ""
    if over:
        top = ctx.img(over[0], cls="overlay")
        toggle = (
            f'<input type="checkbox" class="ov-toggle" id="{box}" checked>'
            f'<label class="ov-label" for="{box}">Show portals, NPCs and spawn points</label>\n'
        )
    link = ""
    if big:
        link = f' <a href="{ctx.url("img/" + big[0].path)}">Double size</a>.'
    cap = ("Portals, NPCs and spawn points are marked." if over else "") + link
    return (
        f'<div class="map-view">{toggle}<figure class="map-figure">{ctx.img(r)}{top}'
        + (f"<figcaption>{cap.strip()}</figcaption>" if cap.strip() else "")
        + "</figure></div>\n"
    )


def map_page(ctx: Context, e: Entity, world: World, by_source) -> Page:
    kb = ctx.kb
    mid = str(e["id"])
    aid = area_of(kb, mid)
    area = kb.get("area", aid) if aid else None
    typ = map_type(e)
    w, h = e.get("width"), e.get("height")
    note = e.get("name_note")
    name_row = ""
    if e.get("name_source") == "default slot":
        name_row = t.esc("probable: a default slot of the name table")
    rows = [
        ("Area", ctx.ref("area", aid) if area is not None else ""),
        ("Map number", t.esc(dotted(mid))),
        ("Type", t.esc(typ[0].upper() + typ[1:])),
        ("Name", name_row),
        ("Size", t.esc(f"{w} by {h} tiles") if w and h else ""),
        ("Music", bgm_markup(kb, e)),
        ("Boss map", "yes" if e.get("boss_map") else "no"),
        ("Saving", t.esc(save_text(e))),
        (
            "Monsters",
            t.esc(
                level_text(monster_levels(kb, [mid]))
                and f"Lv. {level_text(monster_levels(kb, [mid]))}"
            ),
        ),
        ("ID", f"<code>{t.esc(mid)}</code>"),
    ]
    ko = name_of(e, "ko")
    box = t.infobox(
        ctx.name("map", mid),
        [(k, v) for k, v in rows if v],
        subtitle_markup=verbatim(ko, "ko") if ko != name_of(e) else "",
    )
    an = ctx.ref("area", aid) if area is not None else "no area"
    article = "an" if typ[0] in "aeiou" else "a"
    lead = f"<p>{ctx.name('map', mid)} is {article} {t.esc(typ)} of {an}, map {t.esc(dotted(mid))}."
    if e.get("name_source") == "default slot":
        lead += " Its name is probably a town's, not its own (see Notes)."
    if mid in world.unreached:
        lead += " No warp of the game leads here (see Unused content)."
    elif mid in START_MAPS:
        lead += " A new game starts here."
    lead += "</p>\n"
    parts = [box, lead, _worldmap.view_link(ctx, "map", mid)]
    view = map_view(ctx, mid)
    if view:
        parts.append(t.section("Map", view, "map"))
    mons = monsters_table(ctx, e)
    if mons:
        parts.append(t.section("Monsters", mons, "monsters"))
    npcs = npcs_table(ctx, e)
    if npcs:
        parts.append(t.section("NPCs", npcs, "npcs"))
    qs = quests_here(ctx, e)
    if qs:
        parts.append(t.section("Quests here", qs, "quests"))
    conn, anchored = connections(ctx, e, by_source)
    if conn:
        parts.append(t.section("Connections", conn, "connections"))
    arr = arrivals(ctx, e, kb)
    if arr:
        parts.append(t.section("Arriving from", arr, "arrivals"))
    hidden = [p for p in e.get("portals") or [] if p.get("hidden")]
    if hidden:
        items = []
        for p in hidden:
            num = p.get("number")
            tile = p.get("tile") or []
            at = f" at x {tile[0]}, y {tile[1]}" if len(tile) == 2 else ""
            tg = [str(x.get("map")) for x in p.get("targets") or []]
            if tg:
                dest = "leads to " + ", ".join(map_link(ctx, x) for x in dict.fromkeys(tg))
            else:
                arrive = sorted(
                    {
                        str(x["id"])
                        for x in e.get("entrances") or []
                        if x.get("target_portal") == num
                    }
                    | {
                        s
                        for w in kb.entities("warp")
                        if w.get("target_map") == mid and w.get("target_portal") == num
                        for s in _ids(w.get("source"))
                    },
                    key=map_id_key,
                )
                dest = "names no script, so it leads nowhere"
                if arrive:
                    dest += (
                        "; warps from "
                        + ", ".join(map_link(ctx, x) for x in arrive)
                        + " arrive here, so it is probably an arrival point"
                    )
            items.append(f"<li>{t.esc(f'Portal {num}{at}')}: {dest}.</li>")
        body = (
            "<p>A hidden portal is not drawn and does not show on the minimap, but the hero"
            " can still enter it.</p>\n<ul>" + "".join(items) + "</ul>\n"
        )
        parts.append(t.section("Hidden portals", body, "hidden-portals"))
    evs = _ids(e.get("events"))
    # events whose script warps here (a Monster Carnival round and its room)
    for w in kb.entities("warp"):
        if w.get("target_map") != mid:
            continue
        for sid in _ids(w.get("script")):
            sc = kb.get("script", sid)
            for x in _ids(sc.get("events") if sc is not None else None):
                ev = kb.get("event", x)
                if ev is not None and ev.get("type") == "carnival" and x not in evs:
                    evs.append(x)
    if evs:
        items = [
            f"<li>{ctx.ref('event', x, event_label(kb.get('event', x)))}</li>"
            for x in evs
            if kb.get("event", x) is not None
        ]
        parts.append(t.section("Events", "<ul>" + "".join(items) + "</ul>\n", "events"))
    notes = []
    if e.get("name_source") == "default slot":
        notes.append(
            "The name the game shows here is probably not the map's own: "
            + t.esc(
                str(
                    note
                    or "the map lies past the end of its area's row of the name table, so the"
                    " game falls back to the first name of the area's group, a town"
                )
            )
            + "."
        )
    if e.get("no_save"):
        notes.append(
            t.esc(
                "The game refuses to save here: the map is "
                + NO_SAVE_WORDS.get(e["no_save"], str(e["no_save"]))
                + "."
            )
        )
    if e.get("banner") is False:
        notes.append("The game shows no name banner on entering this map.")
    if notes:
        parts.append(
            t.section(
                "Notes", "<ul>" + "".join(f"<li>{n}</li>" for n in notes) + "</ul>\n", "notes"
            )
        )
    if mid in world.unreached:
        parts.append(
            t.section(
                "Unused content",
                "<p>No warp of the game reaches this map from the start: "
                + t.esc(world.unreached[mid])
                + ".</p>\n",
                "unused",
            )
        )
    prose = ctx.prose(f"map/{slug(mid)}.md")
    if prose:
        parts.insert(2, prose)
    page = Page(
        path=d.entity_path("map", mid),
        title=name_of(e),
        heading=ctx.name("map", mid),
        title_verbatim=True,
        body="\n".join(parts),
        crumbs=[("Areas", "world/index.html#areas")]
        + ([(name_of(area), d.entity_path("area", aid))] if area is not None else []),
        description=f"Map {dotted(mid)}: {name_of(e)}",
    ).documents("map", mid)
    for wid in anchored:
        page.documents("warp", wid, warp_anchor(wid))
    return page


# --- area pages -------------------------------------------------------------------------------


def area_diagram(ctx: Context, a: Entity, world: World) -> str:
    kb = ctx.kb
    aid = str(a["id"])
    maps = sorted(_ids(a.get("maps")), key=map_id_key)
    inside = set(maps)
    nodes, edges = [], []
    outside: dict[str, set[str]] = {}
    for mid in maps:
        e = kb.get("map", mid)
        typ = map_type(e)
        cls = {"town": "wd-town", "boss map": "wd-boss"}.get(typ, "")
        if mid in world.unreached:
            cls = (cls + " wd-unreached").strip()
        nodes.append(Node(mid, dotted(mid), ctx.href("map", mid), name_of(e), cls))
    pairs: dict[tuple[str, str], bool] = {}
    for mid in maps:
        e = kb.get("map", mid)
        for x in e.get("exits") or []:
            other = str(x["id"])
            if other in inside:
                k = tuple(sorted((mid, other)))
                pairs[k] = pairs.get(k, True) and bool(x.get("gated"))
            else:
                outside.setdefault(mid, set()).add(other)
    for src, tgt, w in warp_edges(kb):
        if src in inside and tgt in inside:
            k = tuple(sorted((src, tgt)))
            pairs[k] = pairs.get(k, True) and bool(w.get("conditional"))
        elif src in inside:
            outside.setdefault(src, set()).add(tgt)
        elif tgt in inside:
            outside.setdefault(tgt, set()).add(src)
    for (x, y), dashed in sorted(pairs.items()):
        edges.append(Edge(x, y, dashed))
    for mid, others in sorted(outside.items(), key=lambda kv: map_id_key(kv[0])):
        for o in sorted(others, key=map_id_key):
            node_id = f"out:{o}"
            if node_id not in {n.id for n in nodes}:
                oa = kb.get("area", area_of(kb, o) or "")
                nodes.append(
                    Node(
                        node_id,
                        dotted(o),
                        ctx.href("map", o) if kb.get("map", o) is not None else "",
                        name_of(kb.get("map", o)) if kb.get("map", o) is not None else "",
                        "wd-out",
                        name_of(oa) if oa is not None else "",
                    )
                )
            edges.append(Edge(mid, node_id, True))
    root = f"{aid}-0" if f"{aid}-0" in inside else (maps[0] if maps else "")
    title = f"Maps of area {aid} and how they connect"
    return svg_diagram(nodes, edges, root, title, node_w=72, node_h=34)


def area_page(ctx: Context, a: Entity, world: World) -> Page:
    kb = ctx.kb
    aid = str(a["id"])
    maps = sorted(_ids(a.get("maps")), key=map_id_key)
    lv = monster_levels(kb, maps)
    towns = [m for m in maps if map_type(kb.get("map", m)) == "town"]
    rows = [
        ("Maps", t.esc(t.number(len(maps)))),
        ("Towns", ", ".join(ctx.ref("map", m) for m in towns)),
        ("Monster levels", t.esc(f"Lv. {level_text(lv)}") if lv else ""),
        ("Town music", t.esc(f"track {a['town_bgm']}") if a.get("town_bgm") is not None else ""),
        ("Field music", t.esc(f"track {a['field_bgm']}") if a.get("field_bgm") is not None else ""),
        ("ID", f"<code>{t.esc(aid)}</code>"),
    ]
    ko = name_of(a, "ko")
    parts = [
        t.infobox(
            ctx.name("area", aid),
            [(k, v) for k, v in rows if v],
            subtitle_markup=verbatim(ko, "ko") if ko != name_of(a) else "",
        )
    ]
    same = [
        x
        for x in kb.entities("area")
        if x.get("world_spot") == a.get("world_spot") and str(x["id"]) != aid
    ]
    lead = (
        f"<p>Area {t.esc(aid)} has {t.esc(t.number(len(maps)))} maps"
        + (f", with monsters of levels {t.esc(level_text(lv).replace('-', ' to '))}" if lv else "")
        + "."
    )
    if same:
        lead += (
            " It shares its name with "
            + ", ".join(ctx.ref("area", str(x["id"]), f"area {x['id']}") for x in same)
            + ": areas of one place on the world map share the name."
        )
    lead += "</p>\n"
    parts.append(lead)
    parts.append(_worldmap.view_link(ctx, "area", aid))
    prose = ctx.prose(f"area/{slug(aid)}.md")
    if prose:
        parts.append(prose)
    diagram = area_diagram(ctx, a, world)
    if diagram:
        key = (
            '<p class="muted">Each box is a map and links to its page: a box with a heavy'
            " coloured border is a town, one with a heavy dark border a boss map, a dashed"
            " box a map no warp reaches, a dotted box a map of another area."
            " Dashed lines are warps with a condition or to another area.</p>\n"
        )
        parts.append(t.section("Map of the area", diagram + key, "diagram"))
    # maps table
    body = []
    for mid in maps:
        e = kb.get("map", mid)
        mlv = monster_levels(kb, [mid])
        body.append(
            [
                t.markup_cell(ctx.ref("map", mid), map_id_key(mid)[1]),
                t.markup_cell(t.esc(dotted(mid)), map_id_key(mid)[1]),
                t.text_cell(map_type(e)),
                t.Cell(t.esc(level_text(mlv)), mlv[0] if mlv else -1),
                t.num_cell(len(e.get("spawns") or [])),
                t.text_cell("yes" if not e.get("no_save") else "no"),
            ]
        )
    cols = [
        t.Column("Map"),
        t.Column("Number"),
        t.Column("Type"),
        t.Column("Monster level", numeric=True),
        t.Column("Monster kinds", numeric=True),
        t.Column("Saving"),
    ]
    parts.append(t.section("Maps", t.table(cols, body), "maps"))
    # connections to other areas
    conn = []
    seen = set()
    for src, tgt, w in warp_edges(kb):
        sa, ta = area_of(kb, src), area_of(kb, tgt)
        if sa == ta or aid not in (sa, ta):
            continue
        key = (src, tgt, str(w["id"]))
        if key in seen:
            continue
        seen.add(key)
        when, k = _when_markup(ctx, w, bool(w.get("conditional")))
        conn.append(
            [
                t.markup_cell(map_link(ctx, src), dotted(src)),
                t.markup_cell(map_link(ctx, tgt), dotted(tgt)),
                t.markup_cell(when, k),
                t.markup_cell(t.esc(chapters_text(w.get("chapters")))),
            ]
        )
    if conn:
        cols = [t.Column("From"), t.Column("To"), t.Column("When"), t.Column("Chapters")]
        parts.append(t.section("Connections to other areas", t.table(cols, conn), "connections"))
    # monsters
    mons: dict[str, set[str]] = {}
    for mid in maps:
        for mon in _ids(kb.get("map", mid).get("spawns")):
            mons.setdefault(mon, set()).add(mid)
    if mons:
        boxes = d.any_thumb(ctx, (("monster", m) for m in mons))
        mrows = []
        for mon, where in sorted(
            mons.items(), key=lambda kv: ((kb.get("monster", kv[0]) or {}).get("level", 0), kv[0])
        ):
            m = kb.get("monster", mon) or {}
            mrows.append(
                [
                    d.entry_cell(ctx, "monster", mon, placeholder=boxes),
                    t.num_cell(m["level"]) if isinstance(m.get("level"), int) else t.Cell("", -1),
                    t.markup_cell(
                        ", ".join(map_link(ctx, x, False) for x in sorted(where, key=map_id_key))
                    ),
                ]
            )
        cols = [t.Column("Monster"), t.Column("Level", numeric=True), t.Column("Maps")]
        parts.append(t.section("Monsters", t.table(cols, mrows), "monsters"))
    # towns and shops
    srows = []
    for mid in maps:
        for r in kb.get("map", mid).get("npcs") or []:
            npc = kb.get("npc", str(r["id"]))
            if npc is None:
                continue
            for s in npc.get("opens_shop") or []:
                if s.get("map") != mid:
                    continue
                srows.append(
                    [
                        t.markup_cell(d.link_or_text(ctx, "shop", str(s["id"]))),
                        t.markup_cell(ctx.ref("npc", str(npc["id"])), name_of(npc)),
                        t.markup_cell(map_link(ctx, mid), dotted(mid)),
                        t.text_cell(chapters_text(s.get("chapters"))),
                    ]
                )
    if srows:
        cols = [t.Column("Shop"), t.Column("NPC"), t.Column("Map"), t.Column("Chapters")]
        parts.append(t.section("Shops", t.table(cols, srows), "shops"))
    return Page(
        path=d.entity_path("area", aid),
        title=name_of(a),
        heading=ctx.name("area", aid),
        title_verbatim=True,
        body="\n".join(parts),
        crumbs=[("Areas", "world/index.html#areas")],
        description=f"Area {aid}: {name_of(a)}",
    ).documents("area", aid)


# --- events -----------------------------------------------------------------------------------


def event_label(e: Entity | None) -> str:
    """Our own label for an event: its type, chapter and map."""
    if e is None:
        return ""
    typ = EVENT_TYPES.get(str(e.get("type")), str(e.get("type")))
    args = e.get("args") or []
    if e.get("type") == "scene" and args:
        typ += f" {args[0]}"
    if e.get("type") == "carnival" and args:
        typ += f", mode {args[0]}"
    maps = _ids(e.get("maps"))
    where = f" on map {dotted(maps[0])}" if maps else ""
    if not maps and e.get("type") != "carnival":
        sids = _ids(e.get("script"))
        where = f" of {sids[0].replace('-', '/', 1)}" if sids else ""
    ch = e.get("chapters") or []
    when = f", chapter {chapters_text(ch)}" if ch else ""
    unused = " (unused)" if not e.get("used") else ""
    return f"{typ}{where}{when}{unused}"


def _event_script(kb, e: Entity) -> Entity | None:
    sids = _ids(e.get("script"))
    return kb.get("script", sids[0]) if sids else None


def event_page(ctx: Context, e: Entity) -> Page:
    kb = ctx.kb
    eid = str(e["id"])
    typ = str(e.get("type"))
    maps = [m for m in _ids(e.get("maps")) if kb.get("map", m) is not None]
    script = _event_script(kb, e)
    sid = str(script["id"]) if script is not None else ""
    args = e.get("args") or []
    chapters = e.get("chapters") or []
    trig = ", ".join(TRIGGERS.get(x, x) for x in e.get("trigger") or [])
    rows = [
        ("Type", t.esc(EVENT_TYPES.get(typ, typ))),
        ("Map", ", ".join(map_link(ctx, m) for m in maps)),
        ("Chapters", t.esc(chapters_text(chapters)) if chapters else ""),
        ("Started by", t.esc(trig)),
        ("Scene", t.esc(str(args[0])) if typ == "scene" and args else ""),
        ("Mode", t.esc(str(args[0])) if typ == "carnival" and args else ""),
        ("Script", ctx.ref("script", sid, sid.replace("-", "/", 1)) if sid else ""),
        ("Used", "yes" if e.get("used") else "no"),
        ("ID", f"<code>{t.esc(eid)}</code>"),
    ]
    label = event_label(e)
    parts = [t.infobox(t.esc(label[0].upper() + label[1:]), [(k, v) for k, v in rows if v])]
    native = {
        "boss": "startBoss",
        "scene": "runAnimation",
        "carnival": "initMonCa",
        "chapter_end": "clearAll",
    }.get(typ, typ)
    where = " on " + ", ".join(map_link(ctx, m) for m in maps) if maps else ""
    lead = {
        "boss": f"A boss fight starts{where} when the script calls <code>startBoss</code>.",
        "scene": f"A scene plays{where} when the script calls <code>runAnimation</code>.",
        "carnival": "A Monster Carnival round starts when the script calls <code>initMonCa</code>.",
        "chapter_end": f"The chapter ends{where} when the script calls <code>clearAll</code>:"
        " the quest states are reset and the next chapter begins.",
    }.get(typ, f"The script calls <code>{t.esc(native)}</code>.")
    parts.append(f"<p>{lead}</p>\n")
    cond = conditions_markup(ctx, e.get("conditions"), chapters)
    if cond:
        parts.append(
            t.section(
                "When it happens",
                "<p>It runs" + (f" on {t.esc(trig)}" if trig else "") + f" when {cond}.</p>\n",
                "when",
            )
        )
    if typ == "boss":
        bosses = []
        for m in maps:
            for r in kb.get("map", m).get("spawns") or []:
                mon = kb.get("monster", str(r["id"]))
                if mon is not None:
                    bosses.append(str(r["id"]))
        if bosses:
            items = "".join(
                f"<li>{ctx.ref('monster', b)}"
                f" (level {t.esc(str((kb.get('monster', b) or {}).get('level', '?')))})</li>"
                for b in dict.fromkeys(bosses)
            )
            parts.append(
                t.section("Boss", f"<p>The monsters of the map:</p><ul>{items}</ul>\n", "boss")
            )
    if script is not None:
        tg = [
            w
            for w in kb.entities("warp")
            if sid in _ids(w.get("script")) and (typ != "chapter_end" or w.get("chapter_end"))
        ]
        if tg:
            items = "".join(
                f"<li>{map_link(ctx, str(w.get('target_map')))}"
                + (" (the next chapter starts here)" if w.get("chapter_end") else "")
                + "</li>"
                for w in tg
                if isinstance(w.get("target_map"), str)
            )
            title = "Where it leads" if typ != "carnival" else "Room"
            parts.append(t.section(title, f"<ul>{items}</ul>\n", "leads"))
    notes = []
    if not e.get("used") and script is not None:
        why = script.get("unused_reason") or "the game never runs its script"
        notes.append(f"The game never runs this: {t.esc(str(why))}.")
    if typ == "boss" and maps:
        notes.append("The game refuses to save on a boss map.")
    if typ == "boss" and not e.get("used"):
        started = {m for x in kb.entities("event") if x.get("used") for m in _ids(x.get("maps"))}
        bare = sorted(
            (
                str(m["id"])
                for m in kb.entities("map")
                if m.get("boss_map") and str(m["id"]) not in started
            ),
            key=map_id_key,
        )
        if bare:
            notes.append(
                "No used script starts a boss fight on the boss map"
                + ("s " if len(bare) > 1 else " ")
                + ", ".join(map_link(ctx, m) for m in bare)
                + "; how the fight there starts in 1.5 is not known from the scripts."
            )
    if typ == "carnival":
        notes.append(
            "How a round scores is on the"
            f' <a href="{ctx.url("mechanics/index.html")}">mechanics</a> pages.'
        )
    if notes:
        parts.append(
            t.section(
                "Notes", "<ul>" + "".join(f"<li>{n}</li>" for n in notes) + "</ul>\n", "notes"
            )
        )
    return Page(
        path=event_path(eid),
        title=label[0].upper() + label[1:],
        body="\n".join(parts),
        crumbs=[("Events", "world/events.html")],
        description=f"Event: {label}",
    ).documents("event", eid)


def events_index(ctx: Context) -> Page:
    rows = []
    for e in ctx.kb.entities("event"):
        maps = _ids(e.get("maps"))
        rows.append(
            [
                t.markup_cell(ctx.ref("event", str(e["id"]), event_label(e)), event_label(e)),
                t.text_cell(EVENT_TYPES.get(str(e.get("type")), str(e.get("type")))),
                t.markup_cell(
                    ", ".join(map_link(ctx, m) for m in maps if ctx.kb.get("map", m) is not None),
                    dotted(maps[0]) if maps else "",
                ),
                t.text_cell(chapters_text(e.get("chapters")) if e.get("chapters") else "none"),
                t.text_cell("yes" if e.get("used") else "no"),
            ]
        )
    cols = [
        t.Column("Event"),
        t.Column("Type"),
        t.Column("Map"),
        t.Column("Chapters"),
        t.Column("Used"),
    ]
    lead = (
        "<p>Events are what the scripts start beyond talk and warps: boss fights"
        " (<code>startBoss</code>), scenes (<code>runAnimation</code>), Monster Carnival rounds"
        " (<code>initMonCa</code>) and chapter ends (<code>clearAll</code>).</p>\n"
    )
    return Page(
        "world/events.html",
        "Events",
        lead + t.table(cols, rows),
        description="Boss fights, scenes, Monster Carnival and chapter ends.",
    )


# --- warps list -------------------------------------------------------------------------------


def warps_page(ctx: Context) -> Page:
    kb = ctx.kb
    rows, ids = [], []
    page = Page("world/warps.html", "Warps", "", description="Every changeMap of every script.")
    for w in sorted(kb.entities("warp"), key=lambda w: str(w["id"])):
        wid = str(w["id"])
        srcs = _ids(w.get("source"))
        sids = _ids(w.get("script"))
        when, k = _when_markup(ctx, w, bool(w.get("conditional")))
        tgt = str(w.get("target_map"))
        rows.append(
            [
                t.markup_cell(
                    ctx.ref("script", sids[0], sids[0].replace("-", "/", 1)) if sids else "",
                    sids[0] if sids else "",
                ),
                t.markup_cell(
                    ", ".join(map_link(ctx, s, False) for s in srcs)
                    or '<span class="muted">none</span>',
                    dotted(srcs[0]) if srcs else "",
                ),
                t.markup_cell(map_link(ctx, tgt, False), dotted(tgt)),
                t.markup_cell(when, k),
                t.text_cell(chapters_text(w.get("chapters"))),
                t.text_cell("yes" if w.get("used") else "no"),
            ]
        )
        ids.append(warp_anchor(wid))
        page.documents("warp", wid, warp_anchor(wid))
    cols = [
        t.Column("Script"),
        t.Column("From"),
        t.Column("To"),
        t.Column("When"),
        t.Column("Chapters"),
        t.Column("Used"),
    ]
    lead = (
        "<p>A warp is one <code>changeMap</code> call of a script: a portal's, an NPC's or a"
        " map's own. The map pages list the warps that leave them, under Connections.</p>\n"
    )
    page.body = lead + t.table(cols, rows, ids)
    return page


# --- the atlas and lists ----------------------------------------------------------------------


def area_edges(kb, chapter: int | None = None) -> dict[tuple[str, str], dict[str, Any]]:
    """Area pairs a used warp joins: ``{(a, b): {"chapters": set, "conditional": bool}}``."""
    out: dict[tuple[str, str], dict[str, Any]] = {}
    for src, tgt, w in warp_edges(kb):
        sa, ta = area_of(kb, src), area_of(kb, tgt)
        if sa is None or ta is None or sa == ta:
            continue
        ch = set(w.get("chapters") or [])
        if chapter is not None and chapter not in ch:
            continue
        k = tuple(sorted((sa, ta), key=lambda x: map_id_key(x)))
        cur = out.setdefault(k, {"chapters": set(), "conditional": True})
        cur["chapters"] |= ch
        cur["conditional"] = cur["conditional"] and bool(w.get("conditional"))
    return out


def atlas(ctx: Context, chapter: int | None = None) -> str:
    kb = ctx.kb
    areas = sorted((str(a["id"]) for a in kb.entities("area")), key=map_id_key)
    edges_ = area_edges(kb, chapter)
    if chapter is not None:
        present = {x for k in edges_ for x in k}
        st = {area_of(kb, m) for m in start_maps(kb)} if chapter == 0 else set()
        present |= {x for x in st if x}
        areas = [a for a in areas if a in present]
    nodes = []
    for aid in areas:
        a = kb.get("area", aid)
        nodes.append(Node(aid, f"Area {aid}", ctx.href("area", aid), name_of(a), "", name_of(a)))
    edges = [Edge(a, b, v["conditional"]) for (a, b), v in sorted(edges_.items())]
    root_area = area_of(kb, start_maps(kb)[0]) if start_maps(kb) else (areas[0] if areas else "")
    if root_area not in areas and areas:
        root_area = areas[0]
    title = (
        "The areas and the warps between them"
        if chapter is None
        else f"Areas joined by warps in chapter {chapter}"
    )
    return svg_diagram(nodes, edges, root_area, title, node_w=110, node_h=40, gap_x=46, gap_y=18)


def chapter_table(ctx: Context) -> str:
    kb = ctx.kb
    all_ = area_edges(kb)
    rows = []
    for (a, b), v in sorted(
        all_.items(), key=lambda kv: (map_id_key(kv[0][0]), map_id_key(kv[0][1]))
    ):
        cells = [
            t.markup_cell(ctx.ref("area", a) + t.esc(f" ({a})"), map_id_key(a)[0]),
            t.markup_cell(ctx.ref("area", b) + t.esc(f" ({b})"), map_id_key(b)[0]),
        ]
        for c in CHAPTERS:
            cells.append(t.text_cell("yes" if c in v["chapters"] else ""))
        cells.append(t.text_cell("conditional" if v["conditional"] else "open"))
        rows.append(cells)
    cols = (
        [t.Column("Area"), t.Column("Area")]
        + [t.Column(f"Chapter {c}") for c in CHAPTERS]
        + [t.Column("Warp")]
    )
    return t.table(cols, rows)


def _maps_table(ctx: Context, maps: Sequence[Entity], world: World) -> str:
    kb = ctx.kb
    rows = []
    for e in maps:
        mid = str(e["id"])
        aid = area_of(kb, mid)
        lv = monster_levels(kb, [mid])
        rows.append(
            [
                t.markup_cell(ctx.ref("map", mid), name_of(e)),
                t.Cell(t.esc(dotted(mid)), map_id_key(mid)[0] * 1000 + map_id_key(mid)[1]),
                t.markup_cell(
                    ctx.ref("area", aid) if aid and kb.get("area", aid) else "",
                    map_id_key(aid or "0")[0],
                ),
                t.text_cell(map_type(e)),
                t.Cell(t.esc(level_text(lv)), lv[0] if lv else -1),
                t.text_cell("yes" if not e.get("no_save") else "no"),
                t.text_cell("no" if mid in world.unreached else "yes"),
            ]
        )
    cols = [
        t.Column("Map"),
        t.Column("Number", numeric=True),
        t.Column("Area"),
        t.Column("Type"),
        t.Column("Monster level", numeric=True),
        t.Column("Saving"),
        t.Column("Reached"),
    ]
    return t.table(cols, rows)


def maps_page(ctx: Context, world: World) -> Page:
    kb = ctx.kb
    maps = sorted(kb.entities("map"), key=lambda e: map_id_key(str(e["id"])))
    parts = [
        "<p>Every map of the game. Sort by area, type or monster level with the column"
        " headings.</p>\n",
        t.section("All maps", _maps_table(ctx, maps, world), "all"),
    ]
    for title, anchor, typ in (
        ("Towns", "towns", "town"),
        ("Boss maps", "boss-maps", "boss map"),
        ("Carnival rooms", "carnival", "Monster Carnival room"),
    ):
        sel = [e for e in maps if map_type(e) == typ]
        if sel:
            parts.append(t.section(title, _maps_table(ctx, sel, world), anchor))
    return Page(
        "world/maps.html",
        "All maps",
        "\n".join(parts),
        description="Every map, sortable by area and monster level.",
    )


def routes_page(ctx: Context, world: World) -> Page | None:
    kb = ctx.kb
    towns = world.towns
    if len(towns) < 2:
        return None
    edges = warp_edges(kb)
    cols = [t.Column("From")] + [t.Column(dotted(x), numeric=True) for x in towns]
    matrix = []
    routes = []
    route_ids = []
    for a in towns:
        paths = reached_from(kb, [a])
        row = [t.markup_cell(ctx.ref("map", a) + t.esc(f" ({dotted(a)})"), dotted(a))]
        for b in towns:
            if a == b:
                row.append(t.Cell("", -1))
                continue
            p = paths.get(b)
            if p is None:
                row.append(t.Cell("none", 10**6))
                continue
            rid = t.anchor_id(f"route-{a}-{b}")
            n = len(p) - 1
            row.append(t.Cell(f'<a href="#{rid}">{t.esc(t.number(n))}</a>', n))
            steps = []
            for x, y in zip(p, p[1:], strict=False):
                ws = [w for s, tg, w in edges if s == x and tg == y]
                open_ = any(not w.get("conditional") for w in ws)
                steps.append(map_link(ctx, y, False) + ("" if open_ else "*"))
            routes.append(
                [
                    t.markup_cell(ctx.ref("map", a), dotted(a)),
                    t.markup_cell(ctx.ref("map", b), dotted(b)),
                    t.num_cell(n),
                    t.markup_cell(", ".join(steps)),
                ]
            )
            route_ids.append(rid)
        matrix.append(row)
    lead = (
        "<p>The fewest map changes between the main towns, counting every warp of the"
        " game whatever its condition and chapter. A step marked * is a warp with a"
        " condition (a quest state, a level or a ticket): the route is open only when it"
        " holds. Each number links to its route.</p>\n"
    )
    body = (
        lead
        + t.section("Map changes between towns", t.table(cols, matrix, filter=False), "matrix")
        + t.section(
            "Routes",
            t.table(
                [
                    t.Column("From"),
                    t.Column("To"),
                    t.Column("Changes", numeric=True),
                    t.Column("Way"),
                ],
                routes,
                route_ids,
            ),
            "routes",
        )
    )
    return Page(
        "world/routes.html",
        "Routes between towns",
        body,
        description="The shortest way between the main towns.",
    )


def index_page(ctx: Context, world: World, has_routes: bool) -> Page:
    kb = ctx.kb
    parts = []
    prose = ctx.prose("world/index.md")
    if prose:
        parts.append(prose)
    parts.append(
        f'<p class="wm-view-link"><a href="{ctx.url(_worldmap.PAGE)}">Open the interactive'
        " world map</a>: every area and map, what to do on each, and routes between them.</p>\n"
    )
    parts.append(
        t.section(
            "Atlas",
            atlas(ctx)
            + (
                '<p class="muted">Each box is an area and links to its page. A dashed line is a'
                " connection whose every warp has a condition.</p>\n"
            ),
            "atlas",
        )
    )
    # areas table
    rows = []
    for a in sorted(kb.entities("area"), key=lambda a: map_id_key(str(a["id"]))):
        aid = str(a["id"])
        maps = _ids(a.get("maps"))
        lv = monster_levels(kb, maps)
        rows.append(
            [
                t.markup_cell(ctx.ref("area", aid), name_of(a)),
                t.Cell(t.esc(aid), map_id_key(aid)[0]),
                t.num_cell(len(maps)),
                t.Cell(t.esc(level_text(lv)), lv[0] if lv else -1),
            ]
        )
    parts.append(
        t.section(
            "Areas",
            t.table(
                [
                    t.Column("Area"),
                    t.Column("Number", numeric=True),
                    t.Column("Maps", numeric=True),
                    t.Column("Monster level", numeric=True),
                ],
                rows,
            ),
            "areas",
        )
    )
    figs = [atlas(ctx, c) for c in CHAPTERS]
    parts.append(
        t.section(
            "By chapter",
            "<p>The game runs a chapter's own version of a script over the common one, so"
            " the warps differ by chapter. The table shows in which chapters a warp joins two"
            " areas.</p>\n"
            + chapter_table(ctx)
            + '<div class="world-chapters">'
            + "".join(figs)
            + "</div>\n",
            "chapters",
        )
    )
    links = [
        f'<li><a href="{ctx.url("world/maps.html")}">All maps</a>, with the towns, boss maps'
        " and Monster Carnival rooms</li>",
        f'<li><a href="{ctx.url("world/events.html")}">Events</a>: boss fights, scenes,'
        " Monster Carnival and chapter ends</li>",
        f'<li><a href="{ctx.url("world/warps.html")}">Warps</a>: every map change of every'
        " script</li>",
    ]
    if has_routes:
        links.insert(
            1,
            f'<li><a href="{ctx.url("world/routes.html")}">Routes between towns</a></li>',
        )
    parts.append(t.section("Lists", "<ul>" + "".join(links) + "</ul>\n", "lists"))
    # unreached
    if world.unreached:
        urows = []
        for mid, why in sorted(world.unreached.items(), key=lambda kv: map_id_key(kv[0])):
            urows.append(
                [
                    t.markup_cell(map_link(ctx, mid), dotted(mid)),
                    t.text_cell(why[0].upper() + why[1:]),
                ]
            )
        start = ", ".join(map_link(ctx, m) for m in start_maps(kb))
        parts.append(
            t.section(
                "Maps no warp reaches",
                f"<p>A new game starts on {start}. Following every warp from there, whatever"
                f" its condition, reaches {t.esc(t.number(len(world.reached)))} of"
                f" {t.esc(t.number(len(kb.entities('map'))))} maps. These are the rest.</p>\n"
                + t.table([t.Column("Map"), t.Column("Why")], urows),
                "unreached",
            )
        )
    return Page("world/index.html", "World", "\n".join(parts))


# --- the section ------------------------------------------------------------------------------


def pages(ctx: Context):
    kb = ctx.kb
    world = world_facts(kb)
    by_source = _warps_by_source(kb)
    routes = routes_page(ctx, world)
    yield index_page(ctx, world, routes is not None)
    yield _worldmap.page(ctx)
    yield maps_page(ctx, world)
    if routes is not None:
        yield routes
    yield warps_page(ctx)
    yield events_index(ctx)
    for e in kb.entities("event"):
        yield event_page(ctx, e)
    for a in kb.entities("area"):
        yield area_page(ctx, a, world)
    for e in kb.entities("map"):
        yield map_page(ctx, e, world, by_source)


SECTION = Section(
    "world",
    "World",
    40,
    ("area", "map", "warp", "event"),
    pages,
    blurb="The atlas, every area and map, and the way between them.",
    image="map:0-0",
    card_link=("Open the world map", _worldmap.PAGE),
)
