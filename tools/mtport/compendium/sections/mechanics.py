"""The compendium's Mechanics and guides section (issue #224).

Pages:

- one page per topic of the game's rules (``docs/re/mechanics.md``) and per system around
  the main game (cash shop, saving, controls, options), each from its prose file in
  ``docs/compendium/pages/mechanics/``;
- the damage page adds the **damage calculator**: a small inline script whose formulas are
  :data:`CALC_JS` (one string, run under Node by the tests) and are mirrored in Python by
  :func:`hero_damage`, :func:`hit_odds`, :func:`mob_damage` and :func:`avoid_chance`;
- the guides add tables generated from the knowledge base: training spots by level band
  (:func:`training_spots`) and meso per kill (:func:`meso_rows`).

The section documents no knowledge-base kind of its own; every table links the monsters,
items, maps and areas it names.
"""

from __future__ import annotations

import json
import re
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from mtport.compendium import defaults
from mtport.compendium import templates as t
from mtport.compendium.siteapi import Context, Entity, Page, Section, name_of

SECTION_ID = "mechanics"


@dataclass(frozen=True)
class Topic:
    """One page of the section: its prose file (``mechanics/<slug>.md``), title, group on the
    index and a one-line description (our own words)."""

    slug: str
    title: str
    group: str
    blurb: str


RULES, SYSTEMS, GUIDES = "Rules", "Systems", "Guides"

TOPICS: tuple[Topic, ...] = (
    Topic(
        "damage", "Damage", RULES, "The hero's stats, attacks, the damage roll and a calculator."
    ),
    Topic(
        "accuracy",
        "Accuracy, avoid and critical hits",
        RULES,
        "Evade, miss, critical hits, monster damage and knock-back.",
    ),
    Topic("drops", "Drops and meso", RULES, "The random numbers and what a kill drops."),
    Topic("exp", "EXP and levelling", RULES, "EXP, level-ups, skill points and stat gains."),
    Topic(
        "monster-carnival",
        "Monster Carnival",
        RULES,
        "The level-scaling mode: entry, scaling, points and rewards.",
    ),
    Topic(
        "items", "Potions and scrolls", RULES, "Potions, return scrolls, upgrade scrolls and stars."
    ),
    Topic("skills", "Skills and buffs", RULES, "Skill values, MP costs, buffs and passives."),
    Topic("movement", "Movement", RULES, "Walking, jumping, ice and how monsters move."),
    Topic("death", "Death and revives", RULES, "The death penalty and the ways back."),
    Topic(
        "cash-shop",
        "Cash shop and revives",
        SYSTEMS,
        "The ten paid products, what each grants and what the port does offline.",
    ),
    Topic(
        "saving", "Saving", SYSTEMS, "Character slots, what a save holds and where it is refused."
    ),
    Topic("controls", "Controls", SYSTEMS, "The original touch key pad."),
    Topic("options", "Options", SYSTEMS, "Sound, game speed and vibration."),
    Topic(
        "training-spots",
        "Training spots",
        GUIDES,
        "Where to level, by level band, from the monsters' EXP per HP.",
    ),
    Topic(
        "meso-making", "Meso making", GUIDES, "Which monsters pay best in meso and sellable drops."
    ),
    Topic("beginners-guide", "Beginner's guide", GUIDES, "The first hour of play."),
)


def topic_path(slug: str) -> str:
    return f"{SECTION_ID}/{slug}.html"


# --- the rules, in Python (the calculator's mirror) -------------------------------------------


def q(a: int, b: int) -> int:
    """Integer division rounding toward zero, as the game's ``___divsi3`` does."""
    n = abs(a) // abs(b)
    return n if (a >= 0) == (b >= 0) else -n


# Skills whose hit is a thrown star (the star's attack is added): basic throw, Lucky Seven,
# Drain, Avenger.
STAR_SKILLS = frozenset({1, 2, 7, 10})
# The flat term each attack skill adds after its percentage (``CHero::RollDamage``).
SKILL_CONSTANT = {2: -5, 3: -3, 7: 2, 10: 4, 16: 2, 23: 3}
# The attack skills the calculator offers (``skill.dat`` ids).
ATTACK_SKILLS = (0, 1, 2, 3, 7, 10, 16, 23)
CLAW_SKILLS = frozenset({1, 2, 7, 10})
DAGGER_SKILLS = frozenset({16, 23})  # the Bandit attack skills


@dataclass(frozen=True)
class Damage:
    """One hit before evade, miss and critical: the adjusted base ``x`` and the range."""

    x: int
    lo: int
    hi: int


