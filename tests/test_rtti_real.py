"""Real-asset checks for the C++ RTTI / vtable recovery against the armv7 slice (issue #9).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first. The Ghidra cross-checks
also skip until tools/ghidra/import (and, for the names, tools/ghidra/apply-names after
``python3 -m mtport.rtti generate``) has written work/ghidra/.
"""

from __future__ import annotations

import bisect
import json

import pytest

from mtport import bootstrap as bs
from mtport import macho, rtti
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")
EXPORT = REPO / "work" / "ghidra"

# docs/ANALYSIS.md -> C++ engine, minus `CPW` (not an RTTI name, see test_cpw_is_not_rtti).
CLASSES = {
    "CMapleWorld", "CMapleObject", "CHero", "CMob", "CMobs", "CItem", "CDropItems", "CSelItem",
    "CMapArms", "CMobArms", "CQuest", "CQuestList", "CReward", "CRequire", "CSkill", "CMMap",
    "CMMapTileSet", "CMLayer", "CMSprite", "CMSpriteImage", "CMImage", "CMFrame", "CMAction",
    "CAniManager", "SkyCanvas", "CMResMgr", "CDataMgr", "DataReader", "CLoaderThread",
    "Stringtable", "TaggedString", "SVectorClass", "CMSoundPlayer", "CMSocket", "CMTimer",
    "CMUtil", "UtilAdaptor", "IState", "IStateAdaptor", "CStateTitle", "CStateMainMenu",
    "CStateGame", "CPopup", "CMPopup", "CChatBox", "CMScrollBar", "CMsg", "CNotifyText",
    "CNotifyTextList", "CFloating", "CFloatingBG", "CFloatingEff", "CFloatingBGImage",
    "CFloatingBGAlgorithm", "CFloatingBGFlowerAlgorithm", "CDamageFloatings",
    "CEffectFloatings", "Lua", "TFunc", "Table", "Symbol", "Undump", "Build",
}  # fmt: skip

# Every class with more than one base or a base at a non-zero offset, and a few chains.
BASES = {
    "IStateAdaptor": [("IState", 0), ("UtilAdaptor", 4)],
    "CFloatingBG": [("CFloatingEff", 0), ("UtilAdaptor", 36)],
    "CStateGame": [("IStateAdaptor", 0)],
    "CStateMainMenu": [("IStateAdaptor", 0)],
    "CStateTitle": [("IStateAdaptor", 0)],
    "UtilAdaptor": [("CMUtil", 0)],
    "CMUtil": [("Build", 0)],
    "CHero": [("CMapleObject", 0)],
    "CMapleObject": [("UtilAdaptor", 0)],
    "Lua": [("UtilAdaptor", 0)],
    "TFunc": [("UtilAdaptor", 0)],
    "Undump": [("UtilAdaptor", 0)],
    "IState": [],
    "Table": [],
    "Symbol": [],
}


@pytest.fixture(scope="module")
def data() -> bytes:
    return SLICE.read_bytes()


@pytest.fixture(scope="module")
def image(data) -> macho.MachO:
    return macho.MachO(data)


@pytest.fixture(scope="module")
def r(image) -> rtti.Rtti:
    return rtti.analyze(image)


def test_all_classes_with_bases(r):
    assert {t.name for t in r.typeinfos} == CLASSES
    assert len(r.typeinfos) == 63
    counts = rtti.counts(r)
    assert (counts["class_type_info"], counts["si_class_type_info"]) == (21, 40)
    assert counts["vmi_class_type_info"] == 2
    for cls, want in BASES.items():
        got = [(name, b.offset) for name, b in r.bases(cls)]
        assert got == want, cls
        assert all(b.public and not b.virtual for _, b in r.bases(cls))


def test_cpw_is_not_rtti(image, r):
    """ANALYSIS.md once listed a 64th class, CPW: its bytes only occur inside Thumb code."""
    assert "CPW" not in {t.name for t in r.typeinfos}
    text = image.section("__TEXT", "__text")
    hits = []
    start = 0
    while (off := image.data.find(b"CPW", start)) >= 0:
        hits.append(off)
        start = off + 1
    assert len(hits) == 6
    for off in hits:
        vm = text.addr + off - text.offset
        assert text.contains(vm), hex(vm)
        assert 0xBCED8 <= vm <= 0xBCFE6


