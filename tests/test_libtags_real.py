"""Function tags of the real armv7 slice (issue #10).

Needs the slice (scripts/bootstrap), work/re/objc.json (python3 -m mtport.objc generate) and
the Ghidra exports (tools/ghidra/import); skips when any is absent, as in CI. The tags are
recomputed here, so the tests do not depend on a stale work/re/function-tags.json.
"""

import json

import pytest

from mtport import bootstrap as bs
from mtport import libtags, macho
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


def _need(rel):
    path = REPO / rel
    if not path.is_file():
        pytest.skip(
            f"{rel} absent (run scripts/bootstrap, mtport.objc generate, tools/ghidra/import)"
        )
    return path


@pytest.fixture(scope="module")
def image():
    return macho.load(_need(bs.slice_path("armv7")))


@pytest.fixture(scope="module")
def doc(image):
    prog = libtags.build_program(
        image,
        json.loads(_need(libtags.FUNCTIONS_JSON).read_text(encoding="utf-8")),
        json.loads(_need(libtags.XREFS_JSON).read_text(encoding="utf-8")),
        json.loads(_need(libtags.OBJC_JSON).read_text(encoding="utf-8")),
    )
    sha = bs.sha256_file(REPO / bs.slice_path("armv7"))
    return json.loads(libtags.to_json(libtags.tag_program(prog), {"arch": "armv7", "sha256": sha}))


def test_every_function_start_has_exactly_one_tag(doc, image):
    assert len(image.function_starts) == 2655
    counts = libtags.check_doc(doc, image)
    assert sum(counts.values()) == 2655
    assert doc["coverage"] == {"function_starts": 2655, "tagged": 2655}
    assert set(counts) <= {"app-objc", "engine", "unknown"} | {
        t for t in counts if t.startswith("lib:")
    }


def test_objc_imps_keep_their_side(doc):
    objc = json.loads((REPO / libtags.OBJC_JSON).read_text(encoding="utf-8"))
    tags = libtags.FunctionTags(doc)
    for cls in objc["classes"]:
        want = "lib:chartboost" if cls["group"] == "chartboost" else "app-objc"
        for m in cls["instance_methods"] + cls["class_methods"]:
            assert tags.tag_of(m["imp"]) == want, (cls["name"], m["selector"])


def test_known_library_functions(doc):
    tags = libtags.FunctionTags(doc)
    assert tags.tag_of(0x27E0) == "lib:crt1"  # LC_UNIXTHREAD start
    assert tags.tag_of(0x280D) == "app-objc"  # main
    assert tags.tag_of(0xCA69D) == "lib:chartboost"  # exported _bson_init
    assert tags.tag_of(0xC1025) == "lib:uidevice-identifier"  # -[UIDevice macaddress]
    assert tags.tag_of(0xA0045) == "lib:apple-openal-support"  # MyGetOpenALAudioData
    assert tags.tag_of(0x24EA1) == "engine"  # crc32 table inside a C++ constructor


def test_libraries_absent(doc):
    for lib in ("lib:zlib", "lib:png", "lib:lua", "lib:stl"):
        assert lib not in doc["counts"]
        strong = [s for s in doc["signatures"] if s["library"] == lib and s["strong"]]
        assert strong and all(not s["hits"] for s in strong), lib


def test_bytes_cover_text(doc, image):
    text = image.section("__TEXT", "__text")
    first = min(s & ~1 for s in image.function_starts)
    assert sum(c["bytes"] for c in doc["counts"].values()) == text.end - first


def test_vtables_name_engine_classes(doc):
    classes = {v["class"] for v in doc["vtables"]}
    assert {"CMapleWorld", "Lua", "Undump", "CStateGame"} <= classes
    assert not any(c.startswith("std::") for c in classes)


def test_committed_doc_is_current(doc):
    """docs/re/libraries.md is generated; rerun `python3 -m mtport.libtags generate`."""
    committed = (REPO / libtags.MARKDOWN_OUT).read_text(encoding="utf-8")
    assert committed == libtags.to_markdown(doc)


def test_written_tags_file_is_valid(image):
    path = REPO / libtags.JSON_OUT
    if not path.is_file():
        pytest.skip(f"{libtags.JSON_OUT} absent (python3 -m mtport.libtags generate)")
    libtags.check_doc(json.loads(path.read_text(encoding="utf-8")), image)


def test_ghidra_carries_the_tags(doc):
    """apply_tags.py (scripts/re-analyze) put exactly the recomputed tags on the functions."""
    path = REPO / libtags.READBACK_JSON
    if not path.is_file():
        pytest.skip(f"{libtags.READBACK_JSON} absent (run scripts/re-analyze)")
    readback = json.loads(path.read_text(encoding="utf-8"))
    assert readback["count"] == 2655
    assert libtags.readback_problems(doc, readback) == []