def hero_damage(
    attack: int,
    skill: int = 0,
    skill_level: int = 0,
    damage_base: int = 0,
    damage_per_level: int = 0,
    star_attack: int = 0,
    punch: bool = False,
) -> Damage:
    """The hero's damage range for one hit (``CHero::RollDamage`` ``0x64DA1``)."""
    if punch:
        return Damage(1, 1, 1)
    a = attack + (star_attack if skill in STAR_SKILLS else 0)
    x = q(a * (q(a, 30) + 3), 5)
    if skill == 1:
        x -= q(x, 10)
    elif skill in SKILL_CONSTANT:
        p = damage_base + damage_per_level * skill_level
        x += q(x * p, 100) + SKILL_CONSTANT[skill]
    if x < 0:
        x = 10
    lo = q((x - q(x, 3)) * 110, 100)
    hi = q((x + q(x, 5)) * 110, 100)
    return Damage(x, lo, hi)


def _pct(p: int) -> float:
    """``Rand_Percent(p)`` as a probability (p at or below 0 never, 100 or more always)."""
    return min(max(p, 0), 100) / 100


@dataclass(frozen=True)
class Odds:
    """A hit against one mob: evade and miss chances (percent) and the mean damage of one
    attempt, counting evade, miss and critical hits."""

    evade: float
    miss_lo: float  # the miss chance of the smallest hit
    miss_hi: float  # of the largest
    critical: float
    mean: float


def hit_odds(x: int, mob_level: int, accuracy: int, critical: int) -> Odds:
    """Evade, miss and critical for a hit of base ``x`` (the rolls of ``CMob::RollEvade``
    ``0x72639``, ``CMob::TakeHit`` ``0x73715`` and ``CHero::RollCritical`` ``0x6039D``)."""
    evade = _pct(mob_level - accuracy)
    crit = _pct(critical)
    rs = range(-q(x, 3), q(x, 5) + 1)
    total = 0.0
    misses = []
    for r in rs:
        d = q((x + r) * 110, 100)
        miss = 1.0 if d < 1 else _pct(q(mob_level, 2) - d)
        misses.append(miss)
        total += (1 - miss) * d * (1 + crit)
    mean = (1 - evade) * total / len(rs)
    return Odds(evade * 100, misses[0] * 100, misses[-1] * 100, crit * 100, mean)


def evade_chance(mob_level: int, accuracy: int) -> float:
    """The chance in percent that a mob evades a hit (``CMob::RollEvade`` ``0x72639``)."""
    return _pct(mob_level - accuracy) * 100


def miss_chance(damage: int, mob_level: int) -> float:
    """The chance in percent that a hit of ``damage`` misses (``CMob::TakeHit``
    ``0x73715``)."""
    return 100.0 if damage < 1 else _pct(q(mob_level, 2) - damage) * 100


def mob_damage(level: int, defense: int) -> tuple[int, int]:
    """The damage range of a mob's contact (``CMob::RollDamage`` ``0x6F591``)."""
    d = q(level * level, 30) + 2 * level + 20 - q(defense * 7, 3)
    if d < 0:
        lo, hi = 1, max(1, q(level, 2))
    else:
        lo, hi = d - 2 - q(d, 4), d + q(d, 4) + 2
    return q(lo * 90, 100), q(hi * 90, 100)


def avoid_chance(avoidability: int, mob_level: int) -> float:
    """The chance in percent that the hero avoids a contact (``CHero::RollAvoid``
    ``0x603DD``)."""
    return _pct((avoidability - mob_level) * 4) * 100


def chance_in_32768(n: int, d: int) -> int:
    """How many of the 32,768 values of the game's 15-bit ``Rand_Next`` pass
    ``Rand_Chance(n, d)`` (``Rand_Next`` modulo d, plus 1, at most n)."""
    if n <= 0 or d <= 0:
        return 0
    full, rest = divmod(32768, d)
    # remainders 0 .. rest-1 occur full+1 times, the others full times
    hits = min(n, d)
    return hits * full + min(hits, rest)


