"""The World section (#221) on the real bundle (local only: needs work/ipa/).

90 map pages, 11 area pages and a page per event; every named portal's target shown or
marked conditional; every map reached from the start or listed with its reason; five map
pages checked against the map viewer's reading of the same files (mtport.mapview_render).
"""

from __future__ import annotations

import re
from collections import Counter
from pathlib import Path

import pytest

from mtport import mapview_render as mvr
from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.sections import world
from mtport.compendium.siteapi import PAGES_DIR, load_kb_dir

pytestmark = pytest.mark.real_assets

MANIFEST = Path("build/compendium/site/img/manifest.json")
# Maps no warp reaches from the start (docs/ANALYSIS.md, Unused scripts; kb.md).
UNREACHED = {"0-5", "2-11", "4-5", "4-8", "4-16", "4-18", "6-3", "7-6", "8-5", "8-6", "10-4"}
# The five pages compared with the map viewer: a town, the start town with an NPC warp, a
# field with a hidden portal, a boss map, and a field with a hidden portal to a side map.
VIEWER_MAPS = ("0-0", "2-0", "2-4", "3-8", "9-3")


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory) -> tuple[site.Site, object]:
    out = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    manifest = MANIFEST if MANIFEST.is_file() else None
    s = site.build_site(out, PAGES_DIR, manifest, plugins=[world.SECTION])
    return s, load_kb_dir(out)


def html(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def text(markup: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", markup)).strip()


def rows_of(page: str, section_id: str) -> list[str]:
    m = re.search(rf'<h2 id="{section_id}">.*?</section>', page, re.S)
    if not m:
        return []
    return re.findall(r"(<tr[^>]*>.*?</tr>)", m.group(0), re.S)[1:]


def test_page_counts_and_no_missing_pages(built) -> None:
    s, kb = built
    paths = set(s.files)
    assert len([p for p in paths if p.startswith("map/")]) == 90
    assert len([p for p in paths if p.startswith("area/")]) == 11
    assert len([p for p in paths if p.startswith("event/")]) == 14
    missing, counts = site.completeness(s)
    assert missing == []
    assert counts["map"] == (90, 90) and counts["area"] == (11, 11)
    assert counts["warp"] == (173, 173) and counts["event"] == (14, 14)
    img = MANIFEST.parent if MANIFEST.is_file() else None
    assert site.check_links(s.files, img) == []


def test_every_named_portal_target_is_shown_or_conditional(built) -> None:
    s, kb = built
    warps = {w["id"]: w for w in kb.entities("warp")}
    checked = conditional = 0
    for m in kb.entities("map"):
        page = html(s, f"map/{m['id']}.html")
        rows = rows_of(page, "connections")
        for p in m["portals"]:
            if not p["script"]:
                continue
            mine = [r for r in rows if re.match(rf"Portal {p['number']}\b", text(r))]
            assert mine, (m["id"], p["number"])
            for tg in p["targets"]:
                w = warps.get(tg["script"].replace("/", "-") + "-1")
                assert w is not None and w["target_map"] == tg["map"]
                hit = [
                    r
                    for r in mine
                    if f'href="{tg["map"]}.html"' in r and f'id="{world.warp_anchor(w["id"])}"' in r
                ]
                assert hit, (m["id"], p["number"], tg["map"])
                if w["conditional"]:
                    assert all(re.search(r"conditional ?: \S", text(r)) for r in hit), m["id"]
                    conditional += 1
                else:
                    assert all("always" in text(r) for r in hit)
                checked += 1
    assert checked > 150 and conditional > 10


def test_every_map_is_reached_or_listed_with_a_reason(built) -> None:
    s, kb = built
    reached = world.reached_from(kb, world.start_maps(kb))
    reasons = world.unreached_reasons(kb, reached)
    assert set(reached) | set(reasons) == {m["id"] for m in kb.entities("map")}
    assert set(reasons) == UNREACHED
    rows = rows_of(html(s, "world/index.html"), "unreached")
    listed = {re.search(r'href="\.\./map/([0-9-]+)\.html"', r).group(1) for r in rows}
    assert listed == UNREACHED
    assert all(len(text(r)) > 20 for r in rows)
    for mid in ("0-5", "4-18", "6-3"):
        assert "Monster Carnival room" in reasons[mid]


def test_default_slot_maps_say_so(built) -> None:
    s, kb = built
    slots = [m["id"] for m in kb.entities("map") if m["name_source"] == "default slot"]
    assert len(slots) == 15
    for mid in slots:
        assert "a default slot of the name table" in html(s, f"map/{mid}.html")


@pytest.mark.parametrize("mid", VIEWER_MAPS)
def test_map_page_matches_the_map_viewer(built, ipa_dir: Path, mid: str) -> None:
    """The page's portals, NPCs and monster spawn points equal the map viewer's objects of
    the same map (mtport.mapview_render reads the .obj with its own loader)."""
    s, kb = built
    app = ipa_dir / "Payload" / "MapleThief.app"
    ids, _problems, _orphans = mvr.discover(app)
    a, n = (int(x) for x in mid.split("-"))
    lm = mvr.load_map(mvr.Assets(app), next(i for i in ids if (i.area, i.map_no) == (a, n)))
    page = html(s, f"map/{mid}.html")
    # portals: every portal object of the viewer has its row, hidden ones say so
    rows = [text(r) for r in rows_of(page, "connections")]
    for o in lm.objects:
        if o["category"] in ("portal", "hidden_portal"):
            label = f"Portal {o['kind']}" + (
                " (hidden)" if o["category"] == "hidden_portal" else ""
            )
            assert any(r.startswith(label) for r in rows), (mid, label)
    viewer_portals = {o["kind"] for o in lm.objects if o["category"] in ("portal", "hidden_portal")}
    page_portals = {int(m) for r in rows for m in re.findall(r"^Portal (\d+)", r)}
    assert page_portals == viewer_portals
    # NPCs: one row per NPC object
    npcs = [o for o in lm.objects if o["category"] == "npc"]
    assert len(rows_of(page, "npcs")) == len(npcs)
    # monsters: spawn points per monster
    viewer = Counter(o["mob_name"] for o in lm.objects if o["category"] == "mob")
    got = {}
    for r in rows_of(page, "monsters"):
        cells = re.findall(r"<td[^>]*>(.*?)</td>", r, re.S)
        got[text(cells[0])] = int(text(cells[-1]).replace(",", ""))
    assert got == dict(viewer)
    # hidden portals get their note
    if any(o["category"] == "hidden_portal" for o in lm.objects):
        assert "Hidden portals" in page
