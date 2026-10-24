"""Checks on the Ghidra exports in work/ghidra/ against the armv7 slice (issue #7).

Ghidra never runs in CI, so these read the files that ``tools/ghidra/import`` wrote and skip
when they are absent: run scripts/bootstrap, then tools/ghidra/import.
"""

import json

import pytest

from mtport import bootstrap as bs
from mtport import ghidra_common as gc
from mtport import macho
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

EXPORT = REPO / "work" / "ghidra"


def load(name):
    path = EXPORT / name
    if not path.is_file():
        pytest.skip(f"{path.relative_to(REPO)} absent (run tools/ghidra/import)")
    return json.loads(path.read_text(encoding="utf-8"))


@pytest.fixture(scope="module")
def image():
    return macho.load(REPO / bs.slice_path("armv7"))


def test_every_function_start_with_its_mode(image):
    doc = load("functions.json")
    by_ptr = {f["address"]: f for f in doc["functions"]}
    starts = image.function_starts
    assert len(starts) == 2655
    for raw in starts:
        f = by_ptr.get(gc.pointer(raw & ~1, bool(raw & 1)))
        assert f is not None, f"no function for start 0x{raw:X}"
        assert f["mode"] == ("thumb" if raw & 1 else "arm")
        assert f["in_function_starts"]
    report = doc["function_starts"]
    assert (report["matched"], report["missing"], report["wrong_mode"]) == (2655, [], [])
    assert by_ptr["0x27E0"]["mode"] == "arm"


def test_exports_describe_this_slice(image):
    sha = bs.sha256_file(REPO / bs.slice_path("armv7"))
    for name in ("functions.json", "xrefs.json", "strings.json"):
        doc = load(name)
        assert doc["schema"] == gc.SCHEMA_VERSION
        assert doc["program"]["sha256"] == sha
        assert doc["program"]["language"] == "ARM:LE:32:v7"


def test_records_are_sorted():
    funcs = load("functions.json")["functions"]
    keys = [int(f["address"], 16) & ~1 for f in funcs]
    assert keys == sorted(keys) and len(set(keys)) == len(keys)
    xrefs = load("xrefs.json")["xrefs"]
    keys = [(int(x["from"], 16), x["operand"], x["to"], x["type"]) for x in xrefs]
    assert keys == sorted(keys)


def test_strings_include_cfstrings_and_utf16(image):
    strings = load("strings.json")["strings"]
    assert strings == sorted(strings, key=lambda r: int(r["address"], 16))
    kinds = {}
    for r in strings:
        kinds[r["kind"]] = kinds.get(r["kind"], 0) + 1
    cf = image.section("__DATA", "__cfstring")
    assert kinds["cfstring"] == cf.size // 16 == 367
    utf16 = [r for r in strings if r["kind"] == "cfstring" and r["encoding"] == "utf-16-le"]
    assert len(utf16) == 1 and utf16[0]["target"] == "0xEAEE8"
    assert kinds["ustring"] >= 1
