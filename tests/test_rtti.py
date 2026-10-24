"""C++ RTTI / vtable recovery (mtport.rtti) on the synthetic image of rttifixtures.py."""

from __future__ import annotations

import json
import struct

import capstone
import pytest

import rttifixtures as fx
from mtport import macho, names, rtti


@pytest.fixture(scope="module")
def built():
    return fx.build()


@pytest.fixture(scope="module")
def addrs(built):
    return built[1]


@pytest.fixture(scope="module")
def image(built):
    return macho.MachO(built[0])


@pytest.fixture(scope="module")
def r(image):
    return rtti.analyze(image)


def h(addr: int) -> str:
    return f"0x{addr:X}"


# -- demangling -------------------------------------------------------------------------------


@pytest.mark.parametrize(
    ("mangled", "name"),
    [
        ("6IState", "IState"),
        ("26CFloatingBGFlowerAlgorithm", "CFloatingBGFlowerAlgorithm"),
        ("N2ui5NamedE", "ui::Named"),
        ("N1a1b1cE", "a::b::c"),
        ("3Foo_", None),  # trailing junk
        ("4Foo", None),  # too short
        ("05Shape", None),
        ("N3FooE", None),  # a nested name needs two parts
        ("N3Foo3BarIiEE", None),  # templates are not handled
        ("St9exception", None),
        ("", None),
    ],
)
def test_demangle_type(mangled, name):
    assert rtti.demangle_type(mangled) == name


# -- code decoding ------------------------------------------------------------------------------


def _capstone_target(code: bytes, at: int, thumb: bool) -> int:
    mode = capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM
    md = capstone.Cs(capstone.CS_ARCH_ARM, mode)
    insn = next(md.disasm(code, at))
    assert insn.mnemonic in ("bl", "blx"), insn.mnemonic
    return int(insn.op_str.lstrip("#"), 16), insn.mnemonic


@pytest.mark.parametrize(
    ("at", "target", "blx"),
    [
        (0x1800, 0x1900, False),
        (0x1900, 0x1800, False),
        (0x2A3C0, 0xF0E2C, True),
        (0x2A3C2, 0xF0E2C, True),  # BLX from a halfword-aligned PC: Align(PC, 4)
        (0xF0000, 0x27E0, False),
        (0x10000, 0x10000 + 0xFFFFFE, False),
        (0x1000000, 0x10, False),
    ],
)
def test_thumb_calls_match_capstone(at, target, blx):
    code = fx.thumb_bl(at, target | (0 if blx else 1), blx=blx)
    want = target & ~3 if blx else target | 1
    assert rtti.call_targets(code, at, thumb=True) == [want]
    got, mnemonic = _capstone_target(code, at, thumb=True)
    assert mnemonic == ("blx" if blx else "bl")
    assert got == want & ~1


def test_arm_calls_match_capstone():
    at = 0x27E0
    bl = 0xEB000000 | (((0x3000 - (at + 8)) >> 2) & 0xFFFFFF)  # bl 0x3000
    blx = 0xFA000000 | (((0x1002 - (at + 12)) >> 2) & 0xFFFFFF) | 1 << 24  # blx 0x1002 (H=1)
    blne = 0x1B000000 | (((0x2000 - (at + 16)) >> 2) & 0xFFFFFF)  # blne 0x2000
    code = struct.pack("<4I", bl, blx, blne, 0xE12FFF1E)  # ..., bx lr
    assert rtti.call_targets(code, at, thumb=False) == [0x3000, 0x1003, 0x2000]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
    got = [
        int(i.op_str.lstrip("#"), 16) for i in md.disasm(code, at) if i.mnemonic.startswith("bl")
    ]
    assert got == [0x3000, 0x1002, 0x2000]


def test_thumb_call_scan_skips_other_instructions():
    code = fx.h(fx.PUSH_R7_LR) + fx.movw(3, 0x1234) + fx.thumb_bl(0x1806, 0x1901) + fx.h(0xBD80)
    assert rtti.call_targets(code, 0x1800, thumb=True) == [0x1901]
    assert rtti.call_targets(code[:-4], 0x1800, thumb=True) == []  # cut inside the BL


