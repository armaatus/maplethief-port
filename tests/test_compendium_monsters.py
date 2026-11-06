"""The Monsters section of the compendium (#218) on a synthetic knowledge base."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium import templates as t
from mtport.compendium.sections import monsters as mon
from tests.fixtures.compendium import monsters_kb as mfx
from tests.fixtures.compendium import site_kb as fx


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = fx.write_kb(tmp_path, mfx.KB)
    pages = fx.write_prose(tmp_path, mfx.PROSE)
    return site.build_site(kb, pages, None, plugins=[mon.SECTION])


def html(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def section_of(page: str, anchor: str) -> str:
    start = page.index(f'id="{anchor}"')
    end = page.find("</section>", start)
    return page[start:end]


ROW = re.compile(r"<tr data-i=\"\d+\"[^>]*>(.*?)</tr>", re.S)
HREF = re.compile(r'href="([^"]+)"')
PCT = re.compile(r'<span class="pct"[^>]*>([^<]+)</span>')
PER = re.compile(r'<span class="per">\(([^)]+)\)</span>')


def drop_rows(page: str) -> list[tuple[str, str, str]]:
    """``(item page, real chance, the table's value)`` of each row of the page's drop table."""
    start = page.index('<div class="drops">')
    table = page[start : page.index("</table>", start)]
    out = []
    for row in ROW.findall(table):
        href = HREF.search(row).group(1)
        pct = PCT.search(row).group(1)
        per = PER.search(row)
        out.append((href.rsplit("/", 1)[-1], pct, per.group(1) if per else ""))
    return out


def expected_drops(e: dict) -> list[tuple[str, str, str]]:
    out = []
    for r in sorted(e["drops"], key=lambda r: (r["slot"], r["id"])):
        page = r["id"].replace(":", "-") + ".html"
        # the knowledge base's real odds (four decimals) agree with the page's exact ones
        assert abs(mon.real_odds(r["chance"], r["per"]) - r["percent"]) < 5e-5
        out.append((page, t.percent_text(r["percent"]), f"{r['chance']}/{r['per']}"))
    for r in e.get("meso_drops") or []:
        out.append((r["id"].replace(":", "-") + ".html", t.percent_text(r["percent"]), ""))
    return out


def test_every_monster_has_a_page_and_the_lists_exist(built):
    s = built
    for e in mfx.MONSTERS:
        assert f"monster/{e['id']}.html" in s.pages
        assert s.registry[("monster", e["id"])] == (f"monster/{e['id']}.html", "")
    for p in ("monsters/index.html", "monsters/by-area.html", "monsters/bosses.html"):
        assert p in s.pages
    missing, counts = site.completeness(s)
    assert [m for m in missing if ":monster:" in m or "monster:" in m] == []
    assert counts["monster"] == (len(mfx.MONSTERS), len(mfx.MONSTERS))
    assert site.check_links(s.files) == []


def test_every_drop_table_matches_the_knowledge_base(built):
    for e in mfx.MONSTERS:
        page = html(built, f"monster/{e['id']}.html")
        if not e["drops"] and not e.get("meso_drops"):
            assert '<div class="drops">' not in page
            continue
        assert drop_rows(page) == expected_drops(e), e["id"]
    # the 15-bit odds beside the face value: 300 in 100,000 is really 0.92%
    slug = html(built, "monster/1.html")
    assert ("equip-1.html", "0.92%", "300/100000") in drop_rows(slug)
    assert "out of 32,768" in section_of(slug, "drops")


def test_field_monster_page_sections_in_the_style_guide_order(built):
    page = html(built, "monster/1.html")
    order = [
        page.index(f'id="{a}"') for a in ("drops", "locations", "quests", "behaviour", "notes")
    ]
    assert order == sorted(order)
    assert "made-up monster" in page  # our paragraph is the lead
    loc = section_of(page, "locations")
    assert "1.1" in loc and "3.7" in loc and "6 spawn points on 2 maps" in loc
    quests = section_of(page, "quests")
    # the hunting quest, the quest that needs its drop, a recipe and a trade, each with where
    # the NPC stands
    assert "defeat 3" in quests
    assert "8 × " in quests and "3 × " in quests and "50 × " in quests
    assert quests.count("map/1-1.html") >= 4
    assert "Recipes and trades" in quests
    # trivia: the cut name and the other monster of the same name
    notes = section_of(page, "notes")
    assert "16 bytes" in notes and 'href="9.html"' in notes and "the level 70 one" in notes


def test_real_odds_of_the_15_bit_random_numbers():
    # docs/re/mechanics.md: n in 100,000 is really n in 32,768; n in 1,000 about n in 993
    assert mon.real_odds(10, 100000) == 10 * 100 / 32768
    assert t.percent_text(mon.real_odds(10, 100000)) == "0.031%"
    assert mon.real_odds(930, 1000) == (32 * 930 + 768) * 100 / 32768
    assert mon.real_odds(0, 1000) == 0.0 and mon.real_odds(1000, 1000) == 100.0


def test_contact_damage_follows_the_mechanics_example():
    # docs/re/mechanics.md: a level 20 mob against defense 15 deals 24 to 44
    assert mon.contact_damage(20, 15) == (24, 44)
    # d negative: Rand_Range(1, L / 2), times 0.9
    assert mon.contact_damage(10, 200) == (0, 4)
    assert mon.cdiv(-7, 2) == -3 and mon.cdiv(7, 2) == 3


def test_monster_carnival_scaling_and_its_sign_slip():
    # docs/re/mechanics.md's example: H = 40
    assert mon.carnival(60, 1000, 40) == (50, 1500.0)  # pulled halfway down, 150 % HP
    assert mon.carnival(30, 1000, 40) == (25, 1000.0)  # the sign slip: further down, HP kept
    assert mon.carnival(40, 1000, 40) == (40, 1000.0)
    assert mon.carnival(31, 10, 40) == (26, 10.0)  # (31 - 40) >> 1 is -5: rounds down
    assert mon.hero_h({"attack": 51, "defense": 52}) == 51


def test_monster_page_shows_the_scaling_for_three_heroes(built):
    page = html(built, "monster/1.html")
    notes = section_of(page, "notes")
    # level 20, HP 300; heroes of levels 20, 40, 60 have H = 26, 51, 76
    rows = ROW.findall(notes)
    assert len(rows) == 3
    assert ">17<" in rows[0] and ">4<" in rows[1] and ">-8<" in rows[2]
    assert "sign slip" in notes and "cannot be reached in play" in notes
    boss = section_of(html(built, "monster/64.html"), "notes")
    assert ">22,500<" in boss  # 15,000 HP above every H: 150 %


def test_boss_pages(built):
    page = html(built, "monster/64.html")
    order = [
        page.index(f'id="{a}"')
        for a in ("how-to-reach-the-fight", "the-fight", "rewards", "drops", "locations")
    ]
    assert order == sorted(order)
    reach = section_of(page, "how-to-reach-the-fight")
    assert "event/chapter1-LanternMap-boss.html" in reach  # the World section's event
    assert "map/3-8.html" in reach and "map/3-7.html" in reach  # boss map and the way in
    assert "quest/1-main-18.html" in reach and "npc/7.html" in reach
    fight = section_of(page, "the-fight")
    assert "makeAniMelodyIn" in fight and "makeAniMelodyEnd" in fight
    assert 'href="65.html"' in fight  # its bomb
    rewards = section_of(page, "rewards")
    assert ">1,000<" in rewards and ">2,500<" in rewards and "item/etc-4.html" in rewards
    assert "neither is paid" in rewards
    assert "none of them drop in play" in section_of(page, "drops")
    box = page[page.index('<aside class="infobox"') : page.index("</aside>")]
    assert "Recommended level" in box and "Lv. 20 or above" in box  # the slug on 3.7
    assert "not given" in box
    assert 'href="../monsters/bosses.html"' in page  # breadcrumb


def test_boss_whose_event_no_map_runs(built):
    page = html(built, "monster/68.html")
    reach = section_of(page, "how-to-reach-the-fight")
    assert "event/chapter2-FrostMap-boss.html" in reach
    assert "never named by a map object" in reach
    assert "not been confirmed in play" in section_of(page, "the-fight")


def test_bomb_and_unplaced_monster(built):
    bomb = html(built, "monster/65.html")
    assert "summon" in bomb and 'href="64.html"' in section_of(bomb, "locations")
    lost = html(built, "monster/9.html")
    assert "No map&#x27;s spawn list places it" in lost or "No map's spawn list places it" in lost


def test_list_pages(built):
    index = html(built, "monsters/index.html")
    for col in ("Level", "HP", "EXP", "EXP per HP", "Meso"):
        assert f">{col}</th>" in index
    assert ">0.100<" in index  # 30 EXP / 300 HP
    assert 'id="level-scaling"' in index
    by_area = html(built, "monsters/by-area.html")
    assert by_area.count('<h2 id="area-') == 3
    assert 'id="nowhere"' in by_area
    bosses = html(built, "monsters/bosses.html")
    table = bosses[bosses.index("<table") : bosses.index("</table>")]
    assert "monster/64.html" in table and "monster/68.html" in table
    assert "monster/65.html" not in table and "monster/1.html" not in table


def test_the_built_pages_pass_the_prose_lint(built):
    for path, data in built.files.items():
        if path.startswith(("monster/", "monsters/")):
            findings = prose_lint.lint_text(data.decode("utf-8"), kind="html")
            assert findings == [], (path, findings)
