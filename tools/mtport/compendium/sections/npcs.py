"""The NPCs and shops section of the compendium (issue #220).

Pages:

- ``npcs/index.html``: every NPC by area, every shop, links to the crafting and trades pages;
- ``npc/<id>.html``: one per NPC: where it stands and in which chapters (with the route from
  the nearest town), its role, its shops, recipes, trades, quests and job advancements, links
  to its dialogue (the quests section renders the conversations) and when it is hidden;
- ``shop/<id>.html``: one per shop, titled by who runs it and where (our words around the
  game's names), with its full stock (an anchor per ``market.dat`` row: the listings);
- ``npcs/crafting.html``: every recipe by output slot, an anchor each, with when the crafter
  offers them;
- ``npcs/trades.html``: every trade, an anchor each.

Everything the pages say about conditions comes from the knowledge base's script walk
(``dialogue.nodes`` and ``dialogue.state``): a quest state or a requirement check, simplified
here into readable alternatives.
"""

from __future__ import annotations

import re
from collections import deque
from collections.abc import Iterable, Iterator, Mapping, Sequence
from dataclasses import dataclass
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of, verbatim

SECTION_ID = "npcs"
CRAFTING = "npcs/crafting.html"
TRADES = "npcs/trades.html"
INDEX = "npcs/index.html"

ORIGINAL_BUGS = (
    "https://github.com/armaatus/maplethief-port/blob/main/docs/re/original-bugs.md"
    "#12-the-shops-keys-stop-at-row-2"
)
SCRIPT_VM = "https://github.com/armaatus/maplethief-port/blob/main/docs/re/script-vm.md#natives"
MARKET_SPEC = (
    "https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md#marketdat"
)

QUEST_KINDS = {0: "main", 1: "sub"}
QUEST_DOMAIN = frozenset({0, 1, 2})  # getQuestState: not started, in progress, done
CHECK_DOMAIN = frozenset({0, 1})  # checkRequire: 1 when the hero has what the quest asks
STATE_WORDS = {
    frozenset({0}): "is not started",
    frozenset({1}): "is in progress",
    frozenset({2}): "is done",
    frozenset({1, 2}): "is started or done",
    frozenset({0, 1}): "is not done",
    frozenset({0, 2}): "is not in progress",
}
SLOT_ORDER = ("weapon", "hat", "overall", "shoes", "earrings")
SLOT_TITLES = {
    "weapon": "Weapons",
    "hat": "Hats",
    "overall": "Overalls",
    "shoes": "Shoes",
    "earrings": "Earrings",
}


# --- small text helpers -------------------------------------------------------------------


def join_words(parts: Sequence[str], word: str = "and") -> str:
    """``a``, ``a and b``, ``a, b, and c`` (serial comma)."""
    parts = list(parts)
    if not parts:
        return ""
    if len(parts) == 1:
        return parts[0]
    if len(parts) == 2:
        return f"{parts[0]} {word} {parts[1]}"
    return ", ".join(parts[:-1]) + f", {word} {parts[-1]}"


def _runs(nums: Iterable[int]) -> list[tuple[int, int]]:
    out: list[tuple[int, int]] = []
    for n in sorted(set(nums)):
        if out and n == out[-1][1] + 1:
            out[-1] = (out[-1][0], n)
        else:
            out.append((n, n))
    return out


def chapters_text(chapters: Iterable[int]) -> str:
    """Chapters for prose: ``chapter 3``, ``chapters 1 to 3``, ``the tutorial and chapter 1``.
    Chapter 0 is the tutorial (a new game until the tutorial ends)."""
    chs = sorted(set(chapters))
    out = ["the tutorial"] if 0 in chs else []
    rest = [c for c in chs if c != 0]
    if rest:
        runs = _runs(rest)
        bits = [
            str(a) if a == b else f"{a} to {b}" if b > a + 1 else f"{a} and {b}" for a, b in runs
        ]
        single = len(rest) == 1
        out.append(("chapter " if single else "chapters ") + join_words(bits))
    return join_words(out)


def chapters_cell(chapters: Iterable[int]) -> str:
    """Chapters for a table cell: ``1-3``, ``3``, ``tutorial``, ``tutorial, 1``."""
    chs = sorted(set(chapters))
    out = ["tutorial"] if 0 in chs else []
    for a, b in _runs(c for c in chs if c != 0):
        out.append(str(a) if a == b else f"{a}-{b}")
    return ", ".join(out)


def _first(refs: Any) -> Mapping[str, Any] | None:
    if isinstance(refs, Mapping):
        return refs
    if isinstance(refs, list) and refs and isinstance(refs[0], Mapping):
        return refs[0]
    return None


def item_ref(spec: str) -> tuple[int, int] | None:
    """``equip:3`` gives ``(0, 3)``, ``etc:3`` ``(1, 3)`` (the scripts' item kinds)."""
    kind, _, n = spec.partition(":")
    if kind in ("equip", "etc") and n.isdigit():
        return (0 if kind == "equip" else 1, int(n))
    return None


# --- the index of NPCs, maps and scripts ----------------------------------------------------


@dataclass(frozen=True)
class Placement:
    """One map object of an NPC: the map, the script it runs (None: decoration), its tile, and
    the chapters in which the game runs that script there."""

    map_id: str
    script: str | None
    tile: tuple[int, int] | None
    chapters: tuple[int, ...]


