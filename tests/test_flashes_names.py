"""mtport.flashes (#186): the names file is valid, names nothing another names generator names,
the flash sites agree with the runtime's hook, and the colour table's flash rows are white and
black in the shipped binary."""

import json
import re
import subprocess
import sys

import pytest

from mtport import audio_analysis, autosave, flow, network, savedata, screenshake, timing
from mtport import flashes as fl
from mtport import names as nm
from repo_paths import REPO

HOOK = REPO / "runtime" / "src" / "hooks" / "flashes.cpp"


def test_names_file_is_valid():
    source, entries = nm.parse(json.loads(fl.names_json()), "flashes.json")
    assert source == "flashes"
    assert len(entries) == len(fl.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    assert len({e.name for e in entries}) == len(entries)
    assert all(e.thumb for e in entries if e.kind == "function")


def test_names_do_not_collide_with_the_other_generators():
    ours = {int(a, 16) & ~1 for a, _, _, _ in fl.NAMES}
    names = {n for _, _, n, _ in fl.NAMES}
    for module in (savedata, timing, autosave, network, screenshake, audio_analysis):
        rows = module.NAMES
        assert not ours & {int(a, 16) & ~1 for a, _, _, _ in rows}, module.__name__
        assert not names & {n for _, _, n, _ in rows}, module.__name__
    assert not ours & {a & ~1 for a, _, _ in flow.FLOW_NAMES}
    assert not names & {n for _, n, _ in flow.FLOW_NAMES}


def test_the_hook_matches_the_same_sites():
    text = HOOK.read_text(encoding="utf-8")
    table = re.search(r"k_hero_flash_returns\[\] = \{(.*?)\};", text, re.S)
    assert table
    sites = tuple(int(h, 16) for h in re.findall(r"(0x[0-9A-F]+)u", table.group(1)))
    assert sites == fl.HERO_FLASH_RETURNS
    fade = re.search(r"k_fade_in_return = (0x[0-9A-F]+)u", text)
    assert fade and int(fade.group(1), 16) == fl.FADE_IN_RETURN
    assert all(s & 1 for s in sites + (fl.FADE_IN_RETURN,))  # Thumb return addresses


@pytest.mark.real_assets
def test_the_flash_rows_of_the_colour_table():
    """docs/re/flashes.md: row 2 of the colour table is white, row 3 black (one colour each)."""
    slice_path = REPO / "work" / "slices" / "MapleThief.armv7"
    if not slice_path.exists():
        pytest.skip("work/slices/MapleThief.armv7 absent (run scripts/bootstrap)")
    from mtport import macho

    m = macho.load(slice_path)
    assert m.u32(fl.COLOUR_TABLE + fl.WHITE_ROW * 200) == 0xFFFFFF
    assert m.u32(fl.COLOUR_TABLE + fl.BLACK_ROW * 200) == 0x000000


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "flashes.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.flashes", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == fl.names_json()
