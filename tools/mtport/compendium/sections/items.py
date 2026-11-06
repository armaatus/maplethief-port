"""The Items section of the compendium (#219): a page for every item, list pages per type,
comparison tables and the items with no known source.

Every item (``item:equip:N``, ``item:etc:N``) gets ``item/<slug>.html``: an infobox, the
game's description (other items), "How to get" (every source and where it is: drops with the
monster's maps, shops with the NPC that opens them and its map and area, quest rewards,
recipes and trades with their NPC and map, the cash shop), "Used in" (quest requirements,
recipe materials, trade costs, what a scroll applies to), "Upgrading" (equipment: the
scrolls for its slot) and "Notes".

The rules the pages state come from ``docs/re/mechanics.md`` (drops, equipment stats and
upgrade slots, scrolls, return scrolls, special items) and ``docs/re/cash-shop.md`` (what
the cash shop and the paid revives grant); the tables below hold those facts as numbers.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Mapping
from dataclasses import dataclass
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of

SID = "items"

# --- facts from docs/re (numbers only, no game text) ------------------------------------------

# Equipment slot (item_equip.slot) -> list page and its words.
SLOTS = {0: "hat", 1: "weapon", 2: "overall", 3: "earrings", 4: "shoes"}

# The upgrade scrolls of each slot (mechanics.md, Upgrade scrolls: ScrollTargetSlot 0x691E5).
SCROLLS_BY_SLOT = {
    1: ("etc:19", "etc:134"),
    2: ("etc:20", "etc:133"),
    0: ("etc:21", "etc:137"),
    4: ("etc:22", "etc:136"),
    3: ("etc:23", "etc:135"),
}
SCROLL_SLOT = {s: slot for slot, ids in SCROLLS_BY_SLOT.items() for s in ids}
# Success chance of the n-th successful scroll (mechanics.md, CItem::ApplyScroll 0x69511).
FIXED_CHANCES = (100, 75, 50, 25, 10, 0, 0)
ROLLED_CHANCES = (100, 80, 60, 40, 30, 20, 10)

# Return scrolls (type 4) -> the area whose town they lead to; 24 picks the nearest town.
RETURN_AREAS = {
    "etc:25": "0",
    "etc:26": "1",
    "etc:27": "2",
    "etc:28": "3",
    "etc:29": "4",
    "etc:30": "6",
    "etc:31": "9",
    "etc:88": "10",
}
NEAREST_TOWN = "etc:24"

# Potions that restore a percentage of the maximum instead of a flat amount (Elixir, Power
# Elixir: CHero::UseItem 0x66695).
PERCENT_POTIONS = frozenset({"etc:9", "etc:10"})

# Special items (type 7), what using one does (mechanics.md, Special items).
SPECIAL_EFFECTS = {
    "etc:128": "Gives one level.",
    "etc:129": "Resets the skills: every point spent comes back.",
    "etc:130": "Revives the hero from the death popup instead of returning to town.",
    "etc:131": "Gives 20% of the EXP the current level needs.",
    "etc:132": "Gives one skill point.",
}

# The cash shop and the paid revives (cash-shop.md, Products): item -> (where, count).
CASH_SOURCES = {
    "etc:129": [("A tile of the cash shop (the Cash button in the field)", 1)],
    "etc:132": [("A tile of the cash shop (the Cash button in the field)", 1)],
    "etc:10": [
        ("Each of the two paid revives on the death popup, with the revive itself", 30),
    ],
}
# Cash items no product or code path grants (cash-shop.md).
CASH_UNGRANTED = frozenset({"etc:128", "etc:131"})

# Weapon speed class -> ticks between attacks (mechanics.md, table 0x117140; 20 ticks a second).
SPEED_TICKS = {1: 2, 2: 4, 3: 6}

ETC_TYPE_WORDS = {
    0: "meso",
    1: "potion",
    2: "throwing star",
    3: "upgrade scroll",
    4: "return scroll",
    5: "material",
    6: "quest item",
    7: "cash item",
    8: "100% scroll",
}

STAT_FIELDS = (  # (equipment stats key, etc field, label)
    ("attack", "attack", "ATT"),
    ("defense", "defense", "DEF"),
    ("accuracy", "accuracy", "ACC"),
    ("avoid", "avoidability", "AVOID"),
    ("hp", "hp", "HP"),
    ("mp", "mp", "MP"),
)

WORDS = ("zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine")


def words(n: int) -> str:
    return WORDS[n] if 0 <= n < 10 else t.number(n)


def plural(n: int, one: str, many: str = "") -> str:
    return f"{words(n)} {one if n == 1 else (many or one + 's')}"


def and_list(parts: list[str]) -> str:
    """``a``, ``a and b``, ``a, b, and c`` (the serial comma)."""
    if len(parts) <= 2:
        return " and ".join(parts)
    return ", ".join(parts[:-1]) + ", and " + parts[-1]


# --- list pages ------------------------------------------------------------------------------


@dataclass(frozen=True)
class Group:
    """One list page: ``items/<slug>.html``, the items ``match`` accepts."""

    slug: str
    title: str
    match: Callable[[Entity], bool]
    blurb: str


def _equip(slot: int, wt: str | None = "*") -> Callable[[Entity], bool]:
    def m(e: Entity) -> bool:
        if e.get("table") != "equip" or e.get("slot") != slot:
            return False
        return wt == "*" or e.get("weapon_type") == wt

    return m


def _etc(*types: int) -> Callable[[Entity], bool]:
    return lambda e: e.get("table") == "etc" and e.get("type") in types


GROUPS = (
    Group("weapons", "Weapons", _equip(1), "Claws and daggers."),
    Group("hats", "Hats", _equip(0), "Headgear."),
    Group("overalls", "Overalls", _equip(2), "Body armour."),
    Group("earrings", "Earrings", _equip(3), "Earrings."),
    Group("shoes", "Shoes", _equip(4), "Footwear."),
    Group("potions", "Potions", _etc(1), "HP and MP recovery."),
    Group("throwing-stars", "Throwing stars", _etc(2), "Ammunition for claws."),
    Group("scrolls", "Scrolls", _etc(3, 4, 8), "Upgrade scrolls and return scrolls."),
    Group("materials", "Materials", _etc(5), "Monster drops used in crafting and trades."),
    Group("quest-items", "Quest items", _etc(6), "Items that quests ask for."),
    Group("cash-items", "Cash items", _etc(7), "Items of the cash shop."),
    Group("meso", "Meso", _etc(0), "The meso bags monsters drop."),
)


def group_of(e: Entity) -> Group | None:
    for g in GROUPS:
        if g.match(e):
            return g
    return None


# --- knowledge-base helpers --------------------------------------------------------------------


def _refs(e: Entity | None, key: str) -> list[Mapping[str, Any]]:
    if e is None:
        return []
    v = e.get(key)
    if isinstance(v, Mapping):
        v = [v]
    return [r for r in v or [] if isinstance(r, Mapping) and "kind" in r and "id" in r]


def opened(ctx: Context, shop_id: str) -> bool:
    return bool(_refs(ctx.kb.get("shop", shop_id), "opened_by"))


def buy_price(ctx: Context, e: Entity) -> int | None:
    """The lowest price of the item in a shop that an NPC opens."""
    prices = [
        int(r["price"])
        for r in _refs(e, "sold_by")
        if isinstance(r.get("price"), int) and opened(ctx, str(r["id"]))
    ]
    return min(prices) if prices else None


@dataclass(frozen=True)
class Source:
    """One way to get an item: ``how`` (drop, meso, shop, quest, recipe, trade, cash) and
    the knowledge-base entity it comes from (``kind``, ``id``; empty for the cash shop).
    ``reachable`` is false for a shop no NPC opens."""

    how: str
    kind: str
    id: str
    reachable: bool = True


def sources(ctx: Context, e: Entity) -> list[Source]:
    """Every source of item ``e`` the knowledge base and the cash-shop facts know."""
    out = [Source("drop", "monster", str(r["id"])) for r in _refs(e, "dropped_by")]
    out += [Source("meso", "monster", str(r["id"])) for r in _refs(e, "meso_dropped_by")]
    seen = set()
    for r in _refs(e, "sold_by"):
        sid = str(r["id"])
        if sid not in seen:
            seen.add(sid)
            out.append(Source("shop", "shop", sid, opened(ctx, sid)))
    out += [Source("quest", "quest", str(r["id"])) for r in _refs(e, "rewarded_by_quests")]
    out += [Source("recipe", "recipe", str(r["id"])) for r in _refs(e, "crafted_by")]
    out += [Source("trade", "trade", str(r["id"])) for r in _refs(e, "traded_for_in")]
    out += [Source("gift", "npc", str(r["id"])) for r in _refs(e, "given_by_npcs")]
    out += [Source("cash", "", "") for _ in CASH_SOURCES.get(str(e["id"]), ())]
    return out


def has_source(ctx: Context, e: Entity) -> bool:
    return any(s.reachable for s in sources(ctx, e))


SOURCE_WORDS = {
    "drop": "drop",
    "meso": "drop",
    "shop": "shop",
    "quest": "quest",
    "recipe": "recipe",
    "trade": "trade",
    "gift": "NPC gift",
    "cash": "cash shop",
}


def source_summary(ctx: Context, e: Entity) -> str:
    seen: list[str] = []
    for s in sources(ctx, e):
        w = SOURCE_WORDS[s.how]
        if s.reachable and w not in seen:
            seen.append(w)
    return ", ".join(seen)


# --- markup helpers ----------------------------------------------------------------------------


def where(ctx: Context, map_id: Any) -> str:
    """A map link with its area in brackets."""
    if not isinstance(map_id, str) or ctx.kb.get("map", map_id) is None:
        return ""
    m = ctx.kb.get("map", map_id)
    areas = _refs(m, "area")
    out = ctx.ref("map", map_id)
    if areas and ctx.kb.get("area", str(areas[0]["id"])) is not None:
        out += f" ({ctx.ref('area', str(areas[0]['id']))})"
    return out


def chapters_text(chs: Any) -> str:
    """``chapter 2`` or ``chapters 2 and 3``; empty when the list covers chapters 1 to 3."""
    if not isinstance(chs, list) or not chs:
        return ""
    nums = sorted({int(c) for c in chs if isinstance(c, int)})
    if {1, 2, 3} <= set(nums):
        return ""
    if len(nums) == 1:
        return f"chapter {nums[0]}"
    return "chapters " + and_list([str(n) for n in nums])


def npc_where(ctx: Context, refs: Iterable[Mapping[str, Any]]) -> tuple[str, str]:
    """``(NPC links, places)`` of opener-style references (``{kind: npc, id, map}``)."""
    npcs: list[str] = []
    places: list[str] = []
    for r in refs:
        if str(r.get("kind")) != "npc":
            continue
        link = d.link_or_text(ctx, "npc", str(r["id"]))
        if link not in npcs:
            npcs.append(link)
        w = where(ctx, r.get("map"))
        ch = chapters_text(r.get("chapters"))
        if w:
            w += f", {t.esc(ch)}" if ch else ""
            if w not in places:
                places.append(w)
    return ", ".join(npcs), "; ".join(places)


def item_count(ctx: Context, refs: Iterable[Mapping[str, Any]]) -> str:
    """``3 × Item, 2 × Item`` from ``{kind: item, id, count}`` references."""
    parts = []
    for r in refs:
        n = r.get("count")
        pre = f"{t.number(n)} × " if isinstance(n, int) else ""
        parts.append(t.esc(pre) + d.link_or_text(ctx, str(r["kind"]), str(r["id"])))
    return ", ".join(parts)


def _cell(markup: str, key: Any = None) -> t.Cell:
    return t.markup_cell(markup, key)


def _num_or_blank(v: Any, unit: str = "") -> t.Cell:
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return t.num_cell(v, unit)
    return t.Cell("", -1)


def stat_range(e: Entity, key: str) -> tuple[int, int]:
    s = (e.get("stats") or {}).get(key) or {}
    lo, hi = int(s.get("min", 0) or 0), int(s.get("max", 0) or 0)
    return lo, max(lo, hi)


def range_text(lo: int, hi: int) -> str:
    return t.number(lo) if lo == hi else f"{t.number(lo)}-{t.number(hi)}"


def stat_cell(e: Entity, key: str) -> t.Cell:
    lo, hi = stat_range(e, key)
    if not lo and not hi:
        return t.Cell("", -1)
    return t.Cell(t.esc(range_text(lo, hi)), lo)


def speed_text(e: Entity) -> str:
    ticks = SPEED_TICKS.get(e.get("speed"))
    if not ticks:
        return ""
    return f"an attack every {ticks} ticks ({t.number(ticks / 20)} s)"


def type_text(e: Entity) -> str:
    if e.get("table") == "equip":
        slot = SLOTS.get(e.get("slot"), str(e.get("category") or "equipment"))
        wt = e.get("weapon_type")
        return f"{slot.capitalize()} ({wt})" if slot == "weapon" and wt else slot.capitalize()
    w = ETC_TYPE_WORDS.get(e.get("type"), str(e.get("category") or "item"))
    return w[0].upper() + w[1:]


def etc_effect(ctx: Context, e: Entity) -> str:
    """What using (or carrying) an other item does, in our words (markup)."""
    eid, typ = str(e["id"]), e.get("type")
    hp, mp = int(e.get("hp") or 0), int(e.get("mp") or 0)
    if typ == 1:
        if eid in PERCENT_POTIONS:
            parts = [f"{hp}% of maximum HP" if hp else "", f"{mp}% of maximum MP" if mp else ""]
        else:
            parts = [f"{t.number(hp)} HP" if hp else "", f"{t.number(mp)} MP" if mp else ""]
        parts = [p for p in parts if p]
        return t.esc("Restores " + " and ".join(parts) + ".") if parts else ""
    if typ == 2:
        att = int(e.get("attack") or 0)
        return t.esc(f"+{att} ATT on star hits.") if att else ""
    if typ in (3, 8):
        bonus = scroll_stats(e)
        slot = SLOTS.get(SCROLL_SLOT.get(eid, -1), "")
        target = f" to the equipped {slot}" if slot else ""
        chance = " Always succeeds." if typ == 8 else ""
        return t.esc(f"Adds {bonus}{target}.{chance}") if bonus else ""
    if typ == 4:
        if eid == NEAREST_TOWN:
            return t.esc("Moves the hero to the nearest town.")
        area = RETURN_AREAS.get(eid)
        if area and ctx.kb.get("area", area) is not None:
            return f"Moves the hero to the town of {ctx.ref('area', area)}."
        return ""
    if typ == 7:
        return t.esc(SPECIAL_EFFECTS.get(eid, ""))
    if typ == 0:
        return t.esc("Picked up as meso: the bag's count is the amount.")
    return ""


def scroll_stats(e: Entity) -> str:
    parts = [
        f"+{t.number(int(e.get(f) or 0))} {lab}" for _, f, lab in STAT_FIELDS if int(e.get(f) or 0)
    ]
    return ", ".join(parts)


def thumb_name(ctx: Context, e: Entity, boxes: bool = True) -> t.Cell:
    return d.entry_cell(ctx, "item", str(e["id"]), placeholder=boxes)


# --- infobox -----------------------------------------------------------------------------------


def _price(v: Any) -> str:
    return t.esc(f"{t.number(v)} meso") if isinstance(v, int) and v else ""


def equip_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    rows = [("Type", t.esc(type_text(e)))]
    lv = e.get("req_level")
    rows.append(("Required level", t.esc(f"Lv. {lv}") if isinstance(lv, int) and lv else ""))
    rows.append(("Required job", "Thief" if e.get("job") == 4 else ""))
    rows.append(("Attack speed", t.esc(speed_text(e))))
    for key, _, lab in STAT_FIELDS:
        lo, hi = stat_range(e, key)
        if lo or hi:
            rows.append((lab, t.esc(range_text(lo, hi))))
    bonus = e.get("bonus")
    crafted = bool(_refs(e, "crafted_by"))
    if isinstance(bonus, Mapping) and bonus.get("value"):
        stat = dict((k, lab) for k, _, lab in STAT_FIELDS).get(str(bonus.get("stat")), "")
        rows.append(("Bonus", t.esc(f"+{t.number(bonus['value'])} {stat} (crafted copies only)")))
    rows.append(("Upgrade slots", t.esc("5, or 7 when crafted" if crafted else "5")))
    rows.append(("Buy price", _price(buy_price(ctx, e))))
    rows.append(("Sell price", _price(e.get("price"))))
    return rows


def etc_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    rows = [("Type", t.esc(type_text(e))), ("Effect", etc_effect(ctx, e))]
    stack = e.get("max_stack")
    rows.append(("Stack size", t.esc(t.number(stack)) if isinstance(stack, int) and stack else ""))
    star = int(e.get("unknown_84") or 0)
    if e.get("type") == 2 and star and isinstance(stack, int):
        rows.append(("Refill", t.esc(f"{t.number(star * stack / 10)} meso for a full stack")))
    rows.append(("Buy price", _price(buy_price(ctx, e))))
    rows.append(("Sell price", _price(e.get("price"))))
    return rows


def description_markup(e: Entity) -> str:
    """The game's description, English then Korean, line breaks kept."""
    desc = e.get("description")
    if not isinstance(desc, Mapping):
        return ""
    out = []
    for lang in ("en", "ko"):
        text = desc.get(lang)
        if isinstance(text, str) and text.strip():
            lines = "<br>".join(t.esc(x) for x in text.split("\n"))
            out.append(f'<p class="ib-desc" data-verbatim lang="{lang}">{lines}</p>')
    return "".join(out)


