"""mtport.autosave (#150): the names file is valid, names nothing another names generator
names, and the 16 no-save maps equal platform/src/autosave.cpp's and docs/re/autosave.md's."""

import json
import re
import subprocess
import sys

from mtport import autosave as au
from mtport import names as nm
from repo_paths import REPO

CPP = REPO / "platform" / "src" / "autosave.cpp"
DOC = REPO / "docs" / "re" / "autosave.md"


def test_names_file_is_valid():
    source, entries = nm.parse(json.loads(au.names_json()), "autosave.json")
    assert source == "autosave"
    assert len(entries) == len(au.NAMES)
    assert len({e.address for e in entries}) == len(entries)
    functions = [e for e in entries if e.kind == "function"]
    assert functions and all(e.thumb for e in functions)


def test_names_do_not_collide_with_the_other_generators():
    from mtport import savedata

    ours = {int(a, 16) & ~1 for a, _, _, _ in au.NAMES}
    theirs = {int(a, 16) & ~1 for a, _, _, _ in savedata.NAMES}
    assert not ours & theirs


def test_no_save_maps_match_the_frontend():
    text = CPP.read_text(encoding="utf-8")
    table = text[text.index("kNoSaveMaps[] = {") : text.index("};", text.index("kNoSaveMaps"))]
    cpp = tuple(
        (int(a), int(m), kind) for a, m, kind in re.findall(r'\{(\d+), (\d+), "([^"]+)"\}', table)
    )
    assert cpp == au.NO_SAVE_MAPS
    assert len(au.NO_SAVE_MAPS) == 16


def test_no_save_maps_match_the_doc():
    doc = DOC.read_text(encoding="utf-8")
    section = doc[doc.index("### The 16 maps") : doc.index("## The port's guards")]
    listed = []
    for row in re.findall(r"^\| ([\d., ]+) \| ([^|]+) \|", section, re.M):
        for am in row[0].split(","):
            a, m = am.strip().split(".")
            listed.append((int(a), int(m), row[1].strip()))
    assert sorted(listed) == sorted(au.NO_SAVE_MAPS)


def test_cli_writes_the_names_file(tmp_path):
    out = tmp_path / "autosave.json"
    subprocess.run(
        [sys.executable, "-m", "mtport.autosave", "names", str(out)],
        check=True,
        capture_output=True,
        env={"PYTHONPATH": str(REPO / "tools")},
    )
    assert out.read_text(encoding="utf-8") == au.names_json()
