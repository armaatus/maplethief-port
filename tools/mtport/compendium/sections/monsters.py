"""The Monsters section of the compendium (#218): a page per monster and boss, and lists.

Pages:

- ``monster/<id>.html``: one per ``monster`` entity. Our paragraph as the lead
  (``docs/compendium/pages/monster/<id>.md``, else a lead made from the data), the infobox,
  then the style guide's sections:
  Drops, Locations, Quests, Behaviour, Notes, Images. A boss page adds "How to reach the
  fight", "The fight" and "Rewards" before them.
- ``monsters/index.html``: every monster, sortable by level, HP, EXP, EXP per HP and meso;
- ``monsters/by-area.html``: the monsters of each area;
- ``monsters/bosses.html``: the bosses, their maps and quests.

The numbers come from the knowledge base (``mob.dat`` through #213's stages) and the rules
from ``docs/re/mechanics.md``: drops with the 15-bit odds, contact damage, avoid and evade,
and the Monster Carnival scaling with its sign slip. Boss fights are ``event`` entities of
the World section; this section links to them and does not document them.
"""

from __future__ import annotations

from collections.abc import Iterable, Iterator, Mapping
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of

KIND = "monster"
MECHANICS = "https://github.com/armaatus/maplethief-port/blob/main/docs/re/mechanics.md"
QOL = "https://github.com/armaatus/maplethief-port/blob/main/docs/re/qol.md"

# The scenes of each boss (docs/re/qol.md, "Which steps change state"): the scene number,
# its builder, its steps and speech boxes. The entrance is picked by the area when a map
# script calls startBoss; the closing scene is matched to the boss by the builder's name.
SCENES: dict[str, dict[str, tuple[int, str, int, int]]] = {
    "64": {"in": (3, "makeAniMelodyIn", 31, 3), "end": (4, "makeAniMelodyEnd", 84, 2)},
    "66": {"in": (5, "makeAniWolfIn", 61, 3), "end": (6, "makeAniWolfEnd", 41, 2)},
    "67": {"in": (9, "makeAniHomunIn", 61, 4), "end": (10, "makeAniHomunEnd", 56, 5)},
    "68": {"in": (7, "makeAniYetiIn", 101, 3), "end": (8, "makeAniYetiEnd", 49, 3)},
    "69": {"in": (14, "makeAniBalogIn", 79, 6)},
}
# Example heroes for the Monster Carnival table: one level inside each mode's level range
# (the Spiegelman scripts' conditions: 10 to 30, 30 to 50, above 50).
CARNIVAL_HEROES = (20, 40, 60)
HERO_MAX_LEVEL = 99
MALADY = "64"  # the boss whose name the bomb carries
MOVEMENT_TEXT = {
    1: "walks along its platform and turns at the edges",
    2: "floats and never lands",
    3: "explodes when it touches the hero",
}


# --- facts derived from the knowledge base ------------------------------------------------


def is_boss(e: Entity) -> bool:
    """A boss: a mob with the HUD's boss bar that is not a bomb (ids 64 and 66 to 69 in 1.5;
    their deaths start a scene instead of paying EXP and drops)."""
    return bool(e.get("boss_bar")) and e.get("class") != "bomb"


def is_summon(e: Entity) -> bool:
    return e.get("class") == "bomb"


def _refs(v: Any) -> list[Mapping[str, Any]]:
    if isinstance(v, Mapping) and "id" in v:
        return [v]
    if isinstance(v, list):
        return [r for r in v if isinstance(r, Mapping) and "id" in r and "kind" in r]
    return []


def _int(v: Any) -> int | None:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


def area_of(ctx: Context, map_id: str) -> str | None:
    m = ctx.kb.get("map", map_id)
    if m is None:
        return None
    for r in _refs(m.get("area")):
        return str(r["id"])
    return None


def spawns(e: Entity) -> list[tuple[str, int]]:
    """``(map id, spawn points)`` of the monster, in the knowledge base's order."""
    out = []
    for r in _refs(e.get("spawns_on")):
        if r.get("kind") == "map":
            out.append((str(r["id"]), int(r.get("count") or 0)))
    return out


def areas(ctx: Context, e: Entity) -> list[str]:
    out: list[str] = []
    for mid, _ in spawns(e):
        a = area_of(ctx, mid)
        if a is not None and a not in out:
            out.append(a)
    return out


def meso(e: Entity) -> Mapping[str, Any] | None:
    m = e.get("meso")
    return m if isinstance(m, Mapping) and m.get("max") else None


def drops(e: Entity) -> list[Mapping[str, Any]]:
    """The monster's item drops (equipment slot first), as the knowledge base stores them."""
    rows = [r for r in _refs(e.get("drops")) if "chance" in r]
    return sorted(rows, key=lambda r: (int(r.get("slot") or 0), str(r["id"])))


def level_row(ctx: Context, level: int) -> Entity | None:
    return ctx.kb.get("level", str(max(1, min(level, HERO_MAX_LEVEL))))


RAND_SPAN = 32768  # Rand_Next gives 0 to 32767


def real_odds(n: int, d: int) -> float:
    """``Rand_Chance(n, d)`` in percent, exactly: the share of the 15-bit ``Rand_Next``
    values whose remainder mod ``d``, plus 1, is at most ``n``. The knowledge base's
    ``percent`` is the same figure rounded to four decimals, too coarse for the smallest
    equipment chances (10 in 100,000 is 0.0305%)."""
    if n <= 0 or d <= 0:
        return 0.0
    full, rem = divmod(RAND_SPAN, d)
    return (full * min(n, d) + min(n, rem)) * 100 / RAND_SPAN