# The calculator's formulas, the same rules in JavaScript; the page and the tests run this.
CALC_JS = r"""
function mtQ(a, b) { return Math.trunc(a / b); }
var MT_STAR_SKILLS = {1: 1, 2: 1, 7: 1, 10: 1};
var MT_SKILL_CONSTANT = {2: -5, 3: -3, 7: 2, 10: 4, 16: 2, 23: 3};
function mtHeroDamage(o) {
  if (o.punch) { return {x: 1, lo: 1, hi: 1}; }
  var a = o.attack + (MT_STAR_SKILLS[o.skill] ? (o.starAttack || 0) : 0);
  var x = mtQ(a * (mtQ(a, 30) + 3), 5);
  if (o.skill === 1) {
    x -= mtQ(x, 10);
  } else if (MT_SKILL_CONSTANT[o.skill] !== undefined) {
    var p = (o.damageBase || 0) + (o.damagePerLevel || 0) * (o.skillLevel || 0);
    x += mtQ(x * p, 100) + MT_SKILL_CONSTANT[o.skill];
  }
  if (x < 0) { x = 10; }
  return {x: x, lo: mtQ((x - mtQ(x, 3)) * 110, 100), hi: mtQ((x + mtQ(x, 5)) * 110, 100)};
}
function mtPct(p) { return Math.min(Math.max(p, 0), 100) / 100; }
function mtHitOdds(x, mobLevel, accuracy, critical) {
  var evade = mtPct(mobLevel - accuracy), crit = mtPct(critical);
  var lo = -mtQ(x, 3), hi = mtQ(x, 5), total = 0, first = 0, last = 0;
  for (var r = lo; r <= hi; r++) {
    var d = mtQ((x + r) * 110, 100);
    var miss = d < 1 ? 1 : mtPct(mtQ(mobLevel, 2) - d);
    if (r === lo) { first = miss; }
    last = miss;
    total += (1 - miss) * d * (1 + crit);
  }
  return {evade: evade * 100, missLo: first * 100, missHi: last * 100, critical: crit * 100,
          mean: (1 - evade) * total / (hi - lo + 1)};
}
function mtMobDamage(level, defense) {
  var d = mtQ(level * level, 30) + 2 * level + 20 - mtQ(defense * 7, 3), lo, hi;
  if (d < 0) { lo = 1; hi = Math.max(1, mtQ(level, 2)); }
  else { lo = d - 2 - mtQ(d, 4); hi = d + mtQ(d, 4) + 2; }
  return [mtQ(lo * 90, 100), mtQ(hi * 90, 100)];
}
function mtAvoidChance(avoidability, mobLevel) {
  return mtPct((avoidability - mobLevel) * 4) * 100;
}
"""

# The calculator's page wiring: reads the form, calls the formulas, writes the outputs.
CALC_UI_JS = r"""
(function () {
  var f = document.getElementById("damage-calculator");
  if (!f) { return; }
  f.hidden = false;
  var levels = JSON.parse(f.getAttribute("data-levels"));
  function num(id) { var v = parseInt(f.elements[id].value, 10); return isNaN(v) ? 0 : v; }
  function opt(id) { var s = f.elements[id]; return s.options[s.selectedIndex]; }
  function data(id, key) {
    var v = parseInt(opt(id).getAttribute("data-" + key), 10);
    return isNaN(v) ? 0 : v;
  }
  function out(name, text) { f.querySelector("[data-out=" + name + "]").textContent = text; }
  function fmt(v) { return (Math.round(v * 100) / 100).toLocaleString("en"); }
  function pct(v) { return fmt(v) + "%"; }
  function fillFromLevel() {
    var row = levels[String(num("calc-level"))];
    if (!row) { return; }
    f.elements["calc-base-att"].value = row[0];
    f.elements["calc-acc"].value = row[1];
    f.elements["calc-def"].value = row[2];
    f.elements["calc-avoid"].value = row[3];
  }
  function fillFromWeapon() {
    f.elements["calc-weapon-att"].value = data("calc-weapon", "att");
  }
  function run() {
    var skill = parseInt(opt("calc-skill").value, 10);
    var maxLevel = data("calc-skill", "max");
    var skillLevel = Math.min(num("calc-skill-level"), maxLevel);
    var weapon = opt("calc-weapon").getAttribute("data-type") || "";
    var star = data("calc-star", "att");
    var hasStar = opt("calc-star").value !== "";
    var attack = num("calc-base-att") + num("calc-weapon-att") + num("calc-extra-att");
    var punch = skill === 1 && !hasStar;
    var d = mtHeroDamage({attack: attack, skill: skill, skillLevel: skillLevel,
      damageBase: data("calc-skill", "base"), damagePerLevel: data("calc-skill", "per"),
      starAttack: hasStar ? star : 0, punch: punch});
    var mob = data("calc-mob", "level");
    var o = mtHitOdds(d.x, mob, num("calc-acc"), num("calc-crit"));
    var notes = [];
    if (punch) { notes.push("Without stars a claw punches for 1 damage."); }
    if (skill > 0 && maxLevel > 0 && skillLevel < 1) { notes.push("Pick a skill level from 1."); }
    if ((skill === 1 || skill === 2 || skill === 7 || skill === 10) && weapon !== "claw") {
      notes.push("This skill needs a claw.");
    }
    if ((skill === 16 || skill === 23) && weapon !== "dagger") {
      notes.push("This skill needs a dagger.");
    }
    out("attack", fmt(attack + (MT_STAR_SKILLS[skill] && hasStar ? star : 0)));
    out("base", fmt(d.x));
    out("range", fmt(d.lo) + " to " + fmt(d.hi));
    out("critical", fmt(d.lo * 2) + " to " + fmt(d.hi * 2));
    out("evade", pct(o.evade));
    out("miss", o.missLo === o.missHi ? pct(o.missLo) : pct(o.missLo) + " to " + pct(o.missHi));
    out("mean", fmt(o.mean));
    var m = mtMobDamage(mob, num("calc-def"));
    out("taken", fmt(m[0]) + " to " + fmt(m[1]));
    out("avoid", pct(mtAvoidChance(num("calc-avoid"), mob)));
    out("notes", notes.join(" "));
  }
  f.addEventListener("input", function (ev) {
    if (ev.target.name === "calc-level") { fillFromLevel(); }
    if (ev.target.name === "calc-weapon") { fillFromWeapon(); }
    run();
  });
  f.addEventListener("change", function (ev) {
    if (ev.target.name === "calc-level") { fillFromLevel(); }
    if (ev.target.name === "calc-weapon") { fillFromWeapon(); }
    run();
  });
  f.addEventListener("submit", function (ev) { ev.preventDefault(); run(); });
  fillFromLevel();
  fillFromWeapon();
  run();
})();
"""

