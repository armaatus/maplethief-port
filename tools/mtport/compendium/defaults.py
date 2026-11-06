"""Default sections and page builders of the compendium site (ADR 0122).

Every knowledge-base kind has a home here, so the site is complete (``check``'s completeness
test passes) before any section plugin exists. A plugin in ``mtport.compendium.sections``
with the same section ``id`` replaces the default section entirely; it may still call
:func:`entity_page`, :func:`list_table` or :func:`section_pages` (also on the context) and
change what they return.

Default layout:

- ``<section>/index.html``: the section's prose (``docs/compendium/pages/<section>/index.md``)
  and one table per kind;
- ``<kind>/<slug>.html``: one page per entity, for kinds not in :data:`ANCHOR_KINDS`: an
  infobox of its scalar fields, its prose (``pages/<kind>/<slug>.md``), its relations as
  tables, its images;
- kinds in :data:`ANCHOR_KINDS` (shop listings, levels, warps, trades) are rows of the
  section's table, each with an anchor, instead of pages of their own.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Iterator, Mapping
from typing import Any

from mtport.compendium import templates as t
from mtport.compendium.siteapi import (
    Context,
    Entity,
    Page,
    Section,
    name_of,
    slug,
)

DEFAULT_SECTIONS: tuple[Section, ...] = (
    Section("monsters", "Monsters", 10, ("monster",), blurb="Every monster and boss."),
    Section("items", "Items", 20, ("item",), blurb="Equipment, potions and other items."),
    Section(
        "npcs",
        "NPCs and shops",
        30,
        ("npc", "shop", "listing", "recipe", "trade"),
        blurb="The people of the game, their shops and trades.",
    ),
    Section(
        "world", "World", 40, ("area", "map", "warp", "event"), blurb="Areas, maps and portals."
    ),
    Section("quests", "Quests", 50, ("quest", "dialogue"), blurb="Every quest and conversation."),
    Section("walkthrough", "Walkthrough", 60, blurb="The story, chapter by chapter."),
    Section(
        "skills", "Skills", 70, ("skill", "level", "job_advancement"), blurb="Skills and levels."
    ),
    Section("mechanics", "Mechanics", 80, blurb="How the game works."),
    Section("about", "About the game", 90, blurb="History, versions and unused content."),
)

# Kinds documented as anchored rows of their section's table rather than pages of their own.
ANCHOR_KINDS = frozenset({"listing", "level", "warp", "trade"})

KIND_TITLES = {
    "monster": "Monsters",
    "item": "Items",
    "npc": "NPCs",
    "shop": "Shops",
    "listing": "Shop listings",
    "recipe": "Recipes",
    "trade": "Trades",
    "area": "Areas",
    "map": "Maps",
    "warp": "Warps",
    "event": "Events",
    "quest": "Quests",
    "dialogue": "Dialogue",
    "skill": "Skills",
    "level": "Levels",
    "job_advancement": "Job advancements",
}

# Fields shown in a kind's table when present, in this order (the rest is on the pages);
# the fallback for kinds without columns in LIST_COLUMNS.
LIST_FIELDS = ("level", "hp", "mp", "exp", "meso", "price", "type", "area")

SKIP_FIELDS = frozenset({"id", "kind", "name", "confidence", "aliases"})

# Fields that are the knowledge base's bookkeeping, never shown on a page: sprite and icon
# numbers, file stems, raw table numbers and fields whose meaning is unknown.
INTERNAL_FIELDS = frozenset(
    {
        "sprite",
        "icon",
        "look",
        "file",
        "table",
        "table_id",
        "row",
        "name_fills_field",
        "name_source",
        "name_spot",
        "name_note",
        "area_no",
        "map_no",
        "backdrop",
        "tilesets",
        "world_spot",
        "banner",
        "map_script",
        "boss_bar",
        "walk_speed",
        "speed",
        "slot",
        "job",
        "type",
        "unknown_84",
        "unknown_9",
        "base",
        "per_level",
        "detail",
        "levels",
        "portals",
    }
)

# Relations shown elsewhere on the page (or on another page), never as a table of their own.
HIDDEN_RELATIONS = frozenset({"listings", "item", "shop", "area"})

# Field and column names in the style guide's words.
LABELS = {
    "hp": "HP",
    "mp": "MP",
    "exp": "EXP",
    "att": "ATT",
    "attack": "ATT",
    "def": "DEF",
    "defense": "DEF",
    "accuracy": "ACC",
    "avoid": "AVOID",
    "avoidability": "AVOID",
    "strength": "STR",
    "dexterity": "DEX",
    "intelligence": "INT",
    "luck": "LUK",
    "id": "ID",
    "req_level": "Required level",
    "max_level": "Maximum level",
    "max_stack": "Stack size",
    "exp_total": "Total EXP",
    "hp_gain": "HP gained",
    "mp_gain": "MP gained",
    "max_hp": "Maximum HP",
    "max_mp": "Maximum MP",
    "boss_map": "Boss map",
    "count": "Count",
    "price": "Price",
    "category": "Type",
    "chance": "Chance",
    "spawns_on": "Locations",
    "dropped_by": "Dropped by",
    "meso_dropped_by": "Dropped as meso by",
    "sold_by": "Sold at",
    "stands_on": "Location",
    "sells": "Stock",
    "spawns": "Monsters",
    "npcs": "NPCs",
    "maps": "Maps",
    "exits": "Exits",
    "entrances": "Entrances",
    "tile": "Position",
    "portal": "Portal",
    "target_portal": "Arrives at portal",
}

STAT_ORDER = ("attack", "defense", "accuracy", "avoid", "avoidability", "hp", "mp")
MOVEMENT = {1: "walks", 2: "floats", 3: "explodes on contact"}
# The first column of a relation table, by the kind it points at.
ENTRY_TITLES = {
    "monster": "Monster",
    "item": "Item",
    "npc": "NPC",
    "shop": "Shop",
    "map": "Map",
    "area": "Area",
    "skill": "Skill",
    "quest": "Quest",
    "listing": "Listing",
}
# Columns of relation tables that are bookkeeping (the script that runs, which slot of the
# drop table, the raw chance: the drop table shows the odds itself).
HIDDEN_COLUMNS = frozenset(
    {"script", "slot", "per", "percent", "nominal_percent", "chance", "listing", "gated"}
)


def kind_title(kind: str) -> str:
    return KIND_TITLES.get(kind, kind.replace("_", " ").capitalize())


def label(field: str) -> str:
    """A field name as a label: ``hp`` gives ``HP``, ``drop_rate`` gives ``Drop rate``."""
    if field.lower() in LABELS:
        return LABELS[field.lower()]
    return field.replace("_", " ").capitalize()


def entity_path(kind: str, eid: str) -> str:
    return f"{kind}/{slug(eid)}.html"


def row_anchor(kind: str, eid: str) -> str:
    return t.anchor_id(f"{kind}-{eid}")


def is_relation(value: Any) -> bool:
    """A list of ``{"kind", "id", ...}`` objects."""
    return (
        isinstance(value, list)
        and bool(value)
        and all(isinstance(v, Mapping) and "kind" in v and "id" in v for v in value)
    )


def is_unknown(e: Entity, field: str) -> bool:
    """The knowledge base marks the field's meaning as unknown (the style guide leaves those
    out of the infobox)."""
    conf = e.get("confidence")
    return isinstance(conf, Mapping) and conf.get(field) == "unknown"


def value_markup(ctx: Context, value: Any) -> str:
    """A scalar field's value: numbers with separators, strings as game text."""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, (int, float)):
        return t.esc(t.number(value))
    if isinstance(value, str):
        return t.verbatim(value) if value else ""
    if isinstance(value, Mapping) and set(value) <= {"en", "ko"}:
        parts = [t.verbatim(str(value[k]), k) for k in ("en", "ko") if value.get(k)]
        return " / ".join(parts)
    if isinstance(value, list) and all(isinstance(v, (int, float, str)) for v in value):
        return ", ".join(value_markup(ctx, v) for v in value)
    return ""


