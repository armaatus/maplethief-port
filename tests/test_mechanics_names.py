"""mtport.mechanics (#214): the names file is valid, names nothing another names generator names,
and docs/re/mechanics.md cites every function it names."""

import json
import subprocess
import sys

from mtport import (
    audio_analysis,
    autosave,
    flashes,
    flow,
    hudextras,
    network,
    savedata,
    scenes,
    screenshake,
    timing,
)
from mtport import mechanics as mx
from mtport import names as nm
from repo_paths import REPO

DOC = REPO / "docs" / "re" / "mechanics.md"


def test_names_file_is_valid():
    source, entries = nm.parse(json.loads(mx.names_json()), "mechanics.json")
    assert source == "mechanics"
    assert len(entries) == len(mx.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    assert len({e.name for e in entries}) == len(entries)
    assert all(e.thumb for e in entries if e.kind == "function")


def test_names_do_not_collide_with_the_other_generators():
    ours = {int(a, 16) & ~1 for a, _, _, _ in mx.NAMES}
    names = {n for _, _, n, _ in mx.NAMES}
    for module in (
        savedata,
        timing,
        autosave,
        network,
        screenshake,
        audio_analysis,
        flashes,
        hudextras,
        scenes,
    ):
        rows = module.NAMES
        assert not ours & {int(a, 16) & ~1 for a, _, _, _ in rows}, module.__name__
        assert not names & {n for _, _, n, _ in rows}, module.__name__
    assert not ours & {a & ~1 for a, _, _ in flow.FLOW_NAMES}
    assert not names & {n for _, n, _ in flow.FLOW_NAMES}


def test_the_note_cites_every_named_function():
    text = DOC.read_text(encoding="utf-8")
    missing = [a for a, _, _, _ in mx.NAMES if f"`{a}`" not in text]
    assert not missing, missing


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "mechanics.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.mechanics", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == mx.names_json()
