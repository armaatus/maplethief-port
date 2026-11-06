"""The Skills and jobs section of the compendium (#222): the thief's three paths, every
skill at every level, the job advancements, the level table and suggested builds.

Pages:

- ``skills/index.html``: the paths, every skill, every job advancement (one anchored row
  each, so the section documents the ``job_advancement`` kind);
- ``skills/rogue.html``, ``skills/assassin.html``, ``skills/bandit.html``: a job's skill
  tree by tier, its advancements (who, where, the conditions), its weapons by level;
- ``skill/<id>.html``: one page per skill, with a row per level;
- ``skills/growth.html``: the level table (one anchored row per ``level`` entity);
- ``skills/builds.html``: suggested skill-point orders, reasoned from the numbers.

Every rule comes from ``docs/re/mechanics.md`` (#214) and ``docs/formats/save.md`` (the
skill slots): the per-level values (``base + (level - 1) * per_level``), the damage roll's
use of one level more than the skill window shows, ``@etc`` in tenths, durations in ticks,
the skill points of a level-up. The hand-written prose is in
``docs/compendium/pages/skills/`` and ``docs/compendium/pages/skill/``; game text (names,
descriptions, the filled detail text) comes from the knowledge base.
"""

from __future__ import annotations

import re
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from typing import Any

from mtport.compendium import defaults as d
from mtport.compendium import templates as t
from mtport.compendium.siteapi import BuildError, Context, Entity, Page, Section, name_of

SECTION_ID = "skills"
TICKS_PER_SECOND = 20  # the default game speed (mechanics.md: 600 ticks is 30 s)
MAX_HERO_LEVEL = 99  # CHero::LevelUp caps the level at 99; hero.dat's row 100 is never reached


# The jobs: page slug, the KB branch name, the setClassUp job number, the weapon the branch's
# skills need (mechanics.md, CSkill::setActiveSkill 0x7EFF1).
@dataclass(frozen=True)
class Job:
    slug: str
    branch: str
    job: int
    weapon: str

    @property
    def path(self) -> str:
        return f"{SECTION_ID}/{self.slug}.html"


JOBS = (
    Job("rogue", "Rogue", 0, ""),
    Job("assassin", "Assassin", 1, "claw"),
    Job("bandit", "Bandit", 2, "dagger"),
)
JOB_BY_BRANCH = {j.branch: j for j in JOBS}

# The attack skills whose damage percentage p is added on top of the base, with the flat
# constant the damage roll adds (CHero::RollDamage 0x64DA1).
ATTACK_CONSTANTS = {"2": -5, "3": -3, "7": 2, "10": 4, "16": 2, "23": 3}
# Star skills (a claw and stars): stars used per use, then with Shadow Partner on.
STARS = {"1": (1, 2), "2": (2, 4), "7": (1, 1), "10": (3, 3)}

# What each skill's @etc is, in the table's column: (column title, prefix, unit, tip).
# Unit "s" means @etc counts ticks, shown in seconds.
ETC_COLUMNS: dict[str, tuple[str, str, str, str]] = {
    "4": ("ACC and AVOID", "+", "", "Added to accuracy and to avoidability"),
    "6": ("Critical rate", "+", "%", "Added to the critical rate"),
    "7": ("HP drained", "", "%", "HP restored, as a percentage of the damage dealt"),
    "8": ("Maximum HP", "+", "%", "Of the level table's HP sum"),
    "9": ("Maximum MP", "+", "%", "Of the level table's MP sum"),
    "10": ("Targets", "", "", "Monsters one star passes through"),
    "11": ("ATT", "+", "", "Added to attack"),
    "12": ("ACC", "+", "", "Added to accuracy while the buff lasts"),
    "13": ("Bind", "", "s", "How long each monster in the web cannot move"),
    "15": ("Critical rate", "+", "%", "Added to the critical rate"),
    "16": ("Hits", "", "", "Hits on the one target"),
    "17": ("Maximum HP", "+", "%", "Of the level table's HP sum"),
    "18": ("Maximum MP", "+", "%", "Of the level table's MP sum"),
    "19": ("Meso chance", "", "%", "Chance per hit that the monster drops meso"),
    "20": ("ATT", "+", "", "Added to attack"),
    "21": ("ACC", "+", "", "Added to accuracy while the buff lasts"),
    "22": ("Meso paid", "", "%", "Meso paid per blow, as a percentage of half the damage"),
    "23": ("Targets", "", "", "Monsters hit, twice each"),
}
# Skills whose damage column is an ATT bonus (Concentration: attack plus @dmg).
DAMAGE_AS_ATT = frozenset({"12", "21"})

# Notes per skill, in our own words, from mechanics.md.
NOTES: dict[str, list[str]] = {
    "0": [
        "With a dagger the swing picks one of two animations at random; both hit the same way.",
        "With a claw the basic attack is a punch, which always does 1 damage.",
    ],
    "1": [
        "The throw takes a tenth off the base damage before the roll.",
        "With a claw the hero punches instead of throwing when a monster is right next to"
        " the hero or when no stars are left; a punch always does 1 damage.",
    ],
    "14": [
        "While it lasts, every star skill throws twice the stars: Lucky Seven throws four"
        " and the basic throw two, and each extra star rolls its own damage."
    ],
    "15": [
        "The table gives this passive damage values (10, plus 1 a level), but it is not one"
        " of the skills the damage roll adjusts, so they change nothing."
    ],
    "19": [
        "The meso it drops is from a 18th of the monster's meso, minus 5, to a 18th, and"
        " then goes through the usual 80% meso roll."
    ],
    "22": ["When the hero cannot pay, the blow lands in full."],
}

PLACEHOLDER_RE = re.compile(r"@(dmg|time|etc|mp)")
STATES = {0: "not started", 1: "in progress", 2: "done"}
ORDINAL = {1: "first", 2: "second", 3: "third", 4: "fourth"}
WORDS = {0: "no", 1: "one", 2: "two", 3: "three", 4: "four"}


