"""Unit tests for the .mapprj tileset-project parser and tile resolution (bytes built in code).

Format: docs/formats/mapprj.md. ``i16 tile_w, i16 tile_h, u8 count``, then ``count`` Pascal
strings naming PNGs in map/tile/.
"""

import pytest

from mtport import mapprj as mp
from mtport.bytereader import FormatError


def project(tw: int, th: int, *names: bytes, count: int | None = None) -> bytes:
    out = tw.to_bytes(2, "little", signed=True) + th.to_bytes(2, "little", signed=True)
    out += bytes([len(names) if count is None else count])
    for name in names:
        out += bytes([len(name)]) + name
    return out


# The shape of every shipped file: 16 x 16, one sheet (here a made-up name).
ONE = project(16, 16, b"Town.png")


def test_one_sheet_project():
    p = mp.decode(ONE)
    assert p == mp.MapProject(16, 16, ("Town.png",))
    assert p.tileset_paths() == ("map/tile/Town.png",)
    assert mp.encode(p) == ONE
    assert ONE[:6].hex(" ") == "10 00 10 00 01 08"


def test_several_sheets_and_odd_tile_size_round_trip():
    data = project(8, 24, b"a.png", b"b-2.png", b"C_3.PNG")
    p = mp.decode(data)
    assert (p.tile_width, p.tile_height, p.tilesets) == (8, 24, ("a.png", "b-2.png", "C_3.PNG"))
    assert mp.encode(p) == data


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"\x10\x00\x10", "tile height needs 2 bytes"),
        (project(16, 16)[:4], "tileset count needs 1 bytes"),
        (project(0, 16, b"a.png"), "bad tile size 0x16"),
        (project(16, -16, b"a.png"), "bad tile size 16x-16"),
        (project(16, 16), "no tilesets"),
        (project(16, 16, b"a.png", count=2), "tileset 1 name length needs 1 bytes"),
        (project(16, 16, b"a.png")[:-1], "tileset 0 name of 5 bytes runs past the end"),
        (project(16, 16, b""), "empty name"),
        (project(16, 16, b"../x.png"), "not a plain file name"),
        (project(16, 16, b"a\\b.png"), "not a plain file name"),
        (project(16, 16, b"a\x00.png"), "not a plain file name"),
        (project(16, 16, "é.png".encode()), "not a plain file name"),
        (project(16, 16, b".."), "not a plain file name"),
        (ONE + b"\x00", "1 trailing bytes"),
    ],
)
def test_rejects_malformed_input(data, message):
    with pytest.raises(FormatError, match=message):
        mp.decode(data, "x.mapprj")


def test_error_names_file_and_offset():
    with pytest.raises(FormatError, match=r"^7\.mapprj: offset 0x5: tileset 0 has an empty name"):
        mp.decode(project(16, 16, b""), "7.mapprj")


def test_encode_rejects_what_decode_would():
    with pytest.raises(ValueError):
        mp.encode(mp.MapProject(16, 16, ()))
    with pytest.raises(ValueError):
        mp.encode(mp.MapProject(0, 16, ("a.png",)))
    with pytest.raises(ValueError):
        mp.encode(mp.MapProject(16, 16, ("x" * 256,)))


def test_single_sheet_tile_rects():
    # A 320 x 128 sheet of 16 x 16 tiles: 20 columns, 8 rows, 160 tiles.
    lay = mp.layout(mp.decode(ONE), [(320, 128)])
    (sheet,) = lay.sheets
    assert (sheet.cols, sheet.rows, sheet.count, sheet.first) == (20, 8, 160, 0)
    assert lay.tile_count == 160
    assert lay.tile_rect(0) == mp.TileSource(0, 0, 0, 16, 16)
    assert lay.tile_rect(19) == mp.TileSource(0, 304, 0, 16, 16)
    assert lay.tile_rect(20) == mp.TileSource(0, 0, 16, 16, 16)
    assert lay.tile_rect(141) == mp.TileSource(0, 16, 112, 16, 16)
    assert lay.tile_rect(159) == mp.TileSource(0, 304, 112, 16, 16)
    with pytest.raises(IndexError, match="outside sheet 0"):
        lay.tile_rect(160)
    with pytest.raises(IndexError, match="empty"):
        lay.tile_rect(-1)


def test_partial_tiles_at_the_sheet_edge_are_dropped():
    # Integer division, as in the constructor: a 40 x 20 sheet holds 2 x 1 whole tiles.
    lay = mp.layout(mp.decode(ONE), [(40, 20)])
    assert (lay.sheets[0].cols, lay.sheets[0].rows, lay.tile_count) == (2, 1, 2)


def test_multi_sheet_walk_keeps_the_games_off_by_one():
    # Two sheets of 4 tiles each (32 x 16). The loader's `first` offsets are 0 and 4, but the
    # bake walks with `v -= count - 1`, so global 4 lands on tile 1 of sheet 1, not tile 0.
    lay = mp.layout(mp.decode(project(16, 16, b"a.png", b"b.png")), [(32, 32), (32, 32)])
    assert [s.first for s in lay.sheets] == [0, 4]
    assert lay.locate(3) == (0, 3)
    assert lay.locate(4) == (1, 1)
    assert lay.locate(6) == (1, 3)
    assert lay.tile_rect(4) == mp.TileSource(1, 16, 0, 16, 16)
    with pytest.raises(IndexError, match="past the last sheet"):
        lay.locate(7)


def test_layout_needs_one_size_per_sheet():
    with pytest.raises(ValueError):
        mp.layout(mp.decode(ONE), [])


def test_file_names():
    assert mp.project_path(10) == "map/10.mapprj"
    assert mp.map_stem(2, 11) == "211"
    areas = frozenset(range(11))
    assert mp.split_map_stem("00", areas) == (0, 0)
    assert mp.split_map_stem("105", areas) == (10, 5)
    assert mp.split_map_stem("211", areas) == (2, 11)
    assert mp.split_map_stem("910", areas) == (9, 10)
    with pytest.raises(ValueError, match="splits into"):
        mp.split_map_stem("110", frozenset({1, 11}))  # area 1 map 10, or area 11 map 0
    with pytest.raises(ValueError, match="nothing"):
        mp.split_map_stem("77", frozenset({1}))
    with pytest.raises(ValueError):
        mp.split_map_stem("1", areas)


def test_backdrop_per_area():
    assert [mp.back_image(a) for a in range(11)] == [0, 1, 2, 3, 4, 4, 6, 11, 12, 9, 10]
    assert mp.back_path(7) == "map/back/11.png"


def test_cli_info(tmp_path, capsys):
    path = tmp_path / "3.mapprj"
    path.write_bytes(ONE)
    bad = tmp_path / "4.mapprj"
    bad.write_bytes(ONE[:3])
    assert mp.main(["info", str(path), str(bad)]) == 1
    captured = capsys.readouterr()
    assert "3.mapprj: tile 16x16 tilesets=['Town.png']" in captured.out
    assert "4.mapprj: error:" in captured.err