def test_pc_relative_values():
    at = 0x9000
    code = (
        fx.movw(12, 0x62E2)  # 0x9000
        + fx.movt(12, 0x8)  # 0x9004
        + fx.add_pc(12)  # 0x9008: ip = 0x862E2 + 0x900C
        + fx.h(0x4801)  # 0x900A: ldr r0, [pc, #4] -> literal at Align(0x900E, 4) + 4 = 0x9010
        + fx.h(0x4478)  # 0x900C: add r0, pc -> literal + 0x9010
        + fx.h(fx.BX_LR)  # 0x900E
        + struct.pack("<I", 0x100)  # 0x9010: the literal
        + fx.add_pc(3)  # r3 was never loaded: no value
    )
    assert rtti.pc_relative_values(code, at) == [0x862E2 + 0x900C, 0x100 + 0x9010]
    md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
    text = [f"{i.mnemonic} {i.op_str}" for i in md.disasm(code[:16], at)]
    assert text[:5] == [
        "movw ip, #0x62e2",
        "movt ip, #8",
        "add ip, pc",
        "ldr r0, [pc, #4]",
        "add r0, pc",
    ]


def test_stub_symbol_and_delete_calls(image, addrs):
    img = rtti.Image(image)
    assert img.stub_symbol(addrs["stub_delete"]) == "__ZdlPv"
    assert img.stub_symbol(addrs["stub_delete"] | 1) is None
    assert img.calls_import(addrs["Shape::D0"], rtti.OPERATOR_DELETE)
    assert not img.calls_import(addrs["Shape::vf3"], rtti.OPERATOR_DELETE)
    assert img.calls(addrs["Badge::D1@thunk"]) == [addrs["Badge::D1"]]
    assert img.string_refs(addrs["Circle::Draw"]) == ["Circle.Draw.1"]


# -- type_info objects --------------------------------------------------------------------------


def test_typeinfos(r, addrs):
    got = {t.name: t for t in r.typeinfos}
    assert sorted(got) == ["Badge", "Circle", "Plain", "Shape", "ui::Named"]
    assert [t.kind for t in r.typeinfos] == ["class", "si", "class", "vmi", "class"]
    assert got["Shape"].address == addrs["Shape"]
    assert got["Shape"].mangled == "5Shape" and got["Shape"].name_address == addrs["Shape.name"]
    assert got["ui::Named"].mangled == "N2ui5NamedE"
    assert r.bases("Circle") == [("Shape", rtti.Base(addrs["Shape"], 0))]
    assert r.bases("Badge") == [
        ("Shape", rtti.Base(addrs["Shape"], 0, True, False)),
        ("ui::Named", rtti.Base(addrs["Named"], 8, True, False)),
    ]
    assert got["Badge"].size == 32 and got["Circle"].size == 12 and got["Plain"].size == 8
    assert r.primary_base("Badge") == "Shape"
    assert r.derived("Shape") == ["Circle", "Badge"]
    assert r.ancestors("Badge") == {"Shape", "ui::Named"}


def _image_with(mutate) -> macho.MachO:
    data, a = fx.build()
    data = bytearray(data)
    mutate(data, a, macho.MachO(bytes(data)))
    return macho.MachO(bytes(data))


def test_bad_base_pointer_is_an_error():
    def mutate(data, a, img):
        struct.pack_into("<I", data, img.vm_to_offset(a["Circle"] + 8), a["Circle.vtable"])

    with pytest.raises(rtti.RttiError, match="is not a type_info"):
        rtti.analyze(_image_with(mutate))


def test_bad_typeinfo_addend_is_an_error():
    data, a = fx.build()
    stream = fx.bind_stream([(a["Shape"], fx.CLASS_TI, 4)])
    image = macho.MachO(fx.ImageBuilder(fx.LAYOUT).build(bind=stream))
    with pytest.raises(rtti.RttiError, match="addend 4"):
        rtti.find_typeinfos(image)


# -- vtables ------------------------------------------------------------------------------------