class World:
    """What the section derives from the knowledge base once: placements, towns, routes."""

    def __init__(self, ctx: Context):
        self.ctx = ctx
        kb = ctx.kb
        self.scripts_by_base: dict[str, list[Entity]] = {}
        for s in kb.entities("script"):
            self.scripts_by_base.setdefault(str(s.get("base_name") or ""), []).append(s)
        # Towns: the maps where an NPC opens a shop.
        self.towns: list[str] = []
        for s in kb.entities("shop"):
            for r in s.get("opened_by") or []:
                m = str(r.get("map") or "")
                if m and m not in self.towns and kb.get("map", m) is not None:
                    self.towns.append(m)
        names: dict[str, int] = {}
        for m in kb.entities("map"):
            names[name_of(m)] = names.get(name_of(m), 0) + 1
        self._shared = {str(m["id"]) for m in kb.entities("map") if names[name_of(m)] > 1}
        self._routes = self._bfs()

    # places
    def area_of(self, map_id: str) -> str | None:
        m = self.ctx.kb.get("map", map_id)
        r = _first(m.get("area")) if m is not None else None
        return str(r["id"]) if r is not None else None

    def area_name(self, map_id: str) -> str:
        a = self.area_of(map_id)
        e = self.ctx.kb.get("area", a) if a is not None else None
        return name_of(e) if e is not None else ""

    def number(self, map_id: str) -> str:
        """The map's number as the style guide writes it (``2.8``) when other maps share its
        name, else ``""``."""
        return map_id.replace("-", ".") if map_id in self._shared else ""

    def map_link(self, map_id: str) -> str:
        """The map's link, with its number when other maps share its name."""
        if self.ctx.kb.get("map", map_id) is None:
            return t.esc(f"map {map_id.replace('-', '.')}")
        n = self.number(map_id)
        return self.ctx.ref("map", map_id) + (t.esc(f" ({n})") if n else "")

    def place(self, map_id: str) -> str:
        """The map's link and its area's: ``Perion Village (Perion)``, or with the map's number
        when other maps share its name: ``Kerning City Village (2.8, Henesys)``."""
        kb = self.ctx.kb
        if kb.get("map", map_id) is None:
            return t.esc(f"map {map_id.replace('-', '.')}")
        a = self.area_of(map_id)
        bits = [t.esc(self.number(map_id))] if self.number(map_id) else []
        if a is not None and kb.get("area", a):
            bits.append(self.ctx.ref("area", a))
        return self.ctx.ref("map", map_id) + (f" ({', '.join(bits)})" if bits else "")

    def area_link(self, map_id: str) -> str:
        a = self.area_of(map_id)
        return self.ctx.ref("area", a) if a is not None and self.ctx.kb.get("area", a) else ""

    def reached(self, map_id: str) -> bool:
        """Whether any portal or warp leads to the map (or it is a town)."""
        m = self.ctx.kb.get("map", map_id)
        if m is None:
            return False
        return bool(m.get("entrances") or m.get("warps_in")) or map_id in self.towns

    # NPCs
    def placements(self, npc: Entity) -> list[Placement]:
        out = []
        for r in npc.get("stands_on") or []:
            m = str(r["id"])
            script = r.get("script")
            tile = r.get("tile")
            chs: set[int] = set()
            if script:
                for s in self.scripts_by_base.get(str(script), []):
                    if any(str(x.get("id")) == m for x in s.get("maps") or []):
                        chs.update(int(c) for c in s.get("chapters") or [])
            out.append(
                Placement(
                    m,
                    str(script) if script else None,
                    (int(tile[0]), int(tile[1]))
                    if isinstance(tile, list) and len(tile) == 2
                    else None,
                    tuple(sorted(chs)),
                )
            )
        return out

    def npc_chapters(self, npc: Entity) -> list[int]:
        return sorted({c for p in self.placements(npc) for c in p.chapters})

    def home(self, npc: Entity) -> str | None:
        """The map that best says where the NPC is: its first placement with a script the game
        runs, else its first placement."""
        ps = self.placements(npc)
        for p in ps:
            if p.script and p.chapters:
                return p.map_id
        return ps[0].map_id if ps else None

    # routes
    def _bfs(self) -> dict[str, tuple[tuple[str, ...], bool]]:
        """From the towns through the map exits, fewest steps first: per map the maps on the
        way and whether an exit on the way is gated (has a condition)."""
        kb = self.ctx.kb
        prev: dict[str, tuple[tuple[str, ...], bool]] = {m: ((m,), False) for m in self.towns}
        q = deque(self.towns)
        while q:
            cur = q.popleft()
            m = kb.get("map", cur)
            for r in (m.get("exits") if m is not None else None) or []:
                nxt = str(r.get("id"))
                if nxt not in prev and kb.get("map", nxt) is not None:
                    path, gated = prev[cur]
                    prev[nxt] = (path + (nxt,), gated or bool(r.get("gated")))
                    q.append(nxt)
        return prev

    def route(self, map_id: str) -> tuple[tuple[str, ...], bool]:
        """The maps from the nearest town to ``map_id`` through exits (fewest steps) and whether
        one of them has a condition; ``((), False)`` when no town leads there."""
        return self._routes.get(map_id, ((), False))


def world(ctx: Context) -> World:
    cached = getattr(ctx, "_npcs_world", None)
    if cached is None or cached.ctx is not ctx:
        cached = World(ctx)
        ctx._npcs_world = cached  # type: ignore[attr-defined]
    return cached


# --- shop names -----------------------------------------------------------------------------


def shop_kind(ctx: Context, shop: Entity) -> str:
    """What the shop sells, in our words: ``equipment``, ``consumables`` or ``other items``."""
    cats = set()
    for r in shop.get("listings") or []:
        lst = ctx.kb.get("listing", str(r["id"]))
        it = _first(lst.get("item")) if lst is not None else None
        item = ctx.kb.get("item", str(it["id"])) if it is not None else None
        if item is not None:
            cats.add((item.get("table"), item.get("category")))
    if cats and all(tb == "equip" for tb, _ in cats):
        return "equipment"
    if cats and all(c in ("potion", "throwing star", "scroll", "return scroll") for _, c in cats):
        return "consumables"
    return "other items"


def slip_home(ctx: Context, listing: Entity) -> str:
    """The shop a listing marked as a data slip probably belongs to: the shop of the rows on
    both sides of it, when they agree (``""`` otherwise)."""
    row = listing.get("row")
    if not isinstance(row, int):
        return ""
    by_row = {e.get("row"): e for e in ctx.kb.entities("listing")}
    sides = []
    for r in (row - 1, row + 1):
        e = by_row.get(r)
        s = _first(e.get("shop")) if e is not None else None
        sides.append(str(s["id"]) if s is not None else "")
    own = _first(listing.get("shop"))
    if sides[0] and sides[0] == sides[1] and (own is None or str(own["id"]) != sides[0]):
        return sides[0] if ctx.kb.get("shop", sides[0]) is not None else ""
    return ""


def shop_openers(ctx: Context, shop: Entity) -> list[Mapping[str, Any]]:
    """The ``opened_by`` references, earliest chapter first."""
    rows = [r for r in shop.get("opened_by") or [] if isinstance(r, Mapping)]
    return sorted(rows, key=lambda r: (min(r.get("chapters") or [99]), str(r.get("map"))))


def _shop_parts(ctx: Context, shop: Entity) -> tuple[list[tuple[str, str]], str]:
    """The pieces of a shop's name as ``(text, kind)``: ``kind`` is ``"v"`` for a game name,
    ``""`` for our words; and the plain kind word."""
    kind = shop_kind(ctx, shop)
    w = world(ctx)
    openers = shop_openers(ctx, shop)
    if not openers:
        return [(f"Unopened {kind} shop", "")], kind
    npcs = list(dict.fromkeys(str(r["id"]) for r in openers))
    towns = list(dict.fromkeys(w.area_name(str(r["map"])) for r in openers if r.get("map")))
    parts: list[tuple[str, str]] = []
    for i, n in enumerate(npcs):
        if i:
            parts.append((" and " if i == len(npcs) - 1 else ", ", ""))
        e = ctx.kb.get("npc", n)
        parts.append((name_of(e) if e is not None else f"NPC {n}", "v"))
    parts.append((f"'s {kind} shop", ""))
    towns = [x for x in towns if x]
    if towns:
        parts.append((" in ", ""))
        for i, town in enumerate(towns):
            if i:
                sep = ", " if len(towns) > 2 else " "
                if i == len(towns) - 1:
                    sep = ", and " if len(towns) > 2 else " and "
                parts.append((sep, ""))
            parts.append((town, "v"))
    return parts, kind


def shop_title(ctx: Context, sid: str) -> str:
    """The shop's name as plain text: ``Mr. Lee's equipment shop in El Nath and Magatia``, or
    ``Unopened equipment shop`` for a shop no script opens."""
    shop = ctx.kb.get("shop", sid)
    if shop is None:
        return f"shop {sid}"
    return "".join(p for p, _ in _shop_parts(ctx, shop)[0])


def shop_markup(ctx: Context, sid: str) -> str:
    """The shop's name as markup, the game's names marked verbatim."""
    shop = ctx.kb.get("shop", sid)
    if shop is None:
        return t.esc(f"shop {sid}")
    return "".join(verbatim(p) if k == "v" else t.esc(p) for p, k in _shop_parts(ctx, shop)[0])


def shop_link(ctx: Context, sid: str) -> str:
    return f'<a href="{ctx.href("shop", sid)}">{shop_markup(ctx, sid)}</a>'


# --- conditions -----------------------------------------------------------------------------

_TERM = re.compile(r"^(\w+)\(([^)]*)\)$")


@dataclass(frozen=True)
class Var:
    """A value a condition reads: ``getQuestState(kind, n)``, ``checkRequire(kind, n)`` or any
    other term, kept as written (``opaque``)."""

    name: str
    args: tuple[int, ...] = ()
    opaque: str = ""

    @property
    def domain(self) -> frozenset:
        if self.name == "getQuestState":
            return QUEST_DOMAIN
        if self.name == "checkRequire":
            return CHECK_DOMAIN
        return frozenset({True, False})