def test_every_slot_is_a_function_start(r, image):
    """Acceptance (#9): every vtable slot is an LC_FUNCTION_STARTS function; the only
    exceptions are the pure virtual slots, bound to ___cxa_pure_virtual."""
    starts = set(image.function_starts)
    slots = [(v, s) for v in r.vtables for s in v.slots]
    assert len(r.vtables) == 68 and len(slots) == 266
    pure = [(v, s) for v, s in slots if s.pure]
    assert all(s.target in starts for _, s in slots if not s.pure)
    assert len(pure) == 31
    for _, s in pure:
        assert image.bound_symbol(s.address) == rtti.PURE_VIRTUAL
        assert image.u32(s.address) == 0
    exc = rtti.slot_exceptions(r)
    assert len(exc) == 31 and all(e["pure"] for e in exc)
    assert {e["class"] for e in exc} == {
        "IState",
        "IStateAdaptor",
        "CMapleObject",
        "CFloatingBGAlgorithm",
    }


def test_vtable_layout(r):
    assert rtti.layout_problems(r) == []
    assert {t.name for t in r.typeinfos} == {v.cls for v in r.vtables}
    # The walk stops at a structural boundary: the next vtable, the next type_info, or the
    # padding words after the two IStateAdaptor-style secondary vtables.
    assert {v.end for v in r.vtables} == {"typeinfo", "vtable", "data"}
    assert sorted((v.cls, v.end) for v in r.vtables if v.end == "data") == [
        ("CFloatingBG", "data"),
        ("IStateAdaptor", "data"),
    ]
    secondary = {(v.cls, v.offset_to_top, r.subobject_of(v)) for v in r.vtables if not v.primary}
    assert secondary == {
        ("IStateAdaptor", -4, "UtilAdaptor"),
        ("CStateGame", -4, "UtilAdaptor"),
        ("CStateMainMenu", -4, "UtilAdaptor"),
        ("CStateTitle", -4, "UtilAdaptor"),
        ("CFloatingBG", -36, "UtilAdaptor"),
    }
    sizes = {t.name: len(r.primary_vtable(t.name).slots) for t in r.typeinfos}
    assert (sizes["IState"], sizes["IStateAdaptor"], sizes["CStateMainMenu"]) == (23, 24, 25)
    assert sizes["CMapleObject"] == 5 and sizes["Lua"] == sizes["UtilAdaptor"] == 2


def test_destructors_and_names(r):
    assert r.destructors == {t.name: 0 for t in r.typeinfos}
    kinds = [f.kind for f in r.functions.values()]
    assert (kinds.count("complete-dtor"), kinds.count("deleting-dtor")) == (63, 63)
    assert kinds.count("thunk") == 10
    by_name = {f.name: f for f in r.functions.values()}
    assert len(by_name) == len(r.functions) == 189
    for cls in ("Lua", "TFunc", "Undump", "IState", "CStateGame"):
        assert f"{cls}::~{cls}" in by_name and f"{cls}::~{cls}_deleting" in by_name
    # Method names from leaked localization keys (CStateGame.Paint.7, CHero.Flush.1, ...).
    assert r.method("IState", 9) == "Paint" and r.method("IState", 6) == "OnKeyPressed"
    assert r.method("CMapleObject", 4) == "Flush"
    for name in ("CStateGame::Paint", "CStateTitle::Paint", "CStateMainMenu::Paint"):
        assert by_name[name].index == 9
    assert by_name["CStateGame::OnKeyPressed"].index == 6
    # Names from call sites (KNOWN_SLOTS, #12): the state machine's lifecycle and frame slots.
    for index, method in (
        (2, "Enter"),
        (3, "Exit"),
        (5, "Pause"),
        (7, "OnKeyReleased"),
        (10, "Update"),
        (17, "SetCurrentState"),
        (20, "GetParent"),
    ):
        assert r.method("IState", index) == method
    assert by_name["CStateGame::Update"].address == 0x874E1
    assert by_name["IStateAdaptor::SetCurrentState"].address == 0xAC95
    assert by_name["CStateMainMenu::LoadCharacterSlots"].index == 24
    assert not any("call sites say" in e for v in r.evidence.values() for e in v)
    assert {f.thunk_of for f in r.functions.values() if f.kind == "thunk"} <= set(r.functions)
    assert (
        by_name["CStateGame::~CStateGame@thunk-4"].thunk_of
        == by_name["CStateGame::~CStateGame"].address
    )


