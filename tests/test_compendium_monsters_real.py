"""The Monsters section (#218) over the shipped game (local only: needs work/ipa/).

Builds the knowledge base from the game, the site from it (without images), and checks every
monster page against ``mob.dat`` read directly by :mod:`mtport.datatables` (the table
browser's parser), not through the knowledge base: the drop tables (items, the real odds
worked out here from the 15-bit random numbers, the table's own values, the meso bags) and
the infobox's level, HP and EXP.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import datatables as dt
from mtport import prose_lint
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium import templates as t
from mtport.compendium.siteapi import PAGES_DIR
from repo_paths import REPO
from tests.test_compendium_monsters import drop_rows, section_of

pytestmark = pytest.mark.real_assets

BOSSES = {"64", "66", "67", "68", "69"}


@pytest.fixture(scope="module")
def mobs(ipa_dir: Path) -> dict[str, dt.Mob]:
    data = (ipa_dir / "Payload" / "MapleThief.app" / "English" / "data" / "mob.dat").read_bytes()
    return {str(r.id): r for r in dt.parse("mob", data).records}


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory) -> site.Site:
    out = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    return site.build_site(out, REPO / PAGES_DIR, None)  # every section, as the real build


def real_percent(n: int, d: int) -> str:
    """``Rand_Chance(n, d)`` with the 15-bit ``Rand_Next``: the share of 0..32767 whose
    remainder mod ``d``, plus 1, is at most ``n``."""
    hits = sum(1 for r in range(32768) if r % d + 1 <= n)
    return t.percent_text(hits * 100 / 32768)


def expected(m: dt.Mob) -> tuple[list[tuple[str, str, str]], set[str]]:
    items = []
    if m.equip_drop_chance > 0:
        items.append(
            (
                0,
                f"equip-{m.equip_drop}.html",
                real_percent(m.equip_drop_chance, 100000),
                f"{m.equip_drop_chance}/100000",
            )
        )
    for slot in (1, 2, 3):
        n = getattr(m, f"etc_drop_{slot}_chance")
        if n > 0:
            item = getattr(m, f"etc_drop_{slot}")
            items.append((slot, f"etc-{item}.html", real_percent(n, 1000), f"{n}/1000"))
    items.sort(key=lambda x: (x[0], x[1]))
    bags = set()
    lo, hi = m.meso * 8 // 10, m.meso * 12 // 10
    if hi > 0:
        for bag, (a, b) in enumerate(((1, 99), (100, 199), (200, 1 << 30))):
            if lo <= b and hi >= a:
                bags.add(f"etc-{bag}.html")
    return [x[1:] for x in items], bags


def test_69_monster_pages_and_the_lists(built: site.Site, mobs) -> None:
    pages = [p for p in built.pages if p.startswith("monster/")]
    assert len(pages) == 69 == len(mobs)
    assert {p for p in built.pages if p.startswith("monsters/")} == {
        "monsters/index.html",
        "monsters/by-area.html",
        "monsters/bosses.html",
    }
    missing, counts = site.completeness(built)
    assert [m for m in missing if "monster:" in m] == []
    assert counts["monster"] == (69, 69)
    broken = [p for p in site.check_links(built.files) if p.split(":")[0].startswith("monster")]
    assert broken == []


def test_every_drop_table_matches_mob_dat(built: site.Site, mobs) -> None:
    for mid, m in mobs.items():
        page = built.files[f"monster/{mid}.html"].decode("utf-8")
        items, bags = expected(m)
        if not items and not bags:
            assert '<div class="drops">' not in page, mid
            continue
        rows = drop_rows(page)
        assert rows[: len(items)] == items, mid
        meso = rows[len(items) :]
        assert {r[0] for r in meso} == bags and all(r[1:] == ("80%", "") for r in meso), mid


def test_infobox_numbers_match_mob_dat(built: site.Site, mobs) -> None:
    for mid, m in mobs.items():
        page = built.files[f"monster/{mid}.html"].decode("utf-8")
        box = page[page.index('<aside class="infobox"') : page.index("</aside>")]
        assert f"<dt>Level</dt><dd>Lv. {m.level}</dd>" in box, mid
        assert f"<dt>HP</dt><dd>{m.hp:,}</dd>" in box, mid
        if m.exp:
            assert re.search(rf"<dt>EXP</dt><dd>{m.exp:,}[ <]", box), mid
        boss = "<dt>Type</dt><dd>boss</dd>" in box
        assert boss == (mid in BOSSES), mid


def test_boss_pages_name_their_fight(built: site.Site) -> None:
    for mid in BOSSES:
        page = built.files[f"monster/{mid}.html"].decode("utf-8")
        for anchor in ("how-to-reach-the-fight", "the-fight", "rewards"):
            assert f'id="{anchor}"' in page, (mid, anchor)
        assert "event/" in section_of(page, "how-to-reach-the-fight"), mid
    # Yeti's (68) boss script is never named by a map object in 1.5
    yeti = built.files["monster/68.html"].decode("utf-8")
    assert "never named by a map object" in yeti


def test_prose_lint_on_the_monster_pages(built: site.Site) -> None:
    for path, data in built.files.items():
        if path.startswith(("monster/", "monsters/")):
            assert prose_lint.lint_text(data.decode("utf-8"), kind="html") == [], path
