"""The interactive world map (#232) on a synthetic world: the generated JSON, its links, the
text list, the deep links, the layout and the generator's hooks it uses.

Every name and number is made up (tests/fixtures/compendium/world_kb.py, plus a recipe, a
trade, a quest and a training spot added here); nothing comes from the game.
"""

from __future__ import annotations

import json
import posixpath
import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import _worldmap as wm
from mtport.compendium.sections import world
from mtport.compendium.siteapi import BuildError, Context, Images, Page, Section, load_kb_dir
from tests.fixtures.compendium import site_kb
from tests.fixtures.compendium import world_kb as fx

ART = {"path": "world/worldmap.png", "w": 338, "h": 278, "alt": "World art", "role": "render"}


def world_plus() -> dict:
    """The World fixture with what the map's panel shows: a recipe, a trade, a quest that the
    ferryman also completes, monster stats for a training spot and a map thumbnail."""
    kb = fx.world_kb()
    for m in kb["monster"]:
        m.update({"hp": 30, "exp": 6, "class": "normal"})
    kb["monster"][1].update({"class": "boss", "boss_bar": True})
    npc = kb["npc"][0]
    npc["crafts"] = [{"kind": "recipe", "id": "1-etc:5", "map": "2-0"}]
    npc["trades"] = [{"kind": "trade", "id": "1-etc:5-etc:5", "map": "2-0"}]
    npc["completes_quests"] = [{"kind": "quest", "id": "1-main-1", "map": "2-0"}]
    kb["recipe"] = [
        {
            "id": "1-etc:5",
            "kind": "recipe",
            "name": {"en": "Ticket Craft", "ko": "표 제작"},
            "chapter": 1,
            "crafted_by": [{"kind": "npc", "id": "1", "map": "2-0"}],
            "result": [{"kind": "item", "id": "etc:5", "count": 1}],
        }
    ]
    kb["trade"] = [
        {
            "id": "1-etc:5-etc:5",
            "kind": "trade",
            "name": {"en": "Ticket Swap", "ko": "표 교환"},
            "chapter": 2,
            "offered_by": [{"kind": "npc", "id": "1", "map": "2-0"}],
            "result": [{"kind": "item", "id": "etc:5", "count": 1}],
        }
    ]
    kb["quest"][1]["main_story"] = True
    return kb


def manifest(art: bool = True) -> dict:
    m = json.loads(json.dumps(fx.MANIFEST))
    m["images"]["map:2-1"].append(
        {"path": "map/2-1.thumb.png", "w": 10, "h": 5, "alt": "Map 2-1", "role": "thumb"}
    )
    if art:
        m["images"]["world:map"] = [ART]
    return m


def build(tmp_path: Path, art: bool = True, kb: dict | None = None) -> site.Site:
    kbdir = site_kb.write_kb(tmp_path, kb or world_plus())
    img = tmp_path / "site" / "img"
    img.mkdir(parents=True)
    man = manifest(art)
    (img / "manifest.json").write_text(json.dumps(man), encoding="utf-8")
    for rows in man["images"].values():
        for r in rows:
            p = img / r["path"]
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(site_kb.PNG_1X1)
    pages = site_kb.write_prose(tmp_path, {})
    s = site.build_site(kbdir, pages, img / "manifest.json", plugins=[world.SECTION])
    s.img_root = img
    return s


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    return build(tmp_path)


def data_of(s: site.Site) -> dict:
    return json.loads(s.files[wm.JSON_PATH].decode("utf-8"))


def urls(node) -> list[str]:
    """Every link in the data: the ``u`` (page) and ``s`` (picture) strings."""
    out = []
    if isinstance(node, dict):
        for k, v in node.items():
            if k in ("u", "s") and isinstance(v, str):
                out.append(v)
            else:
                out += urls(v)
    elif isinstance(node, list):
        for v in node:
            out += urls(v)
    return out


def ids_of(html: str) -> set[str]:
    return set(re.findall(r'\bid="([^"]+)"', html))


