"""The Skills and jobs section of the compendium (#222) on a synthetic knowledge base."""

from __future__ import annotations

import copy
import html as htmllib
import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import skills as sk
from mtport.compendium.siteapi import BuildError
from tests.fixtures.compendium import site_kb as fx
from tests.fixtures.compendium import skills_kb as sfx


def build(tmp_path: Path, kb: dict | None = None) -> site.Site:
    kb_dir = fx.write_kb(tmp_path, kb or sfx.KB)
    pages = fx.write_prose(tmp_path, sfx.PROSE)
    return site.build_site(kb_dir, pages, None, plugins=[sk.SECTION])


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    return build(tmp_path)


def page(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def text(markup: str) -> str:
    return htmllib.unescape(re.sub(r"<[^>]+>", "", markup)).strip()


def table_rows(markup: str, after: str = "") -> list[list[str]]:
    """The body rows of the first table after ``after``, as cell texts."""
    i = markup.find(after) if after else 0
    assert i >= 0, after
    body = markup[i:]
    body = body[: body.find("</table>")]
    rows = []
    for tr in re.findall(r'<tr data-i="\d+"[^>]*>(.*?)</tr>', body, re.S):
        rows.append([text(c) for c in re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)])
    return rows


def test_section_pages_and_completeness(built: site.Site) -> None:
    expected = {
        "skills/index.html",
        "skills/rogue.html",
        "skills/assassin.html",
        "skills/bandit.html",
        "skills/growth.html",
        "skills/builds.html",
    } | {f"skill/{e['id']}.html" for e in sfx.SKILLS}
    assert expected <= set(built.pages)
    missing, counts = site.completeness(built)
    assert missing == []
    assert counts["skill"] == (len(sfx.SKILLS), len(sfx.SKILLS))
    assert counts["level"] == (len(sfx.LEVELS), len(sfx.LEVELS))
    assert counts["job_advancement"] == (len(sfx.ADVANCEMENTS), len(sfx.ADVANCEMENTS))
    assert built.registry[("level", "3")] == ("skills/growth.html", "level-3")
    assert built.registry[("job_advancement", "1-1-0")] == (
        "skills/index.html",
        "job_advancement-1-1-0",
    )
    assert site.check_links(built.files) == []


def test_every_level_value_is_base_plus_per_level(built: site.Site) -> None:
    for e in sfx.SKILLS:
        if not e["max_level"]:
            assert 'id="effect"' not in page(built, f"skill/{e['id']}.html")
            continue
        b, p = e["base"], e["per_level"]
        markup = page(built, f"skill/{e['id']}.html")
        heads = [
            text(h) for h in re.findall(r"<th[^>]*>(.*?)</th>", markup.split('id="effect"')[1])
        ]
        rows = table_rows(markup, 'id="effect"')
        assert [int(r[0]) for r in rows] == list(range(1, e["max_level"] + 1))
        for r in rows:
            n = int(r[0])
            cell = dict(zip(heads, r, strict=False))
            mp = b["mp"] + (n - 1) * p["mp"]
            assert cell.get("MP", "0") == str(mp)
            if "Window" in cell:  # an attack skill: as the window shows it, and one level more
                assert cell["Window"] == f"{b['damage'] + (n - 1) * p['damage']}%"
                assert cell["Hits with"] == f"{b['damage'] + n * p['damage']}%"
            etc = (b["etc"] + (n - 1) * p["etc"]) // 10
            title = sk.ETC_COLUMNS.get(e["id"], ("",))[0]
            if b["etc"] or p["etc"]:
                assert re.sub(r"[^0-9]", "", cell[title]) == str(etc)
            if e["type"] == "buff":
                ticks = b["time"] + (n - 1) * p["time"]
                assert cell["Duration"] == f"{ticks / 20:g} s"
            assert "@" not in cell["In-game text"]
            assert cell["In-game text"]


def test_level_rows_checks_the_knowledge_base() -> None:
    e = copy.deepcopy(sfx.SKILLS[1])
    rows = sk.level_rows(e)
    assert [(r.level, r.damage, r.damage_hit, r.mp) for r in rows][:2] == [
        (1, 30, 41, 6),
        (2, 41, 52, 8),
    ]
    e["levels"][1]["damage"] += 1
    with pytest.raises(BuildError, match="damage"):
        sk.level_rows(e)
    e = copy.deepcopy(sfx.SKILLS[1])
    e["levels"][0]["detail"]["en"] = "Spend MP @mp"
    with pytest.raises(BuildError, match="placeholder"):
        sk.level_rows(e)
    assert sk.level_rows(sfx.SKILLS[0]) == []


