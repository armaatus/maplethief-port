"""Unit tests for the png/**/*.dat pack splitter and extractor (synthetic PNGs built in code)."""

import json
import struct
import tracemalloc
import zlib

import pytest

from mtport import pngdat, pnginfo
from mtport.bytereader import FormatError


def chunk(ctype: bytes, payload: bytes) -> bytes:
    return (
        struct.pack(">I", len(payload))
        + ctype
        + payload
        + struct.pack(">I", zlib.crc32(ctype + payload))
    )


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def palette_png(
    rows, depth=8, filters=(0,), colours=16, trns=True, color_type=3, interlace=0
) -> bytes:
    """A palette PNG written independently of pngdat.encode_indexed: packs sub-byte depths and
    applies the PNG filter types in ``filters`` (cycled over the rows)."""
    height, width = len(rows), len(rows[0])
    per = 8 // depth
    packed = []
    for r in rows:
        line = bytearray((width * depth + 7) // 8)
        for x, v in enumerate(r):
            line[x // per] |= v << (8 - depth * (x % per + 1))
        packed.append(bytes(line))
    raw, prev = bytearray(), bytes(len(packed[0]))
    for y, line in enumerate(packed):
        f = filters[y % len(filters)]
        out = bytearray()
        for i, v in enumerate(line):
            a = line[i - 1] if i else 0
            b = prev[i]
            c = prev[i - 1] if i else 0
            pred = [0, a, b, (a + b) >> 1, _paeth(a, b, c)][f]
            out.append((v - pred) & 0xFF)
        raw += bytes([f]) + out
        prev = line
    plte = bytes(i % 256 for i in range(colours * 3))
    ihdr = struct.pack(">IIBBBBB", width, height, depth, color_type, 0, 0, interlace)
    body = chunk(b"IHDR", ihdr) + chunk(b"PLTE", plte)
    if trns:
        body += chunk(b"tRNS", bytes([0] + [255] * (colours - 1)))
    return (
        pnginfo.SIGNATURE + body + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")
    )


def solid(w, h, v=1):
    return palette_png([[v] * w for _ in range(h)])


# --- split -----------------------------------------------------------------------------------


def test_split_finds_every_png_with_offsets():
    a, b, c = solid(3, 2), solid(5, 1, 2), solid(1, 1, 3)
    images = pngdat.split(a + b + c, "t.dat")
    assert [i.index for i in images] == [0, 1, 2]
    assert [i.offset for i in images] == [0, len(a), len(a) + len(b)]
    assert [i.data for i in images] == [a, b, c]
    assert [(i.info.width, i.info.height) for i in images] == [(3, 2), (5, 1), (1, 1)]
    assert images[1].length == len(b)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"", "empty pack"),
        (solid(2, 2) + b"\0\0\0\0", "are not a PNG"),
        (solid(2, 2) + solid(2, 2)[:-5], "runs past the end"),
        (solid(2, 2)[:-1] + b"\0", "bad CRC"),
    ],
)
def test_split_rejects_malformed_packs(data, message):
    with pytest.raises(FormatError, match=message):
        pngdat.split(data, "t.dat")


def test_split_caps_a_pack_at_one_table_row():
    one = solid(1, 1)
    assert len(pngdat.split(one * pngdat.ROW_LEN)) == pngdat.ROW_LEN
    with pytest.raises(FormatError, match="more than 255"):
        pngdat.split(one * (pngdat.ROW_LEN + 1))


def test_split_by_lengths_cuts_like_the_engine():
    a, b = solid(2, 2), solid(4, 1)
    assert pngdat.split_by_lengths(a + b, [len(a), len(b)]) == [a, b]
    with pytest.raises(FormatError, match="add up to"):
        pngdat.split_by_lengths(a + b, [len(a)])
    with pytest.raises(FormatError, match="non-positive"):
        pngdat.split_by_lengths(a + b, [0, len(a) + len(b)])