# --- the JSON ---------------------------------------------------------------------------------


def test_the_json_is_generated_from_the_kb_with_every_area_and_map(built: site.Site) -> None:
    d = data_of(built)
    assert d["schema"] == wm.SCHEMA
    assert [a["id"] for a in d["areas"]] == ["2", "3"]
    assert sorted(m["id"] for m in d["maps"]) == sorted(m["id"] for m in fx.MAPS)
    assert d["others"] == []
    by = {m["id"]: m for m in d["maps"]}
    assert by["2-0"]["n"] == {"en": "Mossy Hollow", "ko": "이끼골"}
    assert by["2-1"]["lv"] == [3, 3] and by["3-8"]["lv"] == [30, 30]
    assert by["2-8"]["save"] is False and by["2-8"]["nosave"] == "no (the tutorial)"
    assert by["3-5"]["reached"] is False and by["2-1"]["reached"] is True
    assert by["2-1"]["thumb"]["s"] == "../img/map/2-1.thumb.png"
    assert by["2-1"]["img"]["s"] == "../img/map/2-1.png"
    assert by["2-1"]["ov"]["s"] == "../img/map/2-1.svg"
    # every map of an area lies inside its canvas, on its own cell
    for a in d["areas"]:
        cells = set()
        for mid in a["maps"]:
            m = by[mid]
            assert 0 <= m["x"] and m["x"] + m["w"] <= a["w"]
            assert 0 <= m["y"] and m["y"] + m["h"] <= a["h"]
            cells.add((m["x"], m["y"]))
        assert len(cells) == len(a["maps"])
    # the used warps, with chapters and conditions
    edges = {(e["a"], e["b"]): e for e in d["edges"]}
    assert edges[("2-0", "2-1")] == {"a": "2-0", "b": "2-1", "ch": [1], "c": False, "npc": False}
    assert edges[("2-0", "3-0")]["npc"] is True and edges[("2-0", "3-0")]["c"] is True
    assert ("3-0", "3-6") not in edges  # an unused warp is no way to go


def test_the_things_to_do_on_a_map(built: site.Site) -> None:
    by = {m["id"]: m for m in data_of(built)["maps"]}
    hollow = by["2-0"]
    assert [s["kind"] for s in hollow["shops"]] == ["other items"]
    assert hollow["shops"][0]["npc"]["n"]["en"] == "Old Ferryman"
    assert hollow["shops"][0]["ch"] == [1]
    assert [(q["id"], q["ch"], q["main"], q["role"]) for q in hollow["quests"]] == [
        ("1-main-1", 1, True, ["gives", "completes"])
    ]
    assert [(r["id"], r["ch"]) for r in hollow["recipes"]] == [("1-etc:5", [1])]
    assert [(r["id"], r["ch"]) for r in hollow["trades"]] == [("1-etc:5-etc:5", [2])]
    assert hollow["npcs"][0]["roles"] == ["shop", "quest", "craft", "trade"]
    assert (hollow["npcs"][0]["x"], hollow["npcs"][0]["y"]) == (5 * 16 + 8, 6 * 16 + 8)
    # training value from the mechanics section's training spots
    assert by["2-1"]["train"] == {"exp": 24, "eph": 0.2, "band": 1, "spawns": 4}
    assert by["3-8"]["train"] is None  # a boss map is no training spot
    # a boss, its event, a hidden portal and where it leads
    altar = by["3-8"]
    assert altar["mobs"][0]["boss"] is True
    assert [e["type"] for e in altar["events"]] == ["boss"]
    path = by["2-1"]
    hidden = [p for p in path["portals"] if p["hidden"]]
    assert [(p["n"], p["to"]) for p in hidden] == [(3, ["2-2"])]
    assert [p["side"] for p in path["portals"]] == ["W", "W", "W"]  # tile (1, 2) of 40 by 20