def test_vtables(r, addrs):
    vts = {(v.cls, v.offset_to_top): v for v in r.vtables}
    assert sorted(vts) == [
        ("Badge", -8),
        ("Badge", 0),
        ("Circle", 0),
        ("Shape", 0),
        ("ui::Named", 0),
    ]
    shape = vts[("Shape", 0)]
    assert shape.address == addrs["Shape.vtable"] and shape.address_point == shape.address + 8
    assert [s.target for s in shape.slots] == [
        addrs["Shape::D1"],
        addrs["Shape::D0"],
        0,
        addrs["Shape::vf3"],
    ]
    assert [s.pure for s in shape.slots] == [False, False, True, False]
    assert shape.end == "typeinfo"
    assert vts[("Badge", 0)].end == "vtable"
    second = vts[("Badge", -8)]
    assert second.end == "data" and not second.primary
    assert r.subobject_of(second) == "ui::Named"
    assert [s.target for s in second.slots] == [addrs["Badge::D1@thunk"], addrs["Badge::D0@thunk"]]
    assert r.primary_vtable("Plain") is None


def test_destructors_and_names(r, addrs):
    assert r.destructors == {"Shape": 0, "Circle": 0, "ui::Named": 0, "Badge": 0}
    by_name = {f.name: f for f in r.functions.values()}
    assert by_name["Shape::~Shape"].address == addrs["Shape::D1"]
    assert by_name["Shape::~Shape"].kind == "complete-dtor"
    assert by_name["Shape::~Shape_deleting"].kind == "deleting-dtor"
    assert by_name["ui::Named::~Named"].address == addrs["Named::D1"]
    # Shape::vf3 is inherited by Circle and Badge: owned by Shape, used three times.
    vf3 = by_name["Shape::vf3"]
    assert vf3.owner == "Shape" and {c for c, _, _ in vf3.uses} == {"Shape", "Circle", "Badge"}
    # Slot 2 is pure in Shape; Circle's override looks up "Circle.Draw.N", naming the slot.
    assert r.slot_names[("Shape", 2)][0] == "Draw"
    assert by_name["Circle::Draw"].address == addrs["Circle::Draw"]
    assert by_name["Badge::Draw"].address == addrs["Badge::vf2"]
    thunk = by_name["Badge::~Badge@thunk-8"]
    assert thunk.kind == "thunk" and thunk.thunk_of == addrs["Badge::D1"] and thunk.adjust == -8
    assert by_name["Badge::~Badge_deleting@thunk-8"].thunk_of == addrs["Badge::D0"]
    assert len(r.functions) == 13
    assert all(f.size > 0 for f in r.functions.values())


def test_slot_check(r, addrs):
    exc = rtti.slot_exceptions(r)
    assert exc == [
        {
            "class": "Shape",
            "vtable": h(addrs["Shape.vtable"]),
            "index": 2,
            "slot": h(addrs["Shape.pure_slot"]),
            "target": "0x0",
            "pure": True,
            "reason": "pure virtual: bound to ___cxa_pure_virtual (0 in the file)",
        }
    ]
    assert rtti.layout_problems(r) == []


def test_slot_not_a_function_start_is_reported(addrs):
    def mutate(data, a, img):  # Circle::Draw + 2: inside the function, still in __text
        struct.pack_into(
            "<I", data, img.vm_to_offset(a["Circle.vtable"] + 16), a["Circle::Draw"] + 2
        )

    r = rtti.analyze(_image_with(mutate))
    bad = [e for e in rtti.slot_exceptions(r) if not e["pure"]]
    assert [(e["class"], e["index"], e["reason"]) for e in bad] == [
        ("Circle", 2, "not a function start")
    ]


def test_layout_problem_when_secondary_vtable_is_missing():
    def mutate(data, a, img):  # turn the secondary vtable's offset_to_top into junk
        struct.pack_into("<I", data, img.vm_to_offset(a["Badge.vtable2"]), 0x7)

    r = rtti.analyze(_image_with(mutate))
    assert "Badge: no secondary vtable for base ui::Named at 8" in rtti.layout_problems(r)


# -- outputs ------------------------------------------------------------------------------------