def link_or_text(ctx: Context, kind: str, eid: str) -> str:
    """A link to ``kind:eid`` when the knowledge base holds it, else its id as text."""
    if ctx.kb.get(kind, eid) is not None:
        return ctx.ref(kind, eid)
    return t.esc(f"{kind}:{eid}")


def thumb(ctx: Context, kind: str, eid: str) -> str:
    """A small picture of the entity for a table cell (its icon or sprite), or nothing."""
    img = ctx.images.main(kind, str(eid))
    if img is None or max(img.w, img.h) > 160:
        return ""
    scale = max(1, 36 // max(img.w, img.h, 1))
    return f'<span class="thumb">{ctx.img(img, scale=scale, cls="pixel")}</span>'


def entry_cell(ctx: Context, kind: str, eid: str, with_thumb: bool = True) -> t.Cell:
    e = ctx.kb.get(kind, eid)
    key = name_of(e) if e is not None else f"{kind}:{eid}"
    pic = thumb(ctx, kind, eid) if with_thumb and e is not None else ""
    return t.markup_cell(f'<span class="entry">{pic}{link_or_text(ctx, kind, eid)}</span>', key)


# Units of relation columns.
UNITS = {"price": "meso"}


def _cell(ctx: Context, key: str, v: Any) -> t.Cell:
    if isinstance(v, (int, float)) and not isinstance(v, bool):
        return t.num_cell(v, UNITS.get(key, ""))
    if key == "tile" and isinstance(v, list) and len(v) == 2:
        return t.markup_cell(t.esc(f"x {v[0]}, y {v[1]}"), v[0] * 10000 + v[1])
    return t.markup_cell(value_markup(ctx, v) if v is not None else "")


def relation_table(ctx: Context, rows: list[Mapping[str, Any]], key: str = "") -> str:
    """A relation as a table: the linked entry (with its picture), then the relation's own
    values, bookkeeping columns left out."""
    extra: list[str] = []
    for r in rows:
        for k in r:
            if k not in ("kind", "id") and k not in HIDDEN_COLUMNS and k not in extra:
                extra.append(k)
    kinds = {str(r["kind"]) for r in rows}
    first = ENTRY_TITLES.get(next(iter(kinds)), "Entry") if len(kinds) == 1 else "Entry"
    cols = [t.Column(first)] + [
        t.Column(label(k), numeric=all(isinstance(r.get(k), (int, float)) for r in rows))
        for k in extra
    ]
    body = []
    for r in rows:
        cells = [entry_cell(ctx, str(r["kind"]), str(r["id"]))]
        cells += [_cell(ctx, k, r.get(k)) for k in extra]
        body.append(cells)
    return t.table(cols, body)


# --- infobox ---------------------------------------------------------------------------------


def _level(v: Any) -> str:
    return t.esc(f"Lv. {v}") if isinstance(v, int) and v > 0 else ""


def _num(v: Any, unit: str = "") -> str:
    if not isinstance(v, (int, float)) or isinstance(v, bool):
        return ""
    return t.esc(t.number(v) + (f" {unit}" if unit else ""))


def _yes(v: Any) -> str:
    return "yes" if v else "no"


def _links(ctx: Context, refs: Any, more_anchor: str = "", limit: int = 4) -> str:
    """Up to ``limit`` links from a relation list, then "and N more"."""
    if not is_relation(refs):
        if isinstance(refs, Mapping) and "kind" in refs and "id" in refs:
            refs = [refs]
        else:
            return ""
    seen: list[tuple[str, str]] = []
    for r in refs:
        k = (str(r["kind"]), str(r["id"]))
        if k not in seen:
            seen.append(k)
    shown = [link_or_text(ctx, k, i) for k, i in seen[:limit]]
    rest = len(seen) - limit
    if rest > 0:
        more = f"and {rest} more"
        shown.append(f'<a href="#{more_anchor}">{more}</a>' if more_anchor else more)
    return ", ".join(shown)


def _meso(m: Any) -> str:
    if not isinstance(m, Mapping) or not m.get("max"):
        return ""
    rng = f"{t.number(m.get('min', 0))}-{t.number(m['max'])} meso"
    pct = m.get("percent")
    return t.esc(rng + (f" ({t.percent_text(pct)} of kills)" if pct else ""))


def _monster_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    cls = e.get("class")
    return [
        ("Level", _level(e.get("level"))),
        ("HP", _num(e.get("hp"))),
        ("EXP", _num(e.get("exp"))),
        ("Meso", _meso(e.get("meso"))),
        ("Boss", _yes(cls == "boss") if cls else ""),
        ("Movement", t.esc(MOVEMENT.get(e.get("movement_kind"), ""))),
        ("Found on", _links(ctx, e.get("spawns_on"), "locations")),
    ]


def _item_type(e: Entity) -> str:
    cat = e.get("category")
    if not isinstance(cat, str) or not cat:
        return ""
    text = cat[0].upper() + cat[1:]
    wt = e.get("weapon_type")
    if isinstance(wt, str) and wt:
        text += f" ({wt})"
    return t.esc(text)


def _range(v: Any) -> str:
    if isinstance(v, Mapping):
        lo, hi = v.get("min", 0), v.get("max", 0)
        if not lo and not hi:
            return ""
        return t.esc(t.number(lo) if lo == hi else f"{t.number(lo)}-{t.number(hi)}")
    if isinstance(v, (int, float)) and not isinstance(v, bool) and v:
        return t.esc(t.number(v))
    return ""


def _item_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    rows = [("Type", _item_type(e)), ("Required level", _level(e.get("req_level")))]
    stats = e.get("stats") if isinstance(e.get("stats"), Mapping) else e
    for k in STAT_ORDER:
        if k in stats and not is_unknown(e, k):
            rows.append((label(k), _range(stats[k])))
    bonus = e.get("bonus")
    if isinstance(bonus, Mapping) and bonus.get("value"):
        rows.append(("Bonus", t.esc(f"+{t.number(bonus['value'])} {label(str(bonus['stat']))}")))
    rows += [
        ("Stack size", _num(e.get("max_stack")) if e.get("max_stack") else ""),
        ("Sell price", _num(e.get("price"), "meso") if e.get("price") else ""),
    ]
    return rows


def _npc_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    return [("Location", _links(ctx, e.get("stands_on"), "location"))]


def _shop_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    n = len(e.get("sells") or [])
    return [("Stock", t.esc(f"{n} items" if n != 1 else "1 item") if n else "")]


def _map_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    bgm = e.get("bgm")
    w, h = e.get("width"), e.get("height")
    eid = str(e["id"])
    no_save = e.get("no_save")
    return [
        ("Area", _links(ctx, e.get("area"))),
        ("Map number", t.esc(eid.replace("-", ".")) if "-" in eid else ""),
        ("Size", t.esc(f"{w} by {h} tiles") if w and h else ""),
        ("Music", t.esc(f"track {bgm['sound']}") if isinstance(bgm, Mapping) else ""),
        ("Boss map", _yes(e.get("boss_map")) if "boss_map" in e else ""),
        ("Saving", t.esc(f"off ({no_save})") if isinstance(no_save, str) else ""),
        ("Monsters", _num(len(e.get("spawns") or [])) if e.get("spawns") else ""),
    ]


def _area_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    maps = e.get("maps") or []
    return [
        ("Maps", _num(len(maps)) if maps else ""),
        ("Town music", _num(e.get("town_bgm")) and t.esc(f"track {e['town_bgm']}")),
        ("Field music", _num(e.get("field_bgm")) and t.esc(f"track {e['field_bgm']}")),
    ]


def _skill_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    levels = [lv for lv in e.get("levels") or [] if isinstance(lv, Mapping)]
    mp = [lv.get("mp") for lv in levels if isinstance(lv.get("mp"), int)]
    typ = e.get("type")
    return [
        ("Branch", t.esc(e["branch"]) if isinstance(e.get("branch"), str) else ""),
        ("Tier", _num(e.get("tier"))),
        ("Type", t.esc(typ[0].upper() + typ[1:]) if isinstance(typ, str) and typ else ""),
        ("Maximum level", _num(e.get("max_level"))),
        ("MP cost", _range({"min": min(mp), "max": max(mp)}) if mp and max(mp) else ""),
        ("Learnable", _yes(e.get("learnable")) if "learnable" in e else ""),
    ]


INFOBOX_ROWS = {
    "monster": _monster_rows,
    "item": _item_rows,
    "npc": _npc_rows,
    "shop": _shop_rows,
    "map": _map_rows,
    "area": _area_rows,
    "skill": _skill_rows,
}


def _generic_rows(ctx: Context, e: Entity) -> list[tuple[str, str]]:
    rows = []
    for k, v in e.items():
        if k in SKIP_FIELDS or k in INTERNAL_FIELDS or is_relation(v) or is_unknown(e, k):
            continue
        if k in ("description",):
            continue
        rows.append((label(k), value_markup(ctx, v)))
    return rows


def infobox_rows(ctx: Context, kind: str, e: Entity) -> list[tuple[str, str]]:
    """The infobox rows of ``e``: the style guide's fields for its kind (others: every
    scalar field that is not bookkeeping), then its ID."""
    rows = INFOBOX_ROWS.get(kind, _generic_rows)(ctx, e)
    rows.append(("ID", f"<code>{t.esc(e['id'])}</code>"))
    return [(k, v) for k, v in rows if v]


def description_markup(e: Entity) -> str:
    """The entity's in-game description (English, then Korean), as game text."""
    d = e.get("description")
    if isinstance(d, str) and d:
        return f'<p class="ib-desc">{t.verbatim(d)}</p>'
    if isinstance(d, Mapping):
        parts = [
            f'<p class="ib-desc">{t.verbatim(str(d[k]), k)}</p>' for k in ("en", "ko") if d.get(k)
        ]
        return "".join(parts)
    return ""


def infobox_for(ctx: Context, kind: str, e: Entity) -> str:
    eid = str(e["id"])
    ko = name_of(e, "ko")
    sub = t.verbatim(ko, "ko") if ko != name_of(e, "en") else ""
    return t.infobox(
        ctx.name(kind, eid),
        infobox_rows(ctx, kind, e),
        ctx.sprite(kind, eid),
        subtitle_markup=sub,
        footer_markup=description_markup(e),
    )


# --- relations -------------------------------------------------------------------------------


def _drops(ctx: Context, e: Entity, keys: tuple[str, ...], who: str) -> str:
    """The drops of a monster (``who="item"``) or the monsters that drop an item."""
    drops = []
    for key in keys:
        for r in e.get(key) or []:
            if not isinstance(r, Mapping) or "kind" not in r:
                continue
            link = entry_cell(ctx, str(r["kind"]), str(r["id"])).markup
            if "chance" in r:
                per = int(r.get("per") or 1000)
                pct = r.get("percent")
                drops.append(t.Drop(link, int(r["chance"]), per=per, percent=pct if pct else None))
            elif r.get("percent") is not None:  # meso: a fixed share of kills, an amount
                pct = float(r["percent"])
                note = ""
                if r.get("max"):
                    note = f"{t.number(r.get('min', 0))}-{t.number(r['max'])} meso"
                drops.append(t.Drop(link, round(pct * 10), note, per=1000, percent=pct, face=False))
    if not drops:
        return ""
    return t.drop_table(drops, ENTRY_TITLES.get(who, "Entry"))


def _relation(key: str) -> Callable[[Context, Entity], str]:
    def build(ctx: Context, e: Entity) -> str:
        rows = e.get(key)
        return relation_table(ctx, rows, key) if is_relation(rows) else ""

    return build


# Per kind: (section title, anchor, the relation keys it shows, builder), in the style
# guide's order. Relation keys not listed get a table of their own after these.
RELATIONS: dict[str, list[tuple[str, str, tuple[str, ...], Callable[[Context, Entity], str]]]] = {
    "monster": [
        (
            "Drops",
            "drops",
            ("drops", "meso_drops"),
            lambda c, e: _drops(c, e, ("drops", "meso_drops"), "item"),
        ),
        ("Locations", "locations", ("spawns_on",), _relation("spawns_on")),
    ],
    "item": [
        (
            "Dropped by",
            "dropped-by",
            ("dropped_by", "meso_dropped_by"),
            lambda c, e: _drops(c, e, ("dropped_by", "meso_dropped_by"), "monster"),
        ),
        ("Sold at", "sold-at", ("sold_by",), _relation("sold_by")),
    ],
    "npc": [("Location", "location", ("stands_on",), _relation("stands_on"))],
    "shop": [("Stock", "stock", ("sells",), _relation("sells"))],
    "map": [
        ("Monsters", "monsters", ("spawns",), _relation("spawns")),
        ("NPCs", "npcs", ("npcs",), _relation("npcs")),
        ("Exits", "exits", ("exits",), _relation("exits")),
        ("Entrances", "entrances", ("entrances",), _relation("entrances")),
    ],
    "area": [("Maps", "maps", ("maps",), _relation("maps"))],
}


def _skill_effect(ctx: Context, e: Entity) -> str:
    levels = [lv for lv in e.get("levels") or [] if isinstance(lv, Mapping)]
    if not levels:
        return ""
    fields = [
        (k, title)
        for k, title in (("damage", "Damage"), ("time", "Time"), ("etc", "Effect"), ("mp", "MP"))
        if any(lv.get(k) for lv in levels)
    ]
    cols = [t.Column("Level", numeric=True)] + [t.Column(x, numeric=True) for _, x in fields]
    cols.append(t.Column("In-game text"))
    rows = []
    for lv in levels:
        row = [t.num_cell(lv.get("level", 0))]
        for k, _ in fields:
            v = lv.get(k, 0)
            row.append(t.Cell(t.esc(f"{t.number(v)}%"), v) if k == "damage" else t.num_cell(v))
        d = lv.get("detail")
        en = d.get("en") if isinstance(d, Mapping) else None
        row.append(t.markup_cell(t.verbatim(en) if en else ""))
        rows.append(row)
    return t.table(cols, rows)


RELATIONS["skill"] = [("Effect", "effect", ("levels",), _skill_effect)]


def entity_sections(ctx: Context, kind: str, e: Entity) -> list[str]:
    """The tables of an entity page, as sections: the style guide's for its kind, then a
    table for each other relation."""
    out = []
    done: set[str] = set()
    for title, anchor, keys, build in RELATIONS.get(kind, []):
        done.update(keys)
        body = build(ctx, e)
        if body:
            out.append(t.section(title, body, anchor))
    for k, v in e.items():
        if k in done or k in SKIP_FIELDS or k in HIDDEN_RELATIONS or not is_relation(v):
            continue
        out.append(t.section(label(k), relation_table(ctx, v, k), t.anchor_id(k)))
    return out


def entity_page(ctx: Context, kind: str, e: Entity, path: str | None = None) -> Page:
    """The default page of one entity: the infobox, its prose, its tables, its pictures."""
    eid = str(e["id"])
    path = path or entity_path(kind, eid)
    parts = [infobox_for(ctx, kind, e)]
    prose = ctx.prose(path[: -len(".html")] + ".md")
    if prose:
        parts.append(prose)
    parts += entity_sections(ctx, kind, e)
    images = [i for i in ctx.entity_images(kind, eid) if not i.role.endswith("2x")]
    renders = [i for i in images if i.role == "render"]
    if renders:
        parts.append(t.section("Map", map_figure(ctx, kind, eid), "map"))
        images = [i for i in images if i.role not in ("render", "overlay")]
    if len(images) > 1:
        figs = []
        for i in images:
            big = max(i.w, i.h)
            scale = max(1, min(3, 96 // big)) if big and big <= 96 else 1
            figs.append((ctx.img(i, alt=i.alt or name_of(e), scale=scale), t.esc(_role(i))))
        parts.append(t.section("Images", t.gallery(figs), "images"))
    return Page(
        path=path,
        title=name_of(e),
        heading=ctx.name(kind, eid),
        title_verbatim=True,
        body="\n".join(parts),
        description=f"{kind_title(kind)}: {name_of(e)}",
    ).documents(kind, eid)


def map_figure(ctx: Context, kind: str, eid: str) -> str:
    """The map's render at full size with the images step's overlay (portals, NPCs and
    spawn points) drawn over it."""
    render = ctx.entity_images(kind, eid, ("render",))
    if not render:
        return ""
    r = render[0]
    over = ctx.entity_images(kind, eid, ("overlay",))
    top = ctx.img(over[0], cls="overlay") if over else ""
    cap = "Portals, NPCs and spawn points are marked." if over else ""
    return (
        f'<figure class="map-figure">{ctx.img(r)}{top}'
        + (f"<figcaption>{cap}</figcaption>" if cap else "")
        + "</figure>\n"
    )


ROLE_CAPTIONS = {
    "still": "still",
    "anim": "animation",
    "icon": "icon",
    "worn": "worn",
    "render": "map",
    "overlay": "portals, NPCs and spawns",
}


def _role(i) -> str:
    cap = ROLE_CAPTIONS.get(i.role, i.role)
    return f"{i.action}, {cap}" if i.action else cap


# --- lists -----------------------------------------------------------------------------------


def _list_num(field: str, unit: str = "") -> Callable[[Context, Entity], t.Cell]:
    def get(ctx: Context, e: Entity) -> t.Cell:
        v = e.get(field)
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            return t.num_cell(v, unit)
        return t.Cell("", -1)

    return get


def _list_meso(ctx: Context, e: Entity) -> t.Cell:
    m = e.get("meso")
    if isinstance(m, Mapping) and m.get("max"):
        return t.Cell(t.esc(f"{t.number(m.get('min', 0))}-{t.number(m['max'])}"), m["max"])
    return t.Cell("", -1)


def _list_link(field: str) -> Callable[[Context, Entity], t.Cell]:
    def get(ctx: Context, e: Entity) -> t.Cell:
        v = e.get(field)
        if isinstance(v, Mapping):
            v = [v]
        if is_relation(v):
            r = v[0]
            other = ctx.kb.get(str(r["kind"]), str(r["id"]))
            key = name_of(other) if other is not None else ""
            return t.markup_cell(link_or_text(ctx, str(r["kind"]), str(r["id"])), key)
        return t.Cell("", "")

    return get


def _list_text(fn: Callable[[Entity], str]) -> Callable[[Context, Entity], t.Cell]:
    def get(ctx: Context, e: Entity) -> t.Cell:
        s = fn(e)
        return t.Cell(t.esc(s), s)

    return get


def _list_count(field: str) -> Callable[[Context, Entity], t.Cell]:
    def get(ctx: Context, e: Entity) -> t.Cell:
        return t.num_cell(len(e.get(field) or []))

    return get


def _cap(v: Any) -> str:
    return v[0].upper() + v[1:] if isinstance(v, str) and v else ""


# Per kind: (column title, numeric, cell). Kinds not listed use LIST_FIELDS.
LIST_COLUMNS: dict[str, list[tuple[str, bool, Callable[[Context, Entity], t.Cell]]]] = {
    "monster": [
        ("Level", True, _list_num("level")),
        ("HP", True, _list_num("hp")),
        ("EXP", True, _list_num("exp")),
        ("Meso", True, _list_meso),
    ],
    "item": [
        ("Type", False, _list_text(lambda e: _cap(e.get("category")))),
        ("Required level", True, _list_num("req_level")),
        ("Sell price", True, _list_num("price")),
    ],
    "npc": [("Location", False, _list_link("stands_on"))],
    "shop": [("Items", True, _list_count("sells"))],
    "listing": [
        ("Shop", False, _list_link("shop")),
        ("Price", True, _list_num("price", "meso")),
    ],
    "map": [
        ("Map", False, _list_text(lambda e: str(e["id"]).replace("-", "."))),
        ("Area", False, _list_link("area")),
        ("Monsters", True, _list_count("spawns")),
    ],
    "area": [("Maps", True, _list_count("maps"))],
    "skill": [
        ("Branch", False, _list_text(lambda e: str(e.get("branch") or ""))),
        ("Tier", True, _list_num("tier")),
        ("Type", False, _list_text(lambda e: _cap(e.get("type")))),
        ("Maximum level", True, _list_num("max_level")),
    ],
    "level": [
        ("EXP", True, _list_num("exp")),
        ("Total EXP", True, _list_num("exp_total")),
        ("Maximum HP", True, _list_num("max_hp")),
        ("Maximum MP", True, _list_num("max_mp")),
    ],
}


def list_table(ctx: Context, kind: str, entities: Iterable[Entity], anchors: bool = False) -> str:
    """A table of ``entities``: name (linked, with its picture), Korean name and the
    columns of :data:`LIST_COLUMNS` (else the :data:`LIST_FIELDS` they have). With
    ``anchors`` each row gets the id :func:`row_anchor` gives."""
    entities = list(entities)
    if kind in LIST_COLUMNS:
        spec = LIST_COLUMNS[kind]
        if kind == "listing":  # a listing's name is its item's: link the item
            spec = [("Item", False, _list_link("item"))] + spec[:]
    else:
        fields = [f for f in LIST_FIELDS if any(f in e for e in entities)]
        spec = []
        for f in fields:
            numeric = all(isinstance(e.get(f, 0), (int, float)) for e in entities)
            spec.append((label(f), numeric, lambda c, e, f=f: _cell(c, f, e.get(f))))
    cols = [t.Column("Name"), t.Column("Korean")] + [
        t.Column(title, numeric=numeric) for title, numeric, _ in spec
    ]
    rows = []
    for e in entities:
        eid = str(e["id"])
        if anchors:
            name = ctx.name(kind, eid)
        else:
            name = f'<span class="entry">{thumb(ctx, kind, eid)}{ctx.ref(kind, eid)}</span>'
        cells = [
            t.markup_cell(name, name_of(e)),
            t.Cell(t.verbatim(name_of(e, "ko"), "ko"), name_of(e, "ko"), "ko"),
        ]
        cells += [get(ctx, e) for _, _, get in spec]
        rows.append(cells)
    ids = [row_anchor(kind, str(e["id"])) for e in entities] if anchors else None
    return t.table(cols, rows, ids)


def section_pages(ctx: Context, section: Section) -> Iterator[Page]:
    """The default pages of ``section``: its index and a page per entity of its kinds."""
    index = Page(section.index_path, section.title, "", search=True)
    parts = []
    prose = ctx.prose(f"{section.id}/index.md")
    if prose:
        parts.append(prose)
    entity_pages = []
    filled = [k for k in section.kinds if ctx.kb.entities(k)]
    for kind in section.kinds:
        entities = ctx.kb.entities(kind)
        if not entities:
            continue
        anchored = kind in ANCHOR_KINDS
        table = list_table(ctx, kind, entities, anchors=anchored)
        if len(filled) == 1 and kind_title(kind) == section.title:
            # one table under the page's own title: no second heading saying the same
            label_ = t.esc(kind_title(kind))
            parts.append(f'<section id="{t.esc(kind)}" aria-label="{label_}">\n{table}</section>\n')
        else:
            parts.append(t.section(kind_title(kind), table, kind))
        for e in entities:
            eid = str(e["id"])
            if anchored:
                index.documents(kind, eid, row_anchor(kind, eid))
            else:
                entity_pages.append(entity_page(ctx, kind, e))
    if not parts:
        parts.append('<p class="muted">This section is being written.</p>')
    index.body = "\n".join(parts)
    yield index
    yield from entity_pages
