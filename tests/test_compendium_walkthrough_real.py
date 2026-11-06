"""The walkthrough section (#227) on the real knowledge base (local only: needs work/ipa/).

The story order links every main-story quest and satisfies every prerequisite; every portal
on the route that opens only for a quest comes after that quest; every ending the scripts
have (the tutorial's end, two chapter ends and the ending after the last boss) has a
section; the pages are lint-clean with no broken link.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import walkthrough as wt
from mtport.compendium.siteapi import Context, Images, load_kb_dir

pytestmark = pytest.mark.real_assets

PAGES_DIR = Path(__file__).resolve().parents[1] / "docs" / "compendium" / "pages"


@pytest.fixture(scope="module")
def kb_dir(real_kb: kbm.KB, tmp_path_factory: pytest.TempPathFactory) -> Path:
    out = tmp_path_factory.mktemp("kb") / "kb"
    kbm.write(real_kb, out)
    return out


@pytest.fixture(scope="module")
def ctx(kb_dir: Path) -> Context:
    return Context(load_kb_dir(kb_dir), Images(), PAGES_DIR)


@pytest.fixture(scope="module")
def built(kb_dir: Path) -> site.Site:
    # every section, as the real build has them: the walkthrough links into all of them
    return site.build_site(kb_dir, PAGES_DIR, None)


def test_story_order_satisfies_every_prerequisite(ctx: Context) -> None:
    w = wt.walk(ctx)
    assert wt.order_problems(ctx, w.order) == []
    mains = {str(q["id"]) for q in ctx.kb.entities("quest") if q.get("main_story")}
    assert mains <= set(w.order)
    # the side quests the main story's path needs: gates, chapter ends and item sources
    for qid in ("1-sub-7", "1-sub-8", "1-sub-11", "2-sub-2", "2-sub-37", "3-sub-0"):
        assert qid in w.order, qid
    # every part has story quests, and each quest is in exactly one part
    assert [p.slug for p in w.parts] == [p.slug for p in wt.PARTS]
    assert sum(len(v) for v in w.story.values()) == len(w.order)


def test_gated_portals_follow_their_quest(ctx: Context) -> None:
    assert wt.route_problems(ctx) == []
    w = wt.walk(ctx)
    gated = {q for p in w.parts for q in wt.gate_quests(ctx, w, p)}
    # the guard of the Ant Tunnel, the Orbis Tower, the ruined garden and the boss maps
    assert {"1-sub-7", "2-sub-2", "2-sub-37", "1-main-18", "3-main-7"} <= gated


def test_every_main_story_quest_is_linked_on_the_index(built: site.Site, ctx: Context) -> None:
    page = built.files["walkthrough/index.html"].decode("utf-8")
    m = re.search(r'<ol class="checklist">(.*?)</ol>', page, re.S)
    assert m
    linked = re.findall(r'href="[^"]*quests/([^"#/]+)\.html', m.group(1))
    assert linked == wt.walk(ctx).order


def test_every_ending_has_a_section(built: site.Site, ctx: Context) -> None:
    page = built.files["walkthrough/endings.html"].decode("utf-8")
    found = wt.endings(ctx)
    ends = {str(e["id"]) for e in ctx.kb.entities("event") if e.get("type") == "chapter_end"}
    assert {e.event for e in found} == ends | {"chapter3-BalogMap-boss"}
    assert len(found) == 4
    for e in found:
        assert f'id="{e.anchor}"' in page, e.anchor
    assert wt.final_boss(ctx)[0] == "69"


def test_pages_lint_clean_and_link(built: site.Site) -> None:
    pages = [p for p in built.files if p.startswith("walkthrough/") and p.endswith(".html")]
    assert sorted(pages) == sorted(
        f"walkthrough/{n}.html"
        for n in ("index", "chapter-1", "chapter-2", "chapter-3", "endings", "checklist")
    )
    problems = []
    for p in pages:
        problems += prose_lint.lint_text(built.files[p].decode("utf-8"), kind="html", path=p)
    assert problems == []
    assert [b for b in site.check_links(built.files) if b.startswith("walkthrough/")] == []
    # no prose file under walkthrough/ is left over as a page of its own
    assert not any(p.startswith("walkthrough/") and p.endswith(".md") for p in built.files)
