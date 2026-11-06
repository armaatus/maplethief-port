"""The walkthrough section of the compendium (#227) on a synthetic knowledge base.

The story order satisfies every prerequisite and links every main-story quest; a side quest
that is the only source of an item the story needs joins the story; every ending the scripts
have (each chapter end and the last boss's fight) has a section; a portal that opens only
for a quest comes after that quest; the pages are lint-clean with no broken link.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import quests as qs
from mtport.compendium.sections import walkthrough as wt
from mtport.compendium.siteapi import Context, Images, load_kb_dir
from tests.fixtures.compendium import walkthrough_kb as fx

PAGES = (
    "walkthrough/index.html",
    "walkthrough/chapter-1.html",
    "walkthrough/chapter-2.html",
    "walkthrough/endings.html",
    "walkthrough/checklist.html",
)


@pytest.fixture
def kb_dir(tmp_path: Path) -> Path:
    return fx.write_kb(tmp_path)


@pytest.fixture
def ctx(kb_dir: Path) -> Context:
    return Context(load_kb_dir(kb_dir), Images(), None)


@pytest.fixture
def built(kb_dir: Path, tmp_path: Path) -> site.Site:
    # the real site also has the quests section and the mechanics pages the walkthrough links
    pages = tmp_path / "pages"
    (pages / "mechanics").mkdir(parents=True)
    (pages / "mechanics" / "accuracy.md").write_text("# Accuracy\n\nA stub.\n", encoding="utf-8")
    return site.build_site(kb_dir, pages, None, plugins=[qs.SECTION, wt.SECTION])


def html(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def test_story_order_satisfies_every_prerequisite(ctx: Context) -> None:
    order = wt.story_order(ctx)
    assert wt.order_problems(ctx, order) == []
    # the side quest that alone gives the token the passage needs joins the story, and the
    # route finishes it before the chapter end, which comes last
    assert "1-sub-3" in order
    w = wt.walk(ctx)
    part = w.part_of["1-main-21"]
    stops = wt.part_stops(ctx, w, w.part_of["1-sub-3"]) + wt.part_stops(ctx, w, part)
    whys = [re.sub(r"<[^>]+>", "", s.why) for s in stops]
    done = next(i for i, y in enumerate(whys) if "finish Brine" in y)
    end = next(i for i, y in enumerate(whys) if "end the chapter" in y)
    assert done < end and end == len(stops) - 1 and stops[-1].then == "2-0"
    # the optional one does not
    assert "1-sub-4" not in order
    assert order.index("1-main-7") < order.index("1-main-8") < order.index("1-main-21")
    assert order.index("2-main-0") < order.index("2-main-1") < order.index("2-main-2")


def test_order_problems_catches_a_bad_order(ctx: Context) -> None:
    order = wt.story_order(ctx)
    swapped = [q for q in order if q != "1-main-7"] + ["1-main-7"]
    assert any("1-main-8: needs 1-main-7" in p for p in wt.order_problems(ctx, swapped))
    missing = [q for q in order if q != "2-main-2"]
    assert any("2-main-2: main-story quest not" in p for p in wt.order_problems(ctx, missing))


def test_gated_portals_follow_their_quest(ctx: Context) -> None:
    assert wt.route_problems(ctx) == []
    w = wt.walk(ctx)
    orbis = next(p for p in w.parts if p.slug == "orbis")
    assert wt.gate_quests(ctx, w, orbis) == ["2-main-1"]


def test_every_main_story_quest_is_linked_in_order(built: site.Site, ctx: Context) -> None:
    page = html(built, "walkthrough/index.html")
    m = re.search(r'<ol class="checklist">(.*?)</ol>', page, re.S)
    assert m
    linked = re.findall(r'href="[^"]*quests/([^"#/]+)\.html', m.group(1))
    order = wt.story_order(ctx)
    assert linked == order
    mains = [q["id"] for q in fx.KB["quest"] if q["main_story"]]
    assert set(mains) <= set(linked)


def test_every_ending_has_a_section(built: site.Site, ctx: Context) -> None:
    page = html(built, "walkthrough/endings.html")
    ends = [e for e in fx.KB["event"] if e["type"] == "chapter_end"]
    found = wt.endings(ctx)
    assert len(found) == len(ends) + 1  # every chapter end, and the last boss's fight
    for e in found:
        assert f'id="{e.anchor}"' in page, e.anchor
    for ev in ends:
        assert any(e.event == ev["id"] for e in found)
    assert found[-1].event == "chapter2-NestMap-boss" and found[-1].title == "The ending"


def test_chapter_pages_have_parts_and_point_of_no_return(built: site.Site) -> None:
    ch1 = html(built, "walkthrough/chapter-1.html")
    for heading in ("Route", "Quests", "Level and gear", "What carries over"):
        assert f">{heading}</h3>" in ch1, heading
    assert "Point of no return" in ch1
    # the missable side quest is listed before the chapter end
    assert 'id="before-the-chapter-ends"' in ch1 and "1-sub-4" in ch1
    ch2 = html(built, "walkthrough/chapter-2.html")
    assert ">Boss</h3>" in ch2 and "monster/2.html" in ch2
    # where every NPC and monster is: the route names the maps
    assert "map/2-2.html" in ch2 and "map/1-1.html" in ch1


def test_checklist_lists_missables_with_last_chance(built: site.Site) -> None:
    page = html(built, "walkthrough/checklist.html")
    row = re.search(r"<tr[^>]*>(?:(?!</tr>).)*quests/1-sub-4\.html.*?</tr>", page, re.S)
    assert row and "the end of chapter 1" in row.group(0)


def test_pages_exist_lint_clean_and_link(built: site.Site) -> None:
    for p in PAGES:
        assert p in built.pages, p
    problems = []
    for p in PAGES:
        problems += prose_lint.lint_text(html(built, p), kind="html", path=p)
    assert problems == []
    assert site.check_links(built.files) == [], site.check_links(built.files)
