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


@pytest.mark.real_assets
def test_no_scene_exits_to_the_main_menu():
    """A scene's exit to the main menu (#336, docs/re/autosave.md): the runner's command 28 goes
    to the main menu when p1 = -1, which would leave without a save. No shipped scene uses it:
    the two command-28 ends are the intro's warp to 2.8 and the Henesys ending's to 2.0, so the
    autosave has nothing to cover there. Skipped without the slice."""
    if not SLICE.exists():
        pytest.skip("work/slices/MapleThief.armv7 absent (run scripts/bootstrap)")
    pytest.importorskip("capstone")
    ends = [(c.function, c.p1, c.p2) for c in sc.scan_slice(SLICE) if c.command == 28]
    assert ends == [(0x26F89, 2, 0), (0x302F5, 2, 8)]
    assert all(p1 is not None and p1 != -1 for _, p1, _ in ends)


# --- the stage at the field's width (#396) -------------------------------------------------------

BALOG = 0x2CBD1  # scene 14


def test_every_scene_has_its_map_and_the_list_is_the_runtime_s():
    assert sorted(sc.SCENE_MAPS) == [n for n, _, _ in sc.SCENES]
    assert all(why for _, _, why in sc.SCENE_MAPS.values())
    # runtime/src/hooks/widescreen.cpp's k_wide_scenes is the module's list.
    source = (REPO / "runtime" / "src" / "hooks" / "widescreen.cpp").read_text(encoding="utf-8")
    block = re.search(r"k_wide_scenes\[\] = \{(.*?)\};", source, re.S)
    assert block
    listed = {int(n) for n in re.findall(r"(\d+)u,", block.group(1))}
    assert listed == set(sc.WIDE_SCENES)
    tests = (REPO / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    runs = re.search(r"set\(_wide_scenes ([^)]*)\)", tests)
    assert runs
    played = {int(pair.split(":")[0]) for pair in runs.group(1).split()}
    for n, run in sc.WIDE_SCENES.items():
        assert run.startswith("play.widescreen_scene_")
        assert int(run.split("_")[2]) in played, (n, run)


def test_a_480_point_map_is_never_wide():
    cmds = [cmd(BALOG, 1, 5, 0, 900), cmd(BALOG, 1, 22, 0, 240), cmd(BALOG, 2, 58)]
    st = sc.stage(cmds, 14, 480)
    assert st.view == 480 and st.sightings == ()
    assert sc.verdict(st) == (False, "its map is 480 points wide: the view is 480 either way")
    assert sc.stage(cmds, 14, 560).view == 560 and sc.stage(cmds, 14, 960).view == sc.WIDE


def test_an_npc_waiting_in_the_wings_for_its_entrance_keeps_the_scene_narrow():
    # Map 800 points wide, the camera centred on 400: the 480 view is 160 ... 640, the 568 one
    # 116 ... 684. NPC 0 waits at 660 (seen only wide), then walks left onto the stage.
    cmds = [
        cmd(BALOG, 1, 22, 0, 400),
        cmd(BALOG, 1, 12, 0, 5),
        cmd(BALOG, 1, 13, 0, 660),
        cmd(BALOG, 10, 15, 0, 10),
        cmd(BALOG, 10, 16, 0, 1),
        cmd(BALOG, 20, 17, 0, 3),
        cmd(BALOG, 30, 58),
    ]
    st = sc.stage(cmds, 14, 800)
    assert st.view == 568 and st.camera_from == 1
    (s,) = st.sightings
    assert (s.actor, s.first, s.last, s.why, s.hidden) == (
        "NPC 0",
        1,
        11,
        "before its entrance",
        True,
    )
    assert s.xs == (640, 660)  # it moves 10 a frame from step 10 and crosses 640 at step 12
    wide, why = sc.verdict(st)
    assert not wide and why.startswith("NPC 0 before its entrance at x 640-660, steps 1-11")


def test_an_actor_walking_off_is_seen_longer_but_not_hidden():
    # The hero walks right off the 480 stage (camera fixed at 160 ... 640) and keeps walking.
    cmds = [
        cmd(BALOG, 1, 22, 0, 400),
        cmd(BALOG, 1, 5, 0, 600),
        cmd(BALOG, 1, 7, 0, 10),
        cmd(BALOG, 1, 9, 0, 0),
        cmd(BALOG, 20, 58),
    ]
    st = sc.stage(cmds, 14, 800)
    (s,) = st.sightings
    assert (s.actor, s.why, s.parked, s.hidden) == ("hero", "after its exit", False, False)
    assert sc.verdict(st) == (True, "nothing staged where only the wider view shows it")
    # Stopped in the wings after its exit, it is parked there: hidden.
    st = sc.stage([*cmds[:-1], cmd(BALOG, 6, 10), cmd(BALOG, 20, 58)], 14, 800)
    assert [(s.why, s.hidden) for s in st.sightings] == [("after its exit", True)]


def test_the_camera_follows_and_scrolls_clamped_to_the_map():
    # Followed: the hero at 100 on a 640-point map, both views clamped at 0: nothing extra
    # but the right edge 480 ... 568, where a sprite at 500 is seen only wide (off stage). Then
    # scrolled to the map's right end (160 ... 640 and 72 ... 640): the hero, standing at 100,
    # only in the wider view.
    cmds = [
        cmd(BALOG, 1, 5, 0, 100),
        cmd(BALOG, 1, 21),
        cmd(BALOG, 2, 42, 0, 7),
        cmd(BALOG, 2, 43, 0, 500),
        cmd(BALOG, 2, 47, 0, 0),
        cmd(BALOG, 5, 48, 0, 0),  # hidden again
        cmd(BALOG, 6, 27),
        cmd(BALOG, 6, 24, 0, 100),  # scrolled to the map's right end: both views end at 640
        cmd(BALOG, 9, 58),
    ]
    st = sc.stage(cmds, 14, 640)
    assert [(s.actor, s.first, s.last, s.why) for s in st.sightings] == [
        ("sprite 0", 2, 4, "off stage"),
        ("hero", 7, 9, "after its exit"),
    ]
    # Unknown until the scene places the camera: nothing is judged.
    late = [cmd(BALOG, 1, 12, 0, 5), cmd(BALOG, 1, 13, 0, 500), cmd(BALOG, 5, 22, 0, 240)]
    assert sc.stage([*late, cmd(BALOG, 6, 58)], 14, 640).camera_from == 5


def test_a_picture_is_counted_only_when_faded_in_at_a_set_x():
    cmds = [
        cmd(BALOG, 4, 30, 0, 0),
        cmd(BALOG, 4, 31, 0, None),
        cmd(BALOG, 4, 35, 0, 10),
        cmd(BALOG, 6, 30, 1, 1),  # alpha 0: never seen
        cmd(BALOG, 6, 50, 0, 3),
        cmd(BALOG, 9, 58),
    ]
    st = sc.stage(cmds, 14, 640)
    assert st.pictures == ((4, 0),) and st.boxes == 1


@pytest.mark.real_assets
def test_the_scene_stages_and_docs_widescreen_table():
    """Each scene's verdict as runtime/src/hooks/widescreen.cpp lists it, and
    docs/re/widescreen.md's block is the one `widescreen` writes. Skipped without the slice or
    the bundle."""
    app = REPO / sc.APP
    if not SLICE.exists() or not app.exists():
        pytest.skip("work/ (slice, bundle) absent (run scripts/bootstrap)")
    pytest.importorskip("capstone")
    cmds = sc.scan_slice(SLICE)
    widths = sc.map_widths(app)
    assert sc.wide_problems(cmds, widths) == []
    # Every boss map is 30 tiles wide: the boss scenes are 480 whatever the list.
    for n in (1, 3, 4, 5, 6, 7, 8, 9, 10, 14):
        assert widths[sc.SCENE_MAPS[n][:2]] == 480, n
    # The intro waits its NPCs in the wings: narrow.
    intro = sc.stage(cmds, 0, widths[(2, 10)])
    assert intro.view == 568 and any(s.hidden for s in intro.sightings)
    doc = (REPO / sc.WIDE_DOC).read_text(encoding="utf-8")
    rendered = gc.render_blocks(
        doc,
        {sc.WIDE_BLOCK: sc.stage_table(cmds, widths)},
        "python3 -m mtport.scenes widescreen",
        sc.WIDE_DOC,
    )
    assert rendered == doc, "docs/re/widescreen.md is stale: python3 -m mtport.scenes widescreen"