CALC_CSS = """
.calc { display: grid; gap: var(--s4); margin: var(--s4) 0 var(--s5); padding: var(--s4);
  border: 1px solid var(--line); border-radius: var(--radius); background: var(--raised); }
.calc fieldset { border: 0; margin: 0; padding: 0; display: grid; gap: var(--s3);
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 12rem), 1fr)); }
.calc legend { font-weight: 700; padding: 0; margin-bottom: var(--s2); }
.calc label { display: grid; gap: var(--s1); font-size: 0.92rem; }
.calc input, .calc select { font: inherit; min-height: 40px; width: 100%; padding: 4px 8px;
  color: var(--fg); background: var(--bg); border: 1px solid var(--line-strong);
  border-radius: 6px; }
.calc dl { display: grid; gap: var(--s2) var(--s4); margin: 0;
  grid-template-columns: repeat(auto-fit, minmax(min(100%, 14rem), 1fr)); }
.calc dt { color: var(--muted); font-size: 0.9rem; }
.calc dd { margin: 0; font-weight: 700; font-variant-numeric: tabular-nums; }
.calc .calc-notes { margin: 0; color: var(--muted); }
"""


def _level_table(ctx: Context) -> dict[str, list[int]]:
    out = {}
    for e in ctx.kb.entities("level"):
        lv = e.get("level")
        if isinstance(lv, int) and 1 <= lv <= 99:
            out[str(lv)] = [
                int(e.get(k) or 0) for k in ("attack", "accuracy", "defense", "avoidability")
            ]
    return out


def _attr(name: str, value: Any) -> str:
    return f' data-{name}="{t.esc(str(value))}"'


def _option(value: str, label_markup: str, attrs: str = "", selected: bool = False) -> str:
    sel = " selected" if selected else ""
    return f'<option value="{t.esc(value)}"{attrs}{sel}>{label_markup}</option>'


def _game_select(name: str, label: str, options: list[str]) -> str:
    """A select whose option texts are game names (marked as game text)."""
    return (
        f'<label>{t.esc(label)}<select name="{name}" data-verbatim lang="en">'
        + "".join(options)
        + "</select></label>"
    )


def _num_input(name: str, label: str, value: int, lo: int = 0, hi: int = 9999) -> str:
    return (
        f'<label>{t.esc(label)}<input type="number" name="{name}" inputmode="numeric"'
        f' min="{lo}" max="{hi}" value="{value}"></label>'
    )


def _stat_min(e: Entity, stat: str) -> int:
    stats = e.get("stats")
    if isinstance(stats, Mapping) and isinstance(stats.get(stat), Mapping):
        return int(stats[stat].get("min") or 0)
    return 0


