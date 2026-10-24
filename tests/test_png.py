"""Unit tests for the stdlib PNG codec (synthetic images built in tests/pngfixtures.py)."""

from __future__ import annotations

import random
import struct
import zlib

import pytest

from mtport import png
from mtport.bytereader import FormatError
from pngfixtures import (
    CGBI_FLAGS,
    chunk,
    make_png,
    pack_row,
    random_rgba,
    unfilter_naive,
)

ALL_FILTERS = (0, 1, 2, 3, 4)


def _image(color: int, depth: int, width: int, height: int, seed: int, trns: bool):
    """(png bytes, expected RGBA) for random samples of one colour type / depth."""
    rng = random.Random(seed)
    top = (1 << depth) - 1
    palette = trns_chunk = None
    if color == png.PALETTE:
        entries = min(1 << depth, 200)
        palette = bytes(rng.randrange(256) for _ in range(3 * entries))
        if trns:
            trns_chunk = bytes(rng.randrange(256) for _ in range(max(1, entries // 2)))
    key = None
    if trns and color in (png.GRAY, png.RGB):
        key = [rng.randrange(top + 1) for _ in range(png.CHANNELS[color])]
        trns_chunk = struct.pack(f">{len(key)}H", *key)

    def to8(v: int) -> int:
        return v >> 8 if depth == 16 else v * 255 // top

    rows, expected = [], bytearray()
    for _y in range(height):
        samples = []
        for _x in range(width):
            if color == png.PALETTE:
                i = rng.randrange(len(palette) // 3)
                samples.append(i)
                a = trns_chunk[i] if trns_chunk and i < len(trns_chunk) else 255
                expected += palette[3 * i : 3 * i + 3] + bytes([a])
                continue
            px = [rng.randrange(top + 1) for _ in range(png.CHANNELS[color])]
            if key is not None and rng.random() < 0.3:
                px = list(key)  # make sure the colour key occurs
            samples += px
            if color == png.GRAY:
                v = to8(px[0])
                expected += bytes([v, v, v, 0 if px == key else 255])
            elif color == png.RGB:
                expected += bytes([*map(to8, px), 0 if px == key else 255])
            elif color == png.GRAY_ALPHA:
                v = to8(px[0])
                expected += bytes([v, v, v, to8(px[1])])
            else:
                expected += bytes(map(to8, px))
        rows.append(pack_row(samples, depth))
    data = make_png(
        width, height, color, depth, rows, ALL_FILTERS, palette=palette, trns=trns_chunk
    )
    return data, bytes(expected)


CASES = [
    (color, depth, trns)
    for color, depths in png.BIT_DEPTHS.items()
    for depth in depths
    for trns in ((False, True) if color in (png.GRAY, png.RGB, png.PALETTE) else (False,))
]


@pytest.mark.parametrize(("color", "depth", "trns"), CASES)
def test_every_colour_type_and_bit_depth(color, depth, trns):
    data, expected = _image(color, depth, 13, 6, seed=color * 100 + depth, trns=trns)
    img = png.decode(data)
    assert (img.width, img.height) == (13, 6)
    assert img.rgba == expected


@pytest.mark.parametrize("ftype", ALL_FILTERS)
@pytest.mark.parametrize(("color", "depth"), [(0, 8), (4, 8), (2, 8), (6, 8), (6, 16), (3, 4)])
def test_each_filter_type(ftype, color, depth):
    """Every row uses one filter; distances 1, 2, 3, 4 and 8 bytes per pixel."""
    rng = random.Random(ftype * 31 + color)
    width, height = 37, 9
    stride = png.row_bytes(width, color, depth)
    rows = [bytes(rng.randrange(256) for _ in range(stride)) for _ in range(height)]
    palette = bytes(range(48)) if color == png.PALETTE else None
    data = make_png(width, height, color, depth, rows, (ftype,), palette=palette)
    f = png.read(data)
    raw = png.inflate(f.idat, height * (stride + 1))
    assert raw[0] == ftype
    assert png.unfilter(raw, height, stride, f.bpp) == b"".join(rows)


def test_unfilter_matches_naive_reference_on_random_streams():
    rng = random.Random(7)
    for bpp, stride, height in ((1, 1, 3), (3, 30, 4), (4, 4, 5), (4, 1920, 3), (8, 64, 4)):
        raw = bytearray()
        for _ in range(height):
            raw.append(rng.randrange(5))
            raw += bytes(rng.randrange(256) for _ in range(stride))
        raw = bytes(raw)
        assert png.unfilter(raw, height, stride, bpp) == unfilter_naive(raw, height, stride, bpp)


def test_idat_split_over_many_chunks_and_ancillary_chunks_ignored():
    rgba = random_rgba(20, 10, seed=3)
    rows = [rgba[y * 80 : (y + 1) * 80] for y in range(10)]
    extra = (chunk(b"tEXt", b"Comment\0x"), chunk(b"gAMA", struct.pack(">I", 45455)))
    data = make_png(20, 10, png.RGBA, 8, rows, ALL_FILTERS, idat_parts=7, extra=extra)
    assert png.read(data).chunks.count(b"IDAT") == 7
    assert png.decode(data).rgba == rgba


def test_encode_rgba_round_trips_and_is_deterministic():
    rgba = random_rgba(33, 17, seed=11)
    data = png.encode_rgba(33, 17, rgba)
    assert data == png.encode_rgba(33, 17, bytearray(rgba))
    assert png.decode(data) == png.Image(33, 17, rgba)
    f = png.read(data)
    assert f.chunks == (b"IHDR", b"IDAT", b"IEND")
    assert (f.bit_depth, f.color_type) == (8, png.RGBA)
    # An independent decode: plain zlib and the naive per-byte unfilter.
    raw = zlib.decompress(f.idat)
    assert {raw[y * 133] for y in range(17)} <= {0, 1, 2}
    assert unfilter_naive(raw, 17, 132, 4) == rgba


def test_encoder_picks_cheap_filters():
    gradient = bytes(v for x in range(64) for v in (x, x, x, 255))  # Sub turns it into 1s
    flat = png.encode_rgba(64, 8, gradient * 8)
    raw = zlib.decompress(png.read(flat).idat)
    assert raw[0] == png.FILTER_SUB
    assert {raw[y * 257] for y in range(1, 8)} == {png.FILTER_UP}


def test_image_checks_its_buffer():
    with pytest.raises(ValueError):
        png.Image(2, 2, bytes(15))
    assert png.Image(1, 1, b"\x01\x02\x03\x04").pixel(0, 0) == (1, 2, 3, 4)
    with pytest.raises(ValueError):
        png.encode_rgba(2, 2, bytes(15))


def _rows(n=4):
    return [bytes(range(16))] * n


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"GIF89a", "no signature"),
        (make_png(4, 4, 6, 8, _rows(), interlace=1), "interlaced"),
        (make_png(4, 4, 6, 8, _rows(), cgbi=CGBI_FLAGS), "CgBI"),
        (make_png(4, 4, 6, 7, _rows()), "bit depth"),
        (make_png(16, 4, 3, 8, _rows()), "PLTE"),
        (make_png(16, 4, 3, 8, _rows(), palette=bytes(30)), "palette index"),
        (make_png(16, 4, 3, 8, _rows(), palette=bytes(48), trns=bytes(17)), "tRNS"),
        (make_png(4, 4, 6, 8, _rows(), (5,)), "filter type 5"),
        (make_png(4, 4, 6, 8, _rows(3)), "inflates to"),
        (make_png(4, 4, 6, 8, _rows(5)), "too long"),
        (make_png(4, 4, 6, 8, _rows())[:-30], "runs past the end"),
    ],
    ids=lambda v: v if isinstance(v, str) else "",
)
def test_malformed_input_raises_format_error(data, message):
    with pytest.raises(png.PngError, match=message):
        png.decode(data)
    assert issubclass(png.PngError, FormatError)


def test_bad_crc_is_rejected():
    data = bytearray(make_png(4, 4, 6, 8, _rows()))
    data[-20] ^= 0xFF  # inside the IDAT payload
    with pytest.raises(png.PngError, match="CRC"):
        png.decode(bytes(data))


def test_save_and_load(tmp_path):
    img = png.Image(3, 2, random_rgba(3, 2, seed=5))
    png.save(tmp_path / "a.png", img)
    assert png.load(tmp_path / "a.png") == img