def _parse(c: Mapping[str, Any]) -> tuple[Var, frozenset] | None:
    """One KB condition as (variable, allowed values); None for a box answer."""
    term, op, value = str(c.get("term", "")), str(c.get("op", "")), c.get("value")
    if term.startswith("answer#"):
        return None
    m = _TERM.match(term)
    if m and m.group(1) in ("getQuestState", "checkRequire") and isinstance(value, int):
        args = tuple(int(a) for a in m.group(2).split(",") if a.strip().lstrip("-").isdigit())
        var = Var(m.group(1), args)
        ops = {
            "==": lambda v: v == value,
            "!=": lambda v: v != value,
            "<": lambda v: v < value,
            "<=": lambda v: v <= value,
            ">": lambda v: v > value,
            ">=": lambda v: v >= value,
        }
        if op in ops:
            return var, frozenset(v for v in var.domain if ops[op](v))
    return Var("", (), f"{term} {op} {value}"), frozenset({True})


Alt = dict  # Var -> frozenset of allowed values


def _alts(sets: Iterable[Iterable[Mapping[str, Any]]]) -> list[Alt]:
    out: list[Alt] = []
    for s in sets:
        alt: Alt = {}
        dead = False
        for c in s:
            got = _parse(c)
            if got is None:
                continue
            var, vals = got
            alt[var] = alt.get(var, var.domain) & vals
            if not alt[var]:
                dead = True
        if not dead:
            out.append({k: v for k, v in alt.items() if v != k.domain})
    return out


def _covers(a: Alt, b: Alt) -> bool:
    """Whether ``a`` allows everything ``b`` allows (``a`` is no stricter)."""
    return all(var in b and b[var] <= vals for var, vals in a.items())


def simplify(sets: Iterable[Iterable[Mapping[str, Any]]]) -> list[Alt]:
    """Condition alternatives (each a list of KB conditions, all of which hold) made readable:
    box answers dropped, conditions on one value merged, alternatives that differ in one value
    joined and alternatives another one covers dropped. ``[{}]``: always."""
    alts = _alts(sets)
    changed = True
    while changed:
        changed = False
        # drop alternatives another covers
        keep: list[Alt] = []
        for i, a in enumerate(alts):
            if any(
                j != i and _covers(b, a) and (not _covers(a, b) or j < i)
                for j, b in enumerate(alts)
            ):
                changed = True
                continue
            keep.append(a)
        alts = keep
        # join two alternatives that differ in one value
        done = False
        for i in range(len(alts)):
            for j in range(i + 1, len(alts)):
                a, b = alts[i], alts[j]
                if set(a) != set(b):
                    continue
                diff = [v for v in a if a[v] != b[v]]
                if len(diff) != 1:
                    continue
                var = diff[0]
                merged = dict(a)
                merged[var] = a[var] | b[var]
                if merged[var] == var.domain:
                    del merged[var]
                alts = [x for k, x in enumerate(alts) if k not in (i, j)] + [merged]
                changed = done = True
                break
            if done:
                break
    return sorted(alts, key=lambda a: (len(a), sorted(str(v) for v in a)))


def quest_id(ctx: Context, chapter: int, kind: int, number: int) -> str | None:
    qid = f"{chapter}-{QUEST_KINDS.get(kind, kind)}-{number}"
    return qid if ctx.kb.get("quest", qid) is not None else None


def _quest_words(ctx: Context, chapter: int, args: tuple[int, ...]) -> str:
    if len(args) != 2:
        return "a quest"
    kind, n = args
    label = f"{QUEST_KINDS.get(kind, kind)} quest {n}"
    qid = quest_id(ctx, chapter, kind, n)
    return f"{t.esc(label)} ({ctx.ref('quest', qid)})" if qid else t.esc(label)


def condition_markup(ctx: Context, chapter: int, var: Var, vals: frozenset) -> str:
    if var.name == "getQuestState":
        words = STATE_WORDS.get(vals, "has state " + ", ".join(str(v) for v in sorted(vals)))
        return f"{_quest_words(ctx, chapter, var.args)} {t.esc(words)}"
    if var.name == "checkRequire":
        what = _quest_words(ctx, chapter, var.args)
        if vals == frozenset({1}):
            return f"the hero has everything {what} asks for"
        return f"the hero lacks something {what} asks for"
    text = var.opaque
    m = re.match(r"^getLevel\(\) (>=|<|==|!=|>|<=) (-?\d+)$", text)
    if m:
        op, n = m.groups()
        words = {
            ">=": f"the hero is level {n} or higher",
            ">": f"the hero is above level {n}",
            "<": f"the hero is below level {n}",
            "<=": f"the hero is level {n} or lower",
            "==": f"the hero is level {n}",
            "!=": f"the hero is not level {n}",
        }[op]
        return t.esc(words)
    m = re.match(r"^getClassUpCount\(\) (>=|<|==|!=|>|<=) (-?\d+)$", text)
    if m:
        return t.esc(f"the job advancement count is {m.group(1)} {m.group(2)}")
    m = re.match(r"^isInvenAvailable\((\d+), (\d+)\) (==|!=) 1$", text)
    if m:
        inv = "equipment" if m.group(1) == "0" else "item"
        ok = m.group(3) == "=="
        return t.esc(f"the {inv} inventory {'has' if ok else 'has no'} room")
    if text.startswith("approximate"):
        return t.esc("(the script walk merged paths here, so this may be incomplete)")
    return f"<code>{t.esc(text)}</code>"


def alternatives_markup(ctx: Context, chapter: int, alts: Sequence[Alt]) -> str:
    """Alternatives as a sentence fragment, or a list when there are several."""

    def one(a: Alt) -> str:
        parts = [
            condition_markup(ctx, chapter, v, a[v])
            for v in sorted(a, key=lambda v: (v.name != "getQuestState", v.args, v.opaque))
        ]
        return join_words(parts)

    if not alts:
        return ""
    if len(alts) == 1:
        return one(alts[0]) or "always"
    return (
        "one of these holds:<ul>"
        + "".join(f"<li>{one(a) or 'always'}</li>" for a in alts)
        + "</ul>"
    )


# --- dialogue walking -----------------------------------------------------------------------


def _paths_to(dialogue: Entity, match) -> list[list[Mapping[str, Any]]]:
    """The conditions of the root paths from which the conversation reaches a path for which
    ``match(path)`` holds (answer conditions included; :func:`simplify` drops them)."""
    nodes = dialogue.get("nodes") or {}
    root = dialogue.get("tree")
    if root not in nodes:
        return []
    memo: dict[str, bool] = {}

    def reaches(nid: str, seen: frozenset) -> bool:
        if nid in memo:
            return memo[nid]
        if nid in seen or nid not in nodes:
            return False
        hit = False
        for p in nodes[nid].get("paths") or []:
            if match(p) or (p.get("next") and reaches(p["next"], seen | {nid})):
                hit = True
                break
        memo[nid] = hit
        return hit

    out: list[list[Mapping[str, Any]]] = []
    for p in nodes[root].get("paths") or []:
        if match(p) or (p.get("next") and reaches(p["next"], frozenset({root}))):
            out.extend(list(alt) for alt in p.get("if") or [[]])
    return out


def _prepares(quest: Sequence[int], result: tuple[int, int] | None):
    """A path matcher: the path prepares ``quest`` with ``result`` as its reward item."""

    def match(p: Mapping[str, Any]) -> bool:
        calls = [x for x in p.get("do") or [] if isinstance(x, Mapping) and "call" in x]
        if not any(
            x["call"] == "setQuest" and list(x.get("args") or [])[:2] == list(quest) for x in calls
        ):
            return False
        if result is None:
            return True
        return any(
            x["call"] == "regRewardItem" and tuple((x.get("args") or [])[:2]) == result
            for x in calls
        )

    return match


