"""mtport.scenes (#174): the scene runner's command classification, the scene table and its
checks on synthetic commands, the names file, and (with the slice) the real builders and
docs/re/qol.md's generated block."""

import json
import re
import subprocess
import sys

import pytest

from mtport import audio_analysis as audio
from mtport import autosave, flow, savedata, screenshake, timing
from mtport import ghidra_common as gc
from mtport import names as nm
from mtport import scenes as sc
from repo_paths import REPO

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
SCENE_SKIP = REPO / "platform" / "src" / "scene_skip.h"


def cmd(function, step, command, p1=0, p2=0, site=0):
    return sc.SceneCommand(function, step, command, p1, p2, site or step * 16 + command)


def test_every_case_of_the_switch_is_classified():
    assert set(sc.COMMANDS) | sc.NO_CASE == set(range(1, sc.COMMAND_COUNT + 1))
    assert not set(sc.COMMANDS) & sc.NO_CASE
    for c in sc.COMMANDS.values():
        assert c.kind in sc.KINDS and c.does
        assert bool(c.target) == (c.kind in (sc.STATE, sc.END)), c


def test_the_kinds_the_skip_relies_on():
    # The ends (each frees the scene), the speech box, the state commands of docs/re/qol.md.
    ends = {n for n, c in sc.COMMANDS.items() if c.kind == sc.END}
    assert ends == {28, 52, 58}
    assert sc.COMMANDS[50].kind == sc.BOX and "+0x1E9" in sc.COMMANDS[50].does
    state = {n for n, c in sc.COMMANDS.items() if c.kind == sc.STATE}
    assert state == {5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17, 37, 38, 39, 40, 41, 54, 59, 60}
    # The shake and the sound commands are other modules' (mtport.screenshake, mtport.audio).
    assert sc.COMMANDS[screenshake.COMMAND_SHAKE_ON].kind == sc.PICTURE
    for n in (
        audio.SCENE_VIBRATE,
        audio.SCENE_PLAY,
        audio.SCENE_STOP_BGM,
        audio.SCENE_STOP_VIBRATE,
    ):
        assert sc.COMMANDS[n].kind == sc.SOUND


def test_the_scene_table_of_start_scene():
    numbers = [n for n, _, _ in sc.SCENES]
    assert numbers == list(range(21))
    assert len({f for _, f, _ in sc.SCENES}) == 21
    assert all(f & 1 for _, f, _ in sc.SCENES)


def test_problems_on_synthetic_scenes():
    intro = 0x302F5
    good = [
        cmd(intro, 1, 55, 30, 0),
        cmd(intro, 45, 9),
        cmd(intro, 50, 50, 1, 2),
        cmd(intro, 60, 28, 2, 8),
    ]
    others = [cmd(f, 1, 58) for _, f, _ in sc.SCENES if f != intro]
    assert sc.problems(good + others) == []
    # An unclassified command, an end before the last step, two ends, an unknown builder.
    assert any("not classified" in p for p in sc.problems([*good, cmd(intro, 2, 3), *others]))
    assert any(
        "not one on the last step" in p for p in sc.problems([*good, cmd(intro, 61, 1), *others])
    )
    assert any("2 end commands" in p for p in sc.problems([*good, cmd(intro, 60, 58), *others]))
    assert any("not a builder" in p for p in sc.problems([*good, *others, cmd(0x1001, 1, 58)]))
    assert any("no commands" in p for p in sc.problems(good))


def test_the_table_groups_state_steps_and_names_the_end():
    intro = 0x302F5
    cmds = [
        cmd(intro, 1, 12, 0, 5),
        cmd(intro, 1, 13, 0, 0),
        cmd(intro, 45, 5, 0, 24),
        cmd(intro, 45, 9),
        cmd(intro, 50, 50, 1, 2),
        cmd(intro, 60, 54),
        cmd(intro, 61, 37, 1, None),  # a computed argument
        cmd(intro, 70, 28, 2, 8),
    ]
    text = sc.table(cmds)
    row = next(line for line in text.splitlines() if "makeAni1`" in line)
    assert row == (
        "| 0 | `CAniManager::makeAni1` `0x302F5` | 70 | 1 | "
        "hero 45; NPCs 1; map 61; scripts 60 | 70: 28 (2, 8) |"
    )
    assert "8 commands in 21 scenes: 0 picture, 0 sound, 1 box, 6 state, 1 end." in text


def test_the_skip_reads_the_box_the_runner_waits_on():
    # scene_skip.h's +0x1E9: the byte above +0x1E8 in the word it reads.
    text = SCENE_SKIP.read_text(encoding="utf-8")
    word = re.search(r"kSceneBoxWord = (0x[0-9A-F]+);", text)
    shift = re.search(r"kSceneBoxShift = (\d+);", text)
    assert word and shift
    assert int(word.group(1), 16) + int(shift.group(1)) // 8 == 0x1E9


def test_names_file_is_valid_and_its_own():
    source, entries = nm.parse(json.loads(sc.names_json()), "scenes.json")
    assert source == "scenes"
    assert len(entries) == len(sc.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    assert all(e.thumb for e in entries if e.kind == "function")
    ours = {int(a, 16) & ~1 for a, _, _, _ in sc.NAMES}
    for rows in (savedata.NAMES, timing.NAMES, autosave.NAMES, screenshake.NAMES):
        assert not ours & {int(a, 16) & ~1 for a, _, _, _ in rows}
    assert not ours & {a & ~1 for a, _, _ in flow.FLOW_NAMES}
    assert not ours & {e.address & ~1 for e in audio.ENTRY_POINTS}


def test_re_analyze_writes_the_names_file():
    text = (REPO / "scripts" / "re-analyze").read_text(encoding="utf-8")
    loop = re.search(
        r"for module in ([^;]+); do\n  run \"\$PY\" -B -m \"mtport\.\$module\" names", text
    )
    assert loop and "scenes" in loop.group(1).split()


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "scenes.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.scenes", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == sc.names_json()


@pytest.mark.real_assets
def test_the_real_builders():
    """Every command the 21 builders add is classified and each scene ends once, on its last
    step; the intro and Balog's entrance as play.skip_intros_* expect them. Skipped without the
    slice."""
    if not SLICE.exists():
        pytest.skip("work/slices/MapleThief.armv7 absent (run scripts/bootstrap)")
    pytest.importorskip("capstone")
    cmds = sc.scan_slice(SLICE)
    assert sc.problems(cmds) == []
    intro = [c for c in cmds if c.function == 0x302F5]
    balog = [c for c in cmds if c.function == 0x2CBD1]
    assert max(c.step for c in intro) == 444 and sum(c.command == 50 for c in intro) == 4
    assert max(c.step for c in balog) == 79 and sum(c.command == 50 for c in balog) == 6
    assert [(c.step, c.command, c.p1, c.p2) for c in intro if c.command == 28] == [(444, 28, 2, 8)]
    assert [(c.step, c.command, c.p2) for c in balog if c.command == 58] == [(79, 58, 0)]
    # docs/re/qol.md's block is the one generate writes.
    doc = (REPO / sc.DOC).read_text(encoding="utf-8")
    rendered = gc.render_blocks(
        doc, {sc.BLOCK: sc.table(cmds)}, "python3 -m mtport.scenes generate", sc.DOC
    )
    assert rendered == doc, "docs/re/qol.md is stale: python3 -m mtport.scenes generate"