def test_every_link_in_the_json_resolves(built: site.Site) -> None:
    d = data_of(built)
    links = urls(d)
    assert len(links) > 20
    parsed = {p: ids_of(built.files[p].decode("utf-8")) for p in built.files if p.endswith(".html")}
    for u in links:
        path, _, frag = u.partition("#")
        target = posixpath.normpath(posixpath.join("world", path))
        if target.startswith("img/"):
            assert (built.img_root / target[len("img/") :]).is_file(), u
            continue
        assert target in built.files, u
        if frag:
            assert frag in parsed[target], u


def test_the_page_inlines_the_same_json_safely(built: site.Site) -> None:
    html = built.files[wm.PAGE].decode("utf-8")
    m = re.search(
        r'<script type="application/json" id="wm-data" data-verbatim>(.*?)</script>', html, re.S
    )
    assert m
    assert json.loads(m.group(1)) == data_of(built)
    assert "\x00" not in html and "\x00" not in built.files[wm.JSON_PATH].decode("utf-8")


def test_to_json_keeps_markers_and_escapes_angle_brackets() -> None:
    text = wm.to_json({"u": "\x00Emap\x012-1\x00", "n": "</script><b>"})
    assert "\x00Emap\x012-1\x00" in text
    assert "<" not in text
    assert json.loads(text.replace("\x00", "").replace("\x01", ""))["n"] == "</script><b>"


# --- the page, the text list and the deep links -------------------------------------------------


def test_text_list_has_every_area_and_map_with_deep_link_ids(built: site.Site) -> None:
    html = built.files[wm.PAGE].decode("utf-8")
    ids = ids_of(html)
    for m in fx.MAPS:
        assert f"map={m['id']}" in ids
    for a in fx.AREAS:
        assert f"area={a['id']}" in ids
    # the same content: shop keeper, quest, recipe, trade, boss event, hidden portal
    li = re.search(r'<li id="map=2-0".*?</li></ul></li>', html, re.S).group(0)
    for words in ("Shop:", "kept by", "Quest (chapter 1)", "Recipe:", "Trade:", "chapter 2"):
        assert words in li, words
    assert "Hidden portal 3" in re.search(r'<li id="map=2-1".*?</li></ul></li>', html, re.S).group(
        0
    )
    assert "Event:" in re.search(r'<li id="map=3-8".*?</li></ul></li>', html, re.S).group(0)


def test_every_area_and_map_page_links_to_the_world_map(built: site.Site) -> None:
    for m in fx.MAPS:
        page = built.files[f"map/{m['id']}.html"].decode("utf-8")
        assert f'href="../world/map.html#map={m["id"]}">View on world map</a>' in page
    for a in fx.AREAS:
        page = built.files[f"area/{a['id']}.html"].decode("utf-8")
        assert f'href="../world/map.html#area={a["id"]}">View on world map</a>' in page
    assert 'href="map.html">Open the interactive world map</a>' in built.files[
        "world/index.html"
    ].decode("utf-8")
    assert site.check_links(built.files, built.img_root) == []


def test_home_page_card_links_the_world_map(built: site.Site) -> None:
    home = built.files["index.html"].decode("utf-8")
    assert '<a class="card-more" href="world/map.html">Open the world map</a>' in home


def test_prose_lint_is_clean_on_the_world_map_page(built: site.Site) -> None:
    findings = prose_lint.lint_text(
        built.files[wm.PAGE].decode("utf-8"), kind="html", path=wm.PAGE, names=["MapleThief"]
    )
    assert list(findings) == []


def test_the_viewer_has_its_controls_and_a_no_script_fallback(built: site.Site) -> None:
    html = built.files[wm.PAGE].decode("utf-8")
    for el in ("wm-q", "wm-lv", "wm-ch", "wm-route-btn", "wm-from", "wm-to", "wm-view",
               "wm-in", "wm-out", "wm-fit", "wm-panel", "wm-status"):  # fmt: skip
        assert f'id="{el}"' in html, el
    assert '<div class="wm" id="wm" hidden>' in html  # shown by the script
    assert "<noscript>" in html
    assert [v for v in re.findall(r'name="layer" value="(\w+)"', html)] == [k for k, _ in wm.LAYERS]
    assert "prefers-reduced-motion" in html