def test_committed_markdown_is_current(data, r):
    """Acceptance (#9): docs/re/cpp-classes.md lists every class with its bases."""
    md = rtti.to_markdown(r, rtti.source_info(SLICE, data, REPO), rtti.NOTES)
    committed = (REPO / rtti.MARKDOWN_OUT).read_text(encoding="utf-8")
    assert committed == md, "docs/re/cpp-classes.md is stale: run python3 -m mtport.rtti generate"
    for t in r.typeinfos:
        assert f"\n### {t.name}\n" in md
        row = next(line for line in md.splitlines() if line.startswith(f"| [`{t.name}`]"))
        for base, _ in r.bases(t.name):
            assert f"`{base}`" in row.split(" | ")[1]
            assert f"    {base} <|-- {t.name}" in md


def test_generate_headless(tmp_path, r):
    """Acceptance (#9): work/re/vtables.json comes from a headless script."""
    rtti.generate(tmp_path, SLICE)
    doc = json.loads((tmp_path / rtti.JSON_OUT).read_text(encoding="utf-8"))
    assert doc["counts"] == rtti.counts(r)
    assert len(doc["classes"]) == 63 and doc["layout_problems"] == []
    assert len(doc["slot_exceptions"]) == 31 and all(e["pure"] for e in doc["slot_exceptions"])
    names_doc = json.loads((tmp_path / rtti.NAMES_OUT).read_text(encoding="utf-8"))
    assert sum(e["kind"] == "function" for e in names_doc["names"]) == 189


def _export(name: str) -> dict:
    path = EXPORT / name
    if not path.is_file():
        pytest.skip(f"{path.relative_to(REPO)} absent (run tools/ghidra/import)")
    return json.loads(path.read_text(encoding="utf-8"))


def test_string_refs_agree_with_ghidra(image, r):
    """The cheap PC-relative scan agrees with Ghidra on the method keys that name slots.

    Neither is a superset: Ghidra also resolves selector strings loaded through
    ``__objc_selrefs`` (not PC-relative), and loses track of some constants in the 16-24 KB
    -O0 functions (``CStateGame.Paint.1``-``3``) that the scan sees. Every method key Ghidra
    finds in a slot function must be found by the scan, and every key only the scan finds
    must name the same method as a key Ghidra confirms in that function."""
    strings = _export("strings.json")["strings"]
    funcs = sorted(int(f["address"], 16) & ~1 for f in _export("functions.json")["functions"])
    ghidra: dict[int, set[str]] = {}
    for s in strings:
        for ref in s["refs"]:
            at = int(ref, 16)
            fn = funcs[bisect.bisect_right(funcs, at) - 1]
            ghidra.setdefault(fn, set()).add(s["value"])
    img = rtti.Image(image)

    def methods(keys):
        return {m.group(1, 2) for m in map(rtti.METHOD_KEY.fullmatch, keys) if m}

    confirmed = total = 0
    for f in r.functions.values():
        ours = set(img.string_refs(f.address))
        theirs = ghidra.get(f.address & ~1, set())
        assert methods(theirs) <= methods(ours), f.name
        assert methods(ours - theirs) <= methods(ours & theirs), f.name
        confirmed += len(ours & theirs)
        total += len(ours)
    assert total >= 80 and confirmed >= 0.8 * total


def test_names_are_in_ghidra(r):
    """After apply-names, every slot function has its Class:: name in functions.json."""
    funcs = {f["address"]: f for f in _export("functions.json")["functions"]}
    names = {f["name"] for f in funcs.values()}
    if not any(n.startswith("IState::") for n in names):
        pytest.skip("work/ghidra/ has no rtti names yet: run tools/ghidra/apply-names")
    for f in r.functions.values():
        got = funcs[f"0x{f.address:X}"]
        assert (got["name"], got["name_source"]) == (f.name, "names")
