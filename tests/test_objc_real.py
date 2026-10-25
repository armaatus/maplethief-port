"""Real-asset checks for the ObjC metadata parser against the armv7 slice.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first. The ``otool -oV`` and
``dyld_info -fixups`` cross-checks also skip when those macOS tools are not on PATH.
"""

from __future__ import annotations

import re
import shutil
import subprocess

import pytest

from mtport import bootstrap as bs
from mtport import ghidra_common as gc
from mtport import macho, objc
from repo_paths import REPO, assert_committed_doc

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")

# docs/ANALYSIS.md -> Objective-C shell: app class -> (superclass, instance + class methods).
APP_CLASSES = {
    "MapleThiefAppDelegate": ("NSObject", 12),
    "EAGLView": ("UIView", 80),
    "EAGLView_Instance": ("NSObject", 9),
    "EAGLViewController": ("UIViewController", 7),
    "MainMenuView": ("UIView", 57),
    "ChooseCharacterView": ("UIView", 29),
    "TermsView": ("UIView", 5),
    "TouchKeyPadView": ("UIView", 149),
    "MapKeyPadView": ("UIView", 15),
    "NpcKeyPadView": ("UIView", 47),
    "PopupKeyPadView": ("UIView", 42),
    "MarketKeyPadView": ("UIView", 40),
    "CashShopKeyPadView": ("UIView", 21),
    "HelpScrollKeyPadView": ("UIView", 5),
    "MovieOverlayView": ("UIView", 2),
    "MoviePlayerController": ("UIViewController", 6),
    "GameCenterManager": ("NSObject", 18),
    "InAppPurchaseManager": ("NSObject", 19),
}


@pytest.fixture(scope="module")
def data() -> bytes:
    return SLICE.read_bytes()


@pytest.fixture(scope="module")
def image(data) -> macho.MachO:
    return macho.MachO(data)


@pytest.fixture(scope="module")
def parsed(image) -> objc.ObjCImage:
    return objc.parse(image)


# -- otool -oV --------------------------------------------------------------------------------