def offer_conditions(ctx: Context, e: Entity, result: str | None) -> list[Alt]:
    """When the NPC offers a recipe or trade: the root conditions of the conversation paths
    that lead to the list it is chosen from, simplified."""
    script = str(e.get("script") or "")
    dia = ctx.kb.get("dialogue", script)
    quest = e.get("quest")
    if dia is None or not isinstance(quest, list):
        return []
    cache = getattr(ctx, "_npcs_offers", None)
    if cache is None:
        cache = {}
        ctx._npcs_offers = cache  # type: ignore[attr-defined]
    key = (script, tuple(quest), result)
    if key not in cache:
        ref = item_ref(result) if result else None
        cache[key] = simplify(_paths_to(dia, _prepares(quest, ref)))
    return cache[key]


# --- tables and sections ------------------------------------------------------------------


def _item_type(item: Entity | None) -> str:
    if item is None:
        return ""
    cat = str(item.get("category") or "")
    wt = item.get("weapon_type")
    text = cat[:1].upper() + cat[1:]
    return f"{text} ({wt})" if wt else text


def _count_items(ctx: Context, rows: Iterable[Mapping[str, Any]], key: str = "item") -> str:
    """``3 × Topaz, 1 × Pearl`` from ``{item, count}`` rows (``key`` names the item field)."""
    out = []
    for r in rows:
        iid = str(r.get(key) or r.get("id") or "")
        n = int(r.get("count") or 1)
        link = d.link_or_text(ctx, "item", iid)
        out.append(f"{t.esc(t.number(n))} × {link}")
    return ", ".join(out)


def _chapter_list(e: Entity) -> list[int]:
    c = e.get("chapter")
    return [int(c)] if isinstance(c, int) else []


def _where(ctx: Context, refs: Iterable[Mapping[str, Any]]) -> str:
    """``NPC on Map (Area)`` for ``{id: npc, map}`` references."""
    w = world(ctx)
    out = []
    for r in refs:
        npc = d.link_or_text(ctx, "npc", str(r["id"]))
        m = str(r.get("map") or "")
        out.append(f"{npc} on {w.place(m)}" if m else npc)
    return "; ".join(out)


def stock_table(ctx: Context, shop: Entity) -> str:
    """The shop's stock in ``market.dat`` order, one anchored row per listing."""
    rows, ids = [], []
    listings = [ctx.kb.get("listing", str(r["id"])) for r in shop.get("listings") or []]
    listings = [x for x in listings if x is not None]
    items = []
    for lst in listings:
        it = _first(lst.get("item"))
        items.append(str(it["id"]) if it is not None else "")
    boxes = d.any_thumb(ctx, (("item", i) for i in items if i))
    notes = any(lst.get("note") for lst in listings)
    for lst, iid in zip(listings, items, strict=True):
        item = ctx.kb.get("item", iid) if iid else None
        cells = [
            d.entry_cell(ctx, "item", iid, placeholder=boxes)
            if item is not None
            else t.markup_cell(ctx.name("listing", str(lst["id"])), name_of(lst)),
            t.Cell(t.esc(_item_type(item)), _item_type(item)),
        ]
        lv = item.get("req_level") if item is not None else None
        cells.append(t.num_cell(lv) if isinstance(lv, int) and lv > 0 else t.Cell("", -1))
        cells.append(t.num_cell(int(lst.get("price") or 0), "meso"))
        cells.append(t.num_cell(int(lst.get("row") if lst.get("row") is not None else -1)))
        if notes:
            note = ""
            if lst.get("note"):
                note = "A probable data slip: see the notes."
            cells.append(t.markup_cell(t.esc(note)))
        rows.append(cells)
        ids.append(d.row_anchor("listing", str(lst["id"])))
    cols = [
        t.Column("Item"),
        t.Column("Type"),
        t.Column("Required level", numeric=True),
        t.Column("Price", numeric=True),
        t.Column("Row", "The market.dat row", numeric=True),
    ] + ([t.Column("Note")] if notes else [])
    return t.table(cols, rows, ids)


def opener_table(ctx: Context, shop: Entity) -> str:
    w = world(ctx)
    rows = []
    for r in shop_openers(ctx, shop):
        m = str(r.get("map") or "")
        chs = [int(c) for c in r.get("chapters") or []]
        rows.append(
            [
                d.entry_cell(ctx, "npc", str(r["id"]), with_thumb=False),
                t.markup_cell(w.map_link(m), m),
                t.markup_cell(w.area_link(m), w.area_name(m)),
                t.Cell(t.esc(chapters_cell(chs)), min(chs or [99])),
            ]
        )
    cols = [t.Column("Keeper"), t.Column("Map"), t.Column("Area"), t.Column("Chapters")]
    return t.table(cols, rows, compact=True)


# --- shop pages --------------------------------------------------------------------------


def shop_page(ctx: Context, shop: Entity) -> Page:
    sid = str(shop["id"])
    kb = ctx.kb
    w = world(ctx)
    openers = shop_openers(ctx, shop)
    title = shop_title(ctx, sid)
    heading = shop_markup(ctx, sid)
    n = len(shop.get("listings") or [])
    kind = shop_kind(ctx, shop)
    keepers = list(dict.fromkeys(str(r["id"]) for r in openers))
    maps = list(dict.fromkeys(str(r.get("map")) for r in openers if r.get("map")))
    chs = sorted({int(c) for r in openers for c in r.get("chapters") or []})
    rows = [
        ("Keeper", ", ".join(d.link_or_text(ctx, "npc", k) for k in keepers)),
        ("Location", "<br>".join(w.place(m) for m in maps)),
        ("Chapters", t.esc(chapters_cell(chs))),
        ("Sells", t.esc(kind)),
        ("Stock", t.esc(f"{t.number(n)} items" if n != 1 else "1 item")),
        ("Shop number", f"<code>{t.esc(sid)}</code>"),
    ]
    picture = ctx.sprite("npc", keepers[0]) if keepers else ""
    parts = [t.infobox(heading, rows, picture)]
    if openers:
        sentences = []
        for k in keepers:
            spots = []
            for r in openers:
                if str(r["id"]) != k:
                    continue
                m = str(r.get("map") or "")
                c = chapters_text(int(x) for x in r.get("chapters") or [])
                spots.append(f"on {w.place(m)} in {t.esc(c)}")
            sentences.append(f"{d.link_or_text(ctx, 'npc', k)} opens it {join_words(spots)}.")
        lead = (
            f"<p>{heading} sells {t.esc(kind)}: {t.esc(t.number(n))} items from "
            f"<code>market.dat</code>. {' '.join(sentences)}</p>"
        )
    else:
        lead = (
            f"<p>Shop number {t.esc(sid)} has {t.esc(t.number(n))} rows of {t.esc(kind)} in "
            "<code>market.dat</code>, but no script of the game opens it, so it is never seen "
            "in play.</p>"
        )
    parts.append(lead)
    prose = ctx.prose(f"shop/{d.slug(sid)}.md")
    if prose:
        parts.append(prose)
    if openers:
        parts.append(t.section("Location", opener_table(ctx, shop), "location"))
    parts.append(t.section("Stock", stock_table(ctx, shop), "stock"))
    notes = []
    if openers:
        notes.append(
            "The stock comes from <code>market.dat</code>: it is the same wherever and in "
            "whichever chapter the shop opens."
        )
    else:
        reason = str(shop.get("unopened_reason") or "")
        notes.append(
            "No script opens it. The game's only way to a shop screen is the "
            "<code>showMarket</code> native (<code>0x4628D</code>, called only from "
            "<code>0x99900</code>), and no script passes this shop's number."
            + (
                f" The knowledge base says: {t.esc(reason)}."
                if reason and not reason.startswith("no script opens it")
                else ""
            )
        )
    if openers and kind == "equipment":
        notes.append(
            "Bought equipment has every stat at the item's minimum and 5 upgrade slots; only "
            f'<a href="{ctx.url(CRAFTING)}">crafted</a> equipment rolls its stats.'
        )
    spec = f'(<a href="{t.esc(MARKET_SPEC)}">market.dat</a>)'
    for lst in kb.entities("listing"):
        if not lst.get("note"):
            continue
        row = int(lst.get("row") or 0)
        own = _first(lst.get("shop"))
        own_id = str(own["id"]) if own is not None else ""
        home = slip_home(ctx, lst)
        if own_id == sid:
            anchor = d.row_anchor("listing", str(lst["id"]))
            where = (
                f"it says shop {t.esc(own_id)} in the middle of the rows of "
                f"{shop_link(ctx, home)}, where it probably belongs"
                if home
                else "the knowledge base marks it"
            )
            notes.append(
                f'<a href="#{anchor}">Row {t.esc(t.number(row))}</a> is a probable data slip: '
                f"{where}. The game lists it here all the same {spec}."
            )
        elif home == sid:
            notes.append(
                f"Row {t.esc(t.number(row))} ({ctx.ref('listing', str(lst['id']))}) sits among "
                f"this shop's rows but says shop {t.esc(own_id)}, so the game shows it in "
                f"{shop_link(ctx, own_id)} instead; it probably belongs here {spec}."
            )
    if openers:
        notes.append(
            "In the original game the arrow keys reach only the first two rows of a tab; "
            f'the port fixes it (<a href="{t.esc(ORIGINAL_BUGS)}">original bugs, 12</a>).'
        )
    parts.append(t.section("Notes", "".join(f"<p>{x}</p>" for x in notes), "notes"))
    page = Page(
        path=d.entity_path("shop", sid),
        title=title,
        heading=heading,
        body="\n".join(parts),
        description=f"{title}: stock and prices.",
        crumbs=[],
    ).documents("shop", sid)
    for r in shop.get("listings") or []:
        page.documents("listing", str(r["id"]), d.row_anchor("listing", str(r["id"])))
    return page