# --- numbers ---------------------------------------------------------------------------------


def skill_points(level: int) -> int:
    """Skill points the hero gets on reaching ``level`` (CHero::LevelUp 0x65089): none for
    levels 2 to 9, 1 for 10 to 29, 2 from 30."""
    if level < 10:
        return 0
    return 1 if level < 30 else 2


def skill_points_total(level: int) -> int:
    """Skill points gained from level 1 up to ``level``."""
    return sum(skill_points(n) for n in range(2, level + 1))


def etc_value(raw: int) -> int:
    """``@etc`` as the game uses it (CSkill::EtcValue 0x7D325): tenths, a positive sum
    divided by 10."""
    return raw // 10 if raw > 0 else raw


def seconds(ticks: int) -> str:
    return t.number(ticks / TICKS_PER_SECOND)


@dataclass(frozen=True)
class LevelRow:
    """One level of a skill, from ``skill.dat``'s base and per-level columns."""

    level: int
    damage: int  # @dmg, what the skill window shows: base + (level - 1) * per_level
    damage_hit: int  # what the damage roll uses: base + level * per_level
    time: int  # ticks
    etc_raw: int
    etc: int  # @etc: etc_raw in tenths
    mp: int
    detail: str  # the filled detail text (English), game text


def _ints(e: Entity, key: str) -> dict[str, int]:
    v = e.get(key)
    if not isinstance(v, Mapping):
        return {}
    return {k: int(x) for k, x in v.items() if isinstance(x, int) and not isinstance(x, bool)}


def level_rows(e: Entity) -> list[LevelRow]:
    """Every level of the skill, computed from its base and per-level values and checked
    against the knowledge base's own ``levels`` (a mismatch, or a detail text with a
    placeholder left, stops the build)."""
    base, per = _ints(e, "base"), _ints(e, "per_level")
    max_level = e.get("max_level")
    if not isinstance(max_level, int) or max_level <= 0 or not base:
        return []
    kb_levels = {lv.get("level"): lv for lv in e.get("levels") or [] if isinstance(lv, Mapping)}
    rows = []
    for n in range(1, max_level + 1):

        def val(k: str, n: int = n) -> int:
            return base.get(k, 0) + (n - 1) * per.get(k, 0)

        row = LevelRow(
            level=n,
            damage=val("damage"),
            damage_hit=base.get("damage", 0) + n * per.get("damage", 0),
            time=val("time"),
            etc_raw=val("etc"),
            etc=etc_value(val("etc")),
            mp=val("mp"),
            detail="",
        )
        kb = kb_levels.get(n)
        detail = ""
        if isinstance(kb, Mapping):
            for k, mine in (("damage", row.damage), ("time", row.time), ("mp", row.mp)):
                if k in kb and kb[k] != mine:
                    raise BuildError(f"skill {e['id']} level {n}: {k} {kb[k]} != {mine}")
            if "etc" in kb and kb["etc"] != row.etc_raw:
                raise BuildError(f"skill {e['id']} level {n}: etc {kb['etc']} != {row.etc_raw}")
            dt = kb.get("detail")
            detail = dt.get("en", "") if isinstance(dt, Mapping) else ""
            if PLACEHOLDER_RE.search(detail or ""):
                raise BuildError(f"skill {e['id']} level {n}: detail keeps a placeholder")
        rows.append(LevelRow(**{**row.__dict__, "detail": detail or ""}))
    return rows


def _int(v: Any) -> int | None:
    return v if isinstance(v, int) and not isinstance(v, bool) else None


# --- small markup helpers ------------------------------------------------------------------


def _job_of(e: Entity) -> Job | None:
    return JOB_BY_BRANCH.get(str(e.get("branch") or ""))


def _job_link(ctx: Context, job: Job) -> str:
    return f'<a href="{ctx.url(job.path)}">{t.esc(job.branch)}</a>'


def _skills(ctx: Context) -> list[Entity]:
    return ctx.kb.entities("skill")


def _sort_id(eid: str) -> tuple[int, str]:
    return (int(eid), eid) if eid.isdigit() else (1 << 30, eid)


def _area_of(ctx: Context, map_id: str) -> str:
    m = ctx.kb.get("map", map_id)
    area = m.get("area") if m is not None else None
    if isinstance(area, list) and area:
        area = area[0]
    if isinstance(area, Mapping) and ctx.kb.get("area", str(area.get("id"))) is not None:
        return str(area["id"])
    return ""


def place(ctx: Context, map_id: str, note: str = "") -> str:
    """``map (area)``, both linked: where something is; ``note`` (escaped) joins the area
    in the brackets."""
    if ctx.kb.get("map", map_id) is None:
        return t.esc(f"map {map_id.replace('-', '.')}" + (f" ({note})" if note else ""))
    out = ctx.ref("map", map_id)
    area = _area_of(ctx, map_id)
    inner = [ctx.ref("area", area)] if area else []
    inner += [t.esc(note)] if note else []
    return out + (f" ({', '.join(inner)})" if inner else "")


ALL_CHAPTERS = (1, 2, 3)


def _chapters(chs: Any) -> str:
    """When something is there: "" for every chapter, "from chapter 2", "in chapter 1"..."""
    chs = sorted({c for c in chs or [] if isinstance(c, int)})
    if not chs or tuple(c for c in chs if c in ALL_CHAPTERS) == ALL_CHAPTERS:
        return ""
    if len(chs) == 1:
        return f"in chapter {chs[0]}"
    if chs == list(range(chs[0], ALL_CHAPTERS[-1] + 1)):
        return f"from chapter {chs[0]}"
    return "in chapters " + ", ".join(str(c) for c in chs[:-1]) + f" and {chs[-1]}"


