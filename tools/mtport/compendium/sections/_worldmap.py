"""The interactive world map of the World section (#232): ``world/map.html`` and
``world/worldmap.json``.

The data (:func:`world_data`) is one JSON document built from the knowledge base: the world
spots, every area and map with its position, names, level range, pictures and the things worth
doing there (training value, monsters, shops with their keepers, quests by chapter, recipes,
trades, bosses and events, hidden portals, whether the game saves there), and every used warp
with its chapters and whether it has a condition. Every link in it is a site marker, so it
resolves like a page's links. The page inlines the same document (a ``fetch`` of a file fails
from ``file://``), and writes it to ``world/worldmap.json`` for other tools.

Three levels, each its own coordinate space:

- **world**: the game's own world-map art (``img/world/worldmap.png``, exported by the images
  step) with the areas pinned at its seven world spots (:data:`SPOT_XY`), or, without the art,
  the areas laid out by the warp graph (:func:`world_layout`);
- **area**: the area's maps as tiles on a grid, each placed beside its neighbour in the
  direction of the portal that joins them (:func:`grid_layout`), and the maps of other areas
  it leads to as stubs;
- **map**: the map's render with the overlay, and markers for its NPCs and portals.

The viewer is hand-written script and style (``_worldmap.js``, ``_worldmap.css`` beside this
module), inlined in the page. Its pure functions (level fit, chapter filters, routes, search,
deep links, the pan and zoom maths) are tested under Node (``tests/test_compendium_worldmap*``).
Below the viewer, a text list holds the same content for readers without script and for
screen readers; its entries have the ids the deep links use (``map=2-3``, ``area=4``).
"""

from __future__ import annotations

import json
import math
from collections import deque
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

from mtport.compendium import templates as t
from mtport.compendium.siteapi import IMG_DIR, Context, Entity, Image, Page, name_of, verbatim

PAGE = "world/map.html"
JSON_PATH = "world/worldmap.json"
SCHEMA = 1
HERE = Path(__file__).resolve().parent
WORLD_KEY = ("world", "map")  # the images step's key for the world-map art

# The centres of the seven world-spot dots on the game's world-map art (338 by 278 pixels),
# measured from the picture; the index is the world spot (CDataMgr::AreaWorldSpot 0x36E65,
# docs/re/map-names.md). tests/test_compendium_worldmap_real.py checks each against the art.
SPOT_XY: dict[int, tuple[int, int]] = {
    0: (29, 114),
    1: (68, 97),
    2: (68, 138),
    3: (81, 165),
    4: (127, 52),
    5: (201, 77),
    6: (195, 190),
}
ART_SIZE = (338, 278)

# The area level's grid: a cell per map, in area units (pixels at zoom 1).
TILE_W, TILE_H = 200, 140
GAP_X, GAP_Y = 72, 64
PAD = 40
# The world level without the art: a box per area.
NODE_W, NODE_H, NODE_GAP_X, NODE_GAP_Y = 150, 60, 70, 34
TILE_PX = 16  # map tiles are 16 pixels in the renders (docs/formats/map.md)
# The level filter's reading aid (our own choice, not a rule of the game): a map is "too hard"
# when its weakest monster is more than ABOVE levels over the hero, "too easy" when its
# strongest is more than BELOW levels under.
FIT_ABOVE, FIT_BELOW = 5, 10

DIRS = {"E": (1, 0), "W": (-1, 0), "N": (0, -1), "S": (0, 1)}
LAYERS = (
    ("train", "Training"),
    ("shops", "Shops"),
    ("quests", "Quests"),
    ("craft", "Crafting and trades"),
    ("bosses", "Bosses and events"),
    ("secrets", "Hidden portals"),
)


# --- layout (pure) ----------------------------------------------------------------------------


def portal_direction(tile: Sequence[int] | None, width: int, height: int) -> str | None:
    """The side of the map a portal at ``tile`` lies on: ``E``, ``W``, ``N`` or ``S``, by the
    larger of its horizontal and vertical offsets from the centre (ties go sideways, east
    when centred). ``None`` without a tile (an NPC's warp)."""
    if not tile or len(tile) != 2 or not width or not height:
        return None
    fx = (tile[0] + 0.5) / width - 0.5
    fy = (tile[1] + 0.5) / height - 0.5
    if abs(fx) >= abs(fy):
        return "W" if fx < 0 else "E"
    return "N" if fy < 0 else "S"


_OPPOSITE = {"E": "W", "W": "E", "N": "S", "S": "N", None: None}


def _ring(cx: int, cy: int, r: int) -> list[tuple[int, int]]:
    if r == 0:
        return [(cx, cy)]
    out = []
    for dx in range(-r, r + 1):
        for dy in range(-r, r + 1):
            if max(abs(dx), abs(dy)) == r:
                out.append((cx + dx, cy + dy))
    return out


