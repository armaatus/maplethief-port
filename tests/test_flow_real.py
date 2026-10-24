"""Acceptance (#12): every function named in docs/re/program-flow.md is named in Ghidra.

Needs the slice (scripts/bootstrap), the names files and a Ghidra export with them applied
(tools/ghidra/import); skips when those are absent.
"""

import json
import re
from pathlib import Path

import pytest

from mtport import bootstrap as bs
from mtport import flow, macho, names
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

DOC = REPO / "docs" / "re" / "program-flow.md"
NAMES_DIR = REPO / flow.NAMES_DIR
FUNCTIONS = REPO / "work" / "ghidra" / "functions.json"


@pytest.fixture(scope="module")
def image():
    return macho.load(REPO / bs.slice_path("armv7"))


def _doc_functions(image) -> set[int]:
    """Function pointers the doc mentions (backticked addresses that are function starts)."""
    starts = set(image.function_starts)
    text = DOC.read_text(encoding="utf-8")
    return {int(a, 16) for a in re.findall(r"`(0x[0-9A-F]{4,6})`", text)} & starts


def test_generator_agrees_with_the_other_names_files(image):
    if not (NAMES_DIR / "objc.json").is_file():
        pytest.skip("work/re/names/ is not populated (see tools/ghidra/README.md)")
    entries = flow.build(image, names.other_names(NAMES_DIR, Path(flow.NAMES_OUT).name))
    keyed = [e for e in entries if e["comment"].startswith("looks up ")]
    assert len(keyed) == 42
    assert {e["name"] for e in keyed} >= {
        "CAniManager::makeAniChapter1",
        "CHero::setExp",
        "CDataMgr::keyPopupItemInven_2",
        "CMPopup::CMPopup",
    }
    assert len(entries) - len(keyed) == len(flow.FLOW_NAMES)  # none of ours is named elsewhere


def test_every_doc_function_has_a_name(image):
    if not (NAMES_DIR / "flow.json").is_file():
        pytest.skip("work/re/names/flow.json absent: run python3 -m mtport.flow names")
    merged = {m.entry.location: m.entry.name for m in names.load_dir(NAMES_DIR)}
    functions = _doc_functions(image)
    assert len(functions) >= 100
    missing = sorted(f"0x{fp:X}" for fp in functions if fp & ~1 not in merged)
    assert missing == []


def test_every_doc_function_is_named_in_ghidra(image):
    if not FUNCTIONS.is_file():
        pytest.skip("work/ghidra/functions.json absent (run tools/ghidra/import)")
    funcs = {int(f["address"], 16): f for f in json.loads(FUNCTIONS.read_text())["functions"]}
    if funcs[0x2590D]["name_source"] != "names":
        pytest.skip("the export predates flow.json: run tools/ghidra/apply-names")
    unnamed = sorted(
        f"0x{fp:X}" for fp in _doc_functions(image) if funcs[fp]["name_source"] != "names"
    )
    assert unnamed == []
    assert funcs[0x5C2D]["name"] == "Clet_Start" and funcs[0x27E0]["name"] == "start"
