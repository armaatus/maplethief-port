"""The Mechanics and guides section (#224) on the real knowledge base (local only: needs
work/ipa/): every page builds with every reference resolved, every rule section is on its
page, the calculator offers the game's attack skills, weapons, stars and monsters, and the
guide tables have a row for every training spot and normal monster.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import mechanics as mech
from mtport.compendium.siteapi import PAGES_DIR, load_kb_dir
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory):
    out = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    return out, site.build_site(out, REPO / PAGES_DIR)


def _page(built, slug: str) -> str:
    return built[1].files[f"mechanics/{slug}.html"].decode("utf-8")


def test_every_page_builds_and_links_resolve(built) -> None:
    _, s = built
    for t in mech.TOPICS:
        assert f"mechanics/{t.slug}.html" in s.files
    mine = {k: v for k, v in s.files.items() if k.startswith("mechanics/")}
    problems = [p for p in site.check_links(s.files) if p.split(":")[0] in mine]
    assert problems == []


def test_every_rule_section_is_on_its_page(built) -> None:
    for rule, (slug, anchor) in mech.RULE_SECTIONS.items():
        assert f'id="{anchor}"' in _page(built, slug), rule


def test_calculator_offers_the_games_data(built) -> None:
    html = _page(built, "damage")
    assert html.count('name="calc-skill"') == 1
    assert html.count("data-max=") == len(mech.ATTACK_SKILLS)
    kb = load_kb_dir(built[0])
    weapons = [e for e in kb.entities("item") if e.get("weapon_type") in ("claw", "dagger")]
    assert html.count('data-type="claw"') + html.count('data-type="dagger"') == len(weapons)
    assert html.count("data-level=") == len(kb.entities("monster"))


def test_training_and_meso_tables_cover_the_knowledge_base(built) -> None:
    kb = load_kb_dir(built[0])
    spots = mech.training_spots(kb)
    assert len(spots) >= 40
    html = _page(built, "training-spots")
    assert sum(html.count(f'id="spot-{s.map_id}"') for s in spots) == len(spots)
    rows = mech.meso_rows(kb)
    assert {r.monster_id for r in rows} == {
        str(e["id"]) for e in kb.entities("monster") if mech._is_normal(e)
    }


def test_the_worked_examples_hold_with_real_skills(built) -> None:
    # Double Step is the note's level 5 example: damage base 60, 5 a level.
    kb = load_kb_dir(built[0])
    ds = kb.get("skill", "3")
    d = mech.hero_damage(40, 3, 5, ds["base"]["damage"], ds["per_level"]["damage"])
    assert (d.x, d.lo, d.hi) == (56, 41, 73)


def test_monster_carnival_is_unreachable(built) -> None:
    kb = load_kb_dir(built[0])
    carnival = [e for e in kb.entities("event") if e.get("type") == "carnival"]
    assert carnival and not any(e.get("used") for e in carnival)
    for ticket in ("etc:47", "etc:48", "etc:49"):
        e = kb.get("item", ticket)
        for rel in ("dropped_by", "sold_by", "rewarded_by_quests", "traded_for_in", "crafted_by"):
            assert not e.get(rel), (ticket, rel)


def test_prose_lint_is_clean_on_the_built_pages(built, tmp_path: Path) -> None:
    from mtport import prose_lint

    findings = []
    for path, data in built[1].files.items():
        if path.startswith("mechanics/"):
            findings += prose_lint.lint_text(data.decode("utf-8"), kind="html")
    assert findings == []