def test_json(r, addrs):
    doc = rtti.to_json(r, {"path": "x", "arch": "armv7"})
    json.dumps(doc)  # serializable
    assert doc["schema"] == rtti.SCHEMA_VERSION
    assert doc["counts"]["classes"] == 5
    assert doc["counts"]["vtables"] == 5 and doc["counts"]["secondary_vtables"] == 1
    assert doc["counts"]["pure_slots"] == 1 and doc["counts"]["non_pure_slot_exceptions"] == 0
    badge = next(c for c in doc["classes"] if c["name"] == "Badge")
    assert badge["typeinfo_kind"] == "__vmi_class_type_info"
    assert badge["bases"][1] == {"name": "ui::Named", "offset": 8, "public": True, "virtual": False}
    assert badge["destructor_slots"] == [0, 1]
    slot = badge["vtables"][0]["slots"][2]
    assert slot == {
        "index": 2,
        "slot": h(addrs["Badge.vtable"] + 16),
        "target": h(addrs["Badge::vf2"]),
        "pure": False,
        "name": "Badge::Draw",
        "in_function_starts": True,
    }


def test_names_json_follows_the_contract(r, addrs):
    doc = rtti.names_json(r)
    source, entries = names.parse(doc, "rtti.json")
    assert source == "rtti"
    by_addr = {e.address: e for e in entries}
    assert by_addr[addrs["Circle::Draw"]].name == "Circle::Draw"
    assert by_addr[addrs["Circle::Draw"]].kind == "function"
    assert by_addr[addrs["Shape"]].name == "Shape::typeinfo"
    assert by_addr[addrs["Shape.name"]].name == "Shape::typeinfo_name"
    assert by_addr[addrs["Badge.vtable2"]].name == "Badge::vtable_for_ui_Named"
    assert by_addr[addrs["Badge.vtable2"]].kind == "data"
    assert names.ghidra_symbol("ui::Named::~Named") == (("ui", "Named"), "~Named")
    assert names.ghidra_symbol("Badge::~Badge@thunk-8") == (("Badge",), "~Badge@thunk-8")
    assert len(entries) == 13 + 2 * 5 + 5


def test_markdown(r):
    md = rtti.to_markdown(r, {"path": "x", "arch": "armv7"}, "A note.")
    assert "A note." in md
    assert "    Shape <|-- Badge\n" in md
    assert "    ui_Named <|-- Badge : +8\n" in md
    assert "Classes with no base and no derived class (1): `Plain`." in md
    assert "### Shape interface" in md
    assert "| 2 | Draw | pure | `0x" in md
    assert "### Plain\n" in md and "No vtable in the image." in md
    assert "pure virtual `Draw`" in md
    assert "1 exceptions:" in md
    assert "all consistent." in md


def test_generate_and_cli(tmp_path, built, capsys):
    slice_path = tmp_path / "slice"
    slice_path.write_bytes(built[0])
    counts = rtti.generate(tmp_path, slice_path)
    assert counts["classes"] == 5
    for rel in (rtti.JSON_OUT, rtti.NAMES_OUT, rtti.MARKDOWN_OUT):
        assert (tmp_path / rel).is_file()
    names.load_dir(tmp_path / "work/re/names")
    capsys.readouterr()
    assert rtti.main(["json", str(slice_path)]) == 0
    doc = json.loads(capsys.readouterr().out)
    assert doc["counts"]["functions"] == 13
    assert rtti.main(["json", str(tmp_path / "missing")]) == 1


def test_known_slots_name_slots_without_keys(image, addrs):
    """Call-site evidence (KNOWN_SLOTS) names a slot no key names; keys win on a clash."""
    known = {
        ("Shape", 3): ("Area", "called by the layout pass"),
        ("Shape", 2): ("Render", "contradicts the Circle.Draw.N keys"),
        ("Circle", 3): ("Ignored", "Circle did not introduce slot 3"),
        ("NoSuchClass", 1): ("Ignored", "not in the image"),
    }
    r = rtti.analyze(image, known)
    by_name = {f.name: f for f in r.functions.values()}
    assert r.method("Shape", 3) == r.method("Badge", 3) == "Area"
    assert by_name["Shape::Area"].address == addrs["Shape::vf3"]
    assert r.method("Shape", 2) == "Draw"
    assert any("call sites say Render" in e for e in r.evidence["Shape"])
    assert "Ignored" not in {r.method(c, 3) for c in ("Shape", "Circle", "Badge")}


def test_default_known_slots_do_not_touch_other_images(r):
    """KNOWN_SLOTS is keyed by MapleThief's classes, which the synthetic image doesn't have."""
    assert set(r.slot_names) == {("Shape", 2)}