def _free_cell(
    taken: set[tuple[int, int]], at: tuple[int, int], want: tuple[int, int]
) -> tuple[int, int]:
    """The free cell nearest ``want`` (a neighbour of ``at``): rings around ``want``; in each
    ring the cells still on the wanted side of ``at`` first, then the nearer to ``at``, then
    those one step along that side (a column or row beside ``at``), then top to bottom and
    left to right."""
    d = (want[0] - at[0], want[1] - at[1])

    def key(c: tuple[int, int]) -> tuple[bool, int, int, int, int]:
        along = (c[0] - at[0]) * d[0] + (c[1] - at[1]) * d[1]
        return (along < 1, abs(c[0] - at[0]) + abs(c[1] - at[1]), abs(along - 1), c[1], c[0])

    r = 0
    while True:
        ring = [c for c in _ring(want[0], want[1], r) if c not in taken]
        if ring:
            return min(ring, key=key)
        r += 1


def grid_layout(
    nodes: Sequence[str],
    links: Sequence[tuple[str, str, str | None]],
    root: str,
    max_cols: int | None = None,
) -> dict[str, tuple[int, int]]:
    """Grid cells ``(col, row)`` for ``nodes``: breadth first from ``root``, each node in the
    cell beside the node that reaches it, on the side its portal is (``links``:
    ``(a, b, side of a)``; the reverse link counts with the opposite side), or the nearest
    free cell.

    A row may hold ``max_cols`` cells (default about 1.3 times the square root of the node
    count): a chain that would run wider turns down a row and runs back the other way, as a
    line of text does, so a long string of maps stays readable. Nodes the root does not reach
    start again beside the rest, or below them when that is as wide as a row may be.
    Deterministic; the cells are shifted so the smallest column and row are 0."""
    order = list(dict.fromkeys(nodes))
    if max_cols is None:
        max_cols = max(3, math.ceil(math.sqrt(len(order)) * 1.3))
    adj: dict[str, list[tuple[str, str | None]]] = {n: [] for n in order}
    for a, b, side in links:
        if a in adj and b in adj and a != b:
            adj[a].append((b, side))
            adj[b].append((a, _OPPOSITE.get(side)))
    rank = {n: i for i, n in enumerate(order)}
    for n in adj:
        # a sided link before a sideless one, then node order
        adj[n].sort(key=lambda e: (e[1] is None, rank[e[0]]))
    pos: dict[str, tuple[int, int]] = {}
    mirror: dict[str, bool] = {}
    taken: set[tuple[int, int]] = set()

    def width_with(col: int) -> int:
        cols = [c for c, _ in pos.values()] + [col]
        return max(cols) - min(cols) + 1

    starts = ([root] if root in adj else []) + order
    for start in starts:
        if start in pos:
            continue
        if not pos:
            cell = (0, 0)
        else:
            right = max(c for c, _ in pos.values()) + 2
            if width_with(right) <= max_cols:
                cell = _free_cell(taken, (right - 1, 0), (right, 0))
            else:
                left = min(c for c, _ in pos.values())
                below = max(r for _, r in pos.values()) + 1
                cell = _free_cell(taken, (left, below - 1), (left, below))
        pos[start] = cell
        mirror[start] = False
        taken.add(cell)
        q = deque([start])
        while q:
            a = q.popleft()
            for b, side in adj[a]:
                if b in pos:
                    continue
                dx, dy = DIRS.get(side or "E", (1, 0))
                flip = mirror[a]
                if flip:
                    dx = -dx
                ax, ay = pos[a]
                if dx and width_with(ax + dx) > max_cols:
                    dx, dy, flip = 0, 1, not flip  # turn down a row and run back
                cell = _free_cell(taken, (ax, ay), (ax + dx, ay + dy))
                pos[b] = cell
                mirror[b] = flip
                taken.add(cell)
                q.append(b)
    if not pos:
        return {}
    mc = min(c for c, _ in pos.values())
    mr = min(r for _, r in pos.values())
    return {n: (c - mc, r - mr) for n, (c, r) in pos.items()}


def cell_box(cell: tuple[int, int]) -> tuple[int, int, int, int]:
    """``(x, y, w, h)`` of a grid cell in area units."""
    c, r = cell
    return PAD + c * (TILE_W + GAP_X), PAD + r * (TILE_H + GAP_Y), TILE_W, TILE_H


def grid_size(cells: Iterable[tuple[int, int]]) -> tuple[int, int]:
    cells = list(cells)
    if not cells:
        return 2 * PAD + TILE_W, 2 * PAD + TILE_H
    cols = max(c for c, _ in cells) + 1
    rows = max(r for _, r in cells) + 1
    return (
        2 * PAD + cols * TILE_W + (cols - 1) * GAP_X,
        2 * PAD + rows * TILE_H + (rows - 1) * GAP_Y,
    )