def _indent(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def parse_otool(text: str) -> dict:
    """Classes, categories and protocol names from ``otool -oV`` output.

    Only the fields compared below are read: each class's name, superclass and base method
    lists (class and metaclass), each category's name, class and method lists, and the
    top-level protocol names. Nested protocol dumps are deeper indented and ignored.
    """
    out: dict = {"classes": [], "categories": [], "protocols": []}
    section = ""
    cur: dict | None = None
    methods: list | None = None
    key = None
    meta = False
    for line in text.splitlines():
        if line.startswith("Contents of ("):
            section = line.split(",", 1)[1].split(")", 1)[0]
            cur = None
            continue
        if re.match(r"^[0-9a-f]{8} 0x[0-9a-f]+$", line):
            cur = {"name": None, "super": None, "inst": [], "cls": []}
            meta, key = False, None
            if section == "__objc_classlist":
                out["classes"].append(cur)
            elif section == "__objc_catlist":
                out["categories"].append(cur)
            elif section == "__objc_protolist":
                out["protocols"].append(cur)
            continue
        if cur is None:
            continue
        if line == "Meta Class":
            meta, key = True, None
            continue
        indent, parts = _indent(line), line.split(None, 2)
        if not parts:
            continue
        if section == "__objc_classlist":
            if indent == 4 and parts[0] == "superclass" and not meta:
                cur["super"] = parts[1:]
            elif indent == 8:
                key = parts[0]
                if key == "name" and not meta:
                    cur["name"] = parts[2]
                methods = cur["cls" if meta else "inst"] if key == "baseMethods" else None
            elif indent == 12 and methods is not None:
                _method_line(methods, parts)
        elif section == "__objc_catlist":
            if indent == 4:
                key = parts[0]
                if key == "name":
                    cur["name"] = parts[2]
                elif key == "cls":
                    cur["super"] = parts[1:]
                methods = {"instanceMethods": cur["inst"], "classMethods": cur["cls"]}.get(key)
            elif indent == 8 and methods is not None:
                _method_line(methods, parts)
        elif section == "__objc_protolist" and indent == 4 and parts[0] == "name":
            cur["name"] = parts[2]
    return out


def _method_line(methods: list, parts: list[str]) -> None:
    if parts[0] == "name":
        methods.append([parts[2], None, None])
    elif parts[0] == "types":
        methods[-1][1] = parts[2]
    elif parts[0] == "imp":
        methods[-1][2] = int(parts[1], 16)


@pytest.fixture(scope="module")
def otool():
    if shutil.which("otool") is None:
        pytest.skip("otool not on PATH (macOS Xcode / Command Line Tools)")
    text = subprocess.run(
        ["otool", "-oV", str(SLICE)], check=True, capture_output=True, text=True
    ).stdout
    return parse_otool(text)


def _triples(methods: list[objc.Method]) -> list[list]:
    return [[m.selector, m.types, m.imp] for m in methods]


def test_classes_match_otool(parsed, otool):
    """Acceptance (#8): all 75 classes match otool -oV (method counts and IMP addresses)."""
    assert len(otool["classes"]) == len(parsed.classes) == 75
    total = 0
    for ours, theirs in zip(parsed.classes, otool["classes"], strict=True):
        assert ours.name == theirs["name"]
        sup = theirs["super"]
        if sup[0] == "0x0":
            assert ours.superclass.address is None
            assert sup[1] == objc.CLASS_SYMBOL + ours.superclass.name, ours.name
        else:
            assert ours.superclass.address == int(sup[0], 16), ours.name
        assert len(ours.instance_methods) == len(theirs["inst"]), ours.name
        assert len(ours.class_methods) == len(theirs["cls"]), ours.name
        assert _triples(ours.instance_methods) == theirs["inst"], ours.name
        assert _triples(ours.class_methods) == theirs["cls"], ours.name
        total += len(theirs["inst"]) + len(theirs["cls"])
    assert total == 563 + 486


def test_categories_and_protocols_match_otool(parsed, otool):
    assert len(otool["categories"]) == len(parsed.categories) == 4
    for ours, theirs in zip(parsed.categories, otool["categories"], strict=True):
        assert ours.name == theirs["name"]
        assert theirs["super"] == ["0x0", objc.CLASS_SYMBOL + ours.class_name]
        assert _triples(ours.instance_methods) == theirs["inst"]
        assert _triples(ours.class_methods) == theirs["cls"]
    listed = [p["name"] for p in otool["protocols"]]
    assert len(listed) == 19
    assert [p.name for p in parsed.protocols[: len(listed)]] == listed


# -- dyld_info -fixups ------------------------------------------------------------------------


def test_binds_match_dyld_info(image):
    """Bind addresses, kinds and symbols agree with dyld_info -fixups.

    dyld_info (macOS 26) pairs many armv7 bind addresses with the wrong symbol: it names the
    ``cache`` slot of every class_t ``__objc_empty_vtable`` and the ``vtable`` slot
    ``_AudioServicesPlaySystemSound``. So only the address/kind set and the symbol set are
    compared here; :func:`test_bound_symbols_are_consistent` and the otool classref check
    pin down which symbol goes where.
    """
    if shutil.which("dyld_info") is None:
        pytest.skip("dyld_info not on PATH (macOS)")
    text = subprocess.run(
        ["dyld_info", "-fixups", str(SLICE)], check=True, capture_output=True, text=True
    ).stdout
    slots, symbols = set(), set()
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 5 and parts[3] in ("bind", "lazy-bind"):
            slots.add((int(parts[2], 16), parts[3]))
            symbols.add(parts[4].split("/", 1)[-1])
    ours = [b for b in image.binds if b.kind != "weak"]
    assert len(slots) == len(ours) == 1055 + 213
    assert {(b.address, "lazy-bind" if b.kind == "lazy" else "bind") for b in ours} == slots
    assert {b.symbol for b in ours} == symbols


def test_bound_symbols_are_consistent(image, parsed):
    """Every class_t's cache/vtable slot and every CFString's isa bind the expected symbol."""
    for k in parsed.classes:
        for cls in (k.address, k.metaclass):
            assert image.bound_symbol(cls + 8) == "__objc_empty_cache", k.name
            assert image.bound_symbol(cls + 12) == "__objc_empty_vtable", k.name
    cfstring = image.section("__DATA", "__cfstring")
    for addr in range(cfstring.addr, cfstring.end, 16):
        assert image.bound_symbol(addr) == "___CFConstantStringClassReference", hex(addr)


def test_class_and_super_refs_match_otool(parsed):
    if shutil.which("otool") is None:
        pytest.skip("otool not on PATH (macOS Xcode / Command Line Tools)")
    text = subprocess.run(
        ["otool", "-oV", str(SLICE)], check=True, capture_output=True, text=True
    ).stdout
    tables: dict[str, list] = {"__objc_classrefs": [], "__objc_superrefs": []}
    section = ""
    for line in text.splitlines():
        if line.startswith("Contents of ("):
            section = line.split(",", 1)[1].split(")", 1)[0]
        elif section in tables and (
            m := re.match(r"^([0-9a-f]{8}) 0x([0-9a-f]+)(?: (\S+))?$", line)
        ):
            tables[section].append((int(m[1], 16), int(m[2], 16), m[3]))
    for name, ours in (
        ("__objc_classrefs", parsed.classrefs),
        ("__objc_superrefs", parsed.superrefs),
    ):
        theirs = tables[name]
        assert len(theirs) == len(ours) > 0, name
        for (slot, ref), (their_slot, value, symbol) in zip(ours, theirs, strict=True):
            assert slot == their_slot
            if value:
                assert ref.address == value, hex(slot)
            else:
                prefix = objc.METACLASS_SYMBOL if ref.meta else objc.CLASS_SYMBOL
                assert (ref.address, prefix + ref.name) == (None, symbol), hex(slot)


# -- facts and generated outputs ----------------------------------------------------------------


def test_counts_match_analysis(parsed):
    c = objc.counts(parsed)
    assert (c["classes"], c["app_classes"], c["chartboost_classes"]) == (75, 18, 57)
    assert (c["categories"], c["protocols"]) == (4, 19)
    assert (c["app_methods"], c["chartboost_methods"], c["category_methods"]) == (563, 486, 15)
    assert c["selrefs"] == 0xEDC // 4 == 951
    app = {k.name: k for k in parsed.classes if k.group == "app"}
    assert set(app) == set(APP_CLASSES)
    for name, (superclass, count) in APP_CLASSES.items():
        k = app[name]
        assert (k.superclass.name, len(k.instance_methods) + len(k.class_methods)) == (
            superclass,
            count,
        ), name
    assert all(r is not None for _, r in parsed.classrefs + parsed.superrefs)


def test_every_imp_is_a_function_start(image, parsed):
    imps = objc.imp_names(parsed)
    assert len(imps) == 1064
    assert set(imps) <= set(image.function_starts)


def test_names_document(parsed):
    doc = objc.names_json(parsed)
    objc.check_names(doc)
    assert len(doc["names"]) == 1064
    by_name = {n["name"]: n["address"] for n in doc["names"]}
    assert by_name["-[MapleThiefAppDelegate setWindow:]"] == "0x2879"
    assert by_name["-[UIDevice(IdentifierAddition) macaddress]"] == "0xC1025"
    on_disk = REPO / objc.NAMES_OUT
    if on_disk.exists():
        import json

        objc.check_names(json.loads(on_disk.read_text(encoding="utf-8")))


def test_committed_markdown_is_current(data, parsed):
    """Acceptance (#8): docs/re/objc.md lists all 18 app classes with every method and IMP."""
    md = objc.to_markdown(parsed, gc.source_info(SLICE, data, REPO))
    assert_committed_doc(objc.MARKDOWN_OUT, md, "python3 -m mtport.objc generate")
    for k in parsed.classes:
        if k.group != "app":
            assert f"### {k.name}\n" not in md
            continue
        section = md.split(f"### {k.name}\n", 1)[1].split("\n### ", 1)[0]
        for m in k.instance_methods + k.class_methods:
            assert f"| `{objc.hexaddr(m.imp)}` | `{m.selector}` |" in section, (k.name, m)
