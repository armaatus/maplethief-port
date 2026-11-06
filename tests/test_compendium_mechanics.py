"""The Mechanics and guides section (#224): the rules' Python mirror and the calculator's
JavaScript against the worked examples of docs/re/mechanics.md, the generated guide tables
from a synthetic knowledge base, and the section's pages.

Every name and number in the fixtures is made up; nothing comes from the game.
"""

from __future__ import annotations

import html
import json
import random
import re
import shutil
import subprocess
from pathlib import Path

import pytest

from mtport.compendium.sections import mechanics as mech
from mtport.compendium.siteapi import KB, Context, Images, Page
from repo_paths import REPO

PAGES = REPO / "docs" / "compendium" / "pages"
NOTE = REPO / "docs" / "re" / "mechanics.md"


# --- the worked examples of docs/re/mechanics.md ----------------------------------------------

# (inputs of hero_damage, expected (x, lo, hi)): the three examples of "The hero's damage".
DAMAGE_EXAMPLES = [
    (dict(attack=40, skill=0), (32, 24, 41)),  # a dagger swing at A = 40
    (dict(attack=40, skill=1, star_attack=4), (32, 24, 41)),  # basic throw, 4-attack star
    (
        dict(attack=40, skill=3, skill_level=5, damage_base=60, damage_per_level=5),
        (56, 41, 73),
    ),  # a level 5 Double Step
]


@pytest.mark.parametrize(("kwargs", "want"), DAMAGE_EXAMPLES)
def test_hero_damage_matches_the_worked_examples(kwargs, want) -> None:
    d = mech.hero_damage(**kwargs)
    assert (d.x, d.lo, d.hi) == want


def test_hit_checks_match_the_worked_example() -> None:
    # A level 30 mob, ACC 22, a 9-damage hit, critical rate 8: evade 8%, miss 6%, crit 8%.
    assert mech.evade_chance(30, 22) == 8
    assert mech.miss_chance(9, 30) == 6
    assert mech.hit_odds(32, 30, 22, 8).critical == 8
    assert mech.hit_odds(32, 30, 22, 8).evade == 8


def test_monster_damage_and_avoid_match_the_worked_example() -> None:
    # A level 20 mob against DEF 15: 24 to 44; AVOID 24 avoids 16% of the time.
    assert mech.mob_damage(20, 15) == (24, 44)
    assert mech.avoid_chance(24, 20) == 16


def test_drop_odds_match_the_worked_examples() -> None:
    # Equipment: 312 in 100000 is 312 in 32768; etc 931 in 1000 is 30560 in 32768.
    assert mech.chance_in_32768(312, 100000) == 312
    assert mech.chance_in_32768(931, 1000) == 30560
    assert mech.chance_in_32768(40, 1000) == 1320
    assert mech.chance_in_32768(50, 100) == 50 * 327 + 50


def test_edge_cases() -> None:
    assert mech.hero_damage(40, skill=1, punch=True) == mech.Damage(1, 1, 1)
    # Lucky Seven's -5 on a tiny ATT drives x below 0: it becomes 10.
    assert mech.hero_damage(1, skill=2, skill_level=1, damage_base=30, damage_per_level=11).x == 10
    assert mech.hero_damage(0).x == 0
    assert mech.miss_chance(0, 10) == 100
    assert mech.mob_damage(1, 99)[0] == 0  # a negative d rolls 1 to L / 2, times 0.9
    assert mech.avoid_chance(0, 50) == 0 and mech.avoid_chance(99, 1) == 100
    assert mech.q(-7, 2) == -3  # toward zero, as ___divsi3


# --- the calculator's JavaScript, under Node ---------------------------------------------------

NODE = shutil.which("node")