def spread(center: tuple[int, int], n: int, step: int = 12) -> list[tuple[int, int]]:
    """``n`` points in a row centred on ``center``, ``step`` apart (areas that share a spot)."""
    x, y = center
    return [(round(x + (i - (n - 1) / 2) * step), y) for i in range(n)]


def world_layout(
    areas: Sequence[str],
    spot_of: Mapping[str, int | None],
    edges: Iterable[tuple[str, str]],
    art: bool,
) -> tuple[list[dict[str, Any]], tuple[int, int]]:
    """The world level's spots: ``[{id, x, y, areas}]`` and the canvas size. With the art,
    the areas of one world spot share its dot (:data:`SPOT_XY`); without it (or for a spot the
    art lacks) each area is a spot of its own, laid out in columns by breadth-first distance
    over ``edges`` from the first area."""
    if art and all(spot_of.get(a) in SPOT_XY for a in areas):
        groups: dict[int, list[str]] = {}
        for a in areas:
            groups.setdefault(int(spot_of[a]), []).append(a)  # type: ignore[arg-type]
        spots = [
            {"id": s, "x": SPOT_XY[s][0], "y": SPOT_XY[s][1], "areas": groups[s]}
            for s in sorted(groups)
        ]
        return spots, ART_SIZE
    adj: dict[str, list[str]] = {a: [] for a in areas}
    for a, b in edges:
        if a in adj and b in adj and a != b:
            adj[a].append(b)
            adj[b].append(a)
    depth: dict[str, int] = {}
    base = 0
    for r in areas:
        if r in depth:
            continue
        depth[r] = base
        q = deque([r])
        while q:
            x = q.popleft()
            for y in sorted(adj[x], key=list(areas).index):
                if y not in depth:
                    depth[y] = depth[x] + 1
                    q.append(y)
        base = max(depth.values()) + 1
    rows: dict[int, int] = {}
    spots = []
    for i, a in enumerate(sorted(areas, key=lambda a: (depth[a], list(areas).index(a)))):
        c = depth[a]
        r = rows.get(c, 0)
        rows[c] = r + 1
        spots.append(
            {
                "id": 100 + i,
                "x": PAD + c * (NODE_W + NODE_GAP_X) + NODE_W // 2,
                "y": PAD + r * (NODE_H + NODE_GAP_Y) + NODE_H // 2,
                "areas": [a],
            }
        )
    cols = max(depth.values(), default=0) + 1
    nrows = max(rows.values(), default=1)
    return spots, (
        2 * PAD + cols * NODE_W + (cols - 1) * NODE_GAP_X,
        2 * PAD + nrows * NODE_H + (nrows - 1) * NODE_GAP_Y,
    )


def level_fit(levels: Sequence[int] | None, hero: int) -> str:
    """The level filter in Python (the script's ``levelFit``): ``none`` (no monsters),
    ``hard``, ``easy`` or ``good``."""
    if not levels:
        return "none"
    if levels[0] > hero + FIT_ABOVE:
        return "hard"
    if levels[1] < hero - FIT_BELOW:
        return "easy"
    return "good"


# --- the data -----------------------------------------------------------------------------


def _ids(rows: Any) -> list[str]:
    if isinstance(rows, Mapping):
        rows = [rows]
    return [str(r["id"]) for r in rows or [] if isinstance(r, Mapping) and "id" in r]


def _names(e: Entity) -> dict[str, str]:
    return {"en": _clean(name_of(e, "en")), "ko": _clean(name_of(e, "ko"))}


def _clean(s: str) -> str:
    return str(s).replace("\x00", "").replace("\x01", "")


def _pic(ctx: Context, image: Image | None) -> dict[str, Any] | None:
    if image is None:
        return None
    return {"s": ctx.url(f"{IMG_DIR}/{image.path}"), "w": image.w, "h": image.h}


def _first(ctx: Context, kind: str, eid: str, roles: Sequence[str]) -> Image | None:
    got = ctx.entity_images(kind, eid, roles)
    return got[0] if got else None


def _person(ctx: Context, nid: str) -> dict[str, Any] | None:
    npc = ctx.kb.get("npc", nid)
    if npc is None:
        return None
    return {"id": nid, "n": _names(npc), "u": ctx.href("npc", nid)}


def _chapters(v: Any) -> list[int]:
    if isinstance(v, int):
        return [v]
    return sorted({int(c) for c in v or [] if isinstance(c, int)})