# --- NPC pages ---------------------------------------------------------------------------


def roles(ctx: Context, npc: Entity) -> list[str]:
    out = []
    if npc.get("opens_shop"):
        out.append("shopkeeper")
    if npc.get("gives_quests") or npc.get("completes_quests"):
        out.append("quest giver")
    if npc.get("advancements"):
        out.append("job instructor")
    if npc.get("crafts"):
        out.append("crafter")
    if npc.get("trades"):
        out.append("trader")
    if not out and npc.get("dialogues"):
        out.append("conversation only")
    if not out:
        out.append("decoration" if npc.get("stands_on") else "not placed")
    return out


def location_table(ctx: Context, npc: Entity) -> str:
    w = world(ctx)
    rows = []
    for p in w.placements(npc):
        if p.script is None:
            note = "Decoration: no script, so it does nothing when touched."
            if not w.reached(p.map_id):
                note += " No portal or warp leads to this map."
        elif not p.chapters:
            note = "Its script never runs on this map."
        else:
            note = ""
        tile = t.esc(f"x {p.tile[0]}, y {p.tile[1]}") if p.tile else ""
        rows.append(
            [
                t.markup_cell(w.map_link(p.map_id), p.map_id),
                t.markup_cell(w.area_link(p.map_id), w.area_name(p.map_id)),
                t.markup_cell(f"<code>{t.esc(p.script)}</code>" if p.script else ""),
                t.Cell(tile, (p.tile[0] * 10000 + p.tile[1]) if p.tile else -1),
                t.Cell(t.esc(chapters_cell(p.chapters)), min(p.chapters or (99,))),
                t.markup_cell(t.esc(note)),
            ]
        )
    cols = [
        t.Column("Map"),
        t.Column("Area"),
        t.Column("Object", "The script the map object runs"),
        t.Column("Position", "The map tile it stands on"),
        t.Column("Chapters"),
        t.Column("Note"),
    ]
    return t.table(cols, rows)


def _routes(ctx: Context, npc: Entity) -> str:
    w = world(ctx)
    out = []
    seen = set()
    for p in w.placements(npc):
        if not p.script or not p.chapters or p.map_id in w.towns or p.map_id in seen:
            continue
        seen.add(p.map_id)
        r, gated = w.route(p.map_id)
        if len(r) < 2 or len(r) > 12:
            continue
        steps = [w.map_link(m) for m in r]
        via = f" through {join_words(steps[1:-1])}" if len(steps) > 2 else ""
        n = len(r) - 1
        out.append(
            f"<p>From the town map {steps[0]}, {steps[-1]} is {t.esc(t.number(n))} "
            f"{'step' if n == 1 else 'steps'} away{via}."
            + (" An exit on the way has a condition, such as a quest." if gated else "")
            + "</p>"
        )
    return "".join(out)


def quest_table(ctx: Context, npc: Entity) -> str:
    gives = {str(r["id"]): r for r in npc.get("gives_quests") or []}
    ends = {str(r["id"]): r for r in npc.get("completes_quests") or []}
    ids = list(dict.fromkeys(list(gives) + list(ends)))

    def key(q: str):
        e = ctx.kb.get("quest", q) or {}
        return (e.get("chapter", 99), e.get("quest_kind", ""), e.get("number", 0), q)

    w = world(ctx)
    rows = []
    for q in sorted(ids, key=key):
        e = ctx.kb.get("quest", q)
        if e is None:
            continue
        role = (
            "Gives and completes"
            if q in gives and q in ends
            else ("Gives" if q in gives else "Completes")
        )
        m = str((gives.get(q) or ends.get(q) or {}).get("map") or "")
        ch = e.get("chapter")
        rows.append(
            [
                d.entry_cell(ctx, "quest", q, with_thumb=False),
                t.Cell(
                    t.esc(chapters_cell([ch]) if isinstance(ch, int) else ""),
                    ch if isinstance(ch, int) else 99,
                ),
                t.Cell(
                    t.esc(str(e.get("quest_kind") or "").capitalize()),
                    str(e.get("quest_kind") or ""),
                ),
                t.Cell(t.esc(role), role),
                t.markup_cell(w.place(m) if m else "", m),
            ]
        )
    if not rows:
        return ""
    cols = [
        t.Column("Quest"),
        t.Column("Chapter"),
        t.Column("Type"),
        t.Column("Role"),
        t.Column("Map"),
    ]
    return t.table(cols, rows)


def advancement_table(ctx: Context, npc: Entity) -> str:
    w = world(ctx)
    rows = []
    for r in npc.get("advancements") or []:
        e = ctx.kb.get("job_advancement", str(r["id"]))
        if e is None:
            continue
        ch = e.get("chapter")
        job = e.get("job_name") or ("keeps the job" if e.get("keeps_job") else "")
        m = str(r.get("map") or "")
        rows.append(
            [
                d.entry_cell(ctx, "job_advancement", str(r["id"]), with_thumb=False),
                t.Cell(
                    t.esc(chapters_cell([ch]) if isinstance(ch, int) else ""),
                    ch if isinstance(ch, int) else 99,
                ),
                t.num_cell(int(e.get("advancement") or 0)),
                t.Cell(t.esc(str(job)), str(job)),
                t.markup_cell(w.place(m) if m else "", m),
            ]
        )
    if not rows:
        return ""
    cols = [
        t.Column("Advancement"),
        t.Column("Chapter"),
        t.Column("Count", "The advancement count after it", numeric=True),
        t.Column("Job"),
        t.Column("Map"),
    ]
    return t.table(cols, rows)


