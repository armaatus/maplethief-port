"""Ghidra-free helpers of the Ghidra export (mtport.ghidra_common) on synthetic data."""

import json
import struct

import pytest

from machofixtures import thin_image
from mtport import ghidra_common as gc
from mtport import macho, names


def test_seed_ranges_cover_text_from_each_start():
    # __text is 0x1400..0x1480; one ARM start and two Thumb starts.
    image = macho.MachO(thin_image([0x1400, 0x1411, 0x1441]))
    assert gc.seed_ranges(image) == [
        gc.SeedRange(0x1400, 0x1410, False),
        gc.SeedRange(0x1410, 0x1440, True),
        gc.SeedRange(0x1440, 0x1480, True),
    ]


def test_seed_ranges_reject_starts_outside_sections():
    image = macho.MachO(thin_image([0x1401, 0x2001]))
    with pytest.raises(macho.MachOError):
        gc.seed_ranges(image)


def test_compare_starts():
    starts = (0x27E0, 0x2A3C1, 0x2A401, 0x2A501)
    functions = {0x27E0: False, 0x2A3C0: True, 0x2A400: False, 0xF0C90: False, 0x2A411: True}
    report = gc.compare_starts(starts, functions)
    assert report == {
        "function_starts": 4,
        "matched": 2,
        "missing": ["0x2A501"],
        "wrong_mode": ["0x2A401"],
        "extra": ["0x2A411", "0xF0C90"],
    }


def test_pointer_form():
    assert gc.pointer(0x2A3C0, True) == "0x2A3C1"
    assert gc.pointer(0x27E0, False) == "0x27E0"


class FakeImage:
    """Just enough of MachO for macho_strings: sections plus reads by VM address."""

    def __init__(self, sections):
        self.sections = tuple(s for s, _ in sections)
        self._data = {s.addr: data for s, data in sections}

    def read(self, addr, size):
        for s in self.sections:
            if s.addr <= addr and addr + size <= s.end:
                off = addr - s.addr
                return self._data[s.addr][off : off + size]
        raise macho.MachOError(f"0x{addr:X}+{size} unmapped")

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]


def section(seg, sect, addr, data, flags=0):
    return macho.Section(seg, sect, addr, len(data), 0, 0, flags, 0, 0), data


def test_macho_strings():
    cstr = b"hello\0\0w\xc3\xb6rld\0\xff\xfe\0"
    ustr = "넥슨\n".encode("utf-16-le") + b"\0\0" + "ab".encode("utf-16-le") + b"\0\0"
    cfs = struct.pack("<4I", 0, 0x7C8, 0x2000, 5) + struct.pack("<4I", 0, 0x7D0, 0x3000, 3)
    cfs += struct.pack("<4I", 0, 0x7C8, 0x9000, 2)  # points nowhere
    image = FakeImage(
        [
            section("__TEXT", "__cstring", 0x2000, cstr, flags=gc.S_CSTRING_LITERALS),
            section("__TEXT", "__ustring", 0x3000, ustr),
            section("__TEXT", "__const", 0x3800, b"not\0strings\0"),
            section("__DATA", "__cfstring", 0x4000, cfs),
        ]
    )
    recs = gc.macho_strings(image)
    got = [(r["address"], r["kind"], r["encoding"], r["value"]) for r in recs]
    assert got == [
        ("0x2000", "cstring", "utf-8", "hello"),
        ("0x2007", "cstring", "utf-8", "wörld"),
        ("0x200E", "cstring", "hex", "fffe"),
        ("0x3000", "ustring", "utf-16-le", "넥슨\n"),
        ("0x3008", "ustring", "utf-16-le", "ab"),
        ("0x4000", "cfstring", "utf-8", "hello"),
        ("0x4010", "cfstring", "utf-16-le", "넥슨\n"),
        ("0x4020", "cfstring", "utf-8", ""),
    ]
    cf = recs[6]
    assert (cf["target"], cf["length"], cf["flags"], cf["section"]) == (
        "0x3000",
        3,
        "0x7D0",
        "__DATA,__cfstring",
    )