def _map_things(ctx: Context, e: Entity, world_mod, spots: Mapping[str, Any]) -> dict[str, Any]:
    """The things to do on one map."""
    from mtport.compendium.sections import npcs as npcsec

    kb = ctx.kb
    mid = str(e["id"])
    w, h = int(e.get("width") or 0), int(e.get("height") or 0)
    out: dict[str, Any] = {}
    # monsters
    mobs = []
    for r in e.get("spawns") or []:
        if not isinstance(r, Mapping):
            continue
        mon = kb.get("monster", str(r.get("id")))
        if mon is None:
            continue
        lv = mon.get("level")
        boss = mon.get("class") in ("boss",) or bool(mon.get("boss_bar"))
        mobs.append(
            {
                "id": str(mon["id"]),
                "n": _names(mon),
                "u": ctx.href("monster", str(mon["id"])),
                "lv": lv if isinstance(lv, int) else None,
                "count": int(r.get("count") or 0),
                "exp": mon.get("exp") if isinstance(mon.get("exp"), int) else None,
                "boss": boss,
                "pic": _pic(ctx, ctx.images.main("monster", str(mon["id"]))),
            }
        )
    mobs.sort(key=lambda m: (m["lv"] if m["lv"] is not None else 10**6, m["id"]))
    out["mobs"] = mobs
    spot = spots.get(mid)
    out["train"] = (
        {
            "exp": spot.exp,
            "eph": round(spot.exp_per_hp, 3),
            "band": spot.band,
            "spawns": spot.spawns,
        }
        if spot is not None
        else None
    )
    # NPCs and what they do here
    people = []
    shops, quests, recipes, trades = [], {}, [], []
    for r in e.get("npcs") or []:
        if not isinstance(r, Mapping):
            continue
        nid = str(r.get("id"))
        npc = kb.get("npc", nid)
        if npc is None:
            continue
        roles = []
        for s in npc.get("opens_shop") or []:
            if not isinstance(s, Mapping) or s.get("map") != mid:
                continue
            shop = kb.get("shop", str(s["id"]))
            if shop is None:
                continue
            if "shop" not in roles:
                roles.append("shop")
            shops.append(
                {
                    "id": str(shop["id"]),
                    "u": ctx.href("shop", str(shop["id"])),
                    "kind": npcsec.shop_kind(ctx, shop),
                    "npc": _person(ctx, nid),
                    "ch": _chapters(s.get("chapters")),
                }
            )
        for rel, word in (("gives_quests", "gives"), ("completes_quests", "completes")):
            for q in npc.get(rel) or []:
                if not isinstance(q, Mapping) or q.get("map") != mid:
                    continue
                qid = str(q["id"])
                quest = kb.get("quest", qid)
                if quest is None:
                    continue
                if "quest" not in roles:
                    roles.append("quest")
                key = f"{qid}/{nid}"
                if key not in quests:
                    quests[key] = {
                        "id": qid,
                        "n": _names(quest),
                        "u": ctx.href("quest", qid),
                        "ch": quest.get("chapter")
                        if isinstance(quest.get("chapter"), int)
                        else None,
                        "main": bool(quest.get("main_story")) or quest.get("quest_kind") == "main",
                        "npc": _person(ctx, nid),
                        "role": [],
                    }
                if word not in quests[key]["role"]:
                    quests[key]["role"].append(word)
        if any(isinstance(c, Mapping) and c.get("map") == mid for c in npc.get("crafts") or []):
            roles.append("craft")
        if any(isinstance(c, Mapping) and c.get("map") == mid for c in npc.get("trades") or []):
            roles.append("trade")
        tile = r.get("tile") or []
        people.append(
            {
                "id": nid,
                "n": _names(npc),
                "u": ctx.href("npc", nid),
                "x": tile[0] * TILE_PX + TILE_PX // 2 if len(tile) == 2 else None,
                "y": tile[1] * TILE_PX + TILE_PX // 2 if len(tile) == 2 else None,
                "roles": roles,
                "pic": _pic(ctx, ctx.images.main("npc", nid)),
            }
        )
    for kind, rel, bucket in (("recipe", "crafted_by", recipes), ("trade", "offered_by", trades)):
        for x in kb.entities(kind):
            at = [
                str(c["id"])
                for c in x.get(rel) or []
                if isinstance(c, Mapping) and c.get("map") == mid
            ]
            if not at:
                continue
            res = _ids(x.get("result"))
            item = res[0] if res and kb.get("item", res[0]) is not None else None
            bucket.append(
                {
                    "id": str(x["id"]),
                    "n": _names(x),
                    "u": ctx.href(kind, str(x["id"])),
                    "ch": _chapters(x.get("chapter")),
                    "npc": _person(ctx, at[0]),
                    "pic": _pic(ctx, _first(ctx, "item", item, ("icon",))) if item else None,
                }
            )
    out["npcs"] = people
    out["shops"] = shops
    out["quests"] = sorted(
        quests.values(),
        key=lambda q: (q["ch"] if q["ch"] is not None else 99, not q["main"], q["id"]),
    )
    out["recipes"] = sorted(recipes, key=lambda r: (r["ch"], r["id"]))
    out["trades"] = sorted(trades, key=lambda r: (r["ch"], r["id"]))
    # events: the map's own, and Monster Carnival rounds whose script warps here
    evs = _ids(e.get("events"))
    for wp in kb.entities("warp"):
        if wp.get("target_map") != mid:
            continue
        for sid in _ids(wp.get("script")):
            sc = kb.get("script", sid)
            for x in _ids(sc.get("events") if sc is not None else None):
                ev = kb.get("event", x)
                if ev is not None and ev.get("type") == "carnival" and x not in evs:
                    evs.append(x)
    out["events"] = [
        {
            "id": x,
            "label": world_mod.event_label(kb.get("event", x)),
            "u": ctx.href("event", x),
            "type": str(kb.get("event", x).get("type")),
            "ch": _chapters(kb.get("event", x).get("chapters")),
        }
        for x in evs
        if kb.get("event", x) is not None
    ]
    # portals
    portals = []
    for p in e.get("portals") or []:
        if not isinstance(p, Mapping):
            continue
        tile = p.get("tile") or []
        to = [str(x.get("map")) for x in p.get("targets") or [] if isinstance(x, Mapping)]
        to = [m for m in dict.fromkeys(to) if kb.get("map", m) is not None]
        portals.append(
            {
                "n": p.get("number"),
                "hidden": bool(p.get("hidden")),
                "x": tile[0] * TILE_PX + TILE_PX // 2 if len(tile) == 2 else None,
                "y": tile[1] * TILE_PX + TILE_PX if len(tile) == 2 else None,
                "side": portal_direction(tile, w, h),
                "to": to,
                "gated": any(
                    bool(x.get("gated")) for x in p.get("targets") or [] if isinstance(x, Mapping)
                ),
            }
        )
    out["portals"] = portals
    return out


