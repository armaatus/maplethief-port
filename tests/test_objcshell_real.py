"""Acceptance (#22): docs/re/objc-shell.md against the armv7 slice, the export and the bundle.

Needs the slice and bundle (scripts/bootstrap) and the RE pipeline outputs (scripts/re-analyze:
the Ghidra export with every names file applied, function-tags.json); skips without them.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from mtport import bootstrap as bs
from mtport import macho, names, nib, objcshell, sitecheck
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

DOC = REPO / objcshell.MARKDOWN
FUNCTIONS = REPO / objcshell.GHIDRA_DIR / "functions.json"


def _need(*paths) -> None:
    for rel in paths:
        if not (REPO / rel).exists():
            pytest.skip(f"{rel} absent (run scripts/bootstrap and scripts/re-analyze)")


@pytest.fixture(scope="module")
def built():
    _need(
        bs.slice_path("armv7"),
        objcshell.BUNDLE,
        objcshell.TAGS_JSON,
        f"{objcshell.GHIDRA_DIR}/functions.json",
    )
    return objcshell.build(REPO)


@pytest.fixture(scope="module")
def shell(built) -> objcshell.Shell:
    return built[1]


def test_generated_blocks_are_current(built):
    """The committed doc equals what `python3 -m mtport.objcshell generate` writes."""
    _, _, _, doc = built
    assert DOC.read_text(encoding="utf-8") == doc, "rerun python3 -m mtport.objcshell generate"


def test_all_18_classes_have_a_section_and_a_block(shell):
    text = DOC.read_text(encoding="utf-8")
    assert len(shell.classes) == 18
    for c in shell.classes:
        assert f"\n### {c.name}\n" in text
        section = text.split(f"\n### {c.name}\n", 1)[1].split("<!-- BEGIN generated:", 1)[0]
        assert len(section.strip()) > 80, f"{c.name}: no hand-written description"
        assert f"<!-- BEGIN generated:class:{c.name} -->" in text
    # The effort table names every class.
    table = text.split("## Effort", 1)[1].split("\n## ", 1)[0]
    for c in shell.classes:
        assert f"`{c.name}`" in table, c.name


def test_every_shell_function_belongs_to_one_class(built):
    """IMPs plus helpers cover every app-objc function except main."""
    p, s, _, _ = built
    covered = [f for c in s.classes for f in c.imps + c.helpers]
    assert len(covered) == len(set(covered))
    app = {ptr for ptr, f in p.functions.items() if f.tag == objcshell.APP_OBJC}
    assert app - set(covered) == {0x280D}  # main
    assert sum(c.methods for c in s.classes) == 563


def test_call_edges_and_engine_senders_are_in_the_doc(shell):
    """Every ObjC -> C++ callee and every C++ -> ObjC sending function is named by address."""
    written = sitecheck.doc_addresses(DOC.read_text(encoding="utf-8"))
    missing = []
    for c in shell.classes:
        for callee in c.engine_calls:
            if callee not in written:
                missing.append(f"{c.name}: callee 0x{callee:X}")
        for sel, fns in c.engine_sends.items():
            missing += [f"{c.name} -{sel}: 0x{f:X}" for f in fns if f not in written]
    assert missing == []


def test_boundary_counts(shell):
    by = {c.name: c for c in shell.classes}
    assert {f for c in shell.classes for f in c.engine_calls} >= {0x5A75, 0x5C2D, 0x5B01, 0x5B8D}
    # SkyCanvas::Tick polls the pad's key state.
    polled = {s for s, fns in by["TouchKeyPadView"].engine_sends.items() if 0x2590D in fns}
    assert {"onAttackKey", "onJumpKey", "onLeftKey", "touchKeyPadEnable"} <= polled
    assert not by["MoviePlayerController"].created_by  # dead
    assert by["ChooseCharacterView"].created_by == [0xAAAB9]


def test_phone_key_table(shell):
    """All 52 click/release methods call Clet_HandleEvent(2|3, constant code, 0)."""
    (pad,) = [c for c in shell.classes if c.name == "TouchKeyPadView"]
    assert len(pad.key_events) == 52
    assert all(
        args[0] in (2, 3) and args[1] is not None and args[2] == 0 for _, args in pad.key_events
    )
    other = [e for c in shell.classes if c.name != "TouchKeyPadView" for e in c.key_events]
    assert other == []


def test_curated_names_are_applied(built):
    exported = {
        int(f["address"], 16): f["name"] for f in json.loads(FUNCTIONS.read_text())["functions"]
    }
    for fp, _, name, _ in objcshell.curated_names():
        assert exported.get(fp) == name, f"0x{fp:X}"


def test_names_file_matches_the_generator():
    _need(bs.slice_path("armv7"), objcshell.NAMES_OUT)
    image = macho.load(REPO / bs.slice_path("armv7"))
    entries = objcshell.build_names(
        image, names.other_names(REPO / objcshell.NAMES_DIR, Path(objcshell.NAMES_OUT).name)
    )
    assert (REPO / objcshell.NAMES_OUT).read_text(encoding="utf-8") == objcshell.names_json(entries)
    names.load_dir(REPO / objcshell.NAMES_DIR)  # no conflicts with the other files


def test_bridges_send_their_selector(built):
    """Each Shell_* bridge is [[[EAGLView_Instance Instance] objView] SEL]."""
    p = built[0]
    for fp, sel in objcshell.BRIDGES:
        sent = [
            e["selector"] for e in sorted(p.sends, key=lambda e: e["site"]) if e["function"] == fp
        ]
        assert sent == ["Instance", "objView", sel], f"0x{fp:X}"


def test_images_exist_in_the_bundle(shell):
    bundle = set(objcshell.bundle_files(REPO / objcshell.BUNDLE))
    for c in shell.classes:
        for text, files in c.images.items():
            assert files and set(files) <= bundle, (c.name, text)
    assert len(shell.bundle_png) == 115
    assert len(shell.shell_png) == 98


def test_nib_block_covers_every_object():
    _need(objcshell.BUNDLE)
    archive = nib.load(REPO / objcshell.BUNDLE / objcshell.NIB_NAME)
    block = DOC.read_text(encoding="utf-8").split("<!-- BEGIN generated:nib -->", 1)[1]
    block = block.split("<!-- END generated:nib -->", 1)[0]
    rows = re.findall(r"^\| (\d+) \| `", block, re.M)
    assert [int(r) for r in rows] == list(range(len(archive.objects))) == list(range(23))
    assert len(archive.outlets()) == 2
    assert "`delegate` | #3 `MapleThiefAppDelegate`" in block
    assert "`window` | #10 `UIWindow`" in block