# --- length table ----------------------------------------------------------------------------


class FakeImage:
    """Stands in for mtport.macho.MachO: read(addr, size) over a dict of rows."""

    def __init__(self, rows):
        self.rows = rows

    def read(self, addr, size):
        assert size == pngdat.ROW_STRIDE
        pack_id, rem = divmod(addr - pngdat.TABLE_ADDR, pngdat.ROW_STRIDE)
        assert rem == 0
        row = list(self.rows.get(pack_id, [])) + [0] * pngdat.ROW_LEN
        return struct.pack("<255I", *row[: pngdat.ROW_LEN])


def test_length_row_reads_the_nonzero_prefix():
    image = FakeImage({0: [153, 153, 150, 150], 39: [5]})
    assert pngdat.length_row(image, 0) == (153, 153, 150, 150)
    assert pngdat.length_row(image, 39) == (5,)
    assert pngdat.length_row(image, 7) == ()


def test_length_row_rejects_gaps_and_bad_ids():
    with pytest.raises(FormatError, match="zero entry"):
        pngdat.length_row(FakeImage({3: [10, 0, 10]}), 3)
    with pytest.raises(ValueError):
        pngdat.length_row(FakeImage({}), pngdat.PACK_COUNT)


def test_table_geometry_matches_the_loaders():
    assert pngdat.ROW_STRIDE == 0x3FC
    assert pngdat.PACKS[4].table_row == 0xFC740 + 4 * 0x3FC == 0xFD730


# --- pack list and naming --------------------------------------------------------------------


def test_pack_list_is_the_table_in_order():
    assert [p.id for p in pngdat.PACKS] == list(range(pngdat.PACK_COUNT))
    assert len(pngdat.BY_PATH) == 40
    assert sum(p.count for p in pngdat.PACKS) == 473
    unloaded = [p.path for p in pngdat.PACKS if p.loader is None]
    assert unloaded == [
        f"png/mainmenu/{s}.dat" for s in ("cmenu", "gameqa", "hmenu", "menu", "point")
    ]
    for p in pngdat.PACKS:
        if p.loader is not None:
            assert p.call is not None and p.field is not None
            assert (
                p.helper
                == {1: pngdat.LOAD_FLAT, 2: pngdat.LOAD_2D, 3: pngdat.LOAD_3D}[len(p.shape)]
            )
            assert p.loader & 1 and p.helper & 1  # function pointers carry the Thumb bit
            assert not p.call & 1  # call sites are code addresses


def test_coords_and_names_follow_row_major_order():
    assert pngdat.coords((14,), 5) == (5,)
    assert pngdat.coords((11, 2), 5) == (2, 1)
    assert pngdat.coords((3, 2, 2), 7) == (1, 1, 1)
    assert pngdat.image_name((14,), 5) == "05"
    assert pngdat.image_name((109,), 7) == "007"
    assert pngdat.image_name((11, 2), 5) == "05_2_1"
    assert pngdat.image_name((3, 2, 2), 11) == "11_2_1_1"


# --- indexed PNG codec -----------------------------------------------------------------------


@pytest.mark.parametrize("depth", [1, 2, 4, 8])
def test_decode_every_filter_and_depth(depth):
    top = (1 << depth) - 1
    rows = [[(x * 3 + y * 5) % (top + 1) for x in range(13)] for y in range(10)]
    png = palette_png(rows, depth=depth, filters=(0, 1, 2, 3, 4), colours=top + 1)
    img = pngdat.decode_indexed(png)
    assert (img.width, img.height) == (13, 10)
    assert [list(r) for r in img.rows] == rows
    assert len(img.plte) == 3 * (top + 1) and img.trns is not None