def picture(ctx: Context, e: Entity) -> str:
    """The icon, and for equipment the hero wearing it."""
    eid = str(e["id"])
    icon = ctx.entity_images("item", eid, ("icon",))
    worn = ctx.entity_images("item", eid, ("worn",))
    parts = []
    if icon:
        i = icon[0]
        scale = max(1, min(4, 64 // max(i.w, i.h, 1)))
        parts.append(ctx.img(i, scale=scale))
    if worn:
        w = worn[0]
        scale = max(1, min(3, 120 // max(w.w, w.h, 1)))
        parts.append(ctx.img(w, scale=scale))
    return "".join(parts)


def infobox(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    rows = equip_rows(ctx, e) if e.get("table") == "equip" else etc_rows(ctx, e)
    rows.append(("ID", f"<code>{t.esc(eid)}</code>"))
    ko = name_of(e, "ko")
    sub = t.verbatim(ko, "ko") if ko != name_of(e, "en") else ""
    worn = bool(ctx.entity_images("item", eid, ("worn",)))
    return t.infobox(
        ctx.name("item", eid),
        [(k, v) for k, v in rows if v],
        picture(ctx, e),
        caption_markup="Icon and worn look" if worn else "",
        subtitle_markup=sub,
        footer_markup=description_markup(e),
    )


# --- How to get --------------------------------------------------------------------------------

LEVEL_NOTE = (
    "Chances are per kill for a hero of level 0. Each kill adds the hero's level to an"
    " equipment chance (in 32,768, the game's random numbers being 15-bit) and a tenth of it"
    " to any other item's chance (in 1,000)."
)


def _drops_markup(ctx: Context, e: Entity) -> str:
    refs = _refs(e, "dropped_by")
    boxes = d.any_thumb(ctx, (("monster", r["id"]) for r in refs))
    drops = []
    for r in refs:
        mid = str(r["id"])
        mon = ctx.kb.get("monster", mid)
        link = d.entry_cell(ctx, "monster", mid, placeholder=boxes).markup
        maps = d._links(ctx, _refs(mon, "spawns_on"), limit=2) if mon is not None else ""
        note = f"Found on {maps}" if maps else "Found on no map"
        pct = r.get("percent")
        drops.append(
            t.Drop(
                link,
                int(r.get("chance") or 0),
                per=int(r.get("per") or 1000),
                percent=float(pct) if pct else None,
                note_markup=note,
            )
        )
    if not drops:
        return ""
    return t.drop_table(drops, "Monster") + f"<p>{t.esc(LEVEL_NOTE)}</p>\n"


def _meso_markup(ctx: Context, e: Entity) -> str:
    refs = _refs(e, "meso_dropped_by")
    if not refs:
        return ""
    boxes = d.any_thumb(ctx, (("monster", r["id"]) for r in refs))
    cols = [t.Column("Monster"), t.Column("Amount", numeric=True), t.Column("Chance", numeric=True)]
    rows = []
    for r in refs:
        lo, hi = int(r.get("min") or 0), int(r.get("max") or 0)
        pct = float(r.get("percent") or 0)
        rows.append(
            [
                d.entry_cell(ctx, "monster", str(r["id"]), placeholder=boxes),
                t.Cell(t.esc(f"{range_text(lo, hi)} meso"), hi),
                t.Cell(t.esc(t.percent_text(pct)), pct),
            ]
        )
    lead = (
        "<p>A kill drops this bag when the meso amount falls in its range: below 100 meso the"
        " small bag, 100 to 199 the middle one, 200 and up the large one.</p>\n"
    )
    return lead + t.table(cols, rows)


def _shops_markup(ctx: Context, e: Entity) -> str:
    rows = []
    closed = []
    for r in _refs(e, "sold_by"):
        sid = str(r["id"])
        shop = ctx.kb.get("shop", sid)
        price = r.get("price")
        openers = _refs(shop, "opened_by")
        if not openers:
            closed.append((sid, price))
            continue
        for o in openers:
            npc, place = npc_where(ctx, [o])
            rows.append(
                [
                    _cell(ctx.ref("shop", sid), int(sid) if sid.isdigit() else sid),
                    _num_or_blank(price, "meso"),
                    _cell(npc),
                    _cell(place),
                ]
            )
    out = ""
    if rows:
        cols = [
            t.Column("Shop"),
            t.Column("Price", numeric=True),
            t.Column("Opened by"),
            t.Column("Where"),
        ]
        out += t.table(cols, rows)
    if closed:
        parts = [
            ctx.ref("shop", sid) + (t.esc(f" ({t.number(p)} meso)") if isinstance(p, int) else "")
            for sid, p in closed
        ]
        one = len(parts) == 1
        verb = ("also " if rows else "") + ("lists" if one else "list")
        out += (
            f"<p>{and_list(parts)} {verb} it, but no NPC opens"
            f" {'that shop' if one else 'those shops'}, so it is not reachable in play.</p>\n"
        )
    return out


def _quests_markup(ctx: Context, refs: list[Mapping[str, Any]]) -> str:
    rows = []
    for r in refs:
        qid = str(r["id"])
        q = ctx.kb.get("quest", qid)
        npc, place = npc_where(ctx, _refs(q, "given_by"))
        ch = q.get("chapter") if q is not None else None
        rows.append(
            [
                _cell(d.link_or_text(ctx, "quest", qid), name_of(q) if q else qid),
                _num_or_blank(r.get("count")),
                _num_or_blank(ch),
                _cell(npc),
                _cell(place),
            ]
        )
    if not rows:
        return ""
    cols = [
        t.Column("Quest"),
        t.Column("Count", numeric=True),
        t.Column("Chapter", numeric=True),
        t.Column("Given by"),
        t.Column("Where"),
    ]
    return t.table(cols, rows)


def _cost_markup(ctx: Context, items: list[Mapping[str, Any]], meso: Any) -> str:
    out = item_count(ctx, items)
    if isinstance(meso, int) and meso:
        out += (", " if out else "") + t.esc(f"{t.number(meso)} meso")
    return out


def _recipes_markup(ctx: Context, refs: list[Mapping[str, Any]], made: bool) -> str:
    """Recipes that make the item (``made``) or use it as a material."""
    rows = []
    for r in refs:
        rid = str(r["id"])
        rec = ctx.kb.get("recipe", rid)
        if rec is None:
            continue
        npc, place = npc_where(ctx, _refs(rec, "crafted_by"))
        ch = rec.get("chapter")
        label = f"Chapter {ch} recipe" if isinstance(ch, int) else "Recipe"
        first = _cell(ctx.ref("recipe", rid, label=label), rid)
        cost = _cost_markup(ctx, _refs(rec, "materials"), rec.get("meso"))
        if made:
            rows.append([first, _cell(cost), _cell(npc), _cell(place)])
        else:
            result = item_count(ctx, _refs(rec, "result"))
            rows.append(
                [first, _num_or_blank(r.get("count")), _cell(result), _cell(npc), _cell(place)]
            )
    if not rows:
        return ""
    if made:
        cols = [
            t.Column("Recipe"),
            t.Column("Materials"),
            t.Column("Crafted by"),
            t.Column("Where"),
        ]
    else:
        cols = [
            t.Column("Recipe"),
            t.Column("Count", numeric=True),
            t.Column("Makes"),
            t.Column("Crafted by"),
            t.Column("Where"),
        ]
    return t.table(cols, rows)


def _trades_markup(ctx: Context, refs: list[Mapping[str, Any]], got: bool) -> str:
    """Trades that give the item (``got``) or take it."""
    rows = []
    for r in refs:
        tid = str(r["id"])
        tr = ctx.kb.get("trade", tid)
        if tr is None:
            continue
        npc, place = npc_where(ctx, _refs(tr, "offered_by"))
        ch = tr.get("chapter")
        label = f"Chapter {ch} trade" if isinstance(ch, int) else "Trade"
        first = _cell(ctx.ref("trade", tid, label=label), tid)
        cost = _cost_markup(ctx, _refs(tr, "cost"), tr.get("cost_meso"))
        result = item_count(ctx, _refs(tr, "result"))
        if got:
            rows.append(
                [first, _num_or_blank(r.get("count")), _cell(cost), _cell(npc), _cell(place)]
            )
        else:
            rows.append(
                [first, _num_or_blank(r.get("count")), _cell(result), _cell(npc), _cell(place)]
            )
    if not rows:
        return ""
    cols = [
        t.Column("Trade"),
        t.Column("Count", numeric=True),
        t.Column("Costs" if got else "Gives"),
        t.Column("Offered by"),
        t.Column("Where"),
    ]
    return t.table(cols, rows)


def _gifts_markup(ctx: Context, e: Entity) -> str:
    rows = []
    for r in _refs(e, "given_by_npcs"):
        npc, place = npc_where(ctx, [r])
        rows.append([_cell(npc), _num_or_blank(r.get("count")), _cell(place)])
    if not rows:
        return ""
    cols = [t.Column("NPC"), t.Column("Count", numeric=True), t.Column("Where")]
    text = "<p>The NPC's conversation hands the item over outside any quest reward.</p>\n"
    return text + t.table(cols, rows)


def _cash_markup(ctx: Context, e: Entity) -> str:
    rows = CASH_SOURCES.get(str(e["id"]))
    if not rows:
        return ""
    items = "".join(f"<li>{t.esc(text)}: {t.esc(t.number(n))}.</li>" for text, n in rows)
    return (
        f"<ul>{items}</ul>\n<p>Purchases went through the App Store and a Nexon server that"
        " no longer runs. In the port, the Assist <code>cash_shop = meso</code> sells them for"
        " meso; it is off by default.</p>\n"
    )


def how_to_get(ctx: Context, e: Entity) -> str:
    parts = [
        ("Drops", "drops", _drops_markup(ctx, e)),
        ("Meso drops", "meso-drops", _meso_markup(ctx, e)),
        ("Shops", "shops", _shops_markup(ctx, e)),
        ("Quest rewards", "quest-rewards", _quests_markup(ctx, _refs(e, "rewarded_by_quests"))),
        ("Crafting", "crafting", _recipes_markup(ctx, _refs(e, "crafted_by"), made=True)),
        ("Trades", "trades", _trades_markup(ctx, _refs(e, "traded_for_in"), got=True)),
        ("Given by NPCs", "gifts", _gifts_markup(ctx, e)),
        ("Cash shop", "cash-shop", _cash_markup(ctx, e)),
    ]
    body = "".join(t.section(title, b, a, level=3) for title, a, b in parts if b)
    if not has_source(ctx, e):
        body = (
            f"<p>No known source: no monster, reachable shop, quest, recipe, trade or cash"
            f' product gives this item. See <a href="{ctx.url(SID + "/no-source.html")}">'
            "items with no known source</a>.</p>\n"
        ) + body
    return t.section("How to get", body, "how-to-get")


def used_in(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    parts = [
        ("Quests", "used-quests", _quests_markup(ctx, _refs(e, "needed_by_quests"))),
        ("Crafting", "used-crafting", _recipes_markup(ctx, _refs(e, "material_of"), made=False)),
        ("Trades", "used-trades", _trades_markup(ctx, _refs(e, "traded_away_in"), got=False)),
    ]
    body = "".join(t.section(title, b, a, level=3) for title, a, b in parts if b)
    slot = SCROLL_SLOT.get(eid)
    if slot is not None:
        g = next(x for x in GROUPS if x.slug == _slot_group(slot))
        body = (
            f"<p>It upgrades the equipped {SLOTS[slot]}: any item of the"
            f' <a href="{ctx.url(SID + "/" + g.slug + ".html")}">{t.esc(g.title.lower())}</a>'
            " list.</p>\n"
        ) + body
    return t.section("Used in", body, "used-in") if body else ""


def _slot_group(slot: int) -> str:
    return {0: "hats", 1: "weapons", 2: "overalls", 3: "earrings", 4: "shoes"}[slot]


def upgrading(ctx: Context, e: Entity) -> str:
    scrolls = [s for s in SCROLLS_BY_SLOT.get(e.get("slot"), ()) if ctx.kb.get("item", s)]
    if not scrolls:
        return ""
    rows = []
    for s in scrolls:
        se = ctx.kb.get("item", s)
        rows.append(
            [
                thumb_name(ctx, se),
                _cell(t.esc(scroll_stats(se))),
                _cell(t.esc("always" if se.get("type") == 8 else "by history, below")),
            ]
        )
    cols = [t.Column("Scroll"), t.Column("Adds"), t.Column("Success")]
    fixed = ", ".join(f"{c}%" for c in FIXED_CHANCES[:5])
    rolled = ", ".join(f"{c}%" for c in ROLLED_CHANCES)
    text = (
        "<p>Each scroll uses an upgrade slot whether it works or not. An ordinary scroll's"
        " chance falls with the successful scrolls the item already has: "
        f"{fixed} on a dropped or bought item (so at most five succeed), {rolled} on a crafted"
        " one. The first scroll always works.</p>\n"
    )
    return t.section("Upgrading", t.table(cols, rows, compact=True) + text, "upgrading")


def notes(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    items = []
    if e.get("table") == "equip":
        items.append(
            "A dropped or bought copy, or one a quest gives, has the minimum of every stat"
            " range and 5 upgrade slots; it never rolls and never has the bonus stat."
        )
        if _refs(e, "crafted_by"):
            items.append(
                "Only crafting rolls: a crafted copy gets each stat at random within its"
                " range, the bonus stat added, and 7 upgrade slots."
            )
        inverted = [
            lab
            for key, _, lab in STAT_FIELDS
            if int(((e.get("stats") or {}).get(key) or {}).get("max", 0) or 0)
            < int(((e.get("stats") or {}).get(key) or {}).get("min", 0) or 0)
        ]
        if inverted:
            items.append(
                f"The table gives {and_list(inverted)} a maximum below the minimum; the game"
                " uses the minimum, also when crafting."
            )
    if eid in CASH_UNGRANTED:
        items.append(
            "No cash shop product and no script grants it: a leftover of the feature-phone version."
        )
    if e.get("type") == 2 and int(e.get("unknown_84") or 0):
        star = int(e["unknown_84"])
        items.append(
            f"A shop refills a stack for {t.number(star / 10)} meso per missing star; selling"
            f" a stack pays the sell price plus {t.number(star / 10)} meso per star left."
        )
    if eid in PERCENT_POTIONS:
        items.append("It restores a share of the maximum, not a fixed amount.")
    if not items:
        return ""
    return t.section(
        "Notes", "<ul>" + "".join(f"<li>{t.esc(x)}</li>" for x in items) + "</ul>\n", "notes"
    )


def lead(ctx: Context, e: Entity) -> str:
    name = ctx.name("item", str(e["id"]))
    typ = type_text(e).lower()
    lv = e.get("req_level")
    if e.get("table") == "equip" and isinstance(lv, int) and lv:
        art = "an" if str(lv)[0] == "8" or lv in (11, 18) else "a"
        what = f"{art} level {lv} {typ}"
    else:
        art = "an" if typ[:1] in "aeiou" else "a"
        what = f"{art} {typ}"
        if typ == "meso":
            what = "a meso bag"
    srcs = [s for s in sources(ctx, e) if s.reachable]
    kinds = {SOURCE_WORDS[s.how] for s in srcs}
    if not srcs:
        tail = " It has no known source."
    else:
        bits = []
        n = len({s.id for s in srcs if s.how in ("drop", "meso")})
        if n:
            bits.append(f"dropped by {plural(n, 'monster')}")
        n = len([s for s in srcs if s.how == "shop"])
        if n:
            bits.append(f"sold in {plural(n, 'shop')}")
        n = len([s for s in srcs if s.how == "quest"])
        if n:
            bits.append(f"given by {plural(n, 'quest')}")
        if "recipe" in kinds:
            bits.append("crafted")
        if "trade" in kinds:
            bits.append("traded for")
        if "NPC gift" in kinds:
            bits.append("handed over by an NPC")
        if "cash shop" in kinds:
            bits.append("sold in the cash shop")
        tail = " It is " + and_list(bits) + "."
    return f"<p>{name} is {t.esc(what)}.{t.esc(tail)}</p>\n"


def item_page(ctx: Context, e: Entity) -> Page:
    eid = str(e["id"])
    path = d.entity_path("item", eid)
    parts = [infobox(ctx, e), lead(ctx, e)]
    prose = ctx.prose(path[: -len(".html")] + ".md")
    if prose:
        parts.append(prose)
    parts.append(how_to_get(ctx, e))
    parts.append(used_in(ctx, e))
    if e.get("table") == "equip":
        parts.append(upgrading(ctx, e))
    parts.append(notes(ctx, e))
    g = group_of(e)
    crumbs = [(g.title, f"{SID}/{g.slug}.html")] if g else []
    return Page(
        path=path,
        title=name_of(e),
        heading=ctx.name("item", eid),
        title_verbatim=True,
        body="\n".join(p for p in parts if p),
        crumbs=crumbs,
        description=f"Item: {name_of(e)}. {type_text(e)}.",
    ).documents("item", eid)


# --- list pages --------------------------------------------------------------------------------

Col = tuple[str, bool, Callable[[Context, Entity], t.Cell]]


def _c_level(ctx: Context, e: Entity) -> t.Cell:
    return _num_or_blank(e.get("req_level"))


def _c_stat(key: str) -> Callable[[Context, Entity], t.Cell]:
    return lambda ctx, e: stat_cell(e, key)


def _c_field(key: str, unit: str = "") -> Callable[[Context, Entity], t.Cell]:
    def get(ctx: Context, e: Entity) -> t.Cell:
        v = e.get(key)
        return _num_or_blank(v, unit) if v else t.Cell("", -1)

    return get


def _c_speed(ctx: Context, e: Entity) -> t.Cell:
    ticks = SPEED_TICKS.get(e.get("speed"))
    return t.Cell(t.esc(f"{ticks} ticks"), ticks) if ticks else t.Cell("", -1)


def _c_buy(ctx: Context, e: Entity) -> t.Cell:
    return _num_or_blank(buy_price(ctx, e), "meso")


def _c_sell(ctx: Context, e: Entity) -> t.Cell:
    return _num_or_blank(e.get("price") or None, "meso")


def _c_sources(ctx: Context, e: Entity) -> t.Cell:
    s = source_summary(ctx, e)
    return t.Cell(t.esc(s or "none known"), s)


def _c_effect(ctx: Context, e: Entity) -> t.Cell:
    return _cell(etc_effect(ctx, e))


def _c_type(ctx: Context, e: Entity) -> t.Cell:
    s = type_text(e)
    return t.Cell(t.esc(s), s)


def _equip_present(items: list[Entity]) -> list[Col]:
    cols: list[Col] = [("Required level", True, _c_level)]
    if any(e.get("speed") in SPEED_TICKS for e in items):
        cols.append(("Attack interval", True, _c_speed))
    for key, _, lab in STAT_FIELDS:
        if any(stat_range(e, key) != (0, 0) for e in items):
            cols.append((lab, True, _c_stat(key)))
    return cols + [
        ("Buy price", True, _c_buy),
        ("Sell price", True, _c_sell),
        ("Sources", False, _c_sources),
    ]


def _etc_present(items: list[Entity]) -> list[Col]:
    cols: list[Col] = [("Effect", False, _c_effect)]
    if any(e.get("max_stack") for e in items):
        cols.append(("Stack size", True, _c_field("max_stack")))
    return cols + [
        ("Buy price", True, _c_buy),
        ("Sell price", True, _c_sell),
        ("Sources", False, _c_sources),
    ]


def item_table(ctx: Context, items: list[Entity], cols: list[Col], compact: bool = False) -> str:
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in items))
    columns = [t.Column("Name"), t.Column("Korean")] + [
        t.Column(title, numeric=num) for title, num, _ in cols
    ]
    rows = []
    for e in items:
        cells = [
            thumb_name(ctx, e, boxes),
            t.Cell(t.verbatim(name_of(e, "ko"), "ko"), name_of(e, "ko"), "ko"),
        ]
        cells += [get(ctx, e) for _, _, get in cols]
        rows.append(cells)
    return t.table(columns, rows, compact=compact)


def _by_level(items: Iterable[Entity]) -> list[Entity]:
    return sorted(items, key=lambda e: (int(e.get("req_level") or 0), int(e.get("table_id") or 0)))


def group_table(ctx: Context, items: list[Entity]) -> str:
    if items and items[0].get("table") == "equip":
        items = _by_level(items)
        return item_table(ctx, items, _equip_present(items))
    return item_table(ctx, items, _etc_present(items))


GROUP_PARTS: dict[str, list[tuple[str, str, Callable[[Entity], bool]]]] = {
    "weapons": [
        ("Claws", "claws", lambda e: e.get("weapon_type") == "claw"),
        ("Daggers", "daggers", lambda e: e.get("weapon_type") == "dagger"),
        ("Other weapons", "other", lambda e: e.get("weapon_type") not in ("claw", "dagger")),
    ],
    "scrolls": [
        ("Upgrade scrolls", "upgrade", lambda e: e.get("type") == 3),
        ("100% scrolls", "sure", lambda e: e.get("type") == 8),
        ("Return scrolls", "return", lambda e: e.get("type") == 4),
    ],
}

GROUP_TEXT = {
    "weapons": (
        "Claws throw stars and daggers strike up close; the skills of the Assassin branch need"
        " a claw and those of the Bandit branch a dagger. The attack interval is the number of"
        " ticks between attacks (20 ticks a second at the default speed)."
    ),
    "scrolls": (
        "An upgrade scroll adds its stats to one equipped item and uses one of its upgrade"
        " slots, whether it works or not; a 100% scroll always works. A return scroll moves"
        " the hero to a town."
    ),
    "throwing-stars": (
        "Stars are the ammunition of claws. A star's ATT is added to the hero's for star hits,"
        " and a shop refills an empty stack for its refill price."
    ),
    "cash-items": ("The cash shop sold two of these for real money; the others no product grants."),
}

EQUIP_STATS_NOTE = (
    "Stat columns give the table's range. A dropped, bought or quest copy has the minimum of"
    " each; only a crafted copy rolls within the range."
)


def group_page(ctx: Context, g: Group, items: list[Entity]) -> Page:
    parts = []
    prose = ctx.prose(f"{SID}/{g.slug}.md")
    if prose:
        parts.append(prose)
    elif g.slug in GROUP_TEXT:
        parts.append(f"<p>{t.esc(GROUP_TEXT[g.slug])}</p>\n")
    if items and items[0].get("table") == "equip":
        parts.append(f"<p>{t.esc(EQUIP_STATS_NOTE)}</p>\n")
    if g.slug in GROUP_PARTS:
        for title, anchor, match in GROUP_PARTS[g.slug]:
            sub = [e for e in items if match(e)]
            if sub:
                parts.append(t.section(title, group_table(ctx, sub), anchor))
    else:
        label = t.esc(g.title)
        parts.append(f'<section aria-label="{label}">\n{group_table(ctx, items)}</section>\n')
    return Page(
        f"{SID}/{g.slug}.html", g.title, "\n".join(parts), description=f"Items: {g.title.lower()}."
    )


# --- comparisons -------------------------------------------------------------------------------


def band_text(lv: int) -> str:
    lo = (lv // 10) * 10
    return f"{max(lo, 1)}-{lo + 9}"


def claw_comparison(ctx: Context) -> str:
    claws = [e for e in ctx.kb.entities("item") if e.get("weapon_type") == "claw"]
    if not claws:
        return ""
    claws.sort(key=lambda e: ((int(e.get("req_level") or 0) // 10), -stat_range(e, "attack")[0]))
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in claws))
    rows = []
    for e in claws:
        lv = int(e.get("req_level") or 0)
        lo, hi = stat_range(e, "attack")
        rows.append(
            [
                t.Cell(t.esc(band_text(lv)), lv // 10),
                thumb_name(ctx, e, boxes),
                _num_or_blank(lv),
                t.Cell(t.esc(range_text(lo, hi)), lo),
                _c_speed(ctx, e),
                _c_buy(ctx, e),
            ]
        )
    cols = [
        t.Column("Levels"),
        t.Column("Claw"),
        t.Column("Required level", numeric=True),
        t.Column("ATT", numeric=True),
        t.Column("Attack interval", numeric=True),
        t.Column("Buy price", numeric=True),
    ]
    return t.table(cols, rows)


def star_comparison(ctx: Context) -> str:
    stars = [e for e in ctx.kb.entities("item") if e.get("table") == "etc" and e.get("type") == 2]
    if not stars:
        return ""
    stars.sort(key=lambda e: (int(e.get("attack") or 0), int(e.get("max_stack") or 0)))
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in stars))
    rows = []
    for e in stars:
        stack = int(e.get("max_stack") or 0)
        star = int(e.get("unknown_84") or 0)
        rows.append(
            [
                thumb_name(ctx, e, boxes),
                _num_or_blank(int(e.get("attack") or 0)),
                _num_or_blank(stack),
                _c_buy(ctx, e),
                _num_or_blank(star * stack / 10 if star else None, "meso"),
            ]
        )
    cols = [
        t.Column("Star"),
        t.Column("ATT", numeric=True),
        t.Column("Stack size", numeric=True),
        t.Column("Buy price", numeric=True),
        t.Column("Full refill", numeric=True),
    ]
    return t.table(cols, rows)


def _amount_cell(v: int, pct: bool) -> t.Cell:
    if not v:
        return t.Cell("", -1)
    return t.Cell(t.esc(f"{v}%"), v) if pct else t.num_cell(v)


def _per_meso_cell(v: int, price: int | None, pct: bool) -> t.Cell:
    if pct or not v or not price:
        return t.Cell("", -1)
    r = round(v / price, 2)
    return t.Cell(t.esc(t.number(r)), r)


def potion_comparison(ctx: Context) -> str:
    pots = [e for e in ctx.kb.entities("item") if e.get("table") == "etc" and e.get("type") == 1]
    if not pots:
        return ""
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in pots))
    rows = []
    for e in pots:
        price = buy_price(ctx, e)
        hp, mp = int(e.get("hp") or 0), int(e.get("mp") or 0)
        pct = str(e["id"]) in PERCENT_POTIONS
        rows.append(
            [
                thumb_name(ctx, e, boxes),
                _amount_cell(hp, pct),
                _amount_cell(mp, pct),
                _num_or_blank(price, "meso"),
                _per_meso_cell(hp, price, pct),
                _per_meso_cell(mp, price, pct),
            ]
        )
    rows.sort(key=lambda r: -(r[4].key if isinstance(r[4].key, (int, float)) else -1))
    cols = [
        t.Column("Potion"),
        t.Column("HP", numeric=True),
        t.Column("MP", numeric=True),
        t.Column("Buy price", numeric=True),
        t.Column("HP per meso", numeric=True),
        t.Column("MP per meso", numeric=True),
    ]
    return t.table(cols, rows)


def comparisons_page(ctx: Context) -> Page:
    body = [
        "<p>Side-by-side tables for choosing gear and supplies. Prices are the lowest in a shop"
        " an NPC opens.</p>\n",
        t.section(
            "Claws by level",
            "<p>Claws grouped by ten required levels, strongest first. The attack interval is"
            " the ticks between attacks, so fewer is faster.</p>\n" + claw_comparison(ctx),
            "claws",
        ),
        t.section("Throwing stars", star_comparison(ctx), "stars"),
        t.section(
            "Potions by heal per meso",
            "<p>Elixirs restore a share of the maximum, so their value per meso grows with the"
            " hero and is left blank.</p>\n" + potion_comparison(ctx),
            "potions",
        ),
    ]
    return Page(
        f"{SID}/comparisons.html",
        "Item comparisons",
        "\n".join(body),
        description="Claws, throwing stars and potions compared.",
    )


# --- no known source ---------------------------------------------------------------------------


def no_source_items(ctx: Context) -> list[Entity]:
    """The items with no reachable source (a shop no NPC opens does not count)."""
    return [e for e in ctx.kb.entities("item") if not has_source(ctx, e)]


def no_source_page(ctx: Context) -> Page:
    items = no_source_items(ctx)
    closed = [e for e in items if _refs(e, "sold_by")]
    body = [
        "<p>These items exist in the item tables, but nothing in the game hands them out: no"
        " monster drops them, no shop an NPC opens sells them, and no quest, recipe, trade or"
        " cash product gives them. Some are leftovers of other versions of the game; the"
        " search covers the data tables, the scripts and the cash shop, so a source elsewhere"
        " in the code cannot be ruled out.</p>\n"
    ]
    if items:
        cols: list[Col] = [("Type", False, _c_type)]
        body.append(t.section("Items", item_table(ctx, items, cols), "items"))
    else:
        body.append("<p>Every item has a known source.</p>\n")
    if closed:
        shops = sorted(
            {str(r["id"]) for e in closed for r in _refs(e, "sold_by")},
            key=lambda s: int(s) if s.isdigit() else 0,
        )
        links = and_list([ctx.ref("shop", s) for s in shops])
        body.append(
            t.section(
                "Shops no NPC opens",
                f"<p>Some of these are listed in {links}, which no script opens: the shop screen"
                " is only reached through an NPC.</p>\n",
                "unopened-shops",
            )
        )
    return Page(
        f"{SID}/no-source.html",
        "No known source",
        "\n".join(body),
        description="Items that nothing in the game gives.",
    )


# --- index -------------------------------------------------------------------------------------


def index_page(ctx: Context, groups: list[tuple[Group, list[Entity]]]) -> Page:
    parts = []
    prose = ctx.prose(f"{SID}/index.md")
    if prose:
        parts.append(prose)
    lists = "".join(
        f'<li><a href="{ctx.url(f"{SID}/{g.slug}.html")}">{t.esc(g.title)}</a>'
        f" ({t.number(len(items))}): {t.esc(g.blurb)}</li>"
        for g, items in groups
    )
    lists += (
        f'<li><a href="{ctx.url(SID + "/comparisons.html")}">Item comparisons</a>: claws,'
        " throwing stars and potions side by side.</li>"
        f'<li><a href="{ctx.url(SID + "/no-source.html")}">No known source</a>'
        f" ({t.number(len(no_source_items(ctx)))}): items nothing in the game gives.</li>"
    )
    parts.append(t.section("Lists", f"<ul>{lists}</ul>\n", "lists"))
    items = ctx.kb.entities("item")
    cols: list[Col] = [
        ("Type", False, _c_type),
        ("Required level", True, _c_level),
        ("Sell price", True, _c_sell),
        ("Sources", False, _c_sources),
    ]
    parts.append(t.section("All items", item_table(ctx, items, cols), "all"))
    return Page(
        f"{SID}/index.html",
        "Items",
        "\n".join(parts),
        description="Every item of the game: equipment, potions, scrolls and more.",
    )


def pages(ctx: Context) -> Iterable[Page]:
    items = ctx.kb.entities("item")
    groups = []
    for g in GROUPS:
        members = [e for e in items if g.match(e)]
        if members:
            groups.append((g, members))
    yield index_page(ctx, groups)
    for g, members in groups:
        yield group_page(ctx, g, members)
    yield comparisons_page(ctx)
    yield no_source_page(ctx)
    for e in items:
        yield item_page(ctx, e)


SECTION = Section(
    SID,
    "Items",
    20,
    ("item",),
    pages,
    blurb="Equipment, potions and other items, and where to get each one.",
    image="item:etc:11",
)
