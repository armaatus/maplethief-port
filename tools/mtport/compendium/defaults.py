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

from collections.abc import Iterable, Iterator, Mapping
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

# Fields shown in a kind's table when present, in this order (the rest is on the pages).
LIST_FIELDS = ("level", "hp", "mp", "exp", "meso", "price", "type", "area")

SKIP_FIELDS = frozenset({"id", "kind", "name", "confidence", "aliases"})


def kind_title(kind: str) -> str:
    return KIND_TITLES.get(kind, kind.replace("_", " ").capitalize())


def label(field: str) -> str:
    """A field name as a label: ``hp`` gives ``HP``, ``drop_rate`` gives ``Drop rate``."""
    upper = {"hp": "HP", "mp": "MP", "exp": "EXP", "att": "ATT", "def": "DEF", "id": "ID"}
    if field.lower() in upper:
        return upper[field.lower()]
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


def value_markup(ctx: Context, value: Any) -> str:
    """A scalar field's value: numbers as numbers, strings as game text."""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int):
        return t.esc(f"{value:,}")
    if isinstance(value, float):
        return t.esc(f"{value:g}")
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


def relation_table(ctx: Context, rows: list[Mapping[str, Any]]) -> str:
    extra: list[str] = []
    for r in rows:
        for k in r:
            if k not in ("kind", "id") and k not in extra:
                extra.append(k)
    cols = [t.Column("Entry")] + [
        t.Column(label(k), numeric=all(isinstance(r.get(k), (int, float)) for r in rows))
        for k in extra
    ]
    body = []
    for r in rows:
        cells = [t.markup_cell(link_or_text(ctx, str(r["kind"]), str(r["id"])))]
        for k in extra:
            v = r.get(k)
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                cells.append(t.num_cell(v))
            else:
                cells.append(t.markup_cell(value_markup(ctx, v) if v is not None else ""))
        body.append(cells)
    return t.table(cols, body)


def infobox_for(ctx: Context, kind: str, e: Entity) -> str:
    eid = str(e["id"])
    rows: list[tuple[str, str]] = []
    ko = name_of(e, "ko")
    if ko != name_of(e, "en"):
        rows.append(("Korean name", t.verbatim(ko, "ko")))
    rows.append(("ID", t.esc(eid)))
    for k, v in e.items():
        if k in SKIP_FIELDS or is_relation(v):
            continue
        shown = value_markup(ctx, v)
        if shown:
            rows.append((label(k), shown))
    main = ctx.images.main(kind, eid)
    image = ctx.img(main, alt=main.alt or name_of(e)) if main else ""
    return t.infobox(ctx.name(kind, eid), rows, image)


def entity_page(ctx: Context, kind: str, e: Entity, path: str | None = None) -> Page:
    """The default page of one entity."""
    eid = str(e["id"])
    path = path or entity_path(kind, eid)
    parts = [infobox_for(ctx, kind, e)]
    prose = ctx.prose(path[: -len(".html")] + ".md")
    if prose:
        parts.append(prose)
    for k, v in e.items():
        if k not in SKIP_FIELDS and is_relation(v):
            parts.append(t.section(label(k), relation_table(ctx, v), t.anchor_id(k)))
    images = ctx.entity_images(kind, eid)
    if len(images) > 1:
        figs = [(ctx.img(i, alt=i.alt or name_of(e)), t.esc(i.role)) for i in images]
        parts.append(t.section("Images", t.gallery(figs), "images"))
    return Page(
        path=path,
        title=name_of(e),
        heading=ctx.name(kind, eid),
        title_verbatim=True,
        body="\n".join(parts),
        description=f"{kind_title(kind)}: {name_of(e)}",
    ).documents(kind, eid)


def list_table(ctx: Context, kind: str, entities: Iterable[Entity], anchors: bool = False) -> str:
    """A table of ``entities``: name (linked), Korean name and the :data:`LIST_FIELDS` they
    have. With ``anchors`` each row gets the id :func:`row_anchor` gives."""
    entities = list(entities)
    fields = [f for f in LIST_FIELDS if any(f in e for e in entities)]
    cols = [t.Column("Name"), t.Column("Korean")] + [
        t.Column(label(f), numeric=all(isinstance(e.get(f, 0), (int, float)) for e in entities))
        for f in fields
    ]
    rows = []
    for e in entities:
        eid = str(e["id"])
        name = ctx.name(kind, eid) if anchors else ctx.ref(kind, eid)
        cells = [
            t.markup_cell(name, name_of(e)),
            t.Cell(t.verbatim(name_of(e, "ko"), "ko"), name_of(e, "ko"), "ko"),
        ]
        for f in fields:
            v = e.get(f)
            if isinstance(v, (int, float)) and not isinstance(v, bool):
                cells.append(t.num_cell(v))
            elif isinstance(v, Mapping) and "kind" in v and "id" in v:
                cells.append(t.markup_cell(link_or_text(ctx, str(v["kind"]), str(v["id"]))))
            else:
                cells.append(t.markup_cell(value_markup(ctx, v) if v is not None else ""))
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
    for kind in section.kinds:
        entities = ctx.kb.entities(kind)
        if not entities:
            continue
        anchored = kind in ANCHOR_KINDS
        parts.append(
            t.section(kind_title(kind), list_table(ctx, kind, entities, anchors=anchored), kind)
        )
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