def test_encode_round_trips_and_crops():
    rows = [[(x + 2 * y) % 7 for x in range(6)] for y in range(4)]
    img = pngdat.decode_indexed(palette_png(rows, filters=(4,), colours=7, trns=False))
    assert img.trns is None
    again = pngdat.decode_indexed(pngdat.encode_indexed(img))
    assert again == img
    part = img.crop(2, 1, 3, 2)
    assert [list(r) for r in part.rows] == [rows[1][2:5], rows[2][2:5]]
    with pytest.raises(ValueError):
        img.crop(4, 0, 3, 1)


@pytest.mark.parametrize(
    ("png", "message"),
    [
        (palette_png([[0, 0]], color_type=6), "only non-interlaced palette"),
        (palette_png([[0, 0]], interlace=1), "only non-interlaced palette"),
        (palette_png([[5, 0]], colours=4), "past a 4-colour palette"),
    ],
)
def test_decode_rejects_what_it_cannot_handle(png, message):
    with pytest.raises(FormatError, match=message):
        pngdat.decode_indexed(png)


def test_decode_rejects_short_idat_and_bad_filter():
    good = palette_png([[1, 2], [3, 4]])
    short = good.replace(
        chunk(b"IDAT", zlib.compress(b"\0\1\2\0\3\4")), chunk(b"IDAT", zlib.compress(b"\0\1\2"))
    )
    with pytest.raises(FormatError, match="IDAT holds"):
        pngdat.decode_indexed(short)
    bad = good.replace(
        chunk(b"IDAT", zlib.compress(b"\0\1\2\0\3\4")),
        chunk(b"IDAT", zlib.compress(b"\7\1\2\0\3\4")),
    )
    with pytest.raises(FormatError, match="filter type 7"):
        pngdat.decode_indexed(bad)


def test_decode_inflates_no_more_than_the_rows_need():
    """#324: an IDAT that inflates far past what the rows need (64 MiB of zeros here) is refused
    without ever holding it: the inflate stops one byte past the rows' size."""
    good = palette_png([[1, 2], [3, 4]])
    bomb = good.replace(
        chunk(b"IDAT", zlib.compress(b"\0\1\2\0\3\4")),
        chunk(b"IDAT", zlib.compress(bytes(64 << 20), 9)),
    )
    tracemalloc.start()
    try:
        with pytest.raises(FormatError, match="IDAT holds more than the 6 bytes 2 rows need"):
            pngdat.decode_indexed(bomb)
        _, peak = tracemalloc.get_traced_memory()
    finally:
        tracemalloc.stop()
    assert peak < 4 << 20