def _run_js(cases: list[dict]) -> list[dict]:
    script = (
        mech.CALC_JS
        + "\nvar cases = "
        + json.dumps(cases)
        + ";\nconsole.log(JSON.stringify(cases.map(function (c) {"
        "  var d = mtHeroDamage(c.hero);"
        "  var o = mtHitOdds(d.x, c.mob, c.acc, c.crit);"
        "  return {d: [d.x, d.lo, d.hi], o: [o.evade, o.missLo, o.missHi, o.critical, o.mean],"
        "          m: mtMobDamage(c.mob, c.def), a: mtAvoidChance(c.avoid, c.mob)};"
        "})));\n"
    )
    out = subprocess.run([NODE, "-e", script], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def _js_hero(kwargs: dict) -> dict:
    return {
        "attack": kwargs.get("attack", 0),
        "skill": kwargs.get("skill", 0),
        "skillLevel": kwargs.get("skill_level", 0),
        "damageBase": kwargs.get("damage_base", 0),
        "damagePerLevel": kwargs.get("damage_per_level", 0),
        "starAttack": kwargs.get("star_attack", 0),
        "punch": kwargs.get("punch", False),
    }


@pytest.mark.skipif(NODE is None, reason="node not installed")
def test_calculator_js_matches_the_worked_examples() -> None:
    cases = [
        {"hero": _js_hero(k), "mob": 30, "acc": 22, "crit": 8, "def": 15, "avoid": 24}
        for k, _ in DAMAGE_EXAMPLES
    ]
    cases.append(
        {"hero": _js_hero({"attack": 40}), "mob": 20, "acc": 22, "crit": 8, "def": 15, "avoid": 24}
    )
    got = _run_js(cases)
    for (_, want), g in zip(DAMAGE_EXAMPLES, got[: len(DAMAGE_EXAMPLES)], strict=True):
        assert tuple(g["d"]) == want
    assert got[0]["o"][0] == 8 and got[0]["o"][3] == 8  # evade 8%, critical 8%
    assert got[-1]["m"] == [24, 44]  # level 20 mob against DEF 15
    assert got[-1]["a"] == 16


@pytest.mark.skipif(NODE is None, reason="node not installed")
def test_calculator_js_equals_the_python_mirror() -> None:
    rnd = random.Random(224)
    skills = [(0, 0, 0), (1, 0, 0), (2, 30, 11), (3, 60, 5), (7, 20, 20), (10, 65, 31)]
    skills += [(16, 5, 6), (23, 40, 12), (5, 0, 0)]
    cases, kwargs = [], []
    for _ in range(400):
        sid, base, per = rnd.choice(skills)
        k = dict(
            attack=rnd.randint(0, 200),
            skill=sid,
            skill_level=rnd.randint(0, 20),
            damage_base=base,
            damage_per_level=per,
            star_attack=rnd.randint(0, 25),
            punch=rnd.random() < 0.05,
        )
        kwargs.append(k)
        cases.append(
            {
                "hero": _js_hero(k),
                "mob": rnd.randint(1, 150),
                "acc": rnd.randint(0, 150),
                "crit": rnd.randint(0, 40),
                "def": rnd.randint(0, 150),
                "avoid": rnd.randint(0, 120),
            }
        )
    for k, c, g in zip(kwargs, cases, _run_js(cases), strict=True):
        d = mech.hero_damage(**k)
        assert g["d"] == [d.x, d.lo, d.hi], k
        o = mech.hit_odds(d.x, c["mob"], c["acc"], c["crit"])
        assert g["o"][:4] == pytest.approx([o.evade, o.miss_lo, o.miss_hi, o.critical])
        assert g["o"][4] == pytest.approx(o.mean)
        assert tuple(g["m"]) == mech.mob_damage(c["mob"], c["def"])
        assert g["a"] == pytest.approx(mech.avoid_chance(c["avoid"], c["mob"]))


# --- the rules, page by page --------------------------------------------------------------------


def _anchor(heading: str) -> str:
    """The note's heading anchors, as GitHub makes them."""
    s = re.sub(r"[^\w\- ]", "", heading.lower())
    return s.replace(" ", "-")


def _sections(md: str) -> dict[str, str]:
    """``## `` sections of a prose file by the site's anchors, with their text."""
    from mtport.compendium.prose import heading_id

    out: dict[str, str] = {}
    cur = None
    for line in md.splitlines():
        if line.startswith("## "):
            cur = heading_id(line[3:].strip())
            out[cur] = ""
        elif cur is not None:
            out[cur] += line + "\n"
    return out


def test_every_rule_of_the_note_has_a_page_section_with_an_example() -> None:
    rules = [
        _anchor(line[3:].strip())
        for line in NOTE.read_text(encoding="utf-8").splitlines()
        if line.startswith("## ")
    ]
    rules = [r for r in rules if r not in mech.NOT_RULES]
    assert rules, "no rule sections found in docs/re/mechanics.md"
    assert sorted(rules) == sorted(mech.RULE_SECTIONS)
    slugs = {t.slug for t in mech.TOPICS}
    for rule, (slug, anchor) in mech.RULE_SECTIONS.items():
        assert slug in slugs, rule
        secs = _sections((PAGES / "mechanics" / f"{slug}.md").read_text(encoding="utf-8"))
        assert anchor in secs, f"{rule}: no section #{anchor} on {slug}"
        assert "**Example" in secs[anchor], f"{rule}: #{anchor} on {slug} has no worked example"


def test_every_rule_page_has_sources_with_addresses() -> None:
    for t in mech.TOPICS:
        if t.group not in (mech.RULES, mech.SYSTEMS):
            continue
        secs = _sections((PAGES / "mechanics" / f"{t.slug}.md").read_text(encoding="utf-8"))
        assert "sources" in secs, t.slug
        assert re.search(r"`0x[0-9A-F]{4,}`", secs["sources"]), t.slug


# --- the generated tables -----------------------------------------------------------------------


def _n(en: str) -> dict:
    return {"en": en, "ko": en + " (ko)"}


def _kb(slug_exp: int = 4) -> KB:
    monsters = [
        {
            "id": "1",
            "name": _n("Moss Slug"),
            "level": 3,
            "hp": 20,
            "exp": slug_exp,
            "class": "normal",
            "meso": {"amount": 10, "min": 8, "max": 12, "percent": 80},
            "drops": [{"kind": "item", "id": "etc:5", "percent": 50.0}],
            "spawns_on": [{"kind": "map", "id": "1-1", "count": 5}],
        },
        {
            "id": "2",
            "name": _n("Grim Lamp"),
            "level": 14,
            "hp": 100,
            "exp": 30,
            "class": "normal",
            "meso": None,
            "drops": [{"kind": "item", "id": "equip:1", "percent": 1.0}],
            "spawns_on": [{"kind": "map", "id": "1-2", "count": 2}],
        },
        {
            "id": "3",
            "name": _n("Big Bad"),
            "level": 60,
            "hp": 50000,
            "exp": 5000,
            "class": "boss",
            "boss_bar": True,
            "spawns_on": [{"kind": "map", "id": "1-3", "count": 1}],
        },
    ]
    maps = [
        {
            "id": "1-1",
            "name": _n("Mossy Field"),
            "area": [{"kind": "area", "id": "1"}],
            "spawns": [{"kind": "monster", "id": "1", "count": 5}],
            "no_save": None,
            "boss_map": False,
        },
        {
            "id": "1-2",
            "name": _n("Lamp Hill"),
            "area": [{"kind": "area", "id": "1"}],
            "spawns": [
                {"kind": "monster", "id": "2", "count": 2},
                {"kind": "monster", "id": "1", "count": 1},
            ],
            "no_save": None,
            "boss_map": False,
        },
        {
            "id": "1-3",
            "name": _n("Lair"),
            "area": [{"kind": "area", "id": "1"}],
            "spawns": [{"kind": "monster", "id": "3", "count": 1}],
            "no_save": "boss",
            "boss_map": True,
        },
        {
            "id": "1-4",
            "name": _n("Practice Yard"),
            "area": [{"kind": "area", "id": "1"}],
            "spawns": [{"kind": "monster", "id": "1", "count": 9}],
            "no_save": "tutorial",
            "boss_map": False,
        },
    ]
    items = [
        {"id": "etc:5", "name": _n("Slug Slime"), "price": 6},
        {"id": "equip:1", "name": _n("Lamp Hat"), "price": 900},
    ]
    areas = [{"id": "1", "name": _n("Mossland")}]
    return KB({"monster": monsters, "map": maps, "item": items, "area": areas})


def _ctx(kb: KB, pages: Path | None = None) -> Context:
    return Context(kb, Images(), pages)


def test_training_spots_are_computed_from_the_knowledge_base() -> None:
    spots = mech.training_spots(_kb())
    # boss room and tutorial left out; best EXP per HP first (64 / 220 above 20 / 100)
    assert [s.map_id for s in spots] == ["1-2", "1-1"]
    b, a = spots
    assert (a.spawns, a.exp, a.hp, a.level_lo, a.level_hi) == (5, 20, 100, 3, 3)
    assert a.exp_per_hp == pytest.approx(0.2)
    assert (b.spawns, b.exp, b.hp) == (3, 64, 220)
    assert b.mean_level == pytest.approx((14 * 2 + 3) / 3)
    assert (a.band, b.band) == (1, 1)
    assert a.area_id == "1"


def test_training_table_follows_the_knowledge_base_not_typed_numbers() -> None:
    html = mech.training_tables(_ctx(_kb()))
    assert "Mossy Field" in html and "Lamp Hill" in html and "Mossland" in html
    assert "0.200" in html and "Lair" not in html and "Practice Yard" not in html
    assert 'id="levels-1"' in html and 'id="spot-1-1"' in html
    # Change the knowledge base and the table changes with it.
    html2 = mech.training_tables(_ctx(_kb(slug_exp=8)))
    assert "0.400" in html2 and "0.200" not in html2
    # Rows by EXP per HP, best first within a band.
    assert html.index("Lamp Hill") < html.index("Mossy Field")


def test_meso_rows_count_the_bag_and_the_sold_drops() -> None:
    rows = {r.monster_id: r for r in mech.meso_rows(_kb())}
    assert set(rows) == {"1", "2"}  # the boss is left out
    slug = rows["1"]
    assert slug.meso_mean == pytest.approx(10 * 0.8)
    assert slug.drops_mean == pytest.approx(6 * 0.5)
    assert slug.total == pytest.approx(11.0)
    assert slug.per_100_hp == pytest.approx(55.0)
    lamp = rows["2"]
    assert lamp.meso_mean == 0 and lamp.drops_mean == pytest.approx(9.0)
    assert lamp.best_item == "equip:1"
    html = mech.meso_table(_ctx(_kb()))
    assert "Moss Slug" in html and "Lamp Hat" in html and "11.0" in html


# --- the section ---------------------------------------------------------------------------------


def _prose_dir(tmp_path: Path) -> Path:
    d = tmp_path / "pages" / "mechanics"
    d.mkdir(parents=True)
    (d / "index.md").write_text("# Mechanics and guides\n\nAbout the rules.\n", encoding="utf-8")
    for t in mech.TOPICS:
        body = f"# {t.title}\n\nA lead.\n\n"
        if t.slug in mech.GENERATED:
            body += f"## {mech.GENERATED[t.slug][0].replace('-', ' ').capitalize()}\n\nText.\n\n"
        body += "## Sources\n\n`0x1234`.\n"
        (d / f"{t.slug}.md").write_text(body, encoding="utf-8")
    return tmp_path / "pages"


def _calc_kb() -> KB:
    kb = _kb()
    kinds = {k: kb.entities(k) for k in kb.kinds()}
    kinds["level"] = [
        {
            "id": "10",
            "name": _n("Level 10"),
            "level": 10,
            "attack": 14,
            "accuracy": 13,
            "defense": 15,
            "avoidability": 12,
        },
    ]
    kinds["skill"] = [
        {"id": "0", "name": _n("Poke"), "max_level": 0, "base": {"damage": 0}, "per_level": {}},
        {
            "id": "3",
            "name": _n("Twin Poke"),
            "max_level": 15,
            "base": {"damage": 60},
            "per_level": {"damage": 5},
        },
    ]
    kinds["item"] = kinds["item"] + [
        {
            "id": "equip:9",
            "name": _n("Butter Knife"),
            "weapon_type": "dagger",
            "stats": {"attack": {"min": 8, "max": 12}},
        },
        {
            "id": "etc:11",
            "name": _n("Pebbles"),
            "category": "throwing star",
            "attack": 1,
            "unknown_84": 2,
        },
    ]
    return KB(kinds)


def test_section_pages(tmp_path: Path) -> None:
    ctx = _ctx(_calc_kb(), _prose_dir(tmp_path))
    pages = list(mech.pages(ctx))
    paths = [p.path for p in pages]
    assert paths[0] == "mechanics/index.html"
    assert paths[1:] == [f"mechanics/{t.slug}.html" for t in mech.TOPICS]
    assert all(isinstance(p, Page) and p.body for p in pages)
    by = {p.path: p for p in pages}
    dmg = by["mechanics/damage.html"].body
    # the calculator comes before Sources, with the formulas and the knowledge base's data
    assert dmg.index('id="damage-calculator"') < dmg.index('id="sources"')
    assert "function mtHeroDamage" in dmg and "Butter Knife" in dmg and "Twin Poke" in dmg
    assert 'data-att="8"' in dmg and 'data-base="60"' in dmg and 'data-per="5"' in dmg
    assert "&quot;10&quot;:[14,13,15,12]" in dmg
    # game names in the form are marked as game text
    assert re.search(r"<select name=\"calc-skill\" data-verbatim", dmg)
    spots = by["mechanics/training-spots.html"].body
    assert spots.index("Mossy Field") < spots.index('id="sources"')
    assert "Moss Slug" in by["mechanics/meso-making.html"].body
    index = by["mechanics/index.html"].body
    for t in mech.TOPICS:
        assert html.escape(t.title) in index


def test_missing_prose_still_gives_every_page(tmp_path: Path) -> None:
    # Sites built from other prose (the generator's tests) still get the section's pages.
    pages = list(mech.pages(_ctx(_calc_kb(), None)))
    assert [p.path for p in pages][1:] == [f"mechanics/{t.slug}.html" for t in mech.TOPICS]
    by = {p.path: p.body for p in pages}
    assert "being written" in by["mechanics/options.html"]
    assert 'id="damage-calculator"' in by["mechanics/damage.html"]


def test_insert_after_places_markup_at_the_end_of_a_section() -> None:
    body = '<h2 id="a">A</h2><p>a</p><h2 id="b">B</h2><p>b</p>'
    assert (
        mech.insert_after(body, "a", "X") == '<h2 id="a">A</h2><p>a</p>X<h2 id="b">B</h2><p>b</p>'
    )
    assert mech.insert_after(body, "b", "X").endswith("<p>b</p>X")
    assert mech.insert_after(body, "c", "X") == body + "X"