def cdiv(a: int, b: int) -> int:
    """C's integer division (truncates toward zero), as the game's code divides."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def contact_damage(level: int, defense: int) -> tuple[int, int]:
    """The lowest and highest contact damage of a mob of ``level`` against a hero of
    ``defense`` (``CMob::RollDamage``, docs/re/mechanics.md): never below 0."""
    dmg = level * level // 30 + 2 * level + 20 - defense * 7 // 3
    if dmg < 0:
        lo, hi = 1, max(1, level // 2)
    else:
        lo, hi = dmg + (-2 - dmg // 4), dmg + dmg // 4 + 2
    return max(0, cdiv(lo * 90, 100)), max(0, cdiv(hi * 90, 100))


def clamp_percent(v: int) -> int:
    return max(0, min(100, v))


def carnival(level: int, hp: int, h: int) -> tuple[int, float]:
    """The mob's level and HP in Monster Carnival against a hero whose (attack + defense)
    halved is ``h`` (``CMob::LoadStats``): above ``h`` the level drops halfway toward it and
    the HP is 150 %; below it the shifted difference is added, so the level falls further
    (the sign slip), HP unchanged. The shifts are arithmetic (they round down)."""
    if level > h:
        return level - ((level - h) >> 1), hp * 1.5
    if level < h:
        return level + ((level - h) >> 1), float(hp)
    return level, float(hp)


def hero_h(row: Entity) -> int:
    return (int(row.get("attack") or 0) + int(row.get("defense") or 0)) >> 1


def carnival_used(ctx: Context) -> bool:
    return any(ev.get("type") == "carnival" and ev.get("used") for ev in ctx.kb.entities("event"))


def boss_events(ctx: Context, e: Entity) -> list[Entity]:
    """The ``startBoss`` events of the maps the boss spawns on, else those whose quest is the
    boss's (an unused script's event has no map)."""
    maps = {mid for mid, _ in spawns(e)}
    quests = {(str(r["id"])) for r in _refs(e.get("hunted_by_quests"))}
    out = []
    for ev in ctx.kb.entities("event"):
        if ev.get("type") != "boss":
            continue
        ev_maps = {str(r["id"]) for r in _refs(ev.get("maps"))}
        if ev_maps & maps or (not ev_maps and _event_quest(ev) in quests):
            out.append(ev)
    return out


def _event_quest(ev: Entity) -> str | None:
    """The quest whose requirements the event's condition checks (``checkRequire(kind,
    number)``), as a quest id ``<chapter>-<main|sub>-<number>``."""
    chapters = ev.get("chapters") or []
    if not chapters:  # an unused script: its directory names the chapter
        script = str(ev.get("id", ""))
        if script.startswith("chapter") and script[7:8].isdigit():
            chapters = [int(script[7])]
    for alt in ev.get("conditions") or []:
        for c in alt:
            if c.get("native") == "checkRequire" and len(c.get("args") or []) == 2:
                k, n = c["args"]
                kind = "main" if k == 0 else "sub"
                for ch in chapters:
                    return f"{ch}-{kind}-{n}"
    return None


def boss_maps(ctx: Context, e: Entity) -> list[str]:
    return [mid for mid, _ in spawns(e) if (ctx.kb.get("map", mid) or {}).get("boss_map")]


def recommended_level(ctx: Context, e: Entity) -> int | None:
    """Our estimate for a boss: the level of the strongest monster on the maps that lead to
    its boss map."""
    best = None
    for mid in boss_maps(ctx, e):
        m = ctx.kb.get("map", mid) or {}
        for r in _refs(m.get("entrances")):
            other = ctx.kb.get("map", str(r["id"])) or {}
            for s in _refs(other.get("spawns")):
                mob = ctx.kb.get("monster", str(s["id"]))
                lv = _int((mob or {}).get("level"))
                if mob is not None and not is_boss(mob) and lv is not None:
                    best = lv if best is None else max(best, lv)
    return best


# --- markup helpers -------------------------------------------------------------------------


def _ref(ctx: Context, kind: str, eid: str, label: str | None = None) -> str:
    return d.link_or_text(ctx, kind, eid) if label is None else ctx.ref(kind, eid, label)


def where(ctx: Context, map_id: str) -> str:
    """A map link with its area: ``Map (Area)``."""
    a = area_of(ctx, map_id)
    out = _ref(ctx, "map", map_id)
    if a is not None and ctx.kb.get("area", a) is not None:
        out += f" ({ctx.ref('area', a)})"
    return out


def npc_where(ctx: Context, ref: Mapping[str, Any]) -> str:
    """An NPC link and where it stands for this relation (its ``map`` attribute)."""
    out = _ref(ctx, "npc", str(ref["id"]))
    mid = ref.get("map")
    if isinstance(mid, str) and ctx.kb.get("map", mid) is not None:
        out += f", {where(ctx, mid)}"
    return out


def _and(parts: list[str]) -> str:
    if len(parts) <= 2:
        return " and ".join(parts)
    return ", ".join(parts[:-1]) + ", and " + parts[-1]


def _p(markup: str) -> str:
    return f"<p>{markup}</p>\n"


def _num(v: int | float) -> str:
    return t.esc(t.number(v))


def _plural(n: int, word: str, plural: str | None = None) -> str:
    return f"{t.number(n)} {word if n == 1 else (plural or word + 's')}"


def _meso_text(m: Mapping[str, Any]) -> str:
    lo, hi = int(m.get("min", 0)), int(m["max"])
    return f"{t.number(lo)}-{t.number(hi)} meso"


# --- the monster page ------------------------------------------------------------------------


def lead(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    name = ctx.name(KIND, eid)
    lv = _int(e.get("level")) or 0
    if is_summon(e):
        return _p(
            f"{name} is a level {lv} monster that explodes on contact. No map's spawn"
            " list places it."
        )
    ars = [ctx.ref("area", a) for a in areas(ctx, e) if ctx.kb.get("area", a) is not None]
    if is_boss(e):
        bm = boss_maps(ctx, e)
        if bm:
            return _p(f"{name} is a level {lv} boss, fought on {where(ctx, bm[0])}.")
        return _p(f"{name} is a level {lv} boss.")
    sp = spawns(e)
    if not sp:
        return _p(f"{name} is a level {lv} monster. No map's spawn list places it.")
    in_areas = f" in {_and(ars)}" if ars else ""
    return _p(f"{name} is a level {lv} monster found on {_plural(len(sp), 'map')}{in_areas}.")


def infobox(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    ko = name_of(e, "ko")
    sub = t.verbatim(ko, "ko") if ko != name_of(e, "en") else ""
    lv = _int(e.get("level"))
    exp = _int(e.get("exp"))
    m = meso(e)
    boss = is_boss(e)
    if boss:
        typ = "boss"
    elif is_summon(e):
        typ = "summon (explodes on contact)"
    else:
        typ = "monster"
    exp_text = ""
    if exp is not None:
        exp_text = _num(exp) + (" (not given: see Rewards)" if boss and exp else "")
    meso_text = ""
    if m:
        meso_text = t.esc(_meso_text(m) + (" (not dropped)" if boss else f" ({m['percent']}%)"))
    rows = [
        ("Type", t.esc(typ)),
        ("Level", t.esc(f"Lv. {lv}") if lv else ""),
        ("HP", _num(e["hp"]) if _int(e.get("hp")) is not None else ""),
        ("EXP", exp_text),
        ("Meso", meso_text),
        ("Movement", t.esc(MOVEMENT_TEXT.get(e.get("movement_kind"), ""))),
        ("Found on", d._links(ctx, e.get("spawns_on"), "locations")),
    ]
    if boss:
        rec = recommended_level(ctx, e)
        if rec:
            rows.append(("Recommended level", t.esc(f"Lv. {rec} or above (estimate)")))
    rows.append(("ID", f"<code>{t.esc(eid)}</code>"))
    return t.infobox(ctx.name(KIND, eid), rows, ctx.sprite(KIND, eid), subtitle_markup=sub)


def drop_list(ctx: Context, e: Entity) -> list[t.Drop]:
    """The drop table's rows: each item with the real odds (before the hero's level bonus)
    and the table's own value, then the meso the amount can turn into."""
    refs = [(str(r["kind"]), str(r["id"])) for r in drops(e)]
    refs += [(str(r["kind"]), str(r["id"])) for r in _refs(e.get("meso_drops"))]
    boxes = d.any_thumb(ctx, refs)
    out = []
    for r in drops(e):
        cell = d.entry_cell(ctx, str(r["kind"]), str(r["id"]), placeholder=boxes).markup
        per = int(r.get("per") or 1000)
        note = "equipment, fixed stats" if per != 1000 else ""
        pct = real_odds(int(r["chance"]), per)
        out.append(t.Drop(cell, int(r["chance"]), note, per=per, percent=pct))
    m = meso(e)
    for r in _refs(e.get("meso_drops")):
        cell = d.entry_cell(ctx, str(r["kind"]), str(r["id"]), placeholder=boxes).markup
        lo, hi = int(r.get("min", 0)), int(r.get("max", 0))
        note = f"{t.number(lo)}-{t.number(hi)} meso" if hi else (_meso_text(m) if m else "")
        pct = float(r.get("percent") or 0)
        out.append(t.Drop(cell, round(pct * 10), note, per=1000, percent=pct, face=False))
    return out