def test_etc_is_in_tenths_and_ticks_are_seconds(built: site.Site) -> None:
    flurry = table_rows(page(built, "skill/16.html"), 'id="effect"')
    assert [r[0] for r in flurry] == ["1", "2", "3"]
    assert "3 combo" in flurry[0][-1]  # 35 tenths
    focus = page(built, "skill/12.html")
    rows = table_rows(focus, 'id="effect"')
    assert rows[0][1:4] == ["+10", "25 s", "+8"]  # ATT, duration, ACC
    assert "Duration 500" in rows[0][-1]  # the window counts ticks
    assert "ticks, 20 to a second" in focus


def test_skill_points_rule() -> None:
    assert [sk.skill_points(n) for n in (2, 9, 10, 29, 30, 99)] == [0, 0, 1, 1, 2, 2]
    assert sk.skill_points_total(29) == 20
    assert sk.skill_points_total(99) == 160
    assert sk.etc_value(35) == 3 and sk.etc_value(-10) == -10


def test_advancement_conditions_match_the_knowledge_base(built: site.Site) -> None:
    index = page(built, "skills/index.html")
    for adv in sfx.ADVANCEMENTS:
        anchor = f'id="job_advancement-{adv["id"]}"'
        assert anchor in index
        row = table_rows(index[index.find(anchor) - 20 :])[0]
        conds = row[-1].split("; ")
        assert len(conds) == len(adv["conditions"][0])
        for c, shown in zip(adv["conditions"][0], conds, strict=True):
            if c["native"] == "getLevel":
                assert shown == f"level {c['value']} or higher"
        assert row[0] == str(adv["chapter"])
        assert "Old Tamsin in Hollow Square (Mossy Hollow)" in row
    first = table_rows(index[index.find('id="job_advancement-1-1-0"') - 20 :])[0][-1]
    assert first == (
        "level 10 or higher; The Trial in progress; 1 free slot in the equipment inventory"
    )
    assert "<code>someNative(7) == 2</code>" in index  # an unknown term stays as the script has it
    # each job page shows its own advancements; the later ones belong to both branches
    assassin = page(built, "skills/assassin.html")
    assert "job advancement made" in assassin and "level 60 or higher" in assassin
    assert "level 10 or higher" not in assassin
    assert "level 10 or higher" in page(built, "skills/rogue.html")


def test_job_pages(built: site.Site) -> None:
    assassin = page(built, "skills/assassin.html")
    assert assassin.count('id="tier-') == 3  # tiers 1, 2, 3 of this synthetic tree
    assert "Tin Claw" in assassin and "Tin Knife" not in assassin
    assert "Pebble" in assassin  # throwing stars
    # where to get it: the shop's NPC, map, area and chapters; never an unopened shop
    assert "Sold by Old Tamsin for 500 meso in Hollow Square (Mossy Hollow, from chapter 2)" in (
        text(assassin)
    )
    bandit = page(built, "skills/bandit.html")
    assert "Not sold, crafted, dropped or given by a quest." in bandit
    assert "Flurry" in bandit and "Long Pebble" not in bandit
    assert "Twin Pebbles" in bandit  # the Rogue's skills are in every window


def test_growth_and_builds(built: site.Site) -> None:
    growth = page(built, "skills/growth.html")
    rows = table_rows(growth, 'id="level-table"')
    assert len(rows) == len(sfx.LEVELS)
    by_level = {int(r[0]): r for r in rows}
    assert by_level[10][-1] == "1" and by_level[9][-1] == "0"
    assert by_level[3][1] == "90" and by_level[4][2] == "140"  # need, total EXP
    assert 'id="level-12"' in growth
    builds = page(built, "skills/builds.html")
    assert "160 points in all" in builds


def test_prose_lint_is_clean_on_every_page(built: site.Site) -> None:
    for path, data in built.files.items():
        if path.startswith(("skills/", "skill/")) and path.endswith(".html"):
            assert prose_lint.lint_text(data.decode("utf-8"), kind="html") == [], path


def test_section_replaces_the_default() -> None:
    assert sk.SECTION.id == "skills"
    assert set(sk.SECTION.kinds) == {"skill", "level", "job_advancement"}
    secs = site.all_sections([sk.SECTION])
    assert [s for s in secs if s.id == "skills"] == [sk.SECTION]