def test_two_builds_are_byte_identical(tmp_path: Path) -> None:
    a = build(tmp_path / "a")
    b = build(tmp_path / "b")
    assert a.files[wm.JSON_PATH] == b.files[wm.JSON_PATH]
    assert a.files[wm.PAGE] == b.files[wm.PAGE]


def test_without_the_art_the_world_is_laid_out_from_the_warps(tmp_path: Path) -> None:
    d = data_of(build(tmp_path, art=False))
    assert d["world"]["art"] is None
    spots = {s["areas"][0]: s for s in d["spots"]}
    assert set(spots) == {"2", "3"} and all(len(s["areas"]) == 1 for s in d["spots"])
    assert spots["2"]["x"] < spots["3"]["x"]  # area 3 is one warp further from the start
    assert d["world"]["w"] > spots["3"]["x"]


def test_with_the_art_areas_sit_at_their_world_spots(built: site.Site) -> None:
    d = data_of(built)
    assert d["world"]["art"]["s"] == "../img/world/worldmap.png"
    assert (d["world"]["w"], d["world"]["h"]) == wm.ART_SIZE
    spot = {s["id"]: s for s in d["spots"]}
    assert (spot[1]["x"], spot[1]["y"]) == wm.SPOT_XY[1] and spot[1]["areas"] == ["2"]
    assert (spot[2]["x"], spot[2]["y"]) == wm.SPOT_XY[2] and spot[2]["areas"] == ["3"]


def test_a_map_of_no_area_is_still_in_the_text_list(tmp_path: Path) -> None:
    kb = world_plus()
    kb["map"][6]["area"] = []  # 3-5
    kb["area"][1]["maps"] = [r for r in kb["area"][1]["maps"] if r["id"] != "3-5"]
    s = build(tmp_path, kb=kb)
    d = data_of(s)
    assert [m["id"] for m in d["others"]] == ["3-5"]
    assert 'id="map=3-5"' in s.files[wm.PAGE].decode("utf-8")
    assert site.check_links(s.files, s.img_root) == []


# --- layout (pure) ----------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("tile", "side"),
    [((39, 10), "E"), ((0, 10), "W"), ((20, 0), "N"), ((20, 19), "S"), ((21, 10), "E"),
     (None, None), ((1, 2), "W")],
)  # fmt: skip
def test_portal_direction(tile, side) -> None:
    assert wm.portal_direction(tile, 40, 20) == side


def test_grid_layout_follows_portal_sides() -> None:
    cells = wm.grid_layout(
        ["a", "b", "c", "d"], [("a", "b", "E"), ("a", "c", "N"), ("d", "a", "E")], "a"
    )
    a = cells["a"]
    assert cells["b"] == (a[0] + 1, a[1])
    assert cells["c"] == (a[0], a[1] - 1)
    assert cells["d"] == (a[0] - 1, a[1])  # d's east side leads to a: d lies west of a
    assert min(c for c, _ in cells.values()) == 0 and min(r for _, r in cells.values()) == 0


def test_grid_layout_resolves_collisions_and_is_deterministic() -> None:
    links = [("a", "b", "E"), ("a", "c", "E"), ("a", "d", "E")]
    one = wm.grid_layout(["a", "b", "c", "d"], links, "a")
    assert len(set(one.values())) == 4
    a = one["a"]
    assert all(one[n][0] == a[0] + 1 for n in "bcd")  # all still east of a
    assert one == wm.grid_layout(["a", "b", "c", "d"], links, "a")