def _join(parts: Sequence[str], word: str = "and") -> str:
    parts = [p for p in parts if p]
    if len(parts) <= 1:
        return "".join(parts)
    if len(parts) == 2:
        return f"{parts[0]} {word} {parts[1]}"
    return ", ".join(parts[:-1]) + f", {word} {parts[-1]}"


def _type_text(e: Entity) -> str:
    typ = e.get("type")
    return typ if isinstance(typ, str) else ""


def _split_prose(html: str | None) -> tuple[str, str]:
    """A prose file's lead (before its first ``##`` heading) and the rest."""
    if not html:
        return "", ""
    i = html.find("<h2")
    return (html, "") if i < 0 else (html[:i], html[i:])


# --- job advancements ----------------------------------------------------------------------


def _quest_ref(ctx: Context, chapter: Any, kind: Any, number: Any) -> str:
    word = "main" if kind == 0 else "sub"
    qid = f"{chapter}-{word}-{number}"
    if ctx.kb.get("quest", qid) is not None:
        return ctx.ref("quest", qid)
    return t.esc(f"{word} quest {number}")


def condition_markup(ctx: Context, c: Mapping[str, Any], chapter: Any) -> str:
    """One condition of a job advancement, in our words (links to the quests it names)."""
    native, op, value = c.get("native"), c.get("op"), c.get("value")
    args = c.get("args") or []
    if native == "getLevel":
        if op == ">=":
            return t.esc(f"level {value} or higher")
        return t.esc(f"level {op} {value}")
    if native == "getClassUpCount" and op == "==" and isinstance(value, int):
        if value == 0:
            return "no job advancement yet"
        n = WORDS.get(value, str(value))
        return t.esc(f"{n} job advancement{'s' if value != 1 else ''} made")
    if native in ("getQuestState", "checkRequire", "getReward") and len(args) == 2:
        q = _quest_ref(ctx, chapter, args[0], args[1])
        if native == "getQuestState" and op == "==" and value in STATES:
            return f"{q} {STATES[value]}"
        if native == "checkRequire" and op == "==" and value in (0, 1):
            return f"what {q} needs {'at hand' if value else 'missing'}"
        if native == "getReward" and op == "==" and value == 1:
            return f"{q} completed on the spot (its needs taken, its rewards given)"
    if native == "isInvenAvailable" and op == "==" and value == 1 and len(args) == 2:
        tab = "equipment" if args[0] == 0 else "other items"
        n = args[1]
        return t.esc(f"{n} free slot{'s' if n != 1 else ''} in the {tab} inventory")
    return f"<code>{t.esc(str(c.get('text') or c.get('term') or ''))}</code>"


def advancement_conditions(ctx: Context, adv: Entity) -> list[list[str]]:
    """The advancement's conditions as markup: one list per alternative."""
    out = []
    for alt in adv.get("conditions") or []:
        if isinstance(alt, list):
            out.append(
                [
                    condition_markup(ctx, c, adv.get("chapter"))
                    for c in alt
                    if isinstance(c, Mapping)
                ]
            )
    return out


def _conditions_cell(ctx: Context, adv: Entity) -> str:
    alts = advancement_conditions(ctx, adv)
    if not alts:
        return "never (no path of the script reaches it)"
    if alts == [[]]:
        return "none"
    parts = ["; ".join(a) for a in alts]
    return parts[0] if len(parts) == 1 else " <em>or</em> ".join(parts)


def _givers(ctx: Context, adv: Entity) -> str:
    out = []
    for g in adv.get("given_by") or []:
        if not isinstance(g, Mapping) or ctx.kb.get("npc", str(g.get("id"))) is None:
            continue
        who = ctx.ref("npc", str(g["id"]))
        where = place(ctx, str(g["map"])) if g.get("map") else ""
        out.append(f"{who} in {where}" if where else who)
    return _join(out)


def _advancements(ctx: Context) -> list[Entity]:
    return sorted(
        ctx.kb.entities("job_advancement"),
        key=lambda a: (
            _int(a.get("chapter")) or 0,
            _int(a.get("advancement")) or 0,
            str(a.get("id")),
        ),
    )


def _advancement_job(adv: Entity) -> str:
    if adv.get("keeps_job"):
        return "same job"
    name = adv.get("job_name")
    return name if isinstance(name, str) else ""


def _adv_job_cell(ctx: Context, adv: Entity) -> str:
    job = JOB_BY_BRANCH.get(str(adv.get("job_name") or ""))
    if job is not None:
        return _job_link(ctx, job)
    return t.esc(_advancement_job(adv) or "unknown")


def advancement_table(
    ctx: Context, advs: Sequence[Entity], anchors: bool = False, with_job: bool = True
) -> str:
    cols = [t.Column("Chapter", numeric=True), t.Column("Advancement")]
    cols += [t.Column("Job")] if with_job else []
    cols += [t.Column("Given by"), t.Column("Conditions")]
    rows, ids = [], []
    for a in advs:
        eid = str(a["id"])
        row = [
            t.num_cell(_int(a.get("chapter")) or 0),
            t.markup_cell(ctx.name("job_advancement", eid), name_of(a)),
        ]
        if with_job:
            row.append(t.markup_cell(_adv_job_cell(ctx, a), _advancement_job(a)))
        row += [t.markup_cell(_givers(ctx, a)), t.markup_cell(_conditions_cell(ctx, a))]
        rows.append(row)
        ids.append(d.row_anchor("job_advancement", eid))
    return t.table(cols, rows, ids if anchors else None)


# --- weapons and where to find them ------------------------------------------------------------


def _limit(parts: list[str], n: int = 3) -> str:
    if len(parts) > n:
        parts = parts[:n] + [f"{len(parts) - n} more"]
    return _join(parts)


