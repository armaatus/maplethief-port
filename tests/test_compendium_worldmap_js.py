"""The world map viewer's pure functions (#232), run under Node when it is installed: level
fit, chapter filters, routes, search, deep links and the pan and zoom maths. The routes are
also compared with the World section's own breadth-first search on the synthetic world.

Every name and number is made up; nothing comes from the game.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from mtport.compendium.sections import _worldmap as wm
from mtport.compendium.sections import world
from tests.test_compendium_worldmap import build, data_of

NODE = shutil.which("node")
pytestmark = pytest.mark.skipif(NODE is None, reason="Node is not installed")
SCRIPT = wm.HERE / "_worldmap.js"


def run(calls: list[tuple[str, list]]) -> list:
    """Each ``(function, args)`` called on the script's exports, the results as JSON."""
    driver = (
        "const api = require(process.argv[1]);\n"
        "const calls = JSON.parse(process.argv[2]);\n"
        "process.stdout.write(JSON.stringify(calls.map(([f, args]) => {\n"
        "  const r = api[f](...args); return r === undefined ? null : r; })));\n"
    )
    out = subprocess.run(
        [NODE, "-e", driver, str(SCRIPT), json.dumps(calls)],
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(out.stdout)


def one(f: str, *args):
    return run([(f, list(args))])[0]


FIT = {"above": 5, "below": 10}
EDGES = [
    {"a": "1-0", "b": "1-1", "ch": [0, 1], "c": False},
    {"a": "1-1", "b": "1-0", "ch": [0, 1], "c": False},
    {"a": "1-1", "b": "2-0", "ch": [1], "c": True},
    {"a": "1-0", "b": "3-0", "ch": [0], "c": False},
    {"a": "3-0", "b": "2-0", "ch": [0], "c": False},
    {"a": "2-0", "b": "1-0", "ch": [0, 1], "c": False},
]


def test_level_fit_matches_python() -> None:
    cases = [
        (None, 10),
        ([20, 30], 10),
        ([15, 30], 10),
        ([1, 5], 20),
        ([1, 10], 20),
        ([3, 3], None),
    ]
    got = run([("levelFit", [lv, hero, FIT]) for lv, hero in cases])
    want = [wm.level_fit(lv, hero) if hero is not None else "none" for lv, hero in cases]
    assert got == want


def test_chapter_filters() -> None:
    got = run(
        [
            ("inChapter", [[1, 2], 1]),
            ("inChapter", [[1, 2], 3]),
            ("inChapter", [[], 3]),
            ("inChapter", [2, 2]),
            ("inChapter", [2, 3]),
            ("inChapter", [None, 0]),
            ("inChapter", [[1], None]),
            ("edgeOpen", [{"ch": [1]}, 1]),
            ("edgeOpen", [{"ch": [1]}, 0]),
            ("edgeOpen", [{"ch": []}, None]),
        ]
    )
    assert got == [True, False, True, True, False, True, True, True, False, True]


def test_routes_take_the_fewest_map_changes_in_the_chapter() -> None:
    every, ch0, ch1, none, same = run(
        [
            ("route", [EDGES, "1-0", "2-0", None]),
            ("route", [EDGES, "1-0", "2-0", 0]),
            ("route", [EDGES, "1-0", "2-0", 1]),
            ("route", [EDGES, "2-0", "3-0", 1]),
            ("route", [EDGES, "1-1", "1-1", None]),
        ]
    )
    assert every["path"] == ["1-0", "1-1", "2-0"]  # the first of two equal routes, edge order
    assert [s["c"] for s in every["steps"]] == [False, True]
    assert ch0["path"] == ["1-0", "3-0", "2-0"]
    assert ch1["path"] == ["1-0", "1-1", "2-0"]
    assert none is None
    assert same == {"path": ["1-1"], "steps": []}


def test_a_step_is_conditional_only_when_every_warp_of_it_is() -> None:
    edges = [
        {"a": "1-0", "b": "1-1", "ch": [1], "c": True},
        {"a": "1-0", "b": "1-1", "ch": [1], "c": False},
    ]
    assert one("route", edges, "1-0", "1-1", None)["steps"] == [
        {"a": "1-0", "b": "1-1", "c": False}
    ]


def test_routes_agree_with_the_world_sections_search(tmp_path: Path) -> None:
    s = build(tmp_path)
    d = data_of(s)
    from mtport.compendium.siteapi import load_kb_dir

    kb = load_kb_dir(tmp_path / "kb")
    ids = [m["id"] for m in d["maps"]]
    calls, want = [], []
    for a in ids:
        paths = world.reached_from(kb, [a])
        for b in ids:
            calls.append(("route", [d["edges"], a, b, None]))
            want.append(len(paths[b]) if b in paths else None)
    got = [None if r is None else len(r["path"]) for r in run(calls)]
    assert got == want


def test_search_places() -> None:
    data = {
        "areas": [{"id": "2", "n": {"en": "Mossy Vale", "ko": "이끼골짜기"}}],
        "maps": [
            {"id": "2-0", "num": "2.0", "n": {"en": "Mossy Hollow", "ko": "이끼골"}},
            {"id": "2-1", "num": "2.1", "n": {"en": "Dripping Path", "ko": "물길"}},
            {"id": "3-0", "num": "3.0", "n": {"en": "Pier Town", "ko": "부두마을"}},
        ],
    }
    got = run(
        [
            ("searchPlaces", [data, "mossy", 8]),
            ("searchPlaces", [data, "PATH", 8]),
            ("searchPlaces", [data, "2.1", 8]),
            ("searchPlaces", [data, "3-0", 8]),
            ("searchPlaces", [data, "물길", 8]),
            ("searchPlaces", [data, "  ", 8]),
            ("searchPlaces", [data, "o", 2]),
        ]
    )
    pick = [[(h["kind"], h["id"]) for h in r] for r in got]
    assert pick[0] == [("area", "2"), ("map", "2-0")]
    assert pick[1] == [("map", "2-1")]
    assert pick[2] == [("map", "2-1")]
    assert pick[3] == [("map", "3-0")]
    assert pick[4] == [("map", "2-1")]
    assert pick[5] == []
    assert len(pick[6]) == 2


def test_deep_links_round_trip() -> None:
    states = [
        {"map": "2-3"},
        {"map": "2-3", "open": True},
        {"area": "4"},
        {"map": "0-0", "from": "2-10", "to": "4-1", "ch": 1, "lv": 25},
        {},
    ]
    hashes = run([("formatHash", [s]) for s in states])
    assert hashes == [
        "#map=2-3",
        "#map=2-3&level=map",
        "#area=4",
        "#map=0-0&route=2-10~4-1&ch=1&lv=25",
        "",
    ]
    back = run([("parseHash", [h]) for h in hashes])
    assert back == states
    junk = one("parseHash", "#map=<x>&area=a&route=1-1&ch=-1&lv=x&other=1")
    assert junk == {}


def test_pan_and_zoom_maths() -> None:
    fit, zoom, zoom_capped, clamp_far, clamp_ok = run(
        [
            ("fitView", [{"w": 400, "h": 200}, {"w": 840, "h": 640}, 20]),
            ("zoomAt", [{"x": 10, "y": 20, "k": 1}, 2, 110, 120, 0.5, 4]),
            ("zoomAt", [{"x": 0, "y": 0, "k": 3}, 10, 0, 0, 0.5, 4]),
            (
                "clampView",
                [{"x": 5000, "y": -5000, "k": 1}, {"w": 400, "h": 200}, {"w": 800, "h": 600}, 0.3],
            ),
            (
                "clampView",
                [{"x": 100, "y": 100, "k": 1}, {"w": 400, "h": 200}, {"w": 800, "h": 600}, 0.3],
            ),
        ]
    )
    assert fit == {"x": 20, "y": 120, "k": 2}  # 800 / 400 wide; centred in the 640 height
    # the point under the cursor stays put: (110 - 10) / 1 == (110 - x) / 2
    assert zoom == {"x": -90, "y": -80, "k": 2}
    assert zoom_capped["k"] == 4
    assert clamp_far == {"x": 800 - 0.3 * 400, "y": 0.3 * 200 - 200, "k": 1}
    assert clamp_ok == {"x": 100, "y": 100, "k": 1}


def test_map_counts_and_training_picks() -> None:
    m = {
        "train": {"eph": 0.2},
        "shops": [{"ch": [1]}, {"ch": [2]}],
        "quests": [{"ch": 1}, {"ch": 3}, {"ch": None}],
        "recipes": [{"ch": [1]}],
        "trades": [{"ch": [2]}],
        "events": [{"ch": [1]}],
        "mobs": [{"boss": True}, {"boss": False}],
        "portals": [{"hidden": True}, {"hidden": False}],
    }
    every, ch1 = run([("mapCounts", [m, None]), ("mapCounts", [m, 1])])
    assert every == {"train": 1, "shops": 2, "quests": 3, "craft": 2, "bosses": 2, "secrets": 1}
    assert ch1 == {"train": 1, "shops": 1, "quests": 2, "craft": 1, "bosses": 2, "secrets": 1}
    data = {
        "fit": FIT,
        "maps": [
            {"id": "a", "lv": [10, 12], "train": {"eph": 0.1}},
            {"id": "b", "lv": [11, 14], "train": {"eph": 0.3}},
            {"id": "c", "lv": [40, 45], "train": {"eph": 0.9}},
            {"id": "d", "lv": [10, 10], "train": None},
        ],
    }
    picks = one("trainingPicks", data, 12, 5)
    assert [p["id"] for p in picks] == ["b", "a"]
    assert one("trainingPicks", data, None, 5) == []


def test_route_areas() -> None:
    maps = {"1-0": {"area": "1"}, "1-1": {"area": "1"}, "2-0": {"area": "2"}, "1-2": {"area": "1"}}
    assert one("routeAreas", ["1-0", "1-1", "2-0", "1-2"], maps) == ["1", "2", "1"]


def test_the_script_parses_and_exports_without_a_page() -> None:
    names = json.loads(
        subprocess.run(
            [NODE, "-e", "console.log(JSON.stringify(Object.keys(require(process.argv[1]))))",
             str(SCRIPT)],
            capture_output=True, text=True, timeout=60, check=True,
        ).stdout
    )  # fmt: skip
    assert {"levelFit", "route", "searchPlaces", "parseHash", "formatHash", "zoomAt"} <= set(names)
