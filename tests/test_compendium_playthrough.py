"""The playthrough checklist (#296) on a synthetic knowledge base.

Every quest is ticked once, in the walkthrough's parts and order; a boss follows the quest
that asks for its kill, a job advancement and a climb the quest their conditions need; the
climbs, both job branches and save and load have their sections; every row has a tick box and
names its automated run, and every run named is a CTest; #92's template has a column per preset, a
controller-first section per platform and the Switch row.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport.compendium import playthrough as pt
from mtport.compendium.sections import walkthrough as wt
from mtport.compendium.siteapi import Context, Images, load_kb_dir
from tests.fixtures.compendium import walkthrough_kb as fx

CMAKE = Path(__file__).resolve().parent / "CMakeLists.txt"


def _climb_map(mid: str) -> dict:
    return {
        "id": mid,
        "kind": "map",
        "name": fx._n("Steep", "Steep"),
        "area": [fx._ref("area", "1")],
        "boss_map": False,
        "no_save": "climb",
        "exits": [],
        "entrances": [],
        "spawns": [],
        "npcs": [],
        "warps_in": [fx._ref("warp", "chapter1-portal112-1")] if mid == "1-5" else [],
        "confidence": {},
    }


@pytest.fixture
def ctx(tmp_path: Path) -> Context:
    data = fx.kb()
    data["map"] += [_climb_map("1-5"), _climb_map("1-6")]
    data["warp"].append(
        {
            "id": "chapter1-portal112-1",
            "kind": "warp",
            "name": fx._n("to 1-5", "to 1-5"),
            "chapters": [1],
            "conditions": [[fx._state(1, 3, "==", 1)]],
            "from": [{"map": "1-1", "npc": None, "portal": 2, "role": "portal"}],
            "target_map": "1-5",
            "used": True,
            "confidence": {},
        }
    )
    data["job_advancement"] = [
        {
            "id": "1-1-0",
            "kind": "job_advancement",
            "name": fx._n("first", "first"),
            "chapter": 1,
            "advancement": 1,
            "job": 0,
            "job_name": "Rogue",
            "keeps_job": False,
            "conditions": [[fx._state(0, 7, "==", 2)]],
            "confidence": {},
        },
        {
            "id": "2-2-1",
            "kind": "job_advancement",
            "name": fx._n("second", "second"),
            "chapter": 2,
            "advancement": 2,
            "job": 1,
            "job_name": "Assassin",
            "keeps_job": False,
            "conditions": [[]],
            "confidence": {},
        },
    ]
    return Context(load_kb_dir(fx.write_kb(tmp_path, data)), Images(), None)


def _flow(cl: pt.Checklist) -> list[pt.Row]:
    """The rows of the tutorial and the chapters, in order."""
    out: list[pt.Row] = []
    for s in cl.sections:
        if s.title == "Tutorial" or s.title.startswith("Chapter "):
            out += s.rows + [r for p in s.parts for r in p.rows]
    return out


def test_every_quest_once_in_walkthrough_order(ctx: Context) -> None:
    cl = pt.build(ctx)
    quests = [r.id for r in _flow(cl) if r.kind == "quest"]
    assert sorted(quests) == sorted(str(q["id"]) for q in ctx.kb.entities("quest"))
    assert len(quests) == len(set(quests))
    # part by part as the walkthrough has them: the story quests, then the side quests
    w = wt.walk(ctx)
    assert quests == [q for p in w.parts for q in w.story.get(p.slug, []) + w.side.get(p.slug, [])]


def test_extras_follow_their_quest(ctx: Context) -> None:
    cl = pt.build(ctx)
    keys = [r.key for r in _flow(cl)]
    # the boss after the quest that asks for its kill
    assert keys.index("monster:2") == keys.index("quest:2-main-1") + 1
    # the first advancement after main 7, the climb after the side quest its portal needs
    assert keys.index("job_advancement:1-1-0") > keys.index("quest:1-main-7")
    assert keys.index("climb:1") > keys.index("quest:1-sub-3")
    # an advancement whose conditions name no quest: at the end of its chapter
    chapter2 = next(s for s in cl.sections if s.title == "Chapter 2")
    assert chapter2.parts[-1].title == "Any time in chapter 2"
    assert [r.key for r in chapter2.parts[-1].rows] == ["job_advancement:2-2-1"]
    assert cl.ids("climb") == {"1"}
    assert cl.ids("monster") == {"2"}
    assert cl.ids("job_advancement") == {"1-1-0", "2-2-1"}


def test_sections(ctx: Context) -> None:
    titles = [s.title for s in pt.build(ctx).sections]
    assert titles[0] == "Tutorial"
    for want in (
        "Chapter 1",
        "Chapter 2",
        "Chapter 3",
        "The endings and the post-game",
        "Both job branches",
        "The three climbs",
        "Save and load",
    ):
        assert want in titles


def test_rows_have_a_tick_box_and_a_run(ctx: Context) -> None:
    text = pt.render_playthrough(pt.build(ctx))
    rows = [line for line in text.splitlines() if line.startswith("- ")]
    assert rows
    for line in rows:
        assert re.fullmatch(r"- \[ \] .+\. Automated: (none yet|play\.[\w.]+)", line), line
    assert "Automated: play.chapter_1_a" in text  # 1-main-7 is played by part A


def _ctest_names() -> set[str]:
    text = CMAKE.read_text(encoding="utf-8")
    names = set(re.findall(r"NAME (play\.[\w.]+)", text))
    parts = re.search(r"set\(_chapter_1_parts ([a-z ]+)\)", text)
    assert parts, "tests/CMakeLists.txt has no _chapter_1_parts"
    names |= {f"play.chapter_1_{p}" for p in parts.group(1).split()}
    parts2 = re.search(r"set\(_chapter_2_parts ([a-z ]+)\)", text)
    raised = re.search(r"set\(_chapter_2_raised ([a-z ]*)\)", text)
    assert parts2 and raised, "tests/CMakeLists.txt has no _chapter_2_parts / _chapter_2_raised"
    for p in parts2.group(1).split():
        names.add(f"play.chapter_2_{p}" + ("_synthetic" if p in raised.group(1).split() else ""))
    parts3 = re.search(r"set\(_chapter_3_parts ([a-z ]+)\)", text)
    raised3 = re.search(r"set\(_chapter_3_raised ?([a-z ]*)\)", text)
    assert parts3 and raised3, "tests/CMakeLists.txt has no _chapter_3_parts / _chapter_3_raised"
    for p in parts3.group(1).split():
        tail = "_synthetic" if p in raised3.group(1).split() else ""
        names.add(f"play.chapter_3_{p}{tail}")
        if p == "e":
            names.add(f"play.chapter_3_e_skipped{tail}")
    # Chapter 3's side parts (#301): `_chapter_3_part(<part> <test> ...)`, and a loop of them.
    names |= set(re.findall(r"_chapter_3_part\(\w+ (play\.[\w.]+)", text))
    sides = re.search(r"foreach\(part IN ITEMS ([a-z_0-9 ]+)\)\s+_chapter_3_part\(", text)
    assert sides, "tests/CMakeLists.txt has no loop of _chapter_3_part"
    names |= {f"play.chapter_3_{p}" for p in sides.group(1).split()}
    starts = re.search(r"set\(_from_starts([\w:\s]+)\)", text)
    assert starts, "tests/CMakeLists.txt has no _from_starts"
    names |= {f"play.{pair.split(':')[0]}_synthetic" for pair in starts.group(1).split()}
    return names


def test_every_run_named_is_a_ctest() -> None:
    names = _ctest_names()
    runs = set(pt.RUNS.values()) | {run for _, run in pt.SAVE_ROWS}
    assert runs - names == set()


def test_platform_template(ctx: Context) -> None:
    text = pt.render_platforms(pt.build(ctx))
    for preset in pt.PRESETS:
        assert f" {preset} |" in text
    assert re.search(r"^\| Switch \| .*#245 \|", text, re.M)
    for name, _ in pt.PLATFORMS:
        assert f"## {name}" in text
        assert f"### {name}: controller-first" in text
    assert "| Boss `2` " in text  # a milestone row of the shorter pass
    assert "Quest `" not in text  # the quests are #79's


def test_write_and_main(ctx: Context, tmp_path: Path) -> None:
    out = tmp_path / "checklists"
    pt.write(ctx, out)
    assert (out / "playthrough.md").read_text(encoding="utf-8").startswith("# Playthrough")
    assert (out / "platforms.md").is_file()
    assert pt.main(["--kb", str(tmp_path / "nothing"), "--out", str(out)]) == 1