def drops_section(ctx: Context, e: Entity) -> str:
    rows = drop_list(ctx, e)
    if not rows:
        return t.section("Drops", _p("It drops nothing: its table row has no items and no meso."))
    parts = []
    if is_boss(e):
        parts.append(
            _p(
                "The monster table gives it these drops, but a boss's death starts its closing"
                " scene instead of the usual EXP, kill count and drop rolls, so none of them"
                f' drop in play (<a href="{MECHANICS}#drops">mechanics</a>).'
            )
        )
    parts.append(t.drop_table(rows))
    notes = [
        "Each chance is per kill and rolled on its own, so one kill can drop several items."
        " The first figure is the real chance, the table's own value is in brackets."
    ]
    if any(int(r.get("per") or 1000) != 1000 for r in drops(e)):
        notes.append(
            "The game's random numbers have 15 bits, so an equipment chance out of 100,000"
            " is really out of 32,768, about three times higher, and the hero's level is"
            " added to its number. Dropped equipment always has the table's minimum stats."
        )
    if any(int(r.get("per") or 1000) == 1000 for r in drops(e)):
        notes.append(
            "A chance out of 1,000 is really about the same number out of 993, and a tenth"
            " of the hero's level is added to it."
        )
    m = meso(e)
    if m and not is_boss(e):
        notes.append(
            f"Meso: an amount from {t.number(int(m['min']))} to {t.number(int(m['max']))}"
            f" (80% to 120% of the table's {t.number(int(m.get('amount') or 0))}) drops on"
            f" {m['percent']}% of kills, as a bag of one of three sizes by the amount."
        )
    notes_html = " ".join(t.esc(n) for n in notes)
    parts.append(f'<p class="muted">{notes_html} (<a href="{MECHANICS}#drops">rules</a>)</p>\n')
    return t.section("Drops", "".join(parts))