def recipe_rows_table(ctx: Context, refs: Iterable[str], anchors: bool, with_crafter: bool) -> str:
    """Recipes as a table: result, chapter, materials, meso, (crafter and place)."""
    recipes = [ctx.kb.get("recipe", r) for r in refs]
    recipes = [r for r in recipes if r is not None]
    boxes = d.any_thumb(ctx, (("item", str(r.get("result_item"))) for r in recipes))
    rows, ids = [], []
    for r in recipes:
        rid = str(r["id"])
        res = str(r.get("result_item") or "")
        cnt = int(r.get("result_count") or 1)
        cell = (
            d.entry_cell(ctx, "item", res, placeholder=boxes)
            if ctx.kb.get("item", res)
            else t.text_cell(res)
        )
        if cnt > 1:
            cell = t.markup_cell(cell.markup + t.esc(f" × {cnt}"), cell.key)
        item = ctx.kb.get("item", res)
        lv = item.get("req_level") if item is not None else None
        row = [
            cell,
            t.Cell(t.esc(chapters_cell(_chapter_list(r))), r.get("chapter", 99)),
            t.num_cell(lv) if isinstance(lv, int) and lv > 0 else t.Cell("", -1),
            t.markup_cell(_count_items(ctx, r.get("material_items") or [])),
            t.num_cell(int(r.get("meso") or 0), "meso"),
        ]
        if with_crafter:
            row.append(t.markup_cell(_where(ctx, r.get("crafted_by") or [])))
        rows.append(row)
        ids.append(d.row_anchor("recipe", rid))
    cols = [
        t.Column("Result"),
        t.Column("Chapter"),
        t.Column("Required level", numeric=True),
        t.Column("Materials"),
        t.Column("Fee", "The meso the crafter takes", numeric=True),
    ] + ([t.Column("Crafter")] if with_crafter else [])
    return t.table(cols, rows, ids if anchors else None)


def trade_rows_table(ctx: Context, refs: Iterable[str], anchors: bool, with_trader: bool) -> str:
    trades = [ctx.kb.get("trade", r) for r in refs]
    trades = [x for x in trades if x is not None]
    rows, ids = [], []
    for x in trades:
        give = _count_items(ctx, x.get("cost_items") or [])
        if x.get("cost_meso"):
            give = join_words(
                [g for g in [give, t.esc(f"{t.number(int(x['cost_meso']))} meso")] if g]
            )
        get = _count_items(ctx, x.get("result_items") or [])
        others = [o for o in x.get("result_other") or [] if isinstance(o, Mapping)]
        for o in others:
            if o.get("type") in ("meso", "exp") and o.get("amount"):
                unit = "meso" if o["type"] == "meso" else "EXP"
                get = join_words(
                    [g for g in [get, t.esc(f"{t.number(int(o['amount']))} {unit}")] if g]
                )
        row = [
            t.markup_cell(give),
            t.markup_cell(get, name_of(x)),
            t.Cell(t.esc(chapters_cell(_chapter_list(x))), x.get("chapter", 99)),
        ]
        if with_trader:
            row.append(t.markup_cell(_where(ctx, x.get("offered_by") or [])))
        rows.append(row)
        ids.append(d.row_anchor("trade", str(x["id"])))
    cols = [t.Column("Gives"), t.Column("Receives"), t.Column("Chapter")] + (
        [t.Column("Trader")] if with_trader else []
    )
    return t.table(cols, rows, ids if anchors else None)


def state_notes(ctx: Context, npc: Entity) -> list[str]:
    """When the NPC is hidden and which quest icons its state pass sets, per dialogue."""
    out = []
    w = world(ctx)
    icon_rows = []
    only_default = []
    for r in sorted(
        npc.get("dialogues") or [], key=lambda r: (_dia_chapters(ctx, str(r["id"])), str(r["id"]))
    ):
        dia = ctx.kb.get("dialogue", str(r["id"]))
        if dia is None:
            continue
        chs = [int(c) for c in dia.get("chapters") or []]
        m = str(r.get("map") or "")
        where = f" on {w.map_link(m)}" if ctx.kb.get("map", m) else ""
        obj = f", object <code>{t.esc(script_object(str(dia['id'])))}</code>"
        state = [s for s in dia.get("state") or [] if isinstance(s, Mapping)]
        hidden = [alt for s in state if s.get("hide") == 1 for alt in s.get("if") or [[]]]
        if hidden:
            alts = simplify(hidden)
            ch = chs[0] if chs else 0
            cond = alternatives_markup(ctx, ch, alts)
            if alts == [{}]:
                out.append(f"In {t.esc(chapters_text(chs))}{where}{obj}, the game always hides it.")
            else:
                out.append(
                    f"In {t.esc(chapters_text(chs))}{where}{obj}, the game hides it when {cond}"
                    + ("" if cond.endswith("</ul>") else ".")
                )
        icons = sorted({s["notify"] for s in state if isinstance(s.get("notify"), int)})
        if icons and icons != [-1]:
            icon_rows.append(
                f"{t.esc(join_words([str(v) for v in icons]))} in {t.esc(chapters_text(chs))}"
                f"{where} ({ctx.ref('dialogue', str(dia['id']))}, object "
                f"<code>{t.esc(script_object(str(dia['id'])))}</code>)"
            )
        elif icons:
            only_default.append(str(dia["id"]))
    if icon_rows:
        rest = " Its other conversations set only -1." if only_default else ""
        out.append(
            "The NPC-state pass of a conversation sets its quest icon "
            f'(<a href="{t.esc(SCRIPT_VM)}"><code>setNpcNotify</code></a>) from quest states: '
            f"{'; '.join(icon_rows)}. What each value shows on screen is not documented.{rest}"
        )
    elif only_default:
        out.append(
            "The NPC-state passes of its conversations set its quest icon "
            f'(<a href="{t.esc(SCRIPT_VM)}"><code>setNpcNotify</code></a>) only to -1.'
        )
    return out


def script_object(dialogue_id: str) -> str:
    """The script's name as map objects give it: ``chapter1-Manzie`` gives ``Manzie``."""
    return dialogue_id.split("-", 1)[1] if "-" in dialogue_id else dialogue_id


def _dia_chapters(ctx: Context, did: str) -> int:
    e = ctx.kb.get("dialogue", did)
    return min((e.get("chapters") if e is not None else None) or [99])


def blocks(notes: Iterable[str]) -> str:
    """Notes as paragraphs; a note that ends in a list keeps the list outside its paragraph."""
    out = []
    for n in notes:
        head, sep, tail = n.partition("<ul>")
        out.append(f"<p>{head}</p>" + (sep + tail if sep else ""))
    return "".join(out)