def world_data(ctx: Context) -> dict[str, Any]:
    """The world map's data (see the module docstring); link markers in place of URLs."""
    from mtport.compendium.sections import mechanics
    from mtport.compendium.sections import world as world_mod

    kb = ctx.kb
    facts = world_mod.world_facts(kb)
    spots_by_map = {s.map_id: s for s in mechanics.training_spots(kb)}
    areas = sorted((str(a["id"]) for a in kb.entities("area")), key=world_mod.map_id_key)
    edges = world_mod.warp_edges(kb)
    # world level
    art = _first(ctx, *WORLD_KEY, ("render",))
    area_pairs = [
        (world_mod.area_of(kb, a), world_mod.area_of(kb, b))
        for a, b, _ in edges
        if world_mod.area_of(kb, a) != world_mod.area_of(kb, b)
    ]
    spot_of = {a: kb.get("area", a).get("world_spot") for a in areas}
    spots, size = world_layout(
        areas, spot_of, [(a, b) for a, b in area_pairs if a and b], art is not None
    )
    spot_names = {}
    for s in spots:
        first = kb.get("area", s["areas"][0])
        spot_names[s["id"]] = _names(first)
    # maps
    maps_out: dict[str, dict[str, Any]] = {}
    area_out = []
    for aid in areas:
        a = kb.get("area", aid)
        listed = set(_ids(a.get("maps"))) | {
            str(e["id"]) for e in kb.entities("map") if world_mod.area_of(kb, str(e["id"])) == aid
        }
        mids = sorted((m for m in listed if kb.get("map", m) is not None), key=world_mod.map_id_key)
        inside = set(mids)
        links: list[tuple[str, str, str | None]] = []
        outs: dict[str, str] = {}
        for mid in mids:
            e = kb.get("map", mid)
            w, h = int(e.get("width") or 0), int(e.get("height") or 0)
            for p in e.get("portals") or []:
                side = portal_direction(p.get("tile"), w, h)
                for x in p.get("targets") or []:
                    tgt = str(x.get("map"))
                    if kb.get("map", tgt) is None or tgt == mid:
                        continue
                    if tgt in inside:
                        links.append((mid, tgt, side))
                    else:
                        outs.setdefault(tgt, mid)
                        links.append((mid, f"out:{tgt}", side))
        for src, tgt, _w in edges:  # NPC warps and anything the portals miss
            if src in inside and tgt in inside:
                links.append((src, tgt, None))
            elif src in inside and kb.get("map", tgt) is not None:
                if tgt not in outs:
                    outs[tgt] = src
                    links.append((src, f"out:{tgt}", None))
            elif tgt in inside and kb.get("map", src) is not None and src not in outs:
                outs[src] = tgt
                links.append((tgt, f"out:{src}", None))
        root = f"{aid}-0" if f"{aid}-0" in inside else (mids[0] if mids else "")
        out_nodes = [f"out:{o}" for o in sorted(outs, key=world_mod.map_id_key)]
        cells = grid_layout(mids + out_nodes, links, root)
        aw, ah = grid_size(cells.values())
        lv = world_mod.monster_levels(kb, mids)
        towns = [m for m in mids if world_mod.map_type(kb.get("map", m)) == "town"]
        stubs = []
        for o in out_nodes:
            x, y, tw, th = cell_box(cells[o])
            om = o[4:]
            stubs.append(
                {
                    "map": om,
                    "area": world_mod.area_of(kb, om),
                    "from": outs[om],
                    "x": x,
                    "y": y + th // 2 - 28,
                    "w": tw,
                    "h": 56,
                }
            )
        area_out.append(
            {
                "id": aid,
                "n": _names(a),
                "u": ctx.href("area", aid),
                "spot": next(s["id"] for s in spots if aid in s["areas"]),
                "lv": list(lv) if lv else None,
                "maps": mids,
                "towns": towns,
                "w": aw,
                "h": ah,
                "outs": stubs,
            }
        )
        for mid in mids:
            e = kb.get("map", mid)
            x, y, tw, th = cell_box(cells[mid])
            mlv = world_mod.monster_levels(kb, [mid])
            render = _first(ctx, "map", mid, ("render",))
            entry = {
                "id": mid,
                "area": aid,
                "n": _names(e),
                "num": world_mod.dotted(mid),
                "u": ctx.href("map", mid),
                "type": world_mod.map_type(e),
                "lv": list(mlv) if mlv else None,
                "save": not e.get("no_save"),
                "nosave": world_mod.save_text(e) if e.get("no_save") else None,
                "reached": mid not in facts.unreached,
                "x": x,
                "y": y,
                "w": tw,
                "h": th,
                "thumb": _pic(ctx, _first(ctx, "map", mid, ("thumb",))),
                "img": _pic(ctx, render),
                "ov": _pic(ctx, _first(ctx, "map", mid, ("overlay",))),
                "mw": int(e.get("width") or 0) * TILE_PX,
                "mh": int(e.get("height") or 0) * TILE_PX,
            }
            entry.update(_map_things(ctx, e, world_mod, spots_by_map))
            maps_out[mid] = entry
    edge_out = []
    for src, tgt, wp in edges:
        edge_out.append(
            {
                "a": src,
                "b": tgt,
                "ch": _chapters(wp.get("chapters")),
                "c": bool(wp.get("conditional")),
                "npc": any(
                    isinstance(r, Mapping) and r.get("role") == "npc"
                    for r in wp.get("source") or []
                ),
            }
        )
    # maps no area lists (not on the map; the text list still names them)
    others = [
        {"id": mid, "n": _names(e), "u": ctx.href("map", mid), "num": world_mod.dotted(mid)}
        for e in sorted(kb.entities("map"), key=lambda e: world_mod.map_id_key(str(e["id"])))
        if (mid := str(e["id"])) not in maps_out
    ]
    levels = [int(x["id"]) for x in kb.entities("level") if str(x.get("id", "")).isdigit()]
    for s in spots:
        s["n"] = spot_names[s["id"]]
    return {
        "schema": SCHEMA,
        "world": {
            "w": size[0],
            "h": size[1],
            "art": _pic(ctx, art),
        },
        "spots": spots,
        "areas": area_out,
        "maps": [maps_out[m] for m in sorted(maps_out, key=world_mod.map_id_key)],
        "edges": edge_out,
        "others": others,
        "start": world_mod.start_maps(kb),
        "chapters": list(world_mod.CHAPTERS),
        "maxLevel": max(levels) if levels else 99,
        "fit": {"above": FIT_ABOVE, "below": FIT_BELOW},
        "tile": {"w": TILE_W, "h": TILE_H},
    }


