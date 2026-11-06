"""The World section (#221) on a synthetic world: pages, portals, reachability, diagrams."""

from __future__ import annotations

import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import world
from mtport.compendium.siteapi import Context, Images, load_kb_dir
from tests.fixtures.compendium import site_kb
from tests.fixtures.compendium import world_kb as fx


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = site_kb.write_kb(tmp_path, fx.world_kb())
    out = tmp_path / "site"
    img = out / "img"
    img.mkdir(parents=True)
    (img / "manifest.json").write_text(json.dumps(fx.MANIFEST), encoding="utf-8")
    for rows in fx.MANIFEST["images"].values():
        for r in rows:
            p = img / r["path"]
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(site_kb.PNG_1X1)
    pages = site_kb.write_prose(tmp_path, {})  # the real prose names real entities
    s = site.build_site(kb, pages, img / "manifest.json", plugins=[world.SECTION])
    s.img_root = img  # for the link check below
    return s


@pytest.fixture
def kb(tmp_path: Path):
    return load_kb_dir(site_kb.write_kb(tmp_path, fx.world_kb()))


def page(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def rows_of(html: str, section_id: str) -> list[str]:
    """The table rows of the section whose heading has ``section_id``."""
    m = re.search(rf'<h2 id="{section_id}">.*?</section>', html, re.S)
    assert m, section_id
    return re.findall(r"(<tr[^>]*>.*?</tr>)", m.group(0), re.S)[1:]  # without the header


def text(markup: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", markup)).strip()


def test_a_page_per_map_and_area_and_event_and_no_missing_pages(built: site.Site) -> None:
    for m in fx.MAPS:
        assert f"map/{m['id']}.html" in built.files
    for a in fx.AREAS:
        assert f"area/{a['id']}.html" in built.files
    for e in fx.EVENTS:
        assert world.event_path(e["id"]) in built.files
    for p in (
        "world/index.html",
        "world/maps.html",
        "world/routes.html",
        "world/warps.html",
        "world/events.html",
    ):
        assert p in built.files
    missing, counts = site.completeness(built)
    assert missing == []
    for kind in ("area", "map", "warp", "event"):
        assert counts[kind][0] == counts[kind][1] > 0
    assert site.check_links(built.files, built.img_root) == []


def test_prose_lint_is_clean_on_every_world_page(built: site.Site) -> None:
    for path, data in built.files.items():
        if not path.endswith(".html"):
            continue
        if not path.startswith(("world/", "map/", "area/", "event/")):
            continue
        assert prose_lint.lint_text(data.decode("utf-8"), kind="html") == [], path


def test_every_named_portal_target_is_shown_or_conditional(built: site.Site, kb) -> None:
    warps = {w["id"]: w for w in fx.WARPS}
    checked = 0
    for m in fx.MAPS:
        html = page(built, f"map/{m['id']}.html")
        rows = rows_of(html, "connections") if m["portals"] else []
        for p in m["portals"]:
            if not p["script"]:
                continue
            mine = [r for r in rows if text(r).startswith(f"Portal {p['number']}")]
            assert mine, (m["id"], p["number"])
            for tg in p["targets"]:
                wid = tg["script"].replace("/", "-") + "-1"
                hit = [
                    r
                    for r in mine
                    if f'href="{tg["map"]}.html"' in r and f'id="{world.warp_anchor(wid)}"' in r
                ]
                assert hit, (m["id"], p["number"], tg["map"])
                if warps[wid]["conditional"]:
                    assert all("conditional" in text(r) for r in hit)
                    assert all(re.search(r"conditional ?: \S", text(r)) for r in hit)
                else:
                    assert all("always" in text(r) for r in hit)
                checked += 1
    assert checked == 7
    # the condition is in words, with the quest linked
    row = rows_of(page(built, "map/2-8.html"), "connections")[0]
    assert "conditional : First Steps is in progress" in text(row)
    assert 'href="../quest/0-main-1.html"' in row


def test_npc_warp_and_alternatives(built: site.Site) -> None:
    rows = rows_of(page(built, "map/2-0.html"), "connections")
    npc = [r for r in rows if text(r).startswith("NPC")]
    assert len(npc) == 1
    t = text(npc[0])
    assert "Old Ferryman" in t and "Pier Town (3.0)" in t
    assert "either answer 1 is picked in the conversation; or (answer 2 is picked" in t
    assert "level 20 or more)" in t
    # the NPC table says where the NPC sends the hero and which shop it keeps
    npcs = text(rows_of(page(built, "map/2-0.html"), "npcs")[0])
    assert "Dockside Goods" in npcs and "sends the hero to Pier Town" in npcs


def test_reachability_and_reasons(kb) -> None:
    reached = world.reached_from(kb, world.start_maps(kb))
    assert world.start_maps(kb) == ["2-8"]
    assert set(reached) == {"2-8", "2-0", "2-1", "2-2", "3-0", "3-8"}
    assert reached["3-8"] == ["2-8", "2-0", "2-1", "3-8"]
    why = world.unreached_reasons(kb, reached)
    assert set(why) | set(reached) == {m["id"] for m in fx.MAPS}
    assert why["3-5"] == "no portal, NPC or script of the game leads here"
    assert "Monster Carnival room" in why["3-6"]
    assert "common/Juggler (no map object names it)" in why["3-6"]


def test_unreached_maps_are_listed_with_their_reason(built: site.Site) -> None:
    rows = rows_of(page(built, "world/index.html"), "unreached")
    listed = {re.search(r'href="\.\./map/([0-9-]+)\.html"', r).group(1): text(r) for r in rows}
    assert set(listed) == {"3-5", "3-6"}
    assert "No portal, NPC or script" in listed["3-5"]
    assert "Unused content" in page(built, "map/3-5.html")


def test_map_page_infobox_and_sections(built: site.Site) -> None:
    html = page(built, "map/2-1.html")
    # render with the overlay toggle and the double-size link
    assert 'class="ov-toggle"' in html and 'class="overlay"' in html
    assert "img/map/2-1@2x.png" in html
    box = text(re.search(r'<aside class="infobox".*?</aside>', html, re.S).group(0))
    assert "Music track 7, the field music of Mossy Vale (resumes where it stopped)" in box
    assert "Saving yes" in box and "Type Field" in box
    assert "Lv. 3" in box
    hidden = text(re.search(r'<h2 id="hidden-portals">.*?</section>', html, re.S).group(0))
    assert "not drawn" in hidden and "Portal 3" in hidden and "Secret Grotto" in hidden
    mons = text(rows_of(html, "monsters")[0])
    assert "Puddle Slime" in mons and "4" in mons
    boss = page(built, "map/3-8.html")
    box = text(re.search(r'<aside class="infobox".*?</aside>', boss, re.S).group(0))
    assert "Saving no (a boss map)" in box and "Boss map yes" in box and "the boss music" in box
    assert "probably an arrival point" in boss
    assert "../event/chapter1-GloomMap-boss.html" in boss


def test_default_slot_names_say_so(built: site.Site) -> None:
    for mid in ("2-8", "3-6"):
        html = page(built, f"map/{mid}.html")
        assert "probable: a default slot of the name table" in html
        assert "probably not the map&#x27;s own" in html or "probably not the map's own" in html
        assert "the slot is past the end of the row (synthetic)" in html


def test_event_pages(built: site.Site) -> None:
    boss = text(page(built, world.event_path("chapter1-GloomMap-boss")))
    assert "Boss fight on map 3.8, chapter 1" in boss
    assert "the requirements of Gloom Rising are not met yet" in boss
    assert "Gloomcap (level 30)" in boss
    carnival = text(page(built, world.event_path("common-Juggler-carnival-0")))
    assert "Carnival round, mode 0 (unused)" in carnival
    assert "carries 1 or more of Juggling Ticket" in carnival
    assert "no map object names it" in carnival
    end = text(page(built, world.event_path("common-portal280-chapter_end")))
    assert "the next chapter starts here" in end
    # the carnival room links its round
    assert world.event_path("common-Juggler-carnival-0").split("/")[1] in page(
        built, "map/3-6.html"
    )


def test_area_page_and_diagram(built: site.Site) -> None:
    html = page(built, "area/2.html")
    svg = re.search(r'class="world-diagram"><svg.*?</svg>', html, re.S).group(0)[22:]
    root = ET.fromstring(svg)
    links = [a.get("href") for a in root.iter("a")]
    for m in ("2-8", "2-0", "2-1", "2-2"):
        assert f"../map/{m}.html" in links
    assert "../map/3-0.html" in links  # the map of another area it connects to
    assert "Lv. 3" in html
    shops = text(rows_of(html, "shops")[0])
    assert "Dockside Goods" in shops and "Old Ferryman" in shops and "Mossy Hollow" in shops


def test_atlas_and_routes(built: site.Site) -> None:
    html = page(built, "world/index.html")
    svg = re.search(r'class="world-diagram"><svg.*?</svg>', html, re.S).group(0)[22:]
    ET.fromstring(svg)
    rows = rows_of(html, "chapters")
    assert len(rows) == 1 and "Mossy Vale" in rows[0] and "Pier Coast" in rows[0]
    routes = page(built, "world/routes.html")
    r = [text(x) for x in rows_of(routes, "routes")]
    assert any(x.startswith("Mossy Hollow Pier Town 1") for x in r)


def test_conditions_in_words(kb) -> None:
    ctx = Context(kb, Images(), None)
    c = {"native": "getQuestState", "args": [0, 1], "op": ">=", "value": 2}
    assert world.condition_markup(ctx, c, [1]).endswith("is done")
    c = {"native": "isInvenAvailable", "args": [1, 2], "op": "==", "value": 1}
    assert world.condition_markup(ctx, c) == "2 free slots in the other items inventory"
    c = {"native": "getLevel", "args": [], "op": ">", "value": 50}
    assert world.condition_markup(ctx, c) == "level more than 50"
    assert world.conditions_markup(ctx, [[]]) == "always"
    assert world.conditions_markup(ctx, []) == "never"
    assert world.chapters_text([0, 1, 2, 3]) == "all"
    assert world.chapters_text([1, 2, 3]) == "1 to 3"


def test_layered_layout_is_breadth_first() -> None:
    lay = world.layered(["a", "b", "c", "d"], [("a", "b"), ("b", "c")], "a")
    assert lay.pos == {"a": (0, 0), "b": (1, 0), "c": (2, 0), "d": (3, 0)}