def locations_section(ctx: Context, e: Entity) -> str:
    sp = spawns(e)
    if not sp:
        if is_summon(e):
            fight = (
                f"{ctx.ref(KIND, MALADY)}'s bomb" if ctx.kb.get(KIND, MALADY) else "a boss's bomb"
            )
            body = _p(
                "No map's spawn list places it. Its name and its movement kind make it"
                f" {fight}: it appears during that fight (probable)."
            )
        else:
            body = _p(
                "No map's spawn list places it, so it never appears in the fields. Monster"
                " Carnival rounds take their monsters from a separate spawn table in the"
                " program, which the knowledge base does not read yet; whether it appears"
                " there is unknown."
            )
        return t.section("Locations", body, "locations")
    boxes = d.any_thumb(ctx, (("map", mid) for mid, _ in sp))
    cols = [
        t.Column("Map"),
        t.Column("Area"),
        t.Column("Map number"),
        t.Column("Spawn points", "Places on the map where it appears", numeric=True),
    ]
    boss = is_boss(e)
    if boss:
        cols.append(t.Column("Note"))
    rows = []
    for mid, count in sp:
        a = area_of(ctx, mid)
        area_cell = t.markup_cell(_ref(ctx, "area", a), a) if a else t.Cell("", "")
        row = [
            d.entry_cell(ctx, "map", mid, placeholder=boxes),
            area_cell,
            t.text_cell(mid.replace("-", ".")),
            t.num_cell(count),
        ]
        if boss:
            m = ctx.kb.get("map", mid) or {}
            note = "boss map: the fight" if m.get("boss_map") else "not a boss map (see Notes)"
            row.append(t.text_cell(note))
        rows.append(row)
    total = sum(c for _, c in sp)
    intro = _p(
        f"It has {_plural(total, 'spawn point')} on {_plural(len(sp), 'map')}."
        if not boss
        else "The spawn lists place it on these maps."
    )
    return t.section("Locations", intro + t.table(cols, rows), "locations")


def _quest_rows(ctx: Context, e: Entity) -> list[list[t.Cell]]:
    rows = []
    for r in _refs(e.get("hunted_by_quests")):
        q = ctx.kb.get("quest", str(r["id"]))
        if q is not None:
            rows.append(_quest_row(ctx, q, f"defeat {t.number(int(r.get('count') or 1))}"))
    for dr in drops(e):
        item = ctx.kb.get("item", str(dr["id"])) or {}
        for r in _refs(item.get("needed_by_quests")):
            q = ctx.kb.get("quest", str(r["id"]))
            if q is not None:
                n = int(r.get("count") or 1)
                need = f"{t.esc(t.number(n))} × {_ref(ctx, 'item', str(dr['id']))}"
                rows.append(_quest_row(ctx, q, need, markup=True))
    return rows


def _quest_row(ctx: Context, q: Entity, need: str, markup: bool = False) -> list[t.Cell]:
    qid = str(q["id"])
    givers = [npc_where(ctx, g) for g in _refs(q.get("given_by"))]
    ch = _int(q.get("chapter"))
    kind = str(q.get("quest_kind") or "")
    return [
        t.markup_cell(ctx.ref("quest", qid), name_of(q)),
        t.num_cell(ch) if ch is not None else t.Cell("", -1),
        t.text_cell(kind),
        t.markup_cell(need) if markup else t.text_cell(need),
        t.markup_cell("; ".join(givers) if givers else '<span class="muted">unknown</span>'),
    ]


def _crafting_rows(ctx: Context, e: Entity) -> list[list[t.Cell]]:
    rows = []
    for dr in drops(e):
        iid = str(dr["id"])
        item = ctx.kb.get("item", iid) or {}
        for field, kind, who in (
            ("material_of", "recipe", "crafted_by"),
            ("traded_away_in", "trade", "offered_by"),
        ):
            for r in _refs(item.get(field)):
                ent = ctx.kb.get(kind, str(r["id"]))
                if ent is None:
                    continue
                n = int(r.get("count") or 1)
                at = [npc_where(ctx, g) for g in _refs(ent.get(who))]
                rows.append(
                    [
                        t.markup_cell(ctx.ref(kind, str(r["id"])), name_of(ent)),
                        t.text_cell("recipe" if kind == "recipe" else "trade"),
                        t.markup_cell(f"{t.esc(t.number(n))} × {_ref(ctx, 'item', iid)}"),
                        t.markup_cell("; ".join(at) if at else ""),
                    ]
                )
    return rows


