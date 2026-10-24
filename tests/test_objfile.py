"""Unit tests for the .obj map-object parser (hand-made byte fixtures only).

Format: docs/formats/obj.md. ``u8 count``, then ``count`` records of
``i16 cell, u8 kind, u8 len, char name[len]``; ``cell == -1`` names the map script.
"""

import struct

import pytest

from mtport import objfile as of
from mtport.bytereader import FormatError


def rec(cell: int, kind: int, name: bytes = b"") -> bytes:
    return struct.pack("<hBB", cell, kind, len(name)) + name


# A map script, a portal, an NPC with a script, a mob spawn and a hidden portal.
SMALL = bytes([5]) + (
    rec(-1, 0xFF, b"TownMap")
    + rec(41, 0, b"portal070")
    + rec(0x027C, 7, b"Darklord")  # 0x027C: the 0x7C ('|') is the cell's low byte
    + rec(100, 60)
    + rec(0x7FFF, 3)
)


def test_small_file():
    obj = of.decode(SMALL)
    assert obj.map_script == "TownMap"
    assert [r.category for r in obj.records] == [
        "map_script",
        "portal",
        "npc",
        "mob",
        "hidden_portal",
    ]
    assert [(r.cell, r.kind, r.name) for r in obj.objects] == [
        (41, 0, "portal070"),
        (636, 7, "Darklord"),
        (100, 60, ""),
        (0x7FFF, 3, ""),
    ]
    assert obj.counts() == {"map_script": 1, "portal": 1, "hidden_portal": 1, "npc": 1, "mob": 1}
    assert of.encode(obj) == SMALL


def test_positions():
    portal = of.Record(41, 0, "portal070")
    # On a 40-tile-wide map, cell 41 is tile (1, 1): anchor at its bottom-centre pixel.
    assert portal.tile(40) == (1, 1)
    assert portal.position(40) == (24, 31)
    with pytest.raises(ValueError):
        of.Record(-1, 0xFF, "TownMap").tile(40)


def test_kind_ranges():
    assert of.Record(0, 2, "").category == "portal"
    assert of.Record(0, 4, "").category == "npc"
    assert of.Record(0, 59, "").category == "npc"
    assert of.Record(0, 60, "").category == "mob"
    assert of.Record(0, 255, "").category == "mob"


def test_no_records_and_no_map_script():
    obj = of.decode(b"\x00")
    assert obj.records == () and obj.map_script is None
    assert of.encode(obj) == b"\x00"


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"", "record count needs 1 bytes"),
        (b"\x01\x05", "record 0 cell needs 2 bytes"),
        (b"\x01\x05\x00", "record 0 kind needs 1 bytes"),
        (b"\x01" + rec(5, 1)[:3], "record 0 name length needs 1 bytes"),
        (b"\x01" + rec(5, 1, b"abc")[:-1], "record 0 name of 3 bytes runs past the end"),
        (b"\x02" + rec(5, 1), "record 1 cell needs 2 bytes"),
        (b"\x01" + rec(5, 1) + b"\x00", "1 trailing bytes after the 1 records"),
        (b"\x01" + rec(5, 1, b"\xc3\xa9"), "is not ASCII"),
        (b"\x01" + rec(5, 1, b"a\x00"), "contains a NUL"),
        (b"\x01" + rec(-2, 1), "record 0 has cell -2"),
        (b"\x02" + rec(-1, 255, b"A") + rec(-1, 255, b"B"), "second map-script record"),
    ],
)
def test_malformed(data, message):
    with pytest.raises(FormatError, match=message):
        of.decode(data, "x.obj")


def test_encode_rejects_bad_records():
    with pytest.raises(ValueError):
        of.encode(of.ObjFile((of.Record(0, 256, ""),)))
    with pytest.raises(ValueError):
        of.encode(of.ObjFile((of.Record(0, 0, "x"),) * 256))


def test_cli(tmp_path, capsys):
    (tmp_path / "70.obj").write_bytes(SMALL)
    (tmp_path / "70.map").write_bytes(struct.pack("<hhB", 40, 20, 0))
    (tmp_path / "71.obj").write_bytes(b"\x01")
    assert of.main(["info", str(tmp_path / "70.obj"), str(tmp_path / "71.obj")]) == 1
    out = capsys.readouterr()
    assert "70.obj: records=5 map_script=1 portal=1 hidden_portal=1 npc=1 mob=1" in out.out
    assert "71.obj: error" in out.err
    assert of.main(["dump", str(tmp_path / "70.obj")]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[0] == "cell=   -1 kind=255 map_script    name='TownMap'"
    assert lines[1].endswith("name='portal070' tile=(1, 1) px=(24, 31)")