def test_grid_layout_wraps_a_long_chain_like_text() -> None:
    nodes = [f"m{i}" for i in range(10)]
    links = [(nodes[i], nodes[i + 1], "E") for i in range(9)]
    cells = wm.grid_layout(nodes, links, "m0", max_cols=4)
    assert max(c for c, _ in cells.values()) == 3
    assert len(set(cells.values())) == 10
    rows = [cells[n][1] for n in nodes]
    assert rows == sorted(rows)  # runs on downwards
    # the second row runs back to the west
    second = [n for n in nodes if cells[n][1] == rows[4]]
    assert [cells[n][0] for n in second] == sorted((cells[n][0] for n in second), reverse=True)
    for x, y in zip(nodes, nodes[1:], strict=False):  # neighbours stay next to each other
        (cx, cy), (dx, dy) = cells[x], cells[y]
        assert abs(cx - dx) + abs(cy - dy) == 1


def test_grid_layout_places_unlinked_nodes_too() -> None:
    cells = wm.grid_layout(["a", "b", "x", "y"], [("a", "b", "E")], "a")
    assert set(cells) == {"a", "b", "x", "y"} and len(set(cells.values())) == 4


def test_cell_box_and_grid_size() -> None:
    assert wm.cell_box((0, 0)) == (wm.PAD, wm.PAD, wm.TILE_W, wm.TILE_H)
    w, h = wm.grid_size([(0, 0), (2, 1)])
    assert w == 2 * wm.PAD + 3 * wm.TILE_W + 2 * wm.GAP_X
    assert h == 2 * wm.PAD + 2 * wm.TILE_H + wm.GAP_Y


def test_world_layout_groups_areas_by_spot() -> None:
    spots, size = wm.world_layout(["4", "5", "9"], {"4": 4, "5": 4, "9": 5}, [], art=True)
    assert size == wm.ART_SIZE
    assert [(s["id"], s["areas"]) for s in spots] == [(4, ["4", "5"]), (5, ["9"])]
    spots, _ = wm.world_layout(["4", "5", "9"], {"4": 4, "5": 4, "9": 5}, [("4", "9")], art=False)
    x = {s["areas"][0]: s["x"] for s in spots}
    assert x["4"] < x["9"] and len(spots) == 3


@pytest.mark.parametrize(
    ("levels", "hero", "fit"),
    [(None, 10, "none"), ((20, 30), 10, "hard"), ((15, 30), 10, "good"), ((1, 5), 20, "easy"),
     ((1, 10), 20, "good")],
)  # fmt: skip
def test_level_fit(levels, hero, fit) -> None:
    assert wm.level_fit(levels, hero) == fit


# --- the generator's hooks -----------------------------------------------------------------------


def _plugin(files: dict[str, str]) -> Section:
    def pages(ctx: Context):
        for path, text in files.items():
            ctx.add_file(path, text.replace("@", ctx.href("monster", "1")))
        yield Page("extra/index.html", "Extra", "<p>Extra page.</p>")

    return Section("extra", "Extra", 95, (), pages)


def test_add_file_resolves_markers_relative_to_the_file_unescaped(tmp_path: Path) -> None:
    kb = site_kb.write_kb(tmp_path)
    pages = site_kb.write_prose(tmp_path)
    plug = _plugin({"extra/data/x.json": '{"u": "@", "q": "a&b"}'})
    s = site.build_site(kb, pages, None, plugins=[plug])
    doc = json.loads(s.files["extra/data/x.json"])
    assert doc == {"u": "../../monster/1.html", "q": "a&b"}


def test_add_file_refuses_pages_and_collisions(tmp_path: Path) -> None:
    ctx = Context(load_kb_dir(site_kb.write_kb(tmp_path)), Images(), None)
    with pytest.raises(BuildError):
        ctx.add_file("x/page.html", "")
    ctx.add_file("x/a.json", "{}")
    with pytest.raises(BuildError):
        ctx.add_file("x/a.json", "{}")
    with pytest.raises(BuildError):
        ctx.add_file("../out.json", "{}")
    pages = site_kb.write_prose(tmp_path)
    with pytest.raises(BuildError):
        site.build_site(tmp_path / "kb", pages, None, plugins=[_plugin({"site.js": "x"})])