def quests_section(ctx: Context, e: Entity) -> str:
    q = _quest_rows(ctx, e)
    c = _crafting_rows(ctx, e)
    if not q and not c:
        return ""
    parts = []
    if q:
        cols = [
            t.Column("Quest"),
            t.Column("Chapter", numeric=True),
            t.Column("Kind"),
            t.Column("Needs"),
            t.Column("Given by"),
        ]
        parts.append(t.table(cols, q))
    if c:
        cols = [t.Column("Result"), t.Column("Kind"), t.Column("Uses"), t.Column("Where")]
        title = '<h3 id="recipes-and-trades">Recipes and trades</h3>\n' + _p(
            "Its drops are also materials of these recipes and trades."
        )
        parts.append(title + t.table(cols, c))
    return t.section("Quests", "".join(parts), "quests")


def behaviour_section(ctx: Context, e: Entity) -> str:
    lv = _int(e.get("level")) or 0
    parts = []
    mv = MOVEMENT_TEXT.get(e.get("movement_kind"))
    speed = _int(e.get("walk_speed"))
    sentences = []
    if mv:
        sentences.append(f"It {mv}.")
    if speed is not None and e.get("movement_kind") != 3:
        sentences.append(
            f"Its walking speed is {t.number(speed)} per tick, against the hero's 5."
            if speed
            else "Its walking speed is 0: it does not walk."
        )
    if is_summon(e):
        sentences.append(
            "On touching the hero it dies and deals its damage roll; the hero's avoid does"
            " not apply."
        )
    else:
        sentences.append(
            "Touching it hurts the hero unless the hero avoids the blow; while Dark Sight is"
            " on, contact does nothing. The monster table has no attack column: the damage"
            " comes from its level and the hero's defense alone."
        )
    if is_boss(e):
        sentences.append(
            "Its skill attacks deal twice the contact roll, cannot be avoided, and each hits"
            " at most once."
        )
    parts.append(_p(t.esc(" ".join(sentences))))
    hero = level_row(ctx, lv)
    lo0, hi0 = contact_damage(lv, 0)
    cols = [t.Column("Against"), t.Column("Contact damage"), t.Column("Avoid", numeric=True)]
    cols.append(t.Column("Evade", "Chance that it evades one of the hero's hits", numeric=True))
    rows = [
        [
            t.text_cell("a hero with 0 defense"),
            t.text_cell(f"{lo0}-{hi0}"),
            t.Cell("", -1),
            t.Cell("", -1),
        ]
    ]
    if hero is not None:
        hl = int(hero.get("level") or lv)
        df, av, ac = (int(hero.get(k) or 0) for k in ("defense", "avoidability", "accuracy"))
        lo, hi = contact_damage(lv, df)
        avoid = 0 if is_summon(e) else clamp_percent((av - lv) * 4)
        evade = clamp_percent(lv - ac)
        rows.append(
            [
                t.text_cell(f"a level {hl} hero, base stats (DEF {df}, AVOID {av}, ACC {ac})"),
                t.text_cell(f"{lo}-{hi}"),
                t.Cell(t.esc(f"{avoid}%"), avoid),
                t.Cell(t.esc(f"{evade}%"), evade),
            ]
        )
    parts.append(t.table(cols, rows, filter=False))
    parts.append(
        '<p class="muted">Computed with the rules in <a href="'
        + MECHANICS
        + '#monster-damage-and-the-heros-avoid">the mechanics notes</a>; equipment and'
        " skills change the hero's numbers. Avoid is the chance the hero avoids its contact,"
        " evade the chance it dodges a hit.</p>\n"
    )
    return t.section("Behaviour", "".join(parts), "behaviour")


def carnival_table(ctx: Context, e: Entity) -> str:
    lv, hp = _int(e.get("level")), _int(e.get("hp"))
    if lv is None or hp is None:
        return ""
    cols = [
        t.Column("Hero", "Base stats of the level, no equipment or skills"),
        t.Column("Half of ATT plus DEF", numeric=True),
        t.Column("Its level", numeric=True),
        t.Column("Its HP", numeric=True),
    ]
    rows = []
    for hl in CARNIVAL_HEROES:
        row = ctx.kb.get("level", str(hl))
        if row is None:
            continue
        h = hero_h(row)
        new_lv, new_hp = carnival(lv, hp, h)
        rows.append(
            [
                t.text_cell(f"level {hl}"),
                t.num_cell(h),
                t.Cell(t.esc(str(new_lv)), new_lv),
                t.num_cell(new_hp),
            ]
        )
    return t.table(cols, rows, filter=False) if rows else ""


def notes_section(ctx: Context, e: Entity) -> str:
    parts = []
    lv = _int(e.get("level")) or 0
    reach = (
        ""
        if carnival_used(ctx)
        else " In version 1.5 no map object runs the scripts that start the mode, so it"
        " cannot be reached in play."
    )
    parts.append(
        _p(
            t.esc(
                "In Monster Carnival its level follows the hero: above half the hero's ATT plus"
                " DEF it is pulled halfway down with 150% HP; below it the game adds the halved"
                " difference, which is negative, so the level falls further and the HP stays"
                " (a sign slip in the original). A kill there gives no EXP and no drops." + reach
            )
            + f' (<a href="{ctx.url("monsters/index.html#level-scaling")}">level scaling</a>)'
        )
    )
    table = carnival_table(ctx, e)
    if table:
        parts.append(table)
        if any(carnival(lv, 1, hero_h(r))[0] < 1 for r in _carnival_rows(ctx)):
            parts.append(
                '<p class="muted">A level below 1 is what the rule gives; whether the game'
                " clamps it is not documented.</p>\n"
            )
    trivia = []
    if e.get("name_fills_field"):
        trivia.append(
            "Its English name fills all 16 bytes of the table's name field, so the game"
            " shows it cut short."
        )
    others = [o for o in ctx.kb.entities(KIND) if name_of(o) == name_of(e) and o["id"] != e["id"]]
    for o in others:
        label = f"the level {o.get('level')} one"
        other = ctx.ref(KIND, str(o["id"]), label)
        trivia.append(f"The monster table has another monster of this name: {other}.")
    if trivia:
        parts.append('<h3 id="trivia">Trivia</h3>\n' + "".join(_p(x) for x in trivia))
    return t.section("Notes", "".join(parts), "notes")


