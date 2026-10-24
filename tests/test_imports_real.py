"""Real-asset checks for the import table analysis (#11) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). The analysis also needs the Ghidra exports: those
tests skip when work/ghidra/ is absent (run tools/ghidra/import). The ``nm`` / ``otool``
cross-checks skip when those macOS tools are not on PATH.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess

import pytest

from mtport import bootstrap as bs
from mtport import imports, macho, names, objc
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")
GHIDRA = REPO / imports.GHIDRA_DIR


@pytest.fixture(scope="module")
def data() -> bytes:
    return SLICE.read_bytes()


@pytest.fixture(scope="module")
def image(data) -> macho.MachO:
    return macho.MachO(data)


@pytest.fixture(scope="module")
def ghidra() -> imports.Ghidra:
    if not (GHIDRA / "xrefs.json").is_file():
        pytest.skip("work/ghidra/ exports absent (run tools/ghidra/import)")
    return imports.load_ghidra(GHIDRA)


@pytest.fixture(scope="module")
def analysis(image, ghidra) -> imports.Analysis:
    return imports.run(image, ghidra)


# -- import table (no Ghidra needed) --------------------------------------------------------


def test_import_table_matches_analysis_md(image):
    """docs/ANALYSIS.md → Imports: 333 undefined symbols, 217 stubs + lazy pointers, 82 nl."""
    table = imports.import_table(image)
    assert len(table) == 333
    slots = image.indirect_slots
    assert sum(s.kind == "stub" for s in slots) == 217
    assert sum(s.kind == "lazy" for s in slots) == 217
    assert sum(s.kind == "non_lazy" for s in slots) == 82
    assert sum(len(i.non_lazy) for i in table) == 28  # the other 54 point into the image
    assert imports.check_stubs(image) == []
    assert all(i.framework for i in table)


def test_undefined_symbols_match_nm(image):
    if shutil.which("nm") is None:
        pytest.skip("nm not on PATH")
    text = subprocess.run(
        ["nm", "-m", "-u", str(SLICE)], check=False, capture_output=True, text=True
    ).stdout
    if not text.strip():
        text = subprocess.run(
            ["nm", "-m", str(SLICE)], check=True, capture_output=True, text=True
        ).stdout
    theirs = {}
    for line in text.splitlines():
        m = re.search(r"\(undefined\) (?:weak )?external (\S+) \(from (\S+)\)", line)
        if m:
            theirs[m[1]] = m[2]
    ours = {i.name: i.framework for i in imports.import_table(image)}
    assert set(theirs) == set(ours)
    for name, lib in theirs.items():
        assert ours[name].lower().startswith(lib.lower().split(".", 1)[0]), name


def test_indirect_slots_match_otool(image):
    if shutil.which("otool") is None:
        pytest.skip("otool not on PATH")
    text = subprocess.run(
        ["otool", "-Iv", str(SLICE)], check=True, capture_output=True, text=True
    ).stdout
    theirs = {}
    for line in text.splitlines():
        m = re.match(r"^0x([0-9a-f]+)\s+(?:\d+ (\S+)|LOCAL|ABSOLUTE)", line)
        if m:
            theirs[int(m[1], 16)] = m[2]
    assert {s.address: s.symbol for s in image.indirect_slots} == theirs


def test_stub_symbol_helper(image):
    assert image.stub_symbol(0xF0FB4) == "_objc_setProperty"
    by_name = {s.symbol: s.address for s in image.indirect_slots if s.kind == "stub"}
    assert image.stub_symbol(by_name["_objc_msgSend"]) == "_objc_msgSend"


# -- the analysis ---------------------------------------------------------------------------


def test_every_import_classified(analysis):
    """Acceptance (#11): each import is game / Chartboost-only / both / unreferenced."""
    assert len(analysis.classes) == 333
    assert set(analysis.classes.values()) <= set(imports.CLASSES)
    c = imports.counts(analysis)
    assert c["game"] + c["both"] + c["chartboost"] + c["unreferenced"] == 333
    assert c["hle"] == c["game"] + c["both"]


def test_spot_checks(analysis):
    k = analysis.classes
    assert k["_glDrawArrays"] == "game"
    assert k["_alSourcePlay"] == "game"
    assert k["_objc_msgSend"] == "both"
    assert k["_CC_SHA1"] == "chartboost"
    assert k["_OBJC_CLASS_$_NSURLConnection"] == "chartboost"
    assert all(k[n] == "chartboost" for n in k if n.startswith("_SCNetworkReachability"))
    assert k["dyld_stub_binder"] == "unreferenced"
    # A Chartboost subclass of UIWebView binds the framework class only in Chartboost data.
    assert k["_OBJC_METACLASS_$_UIWebView"] == "chartboost"


def test_function_classes(analysis):
    cls = analysis.function_classes
    assert len(cls) == 2655
    parsed = objc.parse(macho.load(SLICE))
    for k in parsed.classes:
        want = "chartboost" if k.group == "chartboost" else "game"
        for m in k.instance_methods + k.class_methods:
            assert cls[m.imp] == want, (k.name, m.selector)


def test_capstone_agrees_with_ghidra(analysis):
    """Call and load sites per import are identical in Capstone and Ghidra."""
    bad = {n: v for n, v in analysis.ghidra_check.items() if v["only_capstone"] or v["only_ghidra"]}
    assert not bad
    assert sum(v["capstone"] for v in analysis.ghidra_check.values()) > 9000


def test_selectors_resolved(analysis):
    c = imports.counts(analysis)
    assert c["sends"] > 5000
    assert c["sends_with_selector"] / c["sends"] > 0.95
    msgs = imports.message_rows(analysis)
    pairs = {(m["class"], m["class_method"], m["selector"]) for m in msgs}
    assert ("NSString", True, "stringWithFormat:") in pairs
    assert ("EAGLContext", True, "setCurrentContext:") in pairs


def test_names_document(analysis, ghidra):
    doc = imports.names_json(analysis, ghidra)
    names.parse(doc, "imports.json")
    # Ghidra's Mach-O loader names all 217 stubs itself.
    assert doc["names"] == []


def test_committed_markdown_is_current(analysis, data):
    """Acceptance (#11): docs/re/imports.md lists all 333 imports with call counts and the
    HLE total, and is what the generator writes today."""
    md = imports.to_markdown(analysis, objc.source_info(SLICE, data, REPO))
    committed = (REPO / imports.MARKDOWN_OUT).read_text(encoding="utf-8")
    assert committed == md, "docs/re/imports.md is stale: run python3 -m mtport.imports generate"
    for imp in analysis.imports:
        rows = [line for line in md.splitlines() if line.startswith(f"| `{imp.name}` |")]
        assert len(rows) == 1, imp.name
        assert re.search(r"\| \d+ \|", rows[0]), imp.name
    c = imports.counts(analysis)
    assert f"**The HLE layer must implement {c['hle']} of the 333 imports**" in md


def test_json_on_disk_is_current(analysis, data):
    path = REPO / imports.JSON_OUT
    if not path.is_file():
        pytest.skip(f"{imports.JSON_OUT} absent (run python3 -m mtport.imports generate)")
    doc = json.loads(path.read_text(encoding="utf-8"))
    assert doc["counts"] == imports.counts(analysis)
    assert len(doc["imports"]) == 333


def test_chartboost_functions_agree_with_libtags(analysis):
    """The first-pass Chartboost classification against #10's library tags."""
    from mtport import libtags

    path = REPO / libtags.JSON_OUT
    if not path.is_file():
        pytest.skip(f"{libtags.JSON_OUT} absent (run python3 -m mtport.libtags generate)")
    tags = libtags.load(path)
    for f, cls in analysis.function_classes.items():
        tag = tags.tag_of(f)
        if cls == "chartboost":
            assert tag == libtags.CHARTBOOST, hex(f)
        elif tag == libtags.CHARTBOOST:
            # Category methods nothing sends (and their helpers) are dead code here.
            assert cls == "unreferenced", hex(f)
