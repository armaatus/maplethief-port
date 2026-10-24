"""Unit tests for the .map tile-layer parser (hand-made byte fixtures only).

Format: docs/formats/map.md. Header ``i16 w, i16 h, u8 layers``; each layer is RLE tokens:
``n <= 0xF0`` then one value (n copies), ``n > 0xF0`` then ``n - 0xF0`` literal values.
"""

import pytest

from mtport import mapfile as mf
from mtport.bytereader import FormatError

E = mf.EMPTY


def header(w: int, h: int, layers: int) -> bytes:
    return (
        w.to_bytes(2, "little", signed=True)
        + h.to_bytes(2, "little", signed=True)
        + bytes([layers])
    )


# 3x2, one layer: run of 2 x tile 5 | literal (1, empty, 7) | run of 1 x empty.
SMALL = header(3, 2, 1) + bytes.fromhex("02 05  F3 01 FF 07  01 FF")


def test_small_map():
    m = mf.decode(SMALL)
    assert (m.width, m.height, m.cells) == (3, 2, 6)
    assert m.layers == ((5, 5, 1, E, 7, E),)
    assert m.rows(0) == [(5, 5, 1), (E, 7, E)]
    assert m.cell(0, 1, 1) == 7
    assert m.max_tile() == 7
    with pytest.raises(IndexError):
        m.cell(0, 3, 0)


def test_runs_cross_rows_and_layers_follow_each_other():
    data = header(4, 2, 2) + bytes.fromhex("08 FF") + bytes.fromhex("F4 00 01 02 03 04 09")
    m = mf.decode(data)
    assert m.layers == ((E,) * 8, (0, 1, 2, 3, 9, 9, 9, 9))
    m, stats = mf.decode_with_stats(data)
    assert (stats.runs, stats.literals) == (2, 1)


def test_longest_tokens():
    # 0xF0 is the longest run (240 cells), 0xFF the longest literal (15 cells).
    m = mf.decode(header(16, 15, 1) + bytes.fromhex("F0 2A"))
    assert m.layers == ((0x2A,) * 240,)
    lit = bytes(range(15))
    m = mf.decode(header(15, 1, 1) + b"\xff" + lit)
    assert m.layers == (tuple(lit),)


def test_zero_run_consumes_its_value_and_writes_nothing():
    m = mf.decode(header(2, 1, 1) + bytes.fromhex("00 33  02 04"))
    assert m.layers == ((4, 4),)


def test_zero_layers():
    m = mf.decode(header(2, 2, 0))
    assert m.layers == ()
    assert m.max_tile() == E


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"\x03\x00\x02\x00", "layer count needs 1 bytes"),
        (header(0, 2, 1), "bad size 0x2"),
        (header(-1, 2, 1), "bad size -1x2"),
        (header(3, 2, 1) + bytes.fromhex("02 05"), "layer 0 ends after 2 of 6 cells"),
        (header(3, 2, 1) + bytes.fromhex("F3 01 02"), "literal run of 3 needs 3 bytes, 2 left"),
        (header(3, 2, 1) + bytes.fromhex("06"), "run value needs 1 bytes"),
        (header(3, 2, 2) + bytes.fromhex("07 01"), "run of 7 at cell 0 crosses the end of layer 0"),
        (header(3, 2, 1) + bytes.fromhex("05 01 F2 01 02"), "run of 2 at cell 5 crosses"),
        (SMALL + b"\x00", "1 trailing bytes after the 1 layers"),
    ],
)
def test_malformed(data, message):
    with pytest.raises(FormatError, match=message):
        mf.decode(data, "bad.map")


def test_error_names_file_and_offset():
    with pytest.raises(FormatError, match=r"^7\.map: offset 0x5: run of 7 at cell 0"):
        mf.decode(header(3, 2, 1) + bytes.fromhex("07 01"), "7.map")


def test_round_trip_small():
    # SMALL ends with a run of 1, which the original tool would have folded into the literal.
    canonical = header(3, 2, 1) + bytes.fromhex("02 05  F4 01 FF 07 FF")
    assert mf.encode(mf.decode(SMALL)) == canonical
    assert mf.decode(canonical) == mf.decode(SMALL)


def encode_layer(cells, w=None) -> bytes:
    w = len(cells) if w is None else w
    return mf.encode(mf.MapFile(w, len(cells) // w, (tuple(cells),)))[5:]


def test_encoder_runs_and_literals():
    assert encode_layer([1, 2, 3]) == bytes.fromhex("F3 01 02 03")
    assert encode_layer([1, 2, 2, 3, 4]) == bytes.fromhex("F1 01 02 02 F2 03 04")
    assert encode_layer([E, E, E, 5]) == bytes.fromhex("03 FF 01 05")  # last cell: a run of 1


def test_encoder_quirks_of_the_original_tool():
    # Runs are capped at 239 cells although the decoder accepts 240.
    assert encode_layer([E] * 241) == bytes.fromhex("EF FF 02 FF")
    # A literal of 14 always takes the next cell, even when that cell starts a repeat.
    distinct = list(range(14))
    assert encode_layer(distinct + [9, 9, 9]) == b"\xff" + bytes(distinct) + b"\x09\x02\x09"
    assert encode_layer(distinct + [9, 9]) == b"\xff" + bytes(distinct) + b"\x09\x01\x09"
    # A literal of 15 is closed; the next cell starts a new token.
    assert encode_layer(list(range(17))) == (b"\xff" + bytes(range(15)) + bytes.fromhex("F2 0F 10"))


def test_encoder_round_trips_many_layers():
    layers = (
        tuple([E] * 30 + list(range(20)) + [7] * 250),
        tuple((x * 7) % 5 for x in range(300)),
    )
    m = mf.MapFile(20, 15, layers)
    assert mf.decode(mf.encode(m)) == m


def test_encoder_rejects_invalid_maps():
    with pytest.raises(ValueError, match="bad size"):
        mf.encode(mf.MapFile(0, 1, ()))
    with pytest.raises(ValueError, match="has 3 cells, expected 4"):
        mf.encode(mf.MapFile(2, 2, ((1, 2, 3),)))
    with pytest.raises(ValueError, match="outside 0..254"):
        mf.encode(mf.MapFile(1, 1, ((255,),)))


def test_cli(tmp_path, capsys):
    good = tmp_path / "00.map"
    good.write_bytes(SMALL)
    bad = tmp_path / "01.map"
    bad.write_bytes(SMALL[:-1])
    assert mf.main(["info", str(good)]) == 0
    assert "00.map: 3x2 layers=1 runs=2 literals=1 max_tile=7" in capsys.readouterr().out
    assert mf.main(["info", str(good), str(bad)]) == 1
    assert "01.map: error:" in capsys.readouterr().err
    assert mf.main(["dump", str(good)]) == 0
    assert capsys.readouterr().out == "layer 0\n05 05 01\n.. 07 ..\n"