def calculator(ctx: Context) -> str:
    """The damage calculator's markup: a form filled from the knowledge base (levels,
    weapons, stars, attack skills, monsters), the outputs and the script. Hidden without
    JavaScript, where a note says the rules give the same numbers."""
    levels = _level_table(ctx)
    weapons = sorted(
        (e for e in ctx.kb.entities("item") if e.get("weapon_type") in ("claw", "dagger")),
        key=lambda e: (str(e.get("weapon_type")), e.get("req_level") or 0, name_of(e)),
    )
    weapon_opts = [_option("", "no weapon", _attr("att", 0) + _attr("type", ""))]
    first_dagger = next((e for e in weapons if e.get("weapon_type") == "dagger"), None)
    for e in weapons:
        attrs = _attr("att", _stat_min(e, "attack")) + _attr("type", e.get("weapon_type"))
        text = f"{t.esc(name_of(e))} ({t.esc(str(e.get('weapon_type')))})"
        weapon_opts.append(_option(str(e["id"]), text, attrs, e is first_dagger))
    stars = sorted(
        (
            e
            for e in ctx.kb.entities("item")
            if e.get("category") == "throwing star" or (e.get("unknown_84") or 0) > 0
        ),
        key=lambda e: (int(e.get("attack") or 0), name_of(e)),
    )
    star_opts = [_option("", "no stars", _attr("att", 0))]
    for e in stars:
        star_opts.append(
            _option(str(e["id"]), t.esc(name_of(e)), _attr("att", e.get("attack") or 0))
        )
    skill_opts = []
    for sid in ATTACK_SKILLS:
        e = ctx.kb.get("skill", str(sid))
        if e is None:
            continue
        base = e.get("base") if isinstance(e.get("base"), Mapping) else {}
        per = e.get("per_level") if isinstance(e.get("per_level"), Mapping) else {}
        attrs = (
            _attr("base", base.get("damage") or 0)
            + _attr("per", per.get("damage") or 0)
            + _attr("max", e.get("max_level") or 0)
        )
        skill_opts.append(_option(str(sid), t.esc(name_of(e)), attrs, sid == 0))
    mobs = sorted(
        (e for e in ctx.kb.entities("monster") if isinstance(e.get("level"), int)),
        key=lambda e: (e["level"], name_of(e)),
    )
    mob_opts = [
        _option(
            str(e["id"]),
            f"{t.esc(name_of(e))} (Lv. {e['level']})",
            _attr("level", e["level"]),
            i == 0,
        )
        for i, e in enumerate(mobs)
    ]
    start = levels.get("10", [0, 0, 0, 0])
    hero = (
        "<fieldset><legend>Hero</legend>"
        + _num_input("calc-level", "Level", 10, 1, 99)
        + _num_input("calc-base-att", "ATT from the level", start[0])
        + _num_input("calc-acc", "ACC", start[1])
        + _num_input("calc-def", "DEF", start[2])
        + _num_input("calc-avoid", "AVOID", start[3])
        + _num_input("calc-crit", "Critical rate (%)", 2, 0, 100)
        + "</fieldset>"
    )
    gear = (
        "<fieldset><legend>Weapon and attack</legend>"
        + _game_select("calc-weapon", "Weapon", weapon_opts)
        + _num_input("calc-weapon-att", "Weapon ATT", 0)
        + _num_input("calc-extra-att", "Other ATT (equipment, buffs)", 0)
        + _game_select("calc-star", "Throwing stars", star_opts)
        + _game_select("calc-skill", "Skill", skill_opts)
        + _num_input("calc-skill-level", "Skill level", 1, 0, 30)
        + "</fieldset>"
    )
    target = (
        "<fieldset><legend>Target</legend>"
        + _game_select("calc-mob", "Monster", mob_opts)
        + "</fieldset>"
    )
    outputs = (
        "<dl>"
        "<dt>ATT used</dt><dd data-out=attack></dd>"
        "<dt>Adjusted base (x)</dt><dd data-out=base></dd>"
        "<dt>Damage per hit</dt><dd data-out=range></dd>"
        "<dt>Critical hit</dt><dd data-out=critical></dd>"
        "<dt>Monster evades</dt><dd data-out=evade></dd>"
        "<dt>Hit misses</dt><dd data-out=miss></dd>"
        "<dt>Mean damage per attempt</dt><dd data-out=mean></dd>"
        "<dt>Damage taken on contact</dt><dd data-out=taken></dd>"
        "<dt>Hero avoids contact</dt><dd data-out=avoid></dd>"
        "</dl>"
        '<p class="calc-notes" data-out=notes aria-live="polite"></p>'
    )
    return (
        f"<style>{CALC_CSS}</style>\n"
        '<form class="calc" id="damage-calculator" aria-label="Damage calculator"'
        f" data-levels='{t.esc(json.dumps(levels, separators=(',', ':')))}' hidden>"
        + hero
        + gear
        + target
        + outputs
        + "</form>\n"
        '<noscript><p class="muted">The calculator needs JavaScript; the rules above give'
        " the same numbers by hand.</p></noscript>\n"
        f"<script>{CALC_JS}{CALC_UI_JS}</script>\n"
    )


# --- training spots ----------------------------------------------------------------------------

# Maps left out of the training table: kills there give no EXP or the map is passed once.
SKIP_NO_SAVE = frozenset({"tutorial", "boss", "Monster Carnival"})
BAND = 10


