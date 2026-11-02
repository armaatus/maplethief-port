"""mtport.screenshake (#158): the names file is valid, names nothing another names generator
names, and the shake fields agree with the runtime's hook."""

import json
import re
import subprocess
import sys

import pytest

from mtport import autosave, flow, savedata, timing
from mtport import names as nm
from mtport import screenshake as ss
from repo_paths import REPO

HOOK = REPO / "runtime" / "src" / "hooks" / "builtin.cpp"


def test_names_file_is_valid():
    source, entries = nm.parse(json.loads(ss.names_json()), "screenshake.json")
    assert source == "screenshake"
    assert len(entries) == len(ss.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    assert all(e.thumb for e in entries if e.kind == "function")


def test_names_do_not_collide_with_the_other_generators():
    ours = {int(a, 16) & ~1 for a, _, _, _ in ss.NAMES}
    for rows in (savedata.NAMES, timing.NAMES, autosave.NAMES):
        assert not ours & {int(a, 16) & ~1 for a, _, _, _ in rows}
    assert not ours & {a & ~1 for a, _, _ in flow.FLOW_NAMES}


def test_the_hook_reads_the_same_fields():
    text = HOOK.read_text(encoding="utf-8")
    on = re.search(r"k_shake_on = (0x[0-9A-F]+)u", text)
    amp = re.search(r"k_shake_amp = (0x[0-9A-F]+)u", text)
    assert on and int(on.group(1), 16) == ss.SHAKE_ON
    assert amp and int(amp.group(1), 16) == ss.SHAKE_AMPLITUDE


@pytest.mark.real_assets
def test_the_scenes_that_shake():
    """docs/re/screen-shake.md's table, from the armv7 slice (skipped without it)."""
    slice_path = REPO / "work" / "slices" / "MapleThief.armv7"
    if not slice_path.exists():
        pytest.skip("work/slices/MapleThief.armv7 absent (run scripts/bootstrap)")
    got: dict[int, list[tuple[int | None, int | None]]] = {}
    for c in ss.scenes(slice_path):
        got.setdefault(c.function, []).append(
            (c.step, c.amplitude if c.command == ss.COMMAND_SHAKE_ON else None)
        )
    assert got == {
        0x27AED: [(85, 10), (100, 20), (120, None)],  # makeAniChapter2Perion
        0x2BE55: [(64, 5), (177, None)],  # makeAniEndingZacum
        0x2CBD1: [(66, 10), (71, None)],  # makeAniBalogIn
        0x2E5E1: [(s, a) for k in range(7) for s, a in ((3 + 11 * k, 10), (8 + 11 * k, None))],
        0x302F5: [(135, 10), (145, None), (274, 15), (285, None), (359, 5), (362, None)],
    }


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "screenshake.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.screenshake", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == ss.names_json()
