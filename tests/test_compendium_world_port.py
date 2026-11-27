"""Five map pages checked in the running port (#221, local only).

``play.map_page_*_synthetic`` (tests/CMakeLists.txt) starts the port from a built save on one of
the five maps the map-viewer test checks (0-0, 2-0, 2-4, 3-8, 9-3), walks the hero to one
portal, holds Up and writes where the game put it (``map_page.txt``). Each run is compared here
with that map page's Connections row for the portal in the run's chapter: an "always" row names
the map the port went to and the hero lands at the row's arrival portal; a conditional row whose
condition the start leaves unmet keeps the hero on the map, and with it met names the map; a
portal that names no script goes nowhere. Every portal row of the five pages has a run.

Skipped without work/ipa/ or without the play runs' output (build/test-output/mac-debug, or the
directory ``MT_MAP_PAGE_OUTPUT`` names): ``ctest --preset mac-debug -R map_page``.
"""

from __future__ import annotations

import os
import re
from pathlib import Path

import pytest

from mtport.compendium import kb as kbm
from mtport.compendium import site
from mtport.compendium.siteapi import PAGES_DIR, load_kb_dir
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

OUTPUT = Path(os.environ.get("MT_MAP_PAGE_OUTPUT", REPO / "build" / "test-output" / "mac-debug"))
PAGES = ("0-0", "2-0", "2-4", "3-8", "9-3")
# run (start, portal kind) -> what the start makes of the portal's gate: "open" (the hero
# leaves by it) or "shut" (the start leaves the condition unmet, or the portal has no script).
# The starts are tests/starts/map_page_<start>.json; the 2-0 and 2-4 gates are main quest 9
# and side quest 7 in chapter 1 (kb.md, Warps). 9_3_top puts the hero on the hidden portal's
# platform (the walk up from the spawn crosses mobs that outlast the bot).
RUNS = {
    ("0_0", 0): "open",
    ("0_0", 1): "open",
    ("2_0_open", 0): "open",
    ("2_0_open", 1): "open",
    ("2_0_shut", 1): "shut",
    ("2_4_open", 0): "open",
    ("2_4_open", 1): "open",
    ("2_4_open", 2): "open",
    ("2_4_open", 3): "open",
    ("2_4_shut", 2): "shut",
    ("3_8", 3): "shut",
    ("9_3", 0): "open",
    ("9_3", 1): "open",
    ("9_3_top", 3): "open",
}
LINE = re.compile(
    r"from (\d+)\.(\d+) chapter (\d+) portal (\d) (\S+) landed "
    r"(?:none|(\d+)\.(\d+) at (-?\d+) (-?\d+))$"
)
TILE = 16  # a tile is 16 px; an object's anchor is (x * 16 + 8, y * 16 + 15) (obj.md)


@pytest.fixture(scope="module")
def built(real_kb: kbm.KB, tmp_path_factory) -> tuple[site.Site, object]:
    out = tmp_path_factory.mktemp("kb")
    kbm.write(real_kb, out)
    return site.build_site(out, PAGES_DIR, None), load_kb_dir(out)


def text(markup: str) -> str:
    return re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", markup)).strip()


def chapters(cell: str) -> set[int]:
    m = re.fullmatch(r"(\d+) to (\d+)", cell)
    if m:
        return set(range(int(m.group(1)), int(m.group(2)) + 1))
    return {int(c) for c in re.findall(r"\d+", cell)}


def portal_rows(s: site.Site, mid: str) -> list[dict]:
    """The page's Connections rows of portals: kind, target map and portal, the "when" text,
    the chapters."""
    page = s.files[f"map/{mid}.html"].decode("utf-8")
    sec = re.search(r'<h2 id="connections">.*?</section>', page, re.S)
    assert sec, mid
    rows = []
    for tr in re.findall(r"<tr[^>]*>.*?</tr>", sec.group(0), re.S)[1:]:
        cells = re.findall(r"<td[^>]*>(.*?)</td>", tr, re.S)
        m = re.match(r"Portal (\d)", text(cells[0]))
        if not m:
            continue  # an NPC's warp
        target = re.search(r'href="(\d+-\d+)\.html"', cells[1]) if len(cells) > 1 else None
        arrive = re.search(r"portal (\d+)", text(cells[1])) if len(cells) > 1 else None
        rows.append(
            {
                "kind": int(m.group(1)),
                "map": target.group(1) if target else None,
                "arrive": int(arrive.group(1)) if arrive and target else None,
                "when": text(cells[2]) if len(cells) > 2 else text(cells[-1]),
                "chapters": chapters(text(cells[3])) if len(cells) > 3 else {1, 2, 3},
            }
        )
    return rows


def page_of(start: str) -> str:
    """The map a start puts the hero on: "2_0_open" is map 2-0."""
    return "-".join(start.split("_")[:2])


def result(start: str, kind: int) -> dict:
    f = OUTPUT / f"play_map_page_{start}_{kind}" / "map_page.txt"
    if not f.is_file():
        pytest.skip(f"{f}: no run (ctest --preset mac-debug -R map_page)")
    m = LINE.match(f.read_text(encoding="utf-8").strip())
    assert m, f
    g = m.groups()
    return {
        "from": f"{g[0]}-{g[1]}",
        "chapter": int(g[2]),
        "kind": int(g[3]),
        "landed": f"{g[5]}-{g[6]}" if g[5] is not None else None,
        "x": int(g[7]) if g[7] is not None else None,
    }


def test_every_portal_row_of_the_five_pages_has_a_run(built) -> None:
    s, _kb = built
    assert {page_of(start) for start, _ in RUNS} == set(PAGES)
    for mid in PAGES:
        kinds = {r["kind"] for r in portal_rows(s, mid)}
        ran = {k for (start, k) in RUNS if page_of(start) == mid}
        assert kinds == ran, mid


@pytest.mark.parametrize(("start", "kind"), sorted(RUNS), ids=lambda v: str(v))
def test_map_page_matches_the_running_port(built, start: str, kind: int) -> None:
    s, kb = built
    got = result(start, kind)
    assert got["kind"] == kind
    rows = [r for r in portal_rows(s, got["from"]) if r["kind"] == kind]
    mine = [r for r in rows if got["chapter"] in r["chapters"] or r["map"] is None]
    assert mine, (got["from"], kind, got["chapter"])
    if RUNS[(start, kind)] == "shut":
        assert got["landed"] is None, got
        # the page says why: a condition, or no script at all
        assert all(r["when"].startswith("conditional") or r["map"] is None for r in mine), mine
        return
    assert got["landed"] is not None, got
    hit = [r for r in mine if r["map"] == got["landed"]]
    assert hit, (got, mine)
    # the hero arrives at the row's arrival portal: x = its anchor + 16 (0x868D5, obj.md)
    maps = {m["id"]: m for m in kb.entities("map")}
    arrive = [p for p in maps[got["landed"]]["portals"] if p["number"] == hit[0]["arrive"]]
    assert arrive, hit
    anchor = arrive[-1]["tile"][0] * TILE + TILE // 2  # the last match wins (obj.md)
    assert abs(got["x"] - (anchor + 16)) <= TILE, (got, arrive[-1])
