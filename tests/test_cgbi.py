"""Unit tests for the CgBI un-crusher (synthetic CgBI files from tests/pngfixtures.py)."""

from __future__ import annotations

import pytest

from mtport import cgbi, png
from mtport.bytereader import FormatError
from pngfixtures import CGBI_FLAGS, chunk, make_cgbi, make_png, random_rgba

# Alpha >= 128 keeps premultiplication's rounding loss within one step per channel;
# 0 and 255 are the common cases in the shipped files.
ALPHAS = (0, 128, 129, 170, 200, 254, 255)


def _close(a: bytes, b: bytes) -> int:
    return max((abs(x - y) for x, y in zip(a, b, strict=True)), default=0)


@pytest.mark.parametrize(
    ("width", "height", "filters"),
    [(1, 1, (0,)), (7, 5, (1,)), (31, 9, (0, 1, 2, 3, 4)), (64, 3, (4,)), (480, 4, (1, 2))],
)
def test_round_trip_within_one_step_per_channel(width, height, filters):
    straight = random_rgba(width, height, seed=width * height, alphas=ALPHAS)
    data = make_cgbi(width, height, straight, filters)
    assert cgbi.is_cgbi(data)
    img = cgbi.decode(data, "synthetic.png")
    assert (img.width, img.height) == (width, height)
    assert _close(img.rgba, straight) <= 1
    assert img.rgba[3::4] == straight[3::4]  # alpha is never rounded

    # convert(): a standard PNG that the plain decoder reads back to the same pixels.
    out = cgbi.convert(data, "synthetic.png")
    assert not cgbi.is_cgbi(out)
    assert png.read(out).chunks == (b"IHDR", b"IDAT", b"IEND")
    assert png.decode(out) == img
    assert cgbi.convert(data) == out  # deterministic


def test_round_trip_is_exact_for_opaque_images():
    straight = random_rgba(40, 6, seed=1, alphas=(255,))
    assert cgbi.decode(make_cgbi(40, 6, straight, cgbi=b"\x30\x00\x20\x06")).rgba == straight


def test_premultiplied_mode_keeps_the_stored_values():
    straight = random_rgba(16, 8, seed=2)  # every alpha value, including the low ones
    data = make_cgbi(16, 8, straight, (0, 1, 2, 3, 4))
    pre = cgbi.decode(data, premultiplied=True)
    assert pre.rgba == cgbi.premultiply(straight)
    assert png.decode(cgbi.convert(data, premultiplied=True)) == pre


def test_alpha_zero_becomes_transparent_black_and_overflow_clamps():
    # Stored BGRA: transparent with a stray colour, and a colour above its alpha.
    row = bytes((9, 8, 7, 0, 200, 100, 50, 100))
    data = make_png(2, 1, png.RGBA, 8, [row], cgbi=CGBI_FLAGS)
    assert cgbi.decode(data).rgba == bytes((0, 0, 0, 0, 128, 255, 255, 100))


def test_unpremultiply_rounding_matches_stb_image():
    # stb_image: c = (c * 255 + a / 2) / a in integers.
    for a in (1, 3, 77, 128, 254):
        for c in range(a + 1):
            got = cgbi.unpremultiply(bytes((c, c, c, a)))
            assert got[0] == (c * 255 + a // 2) // a


def test_bgr_without_alpha():
    data = make_png(2, 1, png.RGB, 8, [bytes((1, 2, 3, 4, 5, 6))], (1,), cgbi=CGBI_FLAGS)
    assert cgbi.decode(data).rgba == bytes((3, 2, 1, 255, 6, 5, 4, 255))


def test_standard_pngs_pass_through_byte_identical():
    rgba = random_rgba(5, 5, seed=4)
    rows = [rgba[y * 20 : (y + 1) * 20] for y in range(5)]
    data = make_png(5, 5, png.RGBA, 8, rows, (0, 4))
    assert not cgbi.is_cgbi(data)
    assert cgbi.convert(data, "a.png") is data
    assert cgbi.decode(data) == png.decode(data)
    premultiplied = png.decode(cgbi.convert(data, premultiplied=True))
    assert premultiplied.rgba == cgbi.premultiply(rgba)


CGBI_ONLY = png.SIGNATURE + chunk(b"CgBI", CGBI_FLAGS) + chunk(b"IEND", b"")
ZLIB_PNG = make_png(4, 1, png.RGBA, 8, [bytes(16)])  # zlib stream where CgBI has raw deflate
HEAD = len(png.SIGNATURE)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"not a png", "not a PNG"),
        (CGBI_ONLY, "not IHDR"),
        (make_png(4, 1, png.PALETTE, 8, [bytes(4)], palette=bytes(3), cgbi=CGBI_FLAGS), "BGRA"),
        (ZLIB_PNG[:HEAD] + chunk(b"CgBI", CGBI_FLAGS) + ZLIB_PNG[HEAD:], "corrupt image data"),
    ],
    ids=["garbage", "no-ihdr", "palette", "zlib-stream"],
)
def test_malformed_cgbi_raises_format_error(data, message):
    with pytest.raises(FormatError, match=message):
        cgbi.convert(data)


def test_cli_convert_and_check(tmp_path, capsys):
    src = tmp_path / "in"
    src.mkdir()
    straight = random_rgba(6, 4, seed=9, alphas=ALPHAS)
    (src / "crushed.png").write_bytes(make_cgbi(6, 4, straight))
    plain = make_png(2, 1, png.RGB, 8, [bytes(6)])
    (src / "plain.png").write_bytes(plain)
    out = tmp_path / "out"
    assert (
        cgbi.main(["convert", str(src / "crushed.png"), str(src / "plain.png"), "--out", str(out)])
        == 0
    )
    assert "converted=1 passed-through=1 errors=0" in capsys.readouterr().out
    assert (out / "plain.png").read_bytes() == plain
    assert _close(png.load(out / "crushed.png").rgba, straight) <= 1
    assert cgbi.main(["check", str(src)]) == 0
    assert "cgbi=1 standard=1 failures=0" in capsys.readouterr().out

    (src / "broken.png").write_bytes(b"\x89PNG\r\n\x1a\n")
    assert cgbi.main(["check", str(src)]) == 1
    assert cgbi.main(["convert", str(src), "--out", str(tmp_path / "o2")]) == 1
    assert "errors=1" in capsys.readouterr().out