def item_sources(ctx: Context, item_id: str) -> str:
    """Where an item comes from, each source with its map and area: the shops an NPC opens
    (by NPC and price), quest rewards, recipes (by NPC) and monster drops."""
    e = ctx.kb.get("item", item_id)
    if e is None:
        return ""
    out = []
    # shops: (npc, price) -> {map: chapters}
    shops: dict[tuple[str, Any], dict[str, set[int]]] = {}
    for r in e.get("sold_by") or []:
        shop = ctx.kb.get("shop", str(r.get("id")))
        if shop is None or shop.get("unopened_reason"):
            continue
        for o in shop.get("opened_by") or []:
            if not isinstance(o, Mapping) or not o.get("map"):
                continue
            key = (str(o.get("id")), r.get("price"))
            maps = shops.setdefault(key, {})
            maps.setdefault(str(o["map"]), set()).update(o.get("chapters") or [])
    for (npc, price), maps in shops.items():
        who = ctx.ref("npc", npc) if ctx.kb.get("npc", npc) is not None else "a shop"
        cost = f" for {t.number(price)} meso" if isinstance(price, int) else ""
        where = [place(ctx, m, _chapters(chs)) for m, chs in maps.items()]
        out.append(f"Sold by {who}{cost} in {_limit(where)}")
    quests = []
    for r in e.get("rewarded_by_quests") or []:
        q = ctx.kb.get("quest", str(r.get("id")))
        if q is not None:
            ch = q.get("chapter")
            quests.append(
                ctx.ref("quest", str(q["id"])) + (f" (chapter {ch})" if isinstance(ch, int) else "")
            )
    if quests:
        out.append(f"Reward of {_limit(quests)}")
    # recipes: (npc, map) -> (first recipe id, chapters)
    crafts: dict[tuple[str, str], tuple[str, set[int]]] = {}
    for r in e.get("crafted_by") or []:
        rec = ctx.kb.get("recipe", str(r.get("id")))
        if rec is None:
            continue
        for c in rec.get("crafted_by") or []:
            if isinstance(c, Mapping) and ctx.kb.get("npc", str(c.get("id"))) is not None:
                key = (str(c["id"]), str(c.get("map") or ""))
                rid, chs = crafts.setdefault(key, (str(rec["id"]), set()))
                if isinstance(rec.get("chapter"), int):
                    chs.add(rec["chapter"])
    for (npc, map_id), (rid, chs) in crafts.items():
        bit = f"{ctx.ref('recipe', rid, 'Crafted')} by {ctx.ref('npc', npc)}"
        bit += f" in {place(ctx, map_id)}" if map_id else ""
        when = _chapters(chs)
        out.append(bit + (f", {t.esc(when)}" if when else ""))
    drops = []
    for r in e.get("dropped_by") or []:
        mob = ctx.kb.get("monster", str(r.get("id")))
        if mob is None:
            continue
        maps = [str(m["id"]) for m in mob.get("spawns_on") or [] if isinstance(m, Mapping)]
        where = f" on {place(ctx, maps[0])}" if maps else ""
        drops.append(ctx.ref("monster", str(mob["id"])) + where)
    if drops:
        out.append(f"Dropped by {_limit(drops)}")
    if item_id == STARTING_WEAPON:
        out.insert(0, "Worn by every new hero from the start")
    if not out:
        return "Not sold, crafted, dropped or given by a quest."
    return "; ".join(out)


def weapons(ctx: Context, kind: str) -> list[Entity]:
    """Every weapon of ``kind`` (``claw`` or ``dagger``) by required level."""
    rows = [
        e
        for e in ctx.kb.entities("item")
        if e.get("weapon_type") == kind and isinstance(e.get("req_level"), int)
    ]
    return sorted(rows, key=lambda e: (e["req_level"], str(e["id"])))


# The weapon a new character wears (LoadSaveData 0x39989, docs/formats/save.md).
STARTING_WEAPON = "equip:72"
SPEED_TICKS = {1: 2, 2: 4, 3: 6}  # weapon speed: ticks between attacks (table at 0x117140)


def _att(e: Entity) -> tuple[str, int]:
    st = e.get("stats")
    a = st.get("attack") if isinstance(st, Mapping) else None
    if isinstance(a, Mapping):
        lo, hi = int(a.get("min", 0)), int(a.get("max", 0))
        hi = max(hi, lo)
        return (t.number(lo) if lo == hi else f"{t.number(lo)}-{t.number(hi)}"), lo
    return "", 0


def weapon_table(ctx: Context, items: Sequence[Entity], up_to: int | None = None) -> str:
    cols = [
        t.Column("Level", "Required level", numeric=True),
        t.Column("Weapon"),
        t.Column("ATT", "Attack: a dropped or bought weapon has the minimum", numeric=True),
        t.Column("Interval", "Time between attacks, from the weapon's speed", numeric=True),
        t.Column("Where to get it"),
    ]
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in items))
    rows = []
    for e in items:
        if up_to is not None and e["req_level"] > up_to:
            continue
        att, key = _att(e)
        speed = _int(e.get("speed"))
        ticks = SPEED_TICKS.get(speed or 0)
        rows.append(
            [
                t.num_cell(e["req_level"]),
                d.entry_cell(ctx, "item", str(e["id"]), placeholder=boxes),
                t.Cell(t.esc(att), key),
                t.Cell(t.esc(f"{seconds(ticks)} s"), ticks) if ticks else t.Cell("", 0),
                t.markup_cell(item_sources(ctx, str(e["id"]))),
            ]
        )
    return t.table(cols, rows)