def to_json(data: Mapping[str, Any]) -> str:
    """The data as JSON, with its link markers kept whole (``json.dumps`` would escape their
    control characters) and every ``<`` escaped, so the text is safe inside a ``<script>``."""
    text = json.dumps(data, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return text.replace("\\u0000", "\x00").replace("\\u0001", "\x01").replace("<", "\\u003c")


# --- the page -------------------------------------------------------------------------------


def _asset(name: str) -> str:
    return (HERE / name).read_text(encoding="utf-8")


def _v(names: Mapping[str, str], lang: str = "en") -> str:
    return verbatim(names.get(lang) or names.get("en") or "", lang)


def _lv(lv: Sequence[int] | None) -> str:
    if not lv:
        return ""
    return f"Lv. {lv[0]}" if lv[0] == lv[1] else f"Lv. {lv[0]} to {lv[1]}"


def _ch(ch: Sequence[int] | int | None) -> str:
    if ch is None or ch == []:
        return ""
    if isinstance(ch, int):
        return f"chapter {ch}"
    if len(ch) == 1:
        return f"chapter {ch[0]}"
    return "chapters " + ", ".join(str(c) for c in ch[:-1]) + f" and {ch[-1]}"


def _a(u: str, inner: str) -> str:
    return f'<a href="{u}">{inner}</a>'


def map_list_item(m: Mapping[str, Any], maps: Mapping[str, Mapping[str, Any]]) -> str:
    """One map of the text list: its link, kind, levels and the things to do there."""
    head = (
        f"{_a(m['u'], _v(m['n']))} "
        f'<span class="muted">({t.esc(m["num"])}, {t.esc(m["type"])}'
        + (f", {t.esc(_lv(m['lv']))}" if m["lv"] else "")
        + (", no saving" if not m["save"] else "")
        + ")</span>"
    )
    rows = []
    if m["train"]:
        tr = m["train"]
        rows.append(
            f"Training: {t.esc(t.number(tr['exp']))} EXP for one kill of every spawn,"
            f" {t.esc(format(tr['eph'], '.3f'))} EXP per HP."
        )
    if m["mobs"]:
        rows.append(
            "Monsters: "
            + ", ".join(
                _a(x["u"], _v(x["n"]))
                + (f" (Lv. {x['lv']}{', boss' if x['boss'] else ''})" if x["lv"] else "")
                for x in m["mobs"]
            )
            + "."
        )
    for s in m["shops"]:
        who = f" kept by {_a(s['npc']['u'], _v(s['npc']['n']))}" if s["npc"] else ""
        when = f", {t.esc(_ch(s['ch']))}" if s["ch"] else ""
        rows.append(f"Shop: {_a(s['u'], t.esc(s['kind'].capitalize() + ' shop'))}{who}{when}.")
    for q in m["quests"]:
        npc = _a(q["npc"]["u"], _v(q["npc"]["n"])) if q["npc"] else "an NPC"
        rows.append(
            f"Quest ({t.esc(_ch(q['ch']) or 'any chapter')}): {_a(q['u'], _v(q['n']))},"
            f" {npc} {t.esc(' and '.join(q['role']))} it."
        )
    for label, bucket in (("Recipe", m["recipes"]), ("Trade", m["trades"])):
        for r in bucket:
            npc = f" with {_a(r['npc']['u'], _v(r['npc']['n']))}" if r["npc"] else ""
            when = f", {t.esc(_ch(r['ch']))}" if r["ch"] else ""
            rows.append(f"{label}: {_a(r['u'], _v(r['n']))}{npc}{when}.")
    for ev in m["events"]:
        rows.append(f"Event: {_a(ev['u'], t.esc(ev['label']))}.")
    for p in m["portals"]:
        if not p["hidden"]:
            continue
        to = ", ".join(_a(maps[x]["u"], _v(maps[x]["n"])) for x in p["to"] if x in maps)
        rows.append(
            f"Hidden portal {t.esc(str(p['n']))}" + (f" to {to}" if to else ", leads nowhere") + "."
        )
    exits = []
    for p in m["portals"]:
        exits += [x for x in p["to"] if x in maps and x not in exits]
    if exits:
        rows.append(
            "Portals to "
            + ", ".join(_a(maps[x]["u"], _v(maps[x]["n"])) + f" ({maps[x]['num']})" for x in exits)
            + "."
        )
    body = "".join(f"<li>{r}</li>" for r in rows)
    return (
        f'<li id="map={t.esc(m["id"])}" class="wm-li">{head}'
        + (f"<ul>{body}</ul>" if body else "")
        + "</li>"
    )


def text_list(data: Mapping[str, Any]) -> str:
    """The text version of the map: every area with its maps and what to do there."""
    maps = {m["id"]: m for m in data["maps"]}
    parts = []
    for a in data["areas"]:
        items = "".join(map_list_item(maps[mid], maps) for mid in a["maps"])
        lv = f", monsters {_lv(a['lv'])}" if a["lv"] else ""
        parts.append(
            f'<details class="wm-area-list" id="area={t.esc(a["id"])}">'
            f"<summary>{_v(a['n'])} "
            f'<span class="muted">(area {t.esc(a["id"])}, {len(a["maps"])} maps{t.esc(lv)})'
            f"</span></summary>"
            f"<p>{_a(a['u'], 'The area page')}.</p>"
            f'<ul class="wm-maplist">{items}</ul></details>\n'
        )
    if data.get("others"):
        items = "".join(
            f'<li id="map={t.esc(m["id"])}">{_a(m["u"], _v(m["n"]))}'
            f' <span class="muted">({t.esc(m["num"])})</span></li>'
            for m in data["others"]
        )
        parts.append(
            '<details class="wm-area-list"><summary>Maps of no area</summary>'
            f'<ul class="wm-maplist">{items}</ul></details>\n'
        )
    return "".join(parts)


def _layer_boxes() -> str:
    return "".join(
        f'<label class="wm-chip"><input type="checkbox" name="layer" value="{k}" checked>'
        f"<span>{t.esc(label)}</span></label>"
        for k, label in LAYERS
    )


def page(ctx: Context) -> Page:
    data = world_data(ctx)
    payload = to_json(data)
    ctx.add_file(JSON_PATH, payload + "\n")
    chapters = "".join(f'<option value="{c}">Chapter {c}</option>' for c in data["chapters"])
    n_maps = len(data["maps"])
    n_areas = len(data["areas"])
    intro = (
        f'<p class="lede">The whole game world on one map: {t.esc(t.number(n_areas))} areas and'
        f" {t.esc(t.number(n_maps))} maps, with what is worth doing on each. Zoom from the"
        " world into an area and into a single map; pick a map to see its monsters, shops,"
        " quests, crafting, bosses and hidden portals.</p>\n"
    )
    viewer = (
        '<div class="wm" id="wm" hidden>\n'
        '<div class="wm-bar">\n'
        '<div class="wm-search"><label class="sr" for="wm-q">Find a map</label>'
        '<input id="wm-q" type="search" autocomplete="off" placeholder="Find a map or area"'
        ' role="combobox" aria-autocomplete="list" aria-expanded="false"'
        ' aria-controls="wm-q-list">'
        '<ul id="wm-q-list" role="listbox" aria-label="Places" hidden></ul></div>\n'
        '<label class="wm-field"><span>Hero level</span><input id="wm-lv" type="number"'
        f' min="1" max="{data["maxLevel"]}" inputmode="numeric" placeholder="Any"></label>\n'
        '<label class="wm-field"><span>Chapter</span><select id="wm-ch">'
        f'<option value="">All</option>{chapters}</select></label>\n'
        '<details class="wm-layers"><summary>Layers</summary><div class="wm-layer-box">'
        f"{_layer_boxes()}</div></details>\n"
        '<button type="button" class="wm-btn" id="wm-route-btn" aria-pressed="false"'
        ' aria-controls="wm-route">Route</button>\n'
        "</div>\n"
        '<div class="wm-route" id="wm-route" hidden>'
        '<label class="wm-field"><span>From</span><select id="wm-from"></select></label>'
        '<button type="button" class="wm-btn wm-icon" id="wm-swap"'
        ' aria-label="Swap start and end">&#8646;</button>'
        '<label class="wm-field"><span>To</span><select id="wm-to"></select></label>'
        '<p class="wm-route-out" id="wm-route-out" aria-live="polite"></p></div>\n'
        '<div class="wm-body">\n'
        '<div class="wm-view" id="wm-view" tabindex="0" role="region"'
        ' aria-roledescription="interactive map" aria-label="World map"'
        ' aria-describedby="wm-keys">\n'
        '<div class="wm-stage" id="wm-stage"></div>\n'
        '<nav class="wm-crumbs" id="wm-crumbs" aria-label="Map levels"></nav>\n'
        '<div class="wm-zoom"><button type="button" class="wm-btn wm-icon" id="wm-in"'
        ' aria-label="Zoom in">+</button><button type="button" class="wm-btn wm-icon"'
        ' id="wm-out" aria-label="Zoom out">&#8722;</button><button type="button"'
        ' class="wm-btn wm-icon" id="wm-fit" aria-label="Show all">&#10530;</button></div>\n'
        '<p class="wm-keys" id="wm-keys">Drag or use the arrow keys to move, pinch, scroll or'
        " press plus and minus to zoom, Escape to go up a level.</p>\n"
        "</div>\n"
        '<aside class="wm-panel" id="wm-panel" aria-labelledby="wm-panel-title">'
        '<button type="button" class="wm-grip" id="wm-grip" aria-expanded="true"'
        ' aria-controls="wm-panel-body"><span class="sr">Show or hide the details</span>'
        "</button>"
        '<div class="wm-panel-body" id="wm-panel-body"></div></aside>\n'
        "</div>\n"
        '<p class="sr" id="wm-status" aria-live="polite"></p>\n'
        "</div>\n"
        '<noscript><p class="muted">The interactive map needs JavaScript. The list below has'
        " the same places and links.</p></noscript>\n"
    )
    key = (
        '<p class="muted">The level filter is a reading aid, not a rule of the game: a map is'
        f" dimmed as too hard when its weakest monster is more than {FIT_ABOVE} levels above"
        f" the hero, and as too easy when its strongest is more than {FIT_BELOW} levels"
        " below. The chapter filter keeps the warps, quests, shops, recipes and trades of that"
        " chapter. A route counts map changes over every warp of the game (or of the chapter),"
        " whatever its condition; a dashed line is a warp with a condition.</p>\n"
    )
    body = (
        intro
        + viewer
        + key
        + t.section("Every area and map", text_list(data), "list")
        + f"<style>{_asset('_worldmap.css')}</style>\n"
        + f'<script type="application/json" id="wm-data" data-verbatim>{payload}</script>\n'
        + f"<script>{_asset('_worldmap.js')}</script>\n"
    )
    return Page(
        PAGE,
        "World map",
        body,
        crumbs=[],
        description="An interactive map of the whole game world and what to do on every map.",
    )


def view_link(ctx: Context, kind: str, eid: str) -> str:
    """The "View on world map" link of an area or map page."""
    frag = f"{'map' if kind == 'map' else 'area'}={eid}"
    return (
        f'<p class="wm-view-link"><a href="{ctx.url(f"{PAGE}#{frag}")}">View on world map</a></p>\n'
    )