@dataclass
class Spot:
    """A map as a place to train: its normal monsters and what they give."""

    map_id: str
    area_id: str | None
    monsters: list[tuple[str, int]]  # (monster id, spawn count)
    spawns: int
    level_lo: int
    level_hi: int
    mean_level: float
    exp: int  # EXP for one kill of every spawn
    hp: int  # HP of every spawn
    exp_per_hp: float = field(init=False)

    def __post_init__(self) -> None:
        self.exp_per_hp = self.exp / self.hp if self.hp else 0.0

    @property
    def band(self) -> int:
        """The first level of the spot's band: 1, 11, 21 and so on."""
        return max(0, (round(self.mean_level) - 1) // BAND) * BAND + 1


def _first_id(refs: Any) -> str | None:
    if isinstance(refs, list):
        for r in refs:
            if isinstance(r, Mapping) and r.get("id") is not None:
                return str(r["id"])
    return None


def _is_normal(mob: Entity | None) -> bool:
    """A normal monster: not a boss, not a bomb, and not one of the story monsters that get
    the HUD's boss bar."""
    return mob is not None and mob.get("class", "normal") == "normal" and not mob.get("boss_bar")


def training_spots(kb) -> list[Spot]:
    """Every map with normal monsters (not a tutorial, boss or Monster Carnival map), from the
    knowledge base's ``map.spawns`` and the monsters' level, HP and EXP; best EXP per HP
    first within each band."""
    out = []
    for m in kb.entities("map"):
        if m.get("no_save") in SKIP_NO_SAVE or m.get("boss_map"):
            continue
        mons: dict[str, int] = {}
        for s in m.get("spawns") or []:
            if not isinstance(s, Mapping):
                continue
            mob = kb.get("monster", str(s.get("id")))
            if not _is_normal(mob):
                continue
            if not all(isinstance(mob.get(k), int) for k in ("level", "hp", "exp")):
                continue
            mons[str(s["id"])] = mons.get(str(s["id"]), 0) + int(s.get("count") or 0)
        mons = {k: v for k, v in mons.items() if v > 0}
        if not mons:
            continue
        rows = [(kb.get("monster", k), n) for k, n in mons.items()]
        spawns = sum(n for _, n in rows)
        levels = [mob["level"] for mob, _ in rows]
        out.append(
            Spot(
                str(m["id"]),
                _first_id(m.get("area")),
                sorted(mons.items(), key=lambda kv: (-kv[1], int(kv[0]) if kv[0].isdigit() else 0)),
                spawns,
                min(levels),
                max(levels),
                sum(mob["level"] * n for mob, n in rows) / spawns,
                sum(mob["exp"] * n for mob, n in rows),
                sum(mob["hp"] * n for mob, n in rows),
            )
        )
    out.sort(key=lambda s: (s.band, -s.exp_per_hp, s.map_id))
    return out


def _map_cell(ctx: Context, map_id: str) -> t.Cell:
    m = ctx.kb.get("map", map_id)
    key = name_of(m) if m is not None else map_id
    num = t.esc(map_id.replace("-", "."))
    return t.markup_cell(f'{ctx.ref("map", map_id)} <span class="muted">{num}</span>', key)


def _area_cell(ctx: Context, area_id: str | None) -> t.Cell:
    if area_id is None or ctx.kb.get("area", area_id) is None:
        return t.markup_cell("")
    return t.markup_cell(ctx.ref("area", area_id), name_of(ctx.kb.get("area", area_id)))


def _mob_list(ctx: Context, mons: Sequence[tuple[str, int]]) -> str:
    return ", ".join(f"{ctx.ref('monster', k)} {t.esc('×')} {n}" for k, n in mons)


def training_tables(ctx: Context) -> str:
    """The training-spot tables, one per level band, generated from the knowledge base."""
    spots = training_spots(ctx.kb)
    if not spots:
        return '<p class="muted">The knowledge base lists no maps with monsters.</p>\n'
    cols = [
        t.Column("Map"),
        t.Column("Area"),
        t.Column("Monsters", "Normal monsters and their spawn points"),
        t.Column("Levels", "Monster levels", numeric=True),
        t.Column("Spawns", "Spawn points of normal monsters", numeric=True),
        t.Column("EXP per HP", "EXP of all spawns over their HP", numeric=True),
        t.Column("EXP per clear", "EXP for one kill of every spawn", numeric=True),
    ]
    parts = []
    bands = sorted({s.band for s in spots})
    for band in bands:
        rows = []
        ids = []
        for s in (s for s in spots if s.band == band):
            lv = str(s.level_lo) if s.level_lo == s.level_hi else f"{s.level_lo}-{s.level_hi}"
            rows.append(
                [
                    _map_cell(ctx, s.map_id),
                    _area_cell(ctx, s.area_id),
                    t.markup_cell(_mob_list(ctx, s.monsters)),
                    t.Cell(t.esc(lv), s.mean_level),
                    t.num_cell(s.spawns),
                    t.Cell(t.esc(f"{s.exp_per_hp:.3f}"), round(s.exp_per_hp, 6)),
                    t.num_cell(s.exp),
                ]
            )
            ids.append(f"spot-{t.anchor_id(s.map_id)}")
        title = f"Monster levels {band} to {band + BAND - 1}"
        parts.append(
            t.section(title, t.table(cols, rows, ids, filter=False), f"levels-{band}", level=3)
        )
    return "\n".join(parts)


# --- meso making -------------------------------------------------------------------------------


@dataclass
class MesoRow:
    """What one kill of a monster is worth in meso, on average: the meso bag and the sell
    price of each drop times its real odds (before the hero's level bonus)."""

    monster_id: str
    level: int
    hp: int
    meso_lo: int
    meso_hi: int
    meso_mean: float  # including the 80% drop roll
    drops_mean: float
    best_item: str | None
    maps: list[str]

    @property
    def total(self) -> float:
        return self.meso_mean + self.drops_mean

    @property
    def per_100_hp(self) -> float:
        return self.total * 100 / self.hp if self.hp else 0.0


def meso_rows(kb) -> list[MesoRow]:
    """Every normal monster with its expected meso per kill (meso and sold drops), best
    first. Drop odds are the knowledge base's ``percent`` (the real odds of the game's 15-bit
    random numbers); sell prices are the item table's ``price``."""
    out = []
    for e in kb.entities("monster"):
        if not _is_normal(e) or not isinstance(e.get("hp"), int):
            continue
        meso = e.get("meso") if isinstance(e.get("meso"), Mapping) else {}
        lo, hi = int(meso.get("min") or 0), int(meso.get("max") or 0)
        pct = float(meso.get("percent") or 0) / 100
        meso_mean = (lo + hi) / 2 * pct if hi else 0.0
        drops_mean = 0.0
        best, best_v = None, 0.0
        for d in e.get("drops") or []:
            if not isinstance(d, Mapping):
                continue
            item = kb.get("item", str(d.get("id")))
            if item is None:
                continue
            v = float(item.get("price") or 0) * float(d.get("percent") or 0) / 100
            drops_mean += v
            if v > best_v:
                best, best_v = str(d["id"]), v
        maps = [str(s["id"]) for s in e.get("spawns_on") or [] if isinstance(s, Mapping)]
        out.append(
            MesoRow(
                str(e["id"]),
                int(e.get("level") or 0),
                e["hp"],
                lo,
                hi,
                meso_mean,
                drops_mean,
                best,
                maps,
            )
        )
    out.sort(key=lambda r: (-r.total, r.monster_id))
    return out


def where_markup(ctx: Context, map_ids: Sequence[str], limit: int = 3) -> str:
    """The first ``limit`` maps, grouped by area: ``Area: map, map; Area: map``, all linked."""
    groups: dict[str | None, list[str]] = {}
    for m in map_ids[:limit]:
        e = ctx.kb.get("map", m)
        if e is None:
            continue
        groups.setdefault(_first_id(e.get("area")), []).append(ctx.ref("map", m))
    parts = []
    for area, refs in groups.items():
        head = f"{ctx.ref('area', area)}: " if area and ctx.kb.get("area", area) else ""
        parts.append(head + ", ".join(refs))
    return "; ".join(parts)


def meso_table(ctx: Context) -> str:
    rows_ = meso_rows(ctx.kb)
    if not rows_:
        return '<p class="muted">The knowledge base lists no monsters.</p>\n'
    boxes = defaults.any_thumb(ctx, (("monster", r.monster_id) for r in rows_))
    cols = [
        t.Column("Monster"),
        t.Column("Level", numeric=True),
        t.Column("HP", numeric=True),
        t.Column("Meso bag", "The amount when the bag drops (80% of kills)"),
        t.Column("Best drop", "The drop worth most on average when sold"),
        t.Column("Meso per kill", "Mean meso and sale value of the drops per kill", numeric=True),
        t.Column("Per 100 HP", "Meso per kill for every 100 HP the monster has", numeric=True),
        t.Column("Found on"),
    ]
    rows = []
    for r in rows_:
        bag = f"{t.number(r.meso_lo)}-{t.number(r.meso_hi)}" if r.meso_hi else "none"
        if r.best_item:
            item = ctx.kb.get("item", r.best_item)
            sell = t.number(int((item or {}).get("price") or 0))
            best = (
                f'{ctx.ref("item", r.best_item)} <span class="muted">sells for {sell} meso</span>'
            )
        else:
            best = ""
        maps = where_markup(ctx, r.maps)
        if len(r.maps) > 3:
            maps += f' <a href="{ctx.href("monster", r.monster_id)}">and more</a>'
        rows.append(
            [
                defaults.entry_cell(ctx, "monster", r.monster_id, placeholder=boxes),
                t.num_cell(r.level),
                t.num_cell(r.hp),
                t.Cell(f'<span style="white-space: nowrap">{t.esc(bag)}</span>', r.meso_hi),
                t.markup_cell(best),
                t.Cell(t.esc(f"{r.total:,.1f}"), round(r.total, 4)),
                t.Cell(t.esc(f"{r.per_100_hp:,.1f}"), round(r.per_100_hp, 4)),
                t.markup_cell(maps),
            ]
        )
    return t.table(cols, rows)


# --- where each rule of docs/re/mechanics.md is explained -------------------------------------

# The rule sections of ``docs/re/mechanics.md`` (by their heading's anchor) and the page section
# that explains each, as (topic slug, heading anchor). A test keeps this complete.
RULE_SECTIONS = {
    "random-numbers": ("drops", "random-numbers"),
    "the-heros-stats": ("damage", "the-hero-s-stats"),
    "attacking-hit-areas-targets-and-stars": ("damage", "attacks-targets-and-stars"),
    "the-heros-damage": ("damage", "the-damage-roll"),
    "evade-miss-and-critical-hits": ("accuracy", "evade-miss-and-critical-hits"),
    "monster-damage-and-the-heros-avoid": ("accuracy", "monster-damage-and-the-hero-s-avoid"),
    "taking-damage": ("accuracy", "taking-damage"),
    "drops": ("drops", "drops"),
    "exp-and-level-up": ("exp", "level-up"),
    "monster-carnival-the-level-scaling-mode": ("monster-carnival", "level-scaling"),
    "potions-scrolls-return-scrolls-and-special-items": ("items", "upgrade-scrolls"),
    "skills-values-mp-buffs-and-passives": ("skills", "skill-values"),
    "movement": ("movement", "the-hero"),
    "death": ("death", "death"),
}
# Sections of the note that are not rules.
NOT_RULES = frozenset({"contents", "not-found", "verification"})


# --- pages ---------------------------------------------------------------------------------------

_H2 = re.compile(r'<h2 id="([^"]+)">')


def insert_after(body: str, heading_id: str, markup: str) -> str:
    """``markup`` placed at the end of the ``<h2>`` section with ``heading_id`` (before the
    next ``<h2>``); at the end of the body when there is no such heading."""
    marker = f'<h2 id="{heading_id}">'
    i = body.find(marker)
    if i < 0:
        return body + markup
    m = _H2.search(body, i + len(marker))
    j = m.start() if m else len(body)
    return body[:j] + markup + body[j:]


# Generated parts: (topic, heading the part ends, builder).
GENERATED = {
    "damage": ("calculator", calculator),
    "training-spots": ("spots-by-level", training_tables),
    "meso-making": ("meso-per-kill", meso_table),
}


def _topic_page(ctx: Context, topic: Topic) -> Page:
    rel = f"{SECTION_ID}/{topic.slug}.md"
    body = ctx.prose(rel)
    if body is None:  # a site built from other prose (the generator's own tests)
        body = '<p class="muted">This page is being written.</p>\n'
    if topic.slug in GENERATED:
        heading, build = GENERATED[topic.slug]
        body = insert_after(body, heading, build(ctx))
    return Page(
        topic_path(topic.slug),
        ctx.prose_title(rel) or topic.title,
        body,
        description=topic.blurb,
    )


def _index(ctx: Context) -> Page:
    parts = [ctx.prose(f"{SECTION_ID}/index.md") or ""]
    for group in (RULES, SYSTEMS, GUIDES):
        items = [
            f'<li><a href="{ctx.url(topic_path(tp.slug))}">{t.esc(tp.title)}</a>:'
            f" {t.esc(tp.blurb)}</li>"
            for tp in TOPICS
            if tp.group == group
        ]
        parts.append(t.section(group, '<ul class="see-also">' + "".join(items) + "</ul>\n"))
    return Page(
        f"{SECTION_ID}/index.html",
        "Mechanics and guides",
        "\n".join(parts),
        description="How MapleStory Thief Edition works, and guides to playing it.",
    )


def pages(ctx: Context) -> Iterator[Page]:
    yield _index(ctx)
    for topic in TOPICS:
        yield _topic_page(ctx, topic)


SECTION = Section(
    SECTION_ID,
    "Mechanics and guides",
    80,
    (),
    pages,
    blurb="How the game works: damage, drops, EXP, saving, and guides for play.",
    image="item:etc:11",
)