def test_parts_rects():
    strip = pngdat.decode_indexed(palette_png([[x // 3 for x in range(30)]] * 4, colours=10))
    digits = pngdat.parts(pngdat.PARTS_DIGITS, strip)
    assert [n for n, _ in digits] == [f"d{d}" for d in range(10)]
    assert digits[7][1] == (21, 0, 3, 4)
    assert set(strip.crop(*digits[7][1]).rows[0]) == {7}
    sheet = pngdat.decode_indexed(palette_png([[0] * 9] * 9))
    assert pngdat.parts(pngdat.PARTS_CORNERS, sheet) == [
        ("tl", (0, 0, 4, 4)),
        ("tr", (4, 0, 4, 4)),
        ("bl", (0, 4, 4, 4)),
        ("br", (4, 4, 4, 4)),
    ]
    assert pngdat.parts(None, sheet) == []


# --- extraction ------------------------------------------------------------------------------


def test_extract_pack_writes_named_images_and_parts(tmp_path):
    pack = pngdat.Pack(4, "png/static/num.dat", (2,), parts=pngdat.PARTS_DIGITS)
    a = palette_png([[x // 2 for x in range(20)]] * 3, colours=10)
    b = palette_png([[x // 4 for x in range(40)]] * 5, colours=10)
    rows, nparts = pngdat.extract_pack(pack, a + b, tmp_path, [len(a), len(b)])
    assert nparts == 20
    assert (tmp_path / "00.png").read_bytes() == a  # stored bytes, not re-encoded
    assert (tmp_path / "01.png").read_bytes() == b
    glyph = pngdat.decode_indexed((tmp_path / "01_d3.png").read_bytes())
    assert (glyph.width, glyph.height) == (4, 5) and set(glyph.rows[0]) == {3}
    assert rows[1]["offset"] == len(a) and rows[1]["parts"]["d3"]["rect"] == [12, 0, 4, 5]


def test_extract_pack_checks_count_and_table(tmp_path):
    pack = pngdat.Pack(0, "png/x.dat", (2, 1))
    a, b = solid(2, 2), solid(3, 3)
    rows, _ = pngdat.extract_pack(pack, a + b, tmp_path)
    assert [r["file"] for r in rows] == ["00_0_0.png", "01_1_0.png"]
    with pytest.raises(FormatError, match="loader expects 2"):
        pngdat.extract_pack(pack, a, tmp_path)
    with pytest.raises(FormatError, match="does not match the PNG framing"):
        pngdat.extract_pack(pack, a + b, tmp_path, [len(a) + 1, len(b) - 1])


def test_extract_all_collects_errors_and_writes_a_manifest(tmp_path, monkeypatch):
    packs = (
        pngdat.Pack(0, "png/static/a.dat", (2,), 0x1359D, 0x141D8, pngdat.LOAD_FLAT, 0xF0),
        pngdat.Pack(1, "png/static/missing.dat", (1,)),
        pngdat.Pack(2, "png/ui/b.dat", (3,)),
    )
    monkeypatch.setattr(pngdat, "PACKS", packs)
    app = tmp_path / "app"
    (app / "png/static").mkdir(parents=True)
    (app / "png/ui").mkdir(parents=True)
    (app / "png/static/a.dat").write_bytes(solid(1, 1) + solid(2, 2))
    (app / "png/ui/b.dat").write_bytes(solid(1, 1))  # 1 image, 3 expected
    out = tmp_path / "out"
    report = pngdat.extract_all(app, out, FakeImage({0: [len(solid(1, 1)), len(solid(2, 2))]}))
    assert (report.files, report.images, report.table_checked) == (1, 2, 1)
    assert len(report.errors) == 2
    assert "missing.dat" in report.errors[0] and "loader expects 3" in report.errors[1]
    assert sorted(p.name for p in (out / "static" / "a").iterdir()) == ["00.png", "01.png"]
    manifest = json.loads((out / "index.json").read_text(encoding="utf-8"))
    (entry,) = manifest["packs"]
    assert entry["table_row"] == "0xFC740" and entry["loader"] == "0x1359D"
    assert [im["width"] for im in entry["images"]] == [1, 2]


def test_cli_extract_and_list(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(pngdat, "PACKS", (pngdat.Pack(0, "png/a/x.dat", (2,)),))
    app = tmp_path / "app"
    (app / "png/a").mkdir(parents=True)
    (app / "png/a/x.dat").write_bytes(solid(1, 1) + solid(1, 2))
    out = tmp_path / "out"
    assert pngdat.main(["extract", "--app", str(app), "--out", str(out), "--no-binary"]) == 0
    assert "1/1 packs, 2 sub-images, 0 parts, 0 checked against the length table, 0 errors" in (
        capsys.readouterr().out
    )
    assert pngdat.main(["list", "--app", str(app)]) == 0
    assert "a/x.dat" in capsys.readouterr().out
    (app / "png/a/x.dat").write_bytes(b"nope")
    assert pngdat.main(["extract", "--app", str(app), "--out", str(out), "--no-binary"]) == 1
    assert pngdat.main(["list", "--app", str(app)]) == 1
    missing = tmp_path / "no-such-binary"
    assert (
        pngdat.main(["extract", "--app", str(app), "--out", str(out), "--binary", str(missing)])
        == 1
    )
