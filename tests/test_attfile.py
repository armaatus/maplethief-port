"""Unit tests for the .att attribute-grid parser (hand-made byte fixtures only).

Format: docs/formats/att.md. Header ``03 "ATT"`` + u32 cell count, then ``(value, count)``
byte pairs until the end of the file. Values are flag sets (AIR, FOOTHOLD, LADDER, ...).
"""

import struct

import pytest

from mtport import attfile as af
from mtport.bytereader import FormatError


def att(count: int, *pairs: int) -> bytes:
    return af.MAGIC + struct.pack("<I", count) + bytes(pairs)


# A 4x3 grid: a row of air, a foothold row with a ladder top, then wall, ladder, air, air.
SMALL = att(12, 1, 4, 2, 2, 6, 1, 2, 1, 0x10, 1, 4, 1, 1, 2)


def test_small_grid():
    a = af.decode(SMALL)
    assert a.packed
    assert a.cells == bytes([1, 1, 1, 1, 2, 2, 6, 2, 0x10, 4, 1, 1])
    assert a.rows(4) == [b"\1\1\1\1", b"\2\2\6\2", b"\x10\4\1\1"]
    assert a.cell(4, 2, 1) == af.FOOTHOLD | af.LADDER
    assert a.histogram() == {1: 6, 2: 3, 4: 1, 6: 1, 0x10: 1}
    _, stats = af.decode_with_stats(SMALL)
    assert (stats.runs, stats.air_runs) == (7, 2)


def test_lookup_past_the_end_is_a_foothold():
    # 0xBBFD returns 2 for any index at or past the end of the grid (an implicit floor).
    a = af.decode(SMALL)
    assert a.cell(4, 0, 3) == af.OUT_OF_RANGE == af.FOOTHOLD
    assert a.cell(4, 5, 2) == af.FOOTHOLD  # x past the row end wraps into a later index
    with pytest.raises(IndexError):
        a.cell(4, 0, -1)
    with pytest.raises(ValueError):
        a.rows(5)


def test_flag_names():
    assert af.flag_names(6) == "FOOTHOLD|LADDER"
    assert af.flag_names(0x12) == "FOOTHOLD|WALL"
    assert af.flag_names(0x21) == "AIR|0x20"
    assert af.flag_names(0) == "0"
    assert af.grid_size(40, 28) == (80, 56)


def test_empty_grid_and_zero_runs():
    assert af.decode(att(0)).cells == b""
    # A zero-length run is legal (the game writes nothing); none are shipped.
    assert af.decode(att(2, 4, 0, 2, 2)).cells == b"\2\2"


def test_encode_splits_runs_at_255():
    a = af.AttFile(bytes([1]) * 300 + bytes([2]) * 3)
    data = af.encode(a)
    assert data == att(303, 1, 255, 1, 45, 2, 3)
    assert af.decode(data) == a
    assert af.encode(af.decode(SMALL)) == SMALL


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"\x03AT", "magic needs 4 bytes"),
        (b"\x03ATX" + bytes(4), "bad magic 03 41 54 58"),
        (af.MAGIC + b"\x01\x00", "cell count needs 4 bytes"),
        (att(4, 1, 2), "runs end after 2 of 4 cells"),
        (att(4, 1, 2, 2), "run length needs 1 bytes"),
        (att(3, 1, 2, 2, 2), "run of 2 at cell 2 passes the 3 cells of the header"),
    ],
)
def test_malformed(data, message):
    with pytest.raises(FormatError, match=message):
        af.decode(data, "x.att")


def test_raw_grid():
    raw = bytes([1, 1, 8, 8, 4, 4, 2, 0x12])
    a = af.decode_any(raw)
    assert not a.packed
    assert a.rows(4) == [b"\1\1\x08\x08", b"\4\4\2\x12"]
    assert af.encode(a) == raw
    assert af.decode_any(SMALL) == af.decode(SMALL)
    with pytest.raises(FormatError, match="offset 0x1: 0x20 is not an attribute"):
        af.decode_raw(b"\x01\x20")
    with pytest.raises(FormatError, match="0x00 is not an attribute"):
        af.decode_raw(b"\x00")
    with pytest.raises(FormatError, match="empty"):
        af.decode_any(b"")


def test_cli(tmp_path, capsys):
    (tmp_path / "7.att").write_bytes(SMALL)
    (tmp_path / "7.map").write_bytes(struct.pack("<hhB", 2, 2, 0))  # 2x2 tiles -> 4 wide
    (tmp_path / "8.att").write_bytes(b"\x03ATT")
    assert af.main(["info", str(tmp_path / "7.att"), str(tmp_path / "8.att")]) == 1
    out = capsys.readouterr()
    assert "7.att: cells=12 runs=7 air_runs=2 AIR=6 FOOTHOLD=3" in out.out
    assert "8.att: error" in out.err
    assert af.main(["dump", str(tmp_path / "7.att")]) == 0
    assert capsys.readouterr().out.splitlines() == ["....", "==H=", "|#.."]