def _carnival_rows(ctx: Context) -> list[Entity]:
    return [r for hl in CARNIVAL_HEROES if (r := ctx.kb.get("level", str(hl))) is not None]


def images_section(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    stills = ctx.entity_images(KIND, eid, ("still",))
    if len(stills) < 2:
        return ""
    figs = []
    for i in stills:
        pic = ctx.sprite(KIND, eid, box=96, image=i, alt=f"{name_of(e)}, {i.action}")
        figs.append((pic, t.esc(i.action.replace("action", "action ") or "still")))
    note = _p("Each action of its sprite; an animated one plays unless reduced motion is on.")
    return t.section("Images", note + t.gallery(figs, "Sprite actions"), "images")


# --- boss sections ---------------------------------------------------------------------------


def _scene(s: tuple[int, str, int, int]) -> str:
    n, builder, steps, boxes = s
    return (
        f"scene {n} (<code>CAniManager::{t.esc(builder)}</code>, {t.number(steps)} steps,"
        f" {_plural(boxes, 'speech box', 'speech boxes')})"
    )


def reach_section(ctx: Context, e: Entity) -> str:
    parts = []
    bm = boss_maps(ctx, e)
    for mid in bm:
        m = ctx.kb.get("map", mid) or {}
        ins = [str(r["id"]) for r in _refs(m.get("entrances"))]
        ins = list(dict.fromkeys(ins))
        way = f" Its only way in is from {_and([where(ctx, x) for x in ins])}." if ins else ""
        parts.append(_p(f"The fight is on {where(ctx, mid)}, a boss map.{way}"))
    for r in _refs(e.get("hunted_by_quests")):
        q = ctx.kb.get("quest", str(r["id"]))
        if q is None:
            continue
        givers = [npc_where(ctx, g) for g in _refs(q.get("given_by"))]
        by = f", given by {_and(givers)}" if givers else ""
        ch = q.get("chapter")
        parts.append(
            _p(
                f"The quest {ctx.ref('quest', str(q['id']))} (chapter {t.esc(str(ch))}{by}) asks"
                " for the kill."
            )
        )
    evs = boss_events(ctx, e)
    for ev in evs:
        link = ctx.ref("event", str(ev["id"]), "its boss event")
        if ev.get("used"):
            parts.append(
                _p(
                    f"Entering the map runs the map's script ({link}). While the quest's"
                    " requirement is not yet met, the script starts the boss's entrance scene."
                )
            )
        else:
            parts.append(
                _p(
                    f"The script that would start the fight ({link}) is never named by a map"
                    " object in version 1.5, so no script starts the entrance scene."
                )
            )
    if not evs:
        parts.append(_p("The knowledge base has no boss event for it."))
    return t.section("How to reach the fight", "".join(parts), "how-to-reach-the-fight")


def fight_section(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    sc = SCENES.get(eid, {})
    parts = []
    hp = _int(e.get("hp")) or 0
    lv = _int(e.get("level")) or 0
    ev_used = any(ev.get("used") for ev in boss_events(ctx, e))
    if "in" in sc:
        if ev_used:
            parts.append(
                _p(
                    f"The fight opens with {_scene(sc['in'])}. When it ends, the game sets the"
                    " bosses on the map to attack and hands control back to the player."
                )
            )
        else:
            parts.append(
                _p(
                    f"Its entrance would be {_scene(sc['in'])}, which ends by setting the bosses"
                    " to attack. With no script to start it, how the fight plays out on the map"
                    " has not been confirmed in play."
                )
            )
    lo, hi = contact_damage(lv, 0)
    parts.append(
        _p(
            t.esc(
                f"It has {t.number(hp)} HP and level {lv}. Its contact deals up to {hi} damage"
                " to a hero with no defense; its skill attacks deal twice the roll and cannot"
                f" be avoided. A hero needs an accuracy of at least its level, {lv}, so that it"
                " never evades a hit."
            )
        )
    )
    bombs = [o for o in ctx.kb.entities(KIND) if is_summon(o)]
    if eid == MALADY and bombs:
        bomb = bombs[0]
        parts.append(
            _p(
                f"{ctx.ref(KIND, str(bomb['id']))} is the monster that explodes on contact; its"
                " name ties it to this fight (probable)."
            )
        )
    if "end" in sc:
        starts = "starts" if ev_used else "would start"
        parts.append(
            _p(
                f"Its defeat {starts} {_scene(sc['end'])} instead of paying EXP and drops. The"
                " closing scene runs the NPCs' state scripts, which move the quests on. The scene"
                " is matched to the boss by its builder's name."
            )
        )
    elif eid in SCENES:
        parts.append(
            '<details class="spoiler"><summary>After the fight</summary>\n'
            + _p(
                "No closing scene of its own exists: its defeat leads into the game's ending"
                " scenes, which return to the main menu (probable, from the scene list)."
            )
            + "</details>\n"
        )
    parts.append(
        _p(
            "Its attack patterns, timings and any phases are not documented yet;"
            f' the scene runner is described in <a href="{QOL}#cutscene-skip">the scene notes</a>.'
        )
    )
    return t.section("The fight", "".join(parts), "the-fight")


def rewards_section(ctx: Context, e: Entity) -> str:
    rows = []
    for r in _refs(e.get("hunted_by_quests")):
        q = ctx.kb.get("quest", str(r["id"]))
        if q is None:
            continue
        for rw in q.get("rewards") or []:
            typ = rw.get("type")
            if typ == "exp":
                rows.append([t.text_cell("EXP"), t.num_cell(int(rw["amount"]))])
            elif typ == "meso":
                rows.append([t.text_cell("meso"), t.num_cell(int(rw["amount"]))])
            elif typ == "item" and ctx.kb.get("item", str(rw.get("item"))) is not None:
                rows.append(
                    [
                        d.entry_cell(ctx, "item", str(rw["item"])),
                        t.num_cell(int(rw.get("count") or 1)),
                    ]
                )
    exp = _int(e.get("exp")) or 0
    m = meso(e)
    table_says = f"{t.number(exp)} EXP" + (f" and {_meso_text(m)}" if m else "")
    intro = _p(
        t.esc(
            f"The monster table gives it {table_says}, but a boss's death starts its closing"
            " scene instead, so neither is paid. The reward is the quest's."
        )
    )
    body = intro
    if rows:
        body += t.table([t.Column("Reward"), t.Column("Amount", numeric=True)], rows, compact=True)
    return t.section("Rewards", body, "rewards")


# --- pages -----------------------------------------------------------------------------------


def monster_page(ctx: Context, e: Entity) -> Page:
    eid = str(e["id"])
    # Our paragraph is the lead when there is one; else a lead made from the data.
    prose = ctx.prose(f"monster/{eid}.md")
    parts = [infobox(ctx, e), prose or lead(ctx, e)]
    if is_boss(e):
        parts += [reach_section(ctx, e), fight_section(ctx, e), rewards_section(ctx, e)]
    parts += [
        drops_section(ctx, e),
        locations_section(ctx, e),
        quests_section(ctx, e),
        behaviour_section(ctx, e),
        notes_section(ctx, e),
        images_section(ctx, e),
    ]
    lv = _int(e.get("level"))
    what = "boss" if is_boss(e) else "monster"
    return Page(
        path=d.entity_path(KIND, eid),
        title=name_of(e),
        heading=ctx.name(KIND, eid),
        title_verbatim=True,
        body="\n".join(p for p in parts if p),
        crumbs=[("Bosses", "monsters/bosses.html")] if is_boss(e) else [],
        description=f"Level {lv} {what}: {name_of(e)}" if lv else name_of(e),
    ).documents(KIND, eid)


def exp_per_hp(e: Entity) -> float:
    hp, exp = _int(e.get("hp")) or 0, _int(e.get("exp")) or 0
    return exp / hp if hp else 0.0


def _type_word(e: Entity) -> str:
    return "boss" if is_boss(e) else "summon" if is_summon(e) else "monster"


def list_table(ctx: Context, mobs: Iterable[Entity], maps: bool = True) -> str:
    mobs = list(mobs)
    boxes = d.any_thumb(ctx, ((KIND, m["id"]) for m in mobs))
    cols = [
        t.Column("Name"),
        t.Column("Korean"),
        t.Column("Level", numeric=True),
        t.Column("HP", numeric=True),
        t.Column("EXP", numeric=True),
        t.Column("EXP per HP", "EXP divided by HP: how much a point of damage earns", True),
        t.Column("Meso", "The range of one meso drop", numeric=True),
        t.Column("Type"),
    ]
    if maps:
        cols.append(t.Column("Maps", "Maps whose spawn list places it", numeric=True))
    rows = []
    for m in mobs:
        mid = str(m["id"])
        me = meso(m)
        r = [
            d.entry_cell(ctx, KIND, mid, placeholder=boxes),
            t.Cell(t.verbatim(name_of(m, "ko"), "ko"), name_of(m, "ko"), "ko"),
            t.num_cell(_int(m.get("level")) or 0),
            t.num_cell(_int(m.get("hp")) or 0),
            t.num_cell(_int(m.get("exp")) or 0),
            t.Cell(t.esc(f"{exp_per_hp(m):.3f}"), round(exp_per_hp(m), 6)),
            t.Cell(t.esc(_meso_text(me).replace(" meso", "")), int(me["max"]))
            if me
            else t.Cell("", -1),
            t.text_cell(_type_word(m)),
        ]
        if maps:
            r.append(t.num_cell(len(spawns(m))))
        rows.append(r)
    return t.table(cols, rows)


def _nav(ctx: Context, here: str) -> str:
    links = [
        ("monsters/index.html", "All monsters"),
        ("monsters/by-area.html", "By area"),
        ("monsters/bosses.html", "Bosses"),
    ]
    items = []
    for path, label in links:
        if path == here:
            items.append(f'<li><strong aria-current="page">{t.esc(label)}</strong></li>')
        else:
            items.append(f'<li><a href="{ctx.url(path)}">{t.esc(label)}</a></li>')
    return (
        '<nav aria-label="Monster lists"><ul class="see-also">' + "".join(items) + "</ul></nav>\n"
    )


def index_page(ctx: Context) -> Page:
    mobs = ctx.kb.entities(KIND)
    parts = [_nav(ctx, "monsters/index.html")]
    prose = ctx.prose("monsters/index.md")
    if prose:
        parts.append(prose)
    bosses = _plural(sum(1 for m in mobs if is_boss(m)), "boss", "bosses")
    bombs = _plural(sum(1 for m in mobs if is_summon(m)), "summoned bomb")
    parts.append(
        _p(
            t.esc(
                f"The monster table has {_plural(len(mobs), 'row')}: {bosses}, {bombs}, and the"
                " field monsters. Click a column's header to sort by it."
            )
        )
    )
    parts.append(
        f'<section id="monster" aria-label="All monsters">\n{list_table(ctx, mobs)}</section>\n'
    )
    prose = ctx.prose("monsters/level-scaling.md") or ""
    reach = (
        ""
        if carnival_used(ctx)
        else _p(
            "In version 1.5 no map object runs the scripts that start the mode (the"
            " Spiegelman scripts are never named by a map), so it cannot be reached in play."
        )
    )
    parts.append(t.section("Level scaling", prose + reach, "level-scaling"))
    return Page("monsters/index.html", "Monsters", "\n".join(parts), search=True)


def by_area_page(ctx: Context) -> Page:
    parts = [_nav(ctx, "monsters/by-area.html")]
    parts.append(
        _p(
            "The monsters each area's maps place, from the maps' spawn lists. A monster that lives"
            " in several areas is listed under each."
        )
    )
    for a in ctx.kb.entities("area"):
        aid = str(a["id"])
        mobs = [m for m in ctx.kb.entities(KIND) if aid in areas(ctx, m)]
        if not mobs:
            continue
        mobs.sort(key=lambda m: (_int(m.get("level")) or 0, str(m["id"])))
        anchor = t.anchor_id(f"area-{aid}")
        head = (
            f'<h2 id="{anchor}">{ctx.ref("area", aid)}'
            f' <span class="muted">(area {t.esc(aid)})</span></h2>\n'
        )
        maps = [str(r["id"]) for r in _refs(a.get("maps"))]
        maps_here = [mid for mid in maps if any(mid == s for m in mobs for s, _ in spawns(m))]
        intro = _p(
            f"{_plural(len(mobs), 'monster')} on {_plural(len(maps_here), 'map')} of the area."
        )
        parts.append(
            f'<section aria-labelledby="{anchor}">\n{head}{intro}{list_table(ctx, mobs, False)}'
            "</section>\n"
        )
    nowhere = [m for m in ctx.kb.entities(KIND) if not spawns(m)]
    if nowhere:
        parts.append(
            t.section(
                "On no map",
                _p("No map's spawn list places these.") + list_table(ctx, nowhere, False),
                "nowhere",
            )
        )
    return Page(
        "monsters/by-area.html",
        "Monsters by area",
        "\n".join(parts),
        description="The monsters of each area",
    )


def bosses_page(ctx: Context) -> Page:
    parts = [_nav(ctx, "monsters/bosses.html")]
    prose = ctx.prose("monsters/bosses.md")
    if prose:
        parts.append(prose)
    bosses = [m for m in ctx.kb.entities(KIND) if is_boss(m)]
    cols = [
        t.Column("Boss"),
        t.Column("Level", numeric=True),
        t.Column("HP", numeric=True),
        t.Column("Boss map"),
        t.Column("Quest"),
        t.Column("Chapter", numeric=True),
        t.Column("Recommended level", "Our estimate: the strongest monster on the way in", True),
    ]
    boxes = d.any_thumb(ctx, ((KIND, m["id"]) for m in bosses))
    rows = []
    for m in bosses:
        bm = boss_maps(ctx, m)
        qs = [ctx.kb.get("quest", str(r["id"])) for r in _refs(m.get("hunted_by_quests"))]
        qs = [q for q in qs if q is not None]
        rec = recommended_level(ctx, m)
        rows.append(
            [
                d.entry_cell(ctx, KIND, str(m["id"]), placeholder=boxes),
                t.num_cell(_int(m.get("level")) or 0),
                t.num_cell(_int(m.get("hp")) or 0),
                t.markup_cell(", ".join(where(ctx, x) for x in bm)),
                t.markup_cell(", ".join(ctx.ref("quest", str(q["id"])) for q in qs)),
                t.num_cell(_int(qs[0].get("chapter")) or 0) if qs else t.Cell("", -1),
                t.num_cell(rec) if rec else t.Cell("", -1),
            ]
        )
    parts.append(t.table(cols, rows, filter=False))
    summons = [m for m in ctx.kb.entities(KIND) if is_summon(m)]
    if summons:
        links = _and([ctx.ref(KIND, str(m["id"])) for m in summons])
        parts.append(
            _p(
                f"{links} has the boss bar's id range too, but it is the monster that explodes on"
                " contact, not a fight of its own."
            )
        )
    return Page(
        "monsters/bosses.html",
        "Bosses",
        "\n".join(parts),
        description="The bosses, their maps and their quests",
    )


def pages(ctx: Context) -> Iterator[Page]:
    yield index_page(ctx)
    yield by_area_page(ctx)
    yield bosses_page(ctx)
    for e in ctx.kb.entities(KIND):
        yield monster_page(ctx, e)


SECTION = Section(
    "monsters",
    "Monsters",
    10,
    (KIND,),
    pages,
    blurb="Every monster and boss: where it lives, what it drops and why to hunt it.",
    image="monster:1",
)
