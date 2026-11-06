"""The Skills and jobs section (#222) on the real knowledge base (local only: needs work/ipa/).

24 skill pages, 3 job pages, the growth and builds pages; every per-level value against
``skill.dat`` itself; every filled detail text without a placeholder; every job advancement's
conditions against the quest extraction and its ``setClassUp`` call in the script's paths.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import datatables as dt
from mtport import prose_lint
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import skills as sk
from tests.fixtures.compendium import site_kb as fx

pytestmark = pytest.mark.real_assets

PAGES = Path(__file__).resolve().parents[1] / "docs" / "compendium" / "pages"


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory) -> site.Site:
    root = tmp_path_factory.mktemp("skills-site")
    kb_dir = fx.write_kb(root, {k: list(v.values()) for k, v in real_kb.entities.items()})
    return site.build_site(kb_dir, PAGES, None, plugins=[sk.SECTION])


def test_counts_and_completeness(built: site.Site) -> None:
    skill_pages = [p for p in built.pages if p.startswith("skill/")]
    assert len(skill_pages) == 24
    jobs = {"skills/rogue.html", "skills/assassin.html", "skills/bandit.html"}
    assert jobs | {"skills/index.html", "skills/growth.html", "skills/builds.html"} <= set(
        built.pages
    )
    missing, counts = site.completeness(built)
    assert missing == []
    assert counts["skill"] == (24, 24)
    assert counts["level"] == (100, 100)
    assert counts["job_advancement"] == (11, 11)
    for kind in ("skill", "level", "job_advancement"):
        assert all(
            path.startswith(("skill/", "skills/"))
            for (k, _), (path, _) in built.registry.items()
            if k == kind
        )
    assert site.check_links(built.files) == []


def test_every_level_matches_skill_dat(real_kb: kbm.KB, ipa_dir: Path) -> None:
    table = dt.load_dir(ipa_dir / "Payload" / "MapleThief.app" / "English" / "data")["skill"]
    checked = 0
    for s in table.records:
        e = real_kb.entities["skill"][str(s.id)]
        rows = sk.level_rows(e)  # also checks the KB's own levels and placeholders
        assert len(rows) == s.max_level
        for r in rows:
            n = r.level
            assert r.damage == s.damage_base + (n - 1) * s.damage_per_level
            assert r.damage_hit == s.damage_base + n * s.damage_per_level
            assert r.time == s.time_base + (n - 1) * s.time_per_level
            assert r.etc_raw == s.etc_base + (n - 1) * s.etc_per_level
            assert r.mp == s.mp_base + (n - 1) * s.mp_per_level
            assert r.detail and not re.search(r"@(dmg|time|etc|mp)", r.detail)
            checked += 1
    assert checked == sum(s.max_level for s in table.records) > 0


def test_pages_show_every_level(built: site.Site, real_kb: kbm.KB) -> None:
    for eid, e in real_kb.entities["skill"].items():
        markup = built.files[f"skill/{eid}.html"].decode("utf-8")
        rows = sk.level_rows(e)
        effect = markup.split('id="effect"')[-1] if rows else ""
        effect = effect[: effect.find("</table>")]
        for r in rows:
            assert f'<tr data-i="{r.level - 1}"' in effect
            assert f'<span data-verbatim lang="en">{r.detail}</span>' in effect.replace(
                "&#x27;", "'"
            )
        verbatim = re.findall(r"<span data-verbatim[^>]*>(.*?)</span>", effect)
        assert not any("@" in v for v in verbatim)


def test_advancements_match_the_quest_extraction(built: site.Site, real_kb: kbm.KB) -> None:
    index = built.files["skills/index.html"].decode("utf-8")
    advs = real_kb.entities["job_advancement"]
    assert len(advs) == 11
    for aid, a in advs.items():
        start = index.find(f'id="job_advancement-{aid}"')
        assert start >= 0, aid
        row = index[start : index.find("</tr>", start)]
        cond_cell = re.findall(r"<td[^>]*>(.*?)</td>", row, re.S)[-1]
        assert len(a["conditions"]) == 1
        assert cond_cell.count("; ") + 1 == len(a["conditions"][0])
        for c in a["conditions"][0]:
            if c.get("native") == "getLevel":
                assert f"level {c['value']} or higher" in cond_cell
        # the script's own call: setClassUp(job, advancement) on one of its paths
        dialogue = real_kb.entities["dialogue"][a["script"]]
        want = ["getHeroClass()" if a["keeps_job"] else a["job"], a["advancement"]]
        calls = [
            row["args"]
            for node in dialogue["nodes"].values()
            for path in node["paths"]
            for row in path["do"]
            if row.get("call") == "setClassUp"
        ]
        assert want in calls, aid
        assert a["given_by"] == [{"kind": "npc", "id": "3", "map": "0-0"}]
    # the first advancement of chapter 1, in words
    first = index[index.find('id="job_advancement-1-1-0"') :]
    first = first[: first.find("</tr>")]
    assert "level 10 or higher" in first
    assert "Kerning City" in first


def test_prose_lint_is_clean(built: site.Site) -> None:
    for path, data in built.files.items():
        if path.startswith(("skills/", "skill/")) and path.endswith(".html"):
            assert prose_lint.lint_text(data.decode("utf-8"), kind="html") == [], path