def test_dumps_is_valid_stable_json():
    header = {"schema": 1, "program": {"b": 2, "a": "x"}}
    records = [{"to": "0x2", "from": "0x1"}, {"value": "넥슨"}]
    text = gc.dumps(header, "items", records)
    assert json.loads(text) == {**header, "items": records}
    assert text == gc.dumps(dict(reversed(header.items())), "items", records)
    assert text.splitlines() == [
        "{",
        '  "program": {"a":"x","b":2},',
        '  "schema": 1,',
        '  "items": [',
        '    {"from":"0x1","to":"0x2"},',
        '    {"value":"넥슨"}',
        "  ]",
        "}",
    ]
    assert json.loads(gc.dumps({}, "empty", [])) == {"empty": []}
    with pytest.raises(ValueError):
        gc.dumps({"items": 1}, "items", [])


def test_write_text_is_utf8_and_replaces(tmp_path):
    path = tmp_path / "sub" / "out.json"
    gc.write_text(path, "넥슨\n")
    gc.write_text(path, "ok\n")
    assert path.read_bytes() == b"ok\n"
    assert [p.name for p in path.parent.iterdir()] == ["out.json"]


def test_names_json_follows_the_contract():
    rows = [("0x2A3C1", "function", "A::f", "é"), ("0x3000", "data", "g_x", "")]
    text = gc.names_json("test", rows)
    assert text.endswith("}\n") and "é" in text  # dump_json: UTF-8 kept, final newline
    doc = json.loads(text)
    assert doc["names"][1] == {"address": "0x3000", "name": "g_x", "kind": "data"}
    source, entries = names.parse(doc, "test.json")
    assert source == "test" and [e.name for e in entries] == ["A::f", "g_x"]


def test_load_export_checks_schema_and_names_the_producer(tmp_path):
    path = tmp_path / "functions.json"
    with pytest.raises(FileNotFoundError, match="run tools/ghidra/import"):
        gc.load_export(path)
    path.write_text(
        gc.dumps({"schema": gc.SCHEMA_VERSION}, "functions", [{"a": 1}]), encoding="utf-8"
    )
    assert gc.load_export(path)["functions"] == [{"a": 1}]
    path.write_text(json.dumps({"schema": 99, "functions": []}), encoding="utf-8")
    with pytest.raises(ValueError, match="schema 99, expected 1"):
        gc.load_export(path)
    with pytest.raises(ValueError, match="not a Ghidra export file"):
        gc.load_export(tmp_path / "other.json")


def test_load_json(tmp_path):
    path = tmp_path / "doc.json"
    with pytest.raises(FileNotFoundError, match="run scripts/re-analyze first"):
        gc.load_json(path)
    with pytest.raises(FileNotFoundError, match="run step X first"):
        gc.load_json(path, producer="step X")
    path.write_text("[1]", encoding="utf-8")
    with pytest.raises(ValueError, match="not a JSON object"):
        gc.load_json(path)
    path.write_text('{"schema_version": 2}', encoding="utf-8")
    assert gc.load_json(path, 2, key="schema_version") == {"schema_version": 2}
    assert gc.load_json(path) == {"schema_version": 2}
    with pytest.raises(ValueError):
        gc.load_json(path, 2)


def test_render_blocks_rewrites_bodies_and_checks_keys():
    doc = "# T\n<!-- BEGIN generated:a -->\nold\n<!-- END generated:a -->\ntail\n"
    out = gc.render_blocks(doc, {"a": "new"}, "python3 -m mtport.x generate", "docs/x.md")
    assert out == (
        "# T\n<!-- BEGIN generated:a -->\n"
        "<!-- Generated by `python3 -m mtport.x generate`: do not edit by hand. -->\n"
        "new\n<!-- END generated:a -->\ntail\n"
    )
    assert gc.render_blocks(out, {"a": "new\n"}, "python3 -m mtport.x generate", "d") == out

    class DocError(ValueError):
        pass

    with pytest.raises(DocError, match=r"^docs/x.md: no block for b; unknown block a$"):
        gc.render_blocks(doc, {"b": ""}, "c", "docs/x.md", DocError)
    with pytest.raises(ValueError, match="block a appears 2 times"):
        gc.render_blocks(doc + doc, {"a": ""}, "c", "d")