def npc_page(ctx: Context, npc: Entity) -> Page:
    kb = ctx.kb
    nid = str(npc["id"])
    w = world(ctx)
    ps = w.placements(npc)
    chs = w.npc_chapters(npc)
    rl = roles(ctx, npc)
    ko = name_of(npc, "ko")
    sub = verbatim(ko, "ko") if ko != name_of(npc, "en") else ""
    maps = list(dict.fromkeys(p.map_id for p in ps if p.script and p.chapters))
    deco = [p.map_id for p in ps if not (p.script and p.chapters)]
    loc_bits = [w.place(m) for m in maps[:4]]
    if len(maps) > 4:
        loc_bits.append(f'<a href="#location">and {len(maps) - 4} more</a>')
    shops = list(dict.fromkeys(str(r["id"]) for r in npc.get("opens_shop") or []))
    info = [
        ("Location", "<br>".join(loc_bits) if loc_bits else ("decoration only" if ps else "")),
        ("Chapters", t.esc(chapters_cell(chs))),
        ("Role", t.esc(", ".join(rl).capitalize())),
        ("Shop", "<br>".join(shop_link(ctx, s) for s in shops)),
        (
            "Quests",
            t.esc(
                t.number(
                    len(
                        {
                            str(r["id"])
                            for k in ("gives_quests", "completes_quests")
                            for r in npc.get(k) or []
                        }
                    )
                )
            )
            if npc.get("gives_quests") or npc.get("completes_quests")
            else "",
        ),
        ("ID", f"<code>{t.esc(nid)}</code>"),
    ]
    parts = [t.infobox(ctx.name("npc", nid), info, ctx.sprite("npc", nid), subtitle_markup=sub)]
    # lead
    name = ctx.name("npc", nid)
    if maps:
        where = join_words(
            [w.place(m) for m in maps[:3]]
            + ([t.esc(f"{len(maps) - 3} more maps")] if len(maps) > 3 else [])
        )
        lead = (
            f"{name} is an NPC ({t.esc(join_words(rl))}) found on {where} in "
            f"{t.esc(chapters_text(chs))}."
        )
    elif ps:
        lead = (
            f"{name} appears only as decoration: its map objects run no script, so touching it "
            "does nothing."
        )
    else:
        lead = (
            f"{name} is in the NPC table, but no map object places it and the knowledge base "
            "links it to no script."
        )
    parts.append(f"<p>{lead}</p>")
    prose = ctx.prose(f"npc/{d.slug(nid)}.md")
    if prose:
        parts.append(prose)
    # Location
    if ps:
        body = location_table(ctx, npc) + _routes(ctx, npc)
        if deco and maps:
            body += (
                "<p>The rows marked decoration are map objects with no script: the NPC stands "
                "there but does nothing.</p>"
            )
        parts.append(t.section("Location", body, "location"))
    # Role
    role_bits = []
    if shops:
        role_bits.append(f"Runs {join_words([shop_link(ctx, s) for s in shops])}.")
    if npc.get("gives_quests") or npc.get("completes_quests"):
        role_bits.append('Gives or completes quests (<a href="#quests">below</a>).')
    if npc.get("advancements"):
        role_bits.append('Gives job advancements (<a href="#job-advancements">below</a>).')
    if npc.get("crafts"):
        role_bits.append(
            f'Crafts equipment (<a href="#crafting-and-trades">below</a>, all recipes on '
            f'<a href="{ctx.url(CRAFTING)}">the crafting page</a>).'
        )
    if npc.get("trades"):
        role_bits.append(
            f'Trades items (<a href="#crafting-and-trades">below</a>, all trades on '
            f'<a href="{ctx.url(TRADES)}">the trades page</a>).'
        )
    if role_bits:
        parts.append(t.section("Role", "".join(f"<p>{b}</p>" for b in role_bits), "role"))
    # Shop
    if shops:
        rows = []
        for r in sorted(
            npc.get("opens_shop") or [],
            key=lambda r: (min(r.get("chapters") or [99]), str(r["map"])),
        ):
            m = str(r.get("map") or "")
            rows.append(
                [
                    t.markup_cell(shop_link(ctx, str(r["id"])), shop_title(ctx, str(r["id"]))),
                    t.markup_cell(w.place(m), m),
                    t.Cell(
                        t.esc(chapters_cell(int(c) for c in r.get("chapters") or [])),
                        min(r.get("chapters") or [99]),
                    ),
                ]
            )
        parts.append(
            t.section(
                "Shop",
                t.table([t.Column("Shop"), t.Column("Map"), t.Column("Chapters")], rows),
                "shop",
            )
        )
    # Crafting and trades
    ct = []
    recipes = [str(r["id"]) for r in npc.get("crafts") or []]
    if recipes:
        ct.append(
            f"<p>{t.esc(t.number(len(recipes)))} recipes; when the conversation offers them is on "
            f'<a href="{ctx.url(CRAFTING)}">the crafting page</a>.</p>'
        )
        ct.append(recipe_rows_table(ctx, recipes, anchors=False, with_crafter=False))
    trades = [str(r["id"]) for r in npc.get("trades") or []]
    if trades:
        ct.append(
            f"<p>{t.esc(t.number(len(trades)))} trades; when the conversation offers them is on "
            f'<a href="{ctx.url(TRADES)}">the trades page</a>.</p>'
        )
        ct.append(trade_rows_table(ctx, trades, anchors=False, with_trader=False))
    if ct:
        parts.append(t.section("Crafting and trades", "".join(ct), "crafting-and-trades"))
    # Quests
    qt = quest_table(ctx, npc)
    if qt:
        parts.append(t.section("Quests", qt, "quests"))
    at = advancement_table(ctx, npc)
    if at:
        parts.append(t.section("Job advancements", at, "job-advancements"))
    # Dialogue: the quests section renders the conversations; here only the links.
    dias = [r for r in npc.get("dialogues") or [] if kb.get("dialogue", str(r["id"]))]
    if dias:
        rows = []
        for r in sorted(dias, key=lambda r: (_dia_chapters(ctx, str(r["id"])), str(r["id"]))):
            dia = kb.get("dialogue", str(r["id"]))
            m = str(r.get("map") or "")
            dchs = [int(c) for c in dia.get("chapters") or []]
            rows.append(
                [
                    t.markup_cell(ctx.ref("dialogue", str(r["id"])), name_of(dia)),
                    t.markup_cell(f"<code>{t.esc(script_object(str(r['id'])))}</code>"),
                    t.Cell(t.esc(chapters_cell(dchs)), min(dchs or [99])),
                    t.markup_cell(w.place(m) if m else "", m),
                ]
            )
        cols = [
            t.Column("Conversation"),
            t.Column("Object", "The script of the map object (or the map) that runs it"),
            t.Column("Chapters"),
            t.Column("Map"),
        ]
        parts.append(
            t.section(
                "Dialogue",
                "<p>What it says, in English and Korean, chapter by chapter:</p>"
                + t.table(cols, rows),
                "dialogue",
            )
        )
    # Notes
    notes = state_notes(ctx, npc)
    if not npc.get("dialogues") and ps:
        notes.append("None of its map objects runs a script, so it has no conversation.")
    if notes:
        parts.append(t.section("Notes", blocks(notes), "notes"))
    images = [i for i in ctx.entity_images("npc", nid) if not i.role.endswith("2x")]
    if len(images) > 1:
        figs = []
        for i in images:
            big = max(i.w, i.h)
            scale = max(1, min(3, 96 // big)) if big and big <= 96 else 1
            figs.append((ctx.img(i, alt=i.alt or name_of(npc), scale=scale), t.esc(d._role(i))))
        parts.append(t.section("Images", t.gallery(figs), "images"))
    return Page(
        path=d.entity_path("npc", nid),
        title=name_of(npc),
        heading=ctx.name("npc", nid),
        title_verbatim=True,
        body="\n".join(parts),
        description=f"NPC: {name_of(npc)}",
    ).documents("npc", nid)


# --- list pages --------------------------------------------------------------------------


def npc_list(ctx: Context) -> str:
    w = world(ctx)
    npcs = ctx.kb.entities("npc")

    def area_key(e: Entity):
        h = w.home(e)
        a = w.area_of(h) if h else None
        return (
            int(a) if a is not None and a.isdigit() else 999,
            int(e["id"]) if str(e["id"]).isdigit() else 0,
        )

    boxes = d.any_thumb(ctx, (("npc", e["id"]) for e in npcs))
    rows = []
    for e in sorted(npcs, key=area_key):
        nid = str(e["id"])
        h = w.home(e)
        rows.append(
            [
                d.entry_cell(ctx, "npc", nid, placeholder=boxes),
                t.Cell(verbatim(name_of(e, "ko"), "ko"), name_of(e, "ko"), "ko"),
                t.markup_cell(w.area_link(h) if h else t.esc("not placed"), area_key(e)[0]),
                t.markup_cell(w.map_link(h) if h else "", h or ""),
                t.Cell(t.esc(", ".join(roles(ctx, e))), ", ".join(roles(ctx, e))),
                t.Cell(t.esc(chapters_cell(w.npc_chapters(e))), min(w.npc_chapters(e) or [99])),
            ]
        )
    cols = [
        t.Column("NPC"),
        t.Column("Korean"),
        t.Column("Area"),
        t.Column("Map", "Where it mainly stands"),
        t.Column("Role"),
        t.Column("Chapters"),
    ]
    return t.table(cols, rows)


def shop_list(ctx: Context) -> str:
    w = world(ctx)
    rows = []
    for s in ctx.kb.entities("shop"):
        sid = str(s["id"])
        openers = shop_openers(ctx, s)
        keepers = list(dict.fromkeys(str(r["id"]) for r in openers))
        maps = list(dict.fromkeys(str(r.get("map")) for r in openers if r.get("map")))
        chs = sorted({int(c) for r in openers for c in r.get("chapters") or []})
        rows.append(
            [
                t.markup_cell(shop_link(ctx, sid), shop_title(ctx, sid)),
                t.markup_cell(
                    ", ".join(d.link_or_text(ctx, "npc", k) for k in keepers) or t.esc("none")
                ),
                t.markup_cell(", ".join(w.map_link(m) for m in maps) or t.esc("never opened")),
                t.Cell(t.esc(chapters_cell(chs)), min(chs or [99])),
                t.num_cell(len(s.get("listings") or [])),
                t.Cell(t.esc(sid), int(sid) if sid.isdigit() else 0),
            ]
        )
    cols = [
        t.Column("Shop"),
        t.Column("Keeper"),
        t.Column("Maps"),
        t.Column("Chapters"),
        t.Column("Items", numeric=True),
        t.Column("Number", "The shop number in market.dat", numeric=True),
    ]
    return t.table(cols, rows)


def _slot(ctx: Context, r: Entity) -> str:
    item = ctx.kb.get("item", str(r.get("result_item") or ""))
    return str(item.get("category") or "other") if item is not None else "other"


def crafting_page(ctx: Context) -> Page:
    page = Page(CRAFTING, "Crafting", "", description="Every crafting recipe of the game.")
    recipes = ctx.kb.entities("recipe")
    parts = []
    prose = ctx.prose("npcs/crafting.md")
    if prose:
        parts.append(prose)
    else:
        parts.append(
            f"<p>{t.esc(t.number(len(recipes)))} recipes: the materials and meso to bring, and "
            "what they make.</p>"
        )
    # When each crafter offers them, per chapter.
    avail = []
    seen = set()
    for r in sorted(recipes, key=lambda r: (r.get("chapter", 99), str(r.get("script")))):
        key = (str(r.get("script")), r.get("chapter"))
        if key in seen:
            continue
        seen.add(key)
        alts = offer_conditions(ctx, r, None)
        who = _where(ctx, r.get("crafted_by") or [])
        ch = r.get("chapter") if isinstance(r.get("chapter"), int) else 0
        cond = alternatives_markup(ctx, ch, alts) if alts and alts != [{}] else ""
        avail.append(
            f"<li>{t.esc(chapters_text([ch]).capitalize())}: {who}"
            + (f", when {cond}" if cond else ", at any time")
            + ("" if cond.endswith("</ul>") else ".")
            + "</li>"
        )
    if avail:
        parts.append(
            t.section(
                "Where and when",
                "<p>Each recipe is a quest the crafter prepares and completes in one "
                "conversation: it needs the materials and the fee in the inventory and room "
                "in the inventory for the result. The crafter offers the list:</p>"
                f"<ul>{''.join(avail)}</ul>",
                "where-and-when",
            )
        )
    by_slot: dict[str, list[Entity]] = {}
    for r in recipes:
        by_slot.setdefault(_slot(ctx, r), []).append(r)
    order = [s for s in SLOT_ORDER if s in by_slot] + sorted(
        s for s in by_slot if s not in SLOT_ORDER
    )
    for slot in order:
        rows = sorted(
            by_slot[slot],
            key=lambda r: (
                (ctx.kb.get("item", str(r.get("result_item"))) or {}).get("req_level") or 0,
                str(r.get("result_item")),
                r.get("chapter", 99),
            ),
        )
        table = recipe_rows_table(
            ctx, [str(r["id"]) for r in rows], anchors=True, with_crafter=True
        )
        parts.append(t.section(SLOT_TITLES.get(slot, slot.capitalize()), table, t.anchor_id(slot)))
        for r in rows:
            page.documents("recipe", str(r["id"]), d.row_anchor("recipe", str(r["id"])))
    page.body = "\n".join(parts)
    page.crumbs = []
    return page


def trades_page(ctx: Context) -> Page:
    page = Page(TRADES, "Trades", "", description="Every item trade of the game.")
    trades = ctx.kb.entities("trade")
    parts = []
    prose = ctx.prose("npcs/trades.md")
    parts.append(
        prose
        or f"<p>{t.esc(t.number(len(trades)))} trades of items for items that are not crafting.</p>"
    )
    avail = []
    seen = set()
    for x in sorted(trades, key=lambda x: (x.get("chapter", 99), str(x.get("script")))):
        key = (str(x.get("script")), x.get("chapter"))
        if key in seen:
            continue
        seen.add(key)
        alts = offer_conditions(ctx, x, None)
        who = _where(ctx, x.get("offered_by") or [])
        ch = x.get("chapter") if isinstance(x.get("chapter"), int) else 0
        cond = alternatives_markup(ctx, ch, alts) if alts and alts != [{}] else ""
        avail.append(
            f"<li>{t.esc(chapters_text([ch]).capitalize())}: {who}"
            + (f", when {cond}" if cond else ", at any time")
            + ("" if cond.endswith("</ul>") else ".")
            + "</li>"
        )
    if avail:
        parts.append(
            t.section(
                "Where and when",
                "<p>Each trade is a quest the trader prepares and completes in one "
                "conversation: it needs the items in the inventory and room for the result. "
                "The trader "
                f"offers the list:</p><ul>{''.join(avail)}</ul>",
                "where-and-when",
            )
        )
    order = sorted(
        trades,
        key=lambda x: (
            str(_first(x.get("offered_by")) and _first(x.get("offered_by"))["id"]),
            x.get("chapter", 99),
            str(x["id"]),
        ),
    )
    parts.append(
        t.section(
            "All trades",
            trade_rows_table(ctx, [str(x["id"]) for x in order], anchors=True, with_trader=True),
            "all-trades",
        )
    )
    for x in order:
        page.documents("trade", str(x["id"]), d.row_anchor("trade", str(x["id"])))
    page.body = "\n".join(parts)
    return page


def index_page(ctx: Context) -> Page:
    parts = []
    prose = ctx.prose("npcs/index.md")
    if prose:
        parts.append(prose)
    else:
        parts.append(
            "<p>The people of the game: where they stand, what they sell, craft and trade.</p>"
        )
    links = [
        f'<a href="{ctx.url(CRAFTING)}">Crafting</a>: '
        f"{t.esc(t.number(len(ctx.kb.entities('recipe'))))} recipes by output slot",
        f'<a href="{ctx.url(TRADES)}">Trades</a>: '
        f"{t.esc(t.number(len(ctx.kb.entities('trade'))))} item trades",
    ]
    parts.append("<ul>" + "".join(f"<li>{x}</li>" for x in links) + "</ul>\n")
    parts.append(t.section("NPCs by area", npc_list(ctx), "npcs"))
    shops_body = shop_list(ctx) + (
        "<p>A shop's stock is fixed: it comes from <code>market.dat</code> and does not "
        "change between chapters. Three shop numbers have stock but no script opens them. "
        f"In the original game the arrow keys reach only a tab's first two rows (<a href=\""
        f'{t.esc(ORIGINAL_BUGS)}">original bugs, 12</a>); the port fixes it.</p>'
    )
    parts.append(t.section("Shops", shops_body, "shops"))
    return Page(INDEX, "NPCs and shops", "\n".join(parts), description="Every NPC and shop.")


def pages(ctx: Context) -> Iterator[Page]:
    yield index_page(ctx)
    for e in ctx.kb.entities("npc"):
        yield npc_page(ctx, e)
    for s in ctx.kb.entities("shop"):
        yield shop_page(ctx, s)
    yield crafting_page(ctx)
    yield trades_page(ctx)


SECTION = Section(
    SECTION_ID,
    "NPCs and shops",
    30,
    ("npc", "shop", "listing", "recipe", "trade"),
    pages,
    blurb="The people of the game, their shops, crafting and trades.",
    image="npc:3",
    names=lambda ctx, kind, eid: shop_markup(ctx, eid) if kind == "shop" else None,
)