def star_table(ctx: Context) -> str:
    stars = [
        e
        for e in ctx.kb.entities("item")
        if e.get("category") == "throwing star" and isinstance(e.get("attack"), int)
    ]
    if not stars:
        return ""
    boxes = d.any_thumb(ctx, (("item", e["id"]) for e in stars))
    cols = [
        t.Column("Star"),
        t.Column("ATT", "Added to the hero's attack for a hit of this star", numeric=True),
        t.Column("Stack", "Stars in a full stack", numeric=True),
        t.Column("Where to get it"),
    ]
    rows = [
        [
            d.entry_cell(ctx, "item", str(e["id"]), placeholder=boxes),
            t.num_cell(e["attack"]),
            t.num_cell(_int(e.get("max_stack")) or 0),
            t.markup_cell(item_sources(ctx, str(e["id"]))),
        ]
        for e in sorted(stars, key=lambda e: (e["attack"], str(e["id"])))
    ]
    return t.table(cols, rows)


# --- skill pages -------------------------------------------------------------------------------


def _weapon_text(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    job = _job_of(e)
    typ = _type_text(e)
    if eid in STARS or (job is not None and job.weapon == "claw" and typ == "active attack"):
        return "claw"
    if job is not None and job.weapon and typ in ("active attack", "buff"):
        return job.weapon
    if eid == "0":
        return "dagger or claw"
    return ""


def _infobox(ctx: Context, e: Entity, rows: Sequence[LevelRow]) -> str:
    eid = str(e["id"])
    job = _job_of(e)
    tier = _int(e.get("tier"))
    mp = [r.mp for r in rows]
    typ = _type_text(e)
    weapon = _weapon_text(ctx, e)
    info = [
        ("Branch", _job_link(ctx, job) if job else t.esc(str(e.get("branch") or ""))),
        ("Tier", t.esc(f"{tier} ({ORDINAL.get(tier, '')} advancement)") if tier else ""),
        ("Type", t.esc(typ[:1].upper() + typ[1:]) if typ else ""),
        ("Maximum level", t.esc(t.number(e["max_level"])) if _int(e.get("max_level")) else ""),
        (
            "MP cost",
            t.esc(
                t.number(min(mp))
                if min(mp) == max(mp)
                else f"{t.number(min(mp))}-{t.number(max(mp))}"
            )
            if mp and max(mp)
            else "",
        ),
        ("Weapon", t.esc(weapon + (" (probable)" if typ == "buff" and weapon else ""))),
        ("ID", f"<code>{t.esc(eid)}</code>"),
    ]
    ko = name_of(e, "ko")
    sub = t.verbatim(ko, "ko") if ko != name_of(e, "en") else ""
    return t.infobox(
        ctx.name("skill", eid),
        info,
        ctx.sprite("skill", eid, box=96),
        subtitle_markup=sub,
        footer_markup=d.description_markup(e),
    )


def _pct(v: int) -> t.Cell:
    return t.Cell(t.esc(f"{t.number(v)}%"), v)


def effect_table(ctx: Context, e: Entity, rows: Sequence[LevelRow]) -> str:
    """The per-level table: level, damage (as the window shows it and as it hits), duration,
    the skill's own value, MP, and the game's filled-in detail text."""
    if not rows:
        return ""
    eid = str(e["id"])
    cols: list[t.Column] = [t.Column("Level", numeric=True)]
    getters = []
    if eid in ATTACK_CONSTANTS:
        cols.append(t.Column("Window", "Damage the skill window shows (@dmg)", numeric=True))
        getters.append(lambda r: _pct(r.damage))
        cols.append(
            t.Column(
                "Hits with", "Damage percentage the damage roll uses: one level more", numeric=True
            )
        )
        getters.append(lambda r: _pct(r.damage_hit))
    elif eid in DAMAGE_AS_ATT:
        cols.append(t.Column("ATT", "Added to attack while the buff lasts", numeric=True))
        getters.append(lambda r: t.Cell(t.esc(f"+{t.number(r.damage)}"), r.damage))
    if _type_text(e) == "buff" and any(r.time for r in rows):
        cols.append(t.Column("Duration", "At the default game speed", numeric=True))
        getters.append(lambda r: t.Cell(t.esc(f"{seconds(r.time)} s"), r.time))
    if eid in ETC_COLUMNS and any(r.etc_raw for r in rows):
        title, prefix, unit, tip = ETC_COLUMNS[eid]
        cols.append(t.Column(title, tip, numeric=True))

        def etc_cell(r: LevelRow, prefix: str = prefix, unit: str = unit) -> t.Cell:
            if unit == "s":
                return t.Cell(t.esc(f"{seconds(r.etc)} s"), r.etc)
            return t.Cell(t.esc(f"{prefix}{t.number(r.etc)}{unit}"), r.etc)

        getters.append(etc_cell)
    if any(r.mp for r in rows):
        cols.append(t.Column("MP", "MP cost", numeric=True))
        getters.append(lambda r: t.num_cell(r.mp))
    cols.append(t.Column("In-game text"))
    body = []
    for r in rows:
        cells = [t.num_cell(r.level)] + [g(r) for g in getters]
        cells.append(t.markup_cell(t.verbatim(r.detail) if r.detail else ""))
        body.append(cells)
    return t.table(cols, body, filter=False)


def _requirements(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    job = _job_of(e)
    tier = _int(e.get("tier")) or 0
    items = []
    if _int(e.get("max_level")):
        if job is not None and tier:
            if job.job == 0:
                who = "every thief from the first job advancement on"
            else:
                nth = ORDINAL.get(tier, str(tier))
                who = f"the {_job_link(ctx, job)} from the {nth} job advancement on"
            items.append(f"Learnable by {who} (tier {tier}).")
        n = e["max_level"]
        items.append(
            f"Each level costs one skill point; the maximum, level {t.number(n)}, takes"
            f" {t.number(n)} points."
        )
    else:
        items.append("Always available: it has no levels and costs no skill points.")
    weapon = _weapon_text(ctx, e)
    if eid in STARS:
        one, sp = STARS[eid]
        stars = f"{one} star{'s' if one != 1 else ''}"
        extra = (
            f" ({sp} with {ctx.ref('skill', '14')} on)"
            if sp != one and ctx.kb.get("skill", "14")
            else ""
        )
        items.append(
            f"Needs a claw and {stars} per use{extra}, taken from the first stack that"
            " holds enough."
        )
    elif weapon in ("claw", "dagger") and _type_text(e) == "buff":
        items.append(
            f"Probably needs a {weapon}: the game checks the branch's weapon when a skill is"
            " used, and the mechanics notes do not say whether buffs are exempt."
        )
    elif weapon in ("claw", "dagger"):
        items.append(f"Needs a {weapon}: the game refuses the skill with any other weapon.")
    elif _type_text(e) == "passive":
        items.append(
            "A passive: its bonus is applied when the points are learned, so it needs no"
            " weapon (probable)."
        )
    elif eid == "3":
        items.append(
            "No weapon rule is known: the game ties the claw to star skills and the dagger"
            " to the Bandit's skills, and this is neither."
        )
    return "<ul>" + "".join(f"<li>{x}</li>" for x in items) + "</ul>\n"


def _notes(ctx: Context, e: Entity, rows: Sequence[LevelRow]) -> str:
    eid = str(e["id"])
    notes: list[str] = []
    if eid in ATTACK_CONSTANTS and rows:
        c = ATTACK_CONSTANTS[eid]
        last = rows[-1]
        notes.append(
            "The damage roll uses one level more than the skill window shows: at level"
            f" {last.level} the window says {last.damage}% and the skill hits with"
            f" {last.damage_hit}%. The percentage is added on top of the base damage, and"
            f" the roll then {'adds' if c > 0 else 'takes off'} {abs(c)}."
        )
    if eid in ETC_COLUMNS and rows and any(r.etc_raw % 10 for r in rows if r.etc_raw > 0):
        notes.append(
            "The table keeps this value in tenths and the game rounds it down, so some levels"
            " add nothing to it."
        )
    if _type_text(e) == "buff" and any(r.time for r in rows):
        notes.append(
            "The in-game text gives the duration in ticks, 20 to a second at the default"
            " game speed. Casting the buff again while it lasts only restarts its timer."
        )
    notes += [t.esc(n) for n in NOTES.get(eid, [])]
    if not notes:
        return ""
    return "<ul>" + "".join(f"<li>{n}</li>" for n in notes) + "</ul>\n"


def _lead(ctx: Context, e: Entity) -> str:
    eid = str(e["id"])
    job = _job_of(e)
    branch = job.branch if job else str(e.get("branch") or "")
    typ = _type_text(e) or "skill"
    tier = _int(e.get("tier"))
    name = ctx.name("skill", eid)
    if not _int(e.get("max_level")):
        return f"<p>{name} is a {t.esc(typ)} of the {t.esc(branch)} branch.</p>\n"
    return f"<p>{name} is a tier {tier} {t.esc(typ)} of the {t.esc(branch)} branch.</p>\n"


def skill_page(ctx: Context, e: Entity) -> Page:
    eid = str(e["id"])
    rows = level_rows(e)
    path = d.entity_path("skill", eid)
    effect_prose, rest = _split_prose(ctx.prose(path[: -len(".html")] + ".md"))
    parts = [_infobox(ctx, e, rows), _lead(ctx, e)]
    table = effect_table(ctx, e, rows)
    if effect_prose or table:
        parts.append(t.section("Effect", effect_prose + table, "effect"))
    parts.append(t.section("Requirements", _requirements(ctx, e), "requirements"))
    if rest:
        parts.append(rest)
    notes = _notes(ctx, e, rows)
    if notes:
        parts.append(t.section("Notes", notes, "notes"))
    job = _job_of(e)
    crumbs = [(job.branch, job.path)] if job else []
    return Page(
        path=path,
        title=name_of(e),
        heading=ctx.name("skill", eid),
        title_verbatim=True,
        body="\n".join(parts),
        crumbs=crumbs,
        description=f"Skill: {name_of(e)}",
    ).documents("skill", eid)


# --- job pages ---------------------------------------------------------------------------------


def job_skills(ctx: Context, job: Job) -> list[Entity]:
    """The skills in the job's window (docs/formats/save.md, 0x7FD69): every skill that is
    not a basic attack, of branch Rogue or the job's own branch, by tier."""
    out = [
        e
        for e in _skills(ctx)
        if _type_text(e) != "basic attack" and str(e.get("branch")) in ("Rogue", job.branch)
    ]
    return sorted(out, key=lambda e: (_int(e.get("tier")) or 0, _sort_id(str(e["id"]))))


def skill_list_table(ctx: Context, skills: Sequence[Entity], with_branch: bool = True) -> str:
    cols = [t.Column("Skill"), t.Column("Korean")]
    cols += [t.Column("Branch")] if with_branch else []
    cols += [t.Column("Tier", numeric=True)] if with_branch else []
    cols += [t.Column("Type"), t.Column("Maximum level", numeric=True)]
    boxes = d.any_thumb(ctx, (("skill", e["id"]) for e in skills))
    rows = []
    for e in skills:
        eid = str(e["id"])
        job = _job_of(e)
        row = [
            d.entry_cell(ctx, "skill", eid, placeholder=boxes),
            t.Cell(t.verbatim(name_of(e, "ko"), "ko"), name_of(e, "ko"), "ko"),
        ]
        if with_branch:
            row.append(
                t.markup_cell(
                    _job_link(ctx, job) if job else t.esc(str(e.get("branch", ""))),
                    str(e.get("branch", "")),
                )
            )
        typ = _type_text(e)
        row += [t.num_cell(_int(e.get("tier")) or 0)] if with_branch else []
        row += [
            t.text_cell(typ[:1].upper() + typ[1:]),
            t.num_cell(_int(e.get("max_level")) or 0),
        ]
        rows.append(row)
    return t.table(cols, rows)


def job_advancements(ctx: Context, job: Job) -> list[Entity]:
    out = []
    for a in _advancements(ctx):
        name = a.get("job_name")
        if name == job.branch or (job.job != 0 and a.get("keeps_job")):
            out.append(a)
    return out


def _tier_points(skills: Sequence[Entity]) -> dict[int, int]:
    out: dict[int, int] = {}
    for e in skills:
        tier = _int(e.get("tier")) or 0
        out[tier] = out.get(tier, 0) + (_int(e.get("max_level")) or 0)
    return out


def _tree(ctx: Context, job: Job) -> str:
    skills = job_skills(ctx, job)
    if not skills:
        return ""
    parts = []
    for tier in sorted({_int(e.get("tier")) or 0 for e in skills}):
        mine = [e for e in skills if (_int(e.get("tier")) or 0) == tier]
        pts = sum(_int(e.get("max_level")) or 0 for e in mine)
        intro = (
            f"<p>Tier {tier} opens with the {ORDINAL.get(tier, str(tier))} job advancement."
            f" Its skills take {t.number(pts)} points to reach their maximum.</p>\n"
        )
        parts.append(
            t.section(
                f"Tier {tier}", intro + skill_list_table(ctx, mine, False), f"tier-{tier}", level=3
            )
        )
    return "".join(parts)


def job_page(ctx: Context, job: Job) -> Page:
    lead, rest = _split_prose(ctx.prose(f"{SECTION_ID}/{job.slug}.md"))
    parts = [lead]
    tree = _tree(ctx, job)
    if tree:
        parts.append(t.section("Skill tree", tree, "skill-tree"))
    advs = job_advancements(ctx, job)
    if advs:
        intro = (
            "<p>Each chapter runs its own script for the advancement; the conditions"
            " differ between chapters. All conditions of a row must hold together.</p>\n"
        )
        parts.append(
            t.section(
                "Advancement",
                intro + advancement_table(ctx, advs, with_job=job.job != 0),
                "advancement",
            )
        )
    kinds = [job.weapon] if job.weapon else ["claw", "dagger"]
    wparts = []
    for kind in kinds:
        items = weapons(ctx, kind)
        if not items:
            continue
        table = weapon_table(ctx, items, up_to=29 if job.job == 0 else None)
        if len(kinds) > 1:
            wparts.append(t.section(f"{kind.capitalize()}s", table, f"{kind}s", level=3))
        else:
            wparts.append(table)
    if wparts:
        intro = (
            "<p>Every weapon of the kind, by required level. Bought and dropped weapons have"
            " the minimum ATT; only crafting rolls the stats.</p>\n"
        )
        parts.append(t.section("Weapons by level", intro + "".join(wparts), "weapons"))
    if job.weapon == "claw":
        stars = star_table(ctx)
        if stars:
            intro = (
                "<p>A star's ATT is added to the hero's for each hit it makes. Star skills take"
                " their stars from the first stack that holds enough.</p>\n"
            )
            parts.append(t.section("Throwing stars", intro + stars, "stars"))
    if rest:
        parts.append(rest)
    links = [f'<a href="{ctx.url(j.path)}">{t.esc(j.branch)}</a>' for j in JOBS if j != job]
    links += [
        f'<a href="{ctx.url(SECTION_ID + "/growth.html")}">Character growth</a>',
        f'<a href="{ctx.url(SECTION_ID + "/builds.html")}">Skill builds</a>',
    ]
    parts.append(t.see_also(links))
    return Page(
        path=job.path,
        title=job.branch,
        body="\n".join(p for p in parts if p),
        description=f"The {job.branch}: skills, job advancement and weapons.",
    )


# --- growth -------------------------------------------------------------------------------------


def _levels(ctx: Context) -> list[Entity]:
    return sorted(ctx.kb.entities("level"), key=lambda e: (_int(e.get("level")) or 0, str(e["id"])))


def growth_tables(ctx: Context, levels: Sequence[Entity]) -> tuple[str, str]:
    cols = [
        t.Column("Level", numeric=True),
        t.Column("EXP", "EXP to finish the level", numeric=True),
        t.Column("Total EXP", "EXP gained before reaching the level", numeric=True),
        t.Column("HP gained", numeric=True),
        t.Column("Maximum HP", "Sum of the HP gained up to the level", numeric=True),
        t.Column("MP gained", numeric=True),
        t.Column("Maximum MP", "Sum of the MP gained up to the level", numeric=True),
        t.Column("SP", "Skill points on reaching the level", numeric=True),
    ]
    rows, ids = [], []
    for e in levels:
        n = _int(e.get("level")) or 0
        exp = _int(e.get("exp")) or 0
        exp_cell = t.num_cell(exp) if n < MAX_HERO_LEVEL else t.Cell("none", 0)
        rows.append(
            [
                t.num_cell(n),
                exp_cell,
                t.num_cell(_int(e.get("exp_total")) or 0),
                t.num_cell(_int(e.get("hp_gain")) or 0),
                t.num_cell(_int(e.get("max_hp")) or 0),
                t.num_cell(_int(e.get("mp_gain")) or 0),
                t.num_cell(_int(e.get("max_mp")) or 0),
                t.num_cell(skill_points(n) if 1 < n <= MAX_HERO_LEVEL else 0),
            ]
        )
        ids.append(d.row_anchor("level", str(e["id"])))
    first = t.table(cols, rows, ids)
    scols = (
        [t.Column("Level", numeric=True)]
        + [
            t.Column(d.label(k), numeric=True)
            for k in ("attack", "defense", "accuracy", "avoidability")
        ]
        + [
            t.Column(d.label(k), numeric=True)
            for k in ("strength", "dexterity", "intelligence", "luck")
        ]
    )
    srows = []
    for e in levels:
        st = e.get("stats") if isinstance(e.get("stats"), Mapping) else {}
        srows.append(
            [t.num_cell(_int(e.get("level")) or 0)]
            + [
                t.num_cell(_int(e.get(k)) or 0)
                for k in ("attack", "defense", "accuracy", "avoidability")
            ]
            + [
                t.num_cell(_int(st.get(k)) or 0)
                for k in ("strength", "dexterity", "intelligence", "luck")
            ]
        )
    return first, t.table(scols, srows)


def growth_page(ctx: Context) -> Page:
    levels = _levels(ctx)
    lead, rest = _split_prose(ctx.prose(f"{SECTION_ID}/growth.md"))
    page = Page(
        path=f"{SECTION_ID}/growth.html",
        title="Character growth",
        body="",
        description="The level table: EXP, HP, MP, stats and skill points from level 1 to 99.",
    )
    parts = [lead]
    if rest:
        parts.append(rest)
    if levels:
        first, stats = growth_tables(ctx, levels)
        pts = [
            (float(_int(e.get("level")) or 0), float(_int(e.get("exp_total")) or 0))
            for e in levels
            if (_int(e.get("level")) or 0) <= MAX_HERO_LEVEL
        ]
        chart = (
            t.svg_line_chart(pts, "Total EXP by level", "level", "total EXP")
            if len(pts) > 1
            else ""
        )
        parts.append(t.section("Level table", chart + first, "level-table"))
        parts.append(t.section("Stats by level", stats, "stats"))
        for e in levels:
            page.documents("level", str(e["id"]), d.row_anchor("level", str(e["id"])))
    page.body = "\n".join(p for p in parts if p)
    return page


# --- builds -----------------------------------------------------------------------------------


SP_MILESTONES = (10, 20, 29, 30, 45, 59, 60, 75, 99)


def builds_page(ctx: Context) -> Page:
    lead, rest = _split_prose(ctx.prose(f"{SECTION_ID}/builds.md"))
    cols = [
        t.Column("Level", numeric=True),
        t.Column("Skill points", "Gained in all", numeric=True),
    ]
    sp = t.table(
        cols,
        [[t.num_cell(n), t.num_cell(skill_points_total(n))] for n in SP_MILESTONES],
        compact=True,
    )
    tcols = [
        t.Column("Job"),
        t.Column("Tier", numeric=True),
        t.Column("Points to maximum", numeric=True),
    ]
    trows = []
    for job in JOBS[1:]:
        pts = _tier_points(job_skills(ctx, job))
        for tier in sorted(pts):
            trows.append(
                [
                    t.markup_cell(_job_link(ctx, job), job.branch),
                    t.num_cell(tier),
                    t.num_cell(pts[tier]),
                ]
            )
        trows.append(
            [
                t.markup_cell(_job_link(ctx, job), job.branch),
                t.Cell("all", 99),
                t.num_cell(sum(pts.values())),
            ]
        )
    parts = [lead]
    total = skill_points_total(MAX_HERO_LEVEL)
    parts.append(
        t.section(
            "Skill points",
            f"<p>A level-up gives no skill point up to level 9, one from level 10 to 29 and two"
            f" from level 30, so a level {MAX_HERO_LEVEL} hero has had {t.number(total)}"
            " points in all.</p>\n" + sp + (t.table(tcols, trows, compact=True) if trows else ""),
            "skill-points",
        )
    )
    if rest:
        parts.append(rest)
    return Page(
        path=f"{SECTION_ID}/builds.html",
        title="Skill builds",
        body="\n".join(p for p in parts if p),
        description="Suggested skill-point orders for the Rogue, Assassin and Bandit.",
    )


# --- index ------------------------------------------------------------------------------------


def index_page(ctx: Context) -> Page:
    lead, rest = _split_prose(ctx.prose(f"{SECTION_ID}/index.md"))
    page = Page(f"{SECTION_ID}/index.html", "Skills and jobs", "")
    parts = [lead]
    jcols = [
        t.Column("Job"),
        t.Column("Weapon"),
        t.Column("Skills", "In the skill window", numeric=True),
    ]
    jrows = [
        [
            t.markup_cell(_job_link(ctx, j), j.branch),
            t.text_cell(j.weapon or "either"),
            t.num_cell(len(job_skills(ctx, j))),
        ]
        for j in JOBS
    ]
    parts.append(t.section("Jobs", t.table(jcols, jrows, compact=True), "jobs"))
    skills = sorted(_skills(ctx), key=lambda e: _sort_id(str(e["id"])))
    if skills:
        parts.append(t.section("Skills", skill_list_table(ctx, skills), "skills"))
    advs = _advancements(ctx)
    if advs:
        intro = (
            "<p>Every job advancement the scripts make, by chapter. All conditions of a row"
            " must hold together.</p>\n"
        )
        parts.append(
            t.section(
                "Job advancements", intro + advancement_table(ctx, advs, True), "advancements"
            )
        )
        for a in advs:
            page.documents(
                "job_advancement", str(a["id"]), d.row_anchor("job_advancement", str(a["id"]))
            )
    if rest:
        parts.append(rest)
    parts.append(
        t.see_also(
            [
                f'<a href="{ctx.url(SECTION_ID + "/growth.html")}">Character growth</a>',
                f'<a href="{ctx.url(SECTION_ID + "/builds.html")}">Skill builds</a>',
            ]
        )
    )
    page.body = "\n".join(p for p in parts if p)
    return page


def pages(ctx: Context) -> Iterator[Page]:
    yield index_page(ctx)
    for job in JOBS:
        yield job_page(ctx, job)
    yield growth_page(ctx)
    yield builds_page(ctx)
    for e in sorted(_skills(ctx), key=lambda e: _sort_id(str(e["id"]))):
        yield skill_page(ctx, e)


SECTION = Section(
    SECTION_ID,
    "Skills and jobs",
    70,
    ("skill", "level", "job_advancement"),
    pages,
    blurb="The three thief jobs, every skill at every level, and the level table.",
    image="skill:2",
)
