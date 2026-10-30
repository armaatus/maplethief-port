"""The macOS app icon builder (tools/mtport/macicon.py, #81, ADR 0064) on synthetic images.

The resize step is ``sips`` (macOS); these tests replace it with a stand-in that writes a flat
image, so they run everywhere. The ``sips`` and ``iconutil`` checks run on macOS only.
"""

from __future__ import annotations

import plistlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from mtport import macicon, png
from pngfixtures import make_cgbi

needs_macos_tools = pytest.mark.skipif(
    sys.platform != "darwin" or not (shutil.which("sips") and shutil.which("iconutil")),
    reason="sips and iconutil: macOS only",
)

RED = (200, 30, 40, 255)


def _flat(width: int, height: int, rgba=RED) -> bytes:
    return bytes(rgba) * (width * height)


def _flat_resize(source: Path, size: int, out: Path) -> None:
    """A stand-in for sips: a flat red size x size PNG, whatever the source."""
    out.write_bytes(png.encode_rgba(size, size, _flat(size, size)))


def _entries(data: bytes) -> list[tuple[bytes, bytes]]:
    assert data[:4] == b"icns"
    assert struct.unpack(">I", data[4:8])[0] == len(data)
    out, o = [], 8
    while o < len(data):
        kind, length = data[o : o + 4], struct.unpack(">I", data[o + 4 : o + 8])[0]
        assert length >= 8 and o + length <= len(data)
        out.append((kind, data[o + 8 : o + length]))
        o += length
    return out


# --- Geometry --------------------------------------------------------------------------------


@pytest.mark.parametrize(("canvas", "side"), [(1024, 824), (512, 412), (64, 52), (16, 12)])
def test_content_size_is_the_icon_grid_and_even(canvas, side):
    assert macicon.content_size(canvas) == side
    assert side % 2 == 0


def test_compose_centres_the_art_and_rounds_the_corners():
    canvas = 128
    side = macicon.content_size(canvas)
    margin = (canvas - side) // 2
    img = macicon.compose(png.Image(side, side, _flat(side, side)), canvas)
    assert (img.width, img.height) == (canvas, canvas)
    # The margin is transparent, the middle and the edges' midpoints are the art.
    assert img.pixel(0, 0) == (0, 0, 0, 0)
    assert img.pixel(margin - 1, canvas // 2) == (0, 0, 0, 0)
    assert img.pixel(canvas // 2, canvas // 2) == RED
    assert img.pixel(margin, canvas // 2) == RED
    assert img.pixel(canvas // 2, margin + side - 1) == RED
    # The rectangle's own corner pixel lies outside the rounded corner, in all four corners.
    last = margin + side - 1
    for x, y in ((margin, margin), (last, margin), (margin, last), (last, last)):
        assert img.pixel(x, y)[3] == 0
    # Symmetric under both mirrors, and some edge pixels are partly covered (anti-aliased).
    alpha = img.rgba[3::4]
    rows = [alpha[y * canvas : (y + 1) * canvas] for y in range(canvas)]
    assert rows == rows[::-1]
    assert all(r == r[::-1] for r in rows)
    assert any(0 < a < 255 for a in alpha)
    # Colour is untouched where alpha is not 0 (straight alpha).
    for i in range(0, len(img.rgba), 4):
        if img.rgba[i + 3]:
            assert tuple(img.rgba[i : i + 3]) == RED[:3]


def test_compose_rejects_art_of_the_wrong_size():
    with pytest.raises(ValueError):
        macicon.compose(png.Image(10, 10, _flat(10, 10)), 64)


def test_halve_averages_premultiplied_blocks():
    # One 2x2 block: two opaque white pixels, two transparent ones -> white at half alpha.
    white, clear = (255, 255, 255, 255), (0, 0, 0, 0)
    img = png.Image(2, 2, bytes(white + clear + clear + white))
    assert macicon.halve(img).pixel(0, 0) == (255, 255, 255, 128)
    # A 4x2 image of two flat blocks keeps both colours.
    a, b = (10, 20, 30, 255), (200, 100, 50, 255)
    row = bytes(a + a + b + b)
    out = macicon.halve(png.Image(4, 2, row + row))
    assert (out.width, out.height) == (2, 1)
    assert out.pixel(0, 0) == a and out.pixel(1, 0) == b


def test_halve_rejects_odd_sizes():
    with pytest.raises(ValueError):
        macicon.halve(png.Image(3, 2, _flat(3, 2)))


# --- Container -------------------------------------------------------------------------------


def test_icns_has_one_entry_per_type_with_a_size_it_was_given():
    pngs = {s: f"png{s}".encode() for s in (16, 32, 64, 128)}
    entries = _entries(macicon.icns(pngs))
    assert entries == [
        (b"icp4", b"png16"),
        (b"icp5", b"png32"),
        (b"ic07", b"png128"),
        (b"ic11", b"png32"),
        (b"ic12", b"png64"),
    ]
    with pytest.raises(ValueError):
        macicon.icns({})


def test_build_writes_every_size_from_the_canvas_down_to_16():
    data = macicon.build(Path("unused"), canvas=64, resize=_flat_resize)
    kinds = {}
    for kind, payload in _entries(data):
        img = png.decode(payload)
        kinds[kind] = (img.width, img.height)
    assert kinds == {
        b"icp4": (16, 16),
        b"icp5": (32, 32),
        b"ic11": (32, 32),
        b"ic12": (64, 64),
    }


def test_build_checks_the_canvas_and_what_resize_wrote():
    with pytest.raises(ValueError):
        macicon.build(Path("unused"), canvas=100, resize=_flat_resize)

    def wrong(source: Path, size: int, out: Path) -> None:
        _flat_resize(source, size + 2, out)

    with pytest.raises(macicon.IconError):
        macicon.build(Path("unused"), canvas=64, resize=wrong)


# --- Source ----------------------------------------------------------------------------------


def _ipa(tmp_path: Path, info: dict, icons: dict[str, tuple[int, bool]]) -> Path:
    """A fake extracted IPA: Payload/Test.app with Info.plist and icons {name: (size, cgbi)}."""
    app = tmp_path / "ipa" / "Payload" / "Test.app"
    app.mkdir(parents=True)
    (app / "Info.plist").write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))
    for name, (size, crushed) in icons.items():
        rgba = _flat(size, size)
        data = make_cgbi(size, size, rgba, (0,)) if crushed else png.encode_rgba(size, size, rgba)
        (app / name).write_bytes(data)
    return tmp_path / "ipa"


def test_find_source_prefers_itunes_artwork(tmp_path):
    ipa = _ipa(tmp_path, {"CFBundleIconFile": "Icon.png"}, {"Icon.png": (57, True)})
    (ipa / "iTunesArtwork").write_bytes(b"\xff\xd8 not decoded here")
    assert macicon.find_source(ipa) == ipa / "iTunesArtwork"


def test_find_source_takes_the_largest_icon_the_plist_names(tmp_path):
    info = {
        "CFBundleIconFile": "Icon.png",
        "CFBundleIconFiles": ["Icon.png", "Icon@2x.png", "Missing.png", "../escape.png"],
        "CFBundleIcons": {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["Icon-72"]}},
    }
    icons = {"Icon.png": (57, True), "Icon@2x.png": (114, True), "Icon-72.png": (72, False)}
    (tmp_path / "ipa" / "Payload").mkdir(parents=True)
    (tmp_path / "ipa" / "Payload" / "escape.png").write_bytes(
        png.encode_rgba(500, 500, _flat(500, 500))
    )
    ipa = _ipa(tmp_path, info, icons)
    # A bigger PNG that the plist doesn't name is not a candidate.
    (ipa / "Payload" / "Test.app" / "Other.png").write_bytes(
        png.encode_rgba(300, 300, _flat(300, 300))
    )
    assert macicon.find_source(ipa) == ipa / "Payload" / "Test.app" / "Icon@2x.png"


def test_find_source_without_any_icon_fails(tmp_path):
    ipa = _ipa(tmp_path, {"CFBundleName": "Test"}, {})
    with pytest.raises(macicon.IconError):
        macicon.find_source(ipa)
    with pytest.raises(macicon.IconError):
        macicon.find_source(tmp_path / "nowhere")


def test_main_reports_errors_without_a_traceback(tmp_path, capsys):
    ipa = _ipa(tmp_path, {}, {})
    assert macicon.main(["--ipa", str(ipa), "--out", str(tmp_path / "x.icns")]) == 1
    assert capsys.readouterr().err.startswith("error: ")
    assert not (tmp_path / "x.icns").exists()


# --- With sips and iconutil (macOS) ----------------------------------------------------------


@needs_macos_tools
def test_real_tools_read_a_cgbi_icon_and_iconutil_reads_the_result(tmp_path):
    ipa = _ipa(tmp_path, {"CFBundleIconFiles": ["Icon@2x.png"]}, {"Icon@2x.png": (114, True)})
    out = tmp_path / "out" / "Test.icns"
    assert macicon.main(["--ipa", str(ipa), "--out", str(out)]) == 0
    iconset = tmp_path / "Test.iconset"
    subprocess.run(["iconutil", "-c", "iconset", str(out), "-o", str(iconset)], check=True)
    sizes = {}
    for f in sorted(iconset.glob("*.png")):
        img = png.decode(f.read_bytes(), f.name)
        sizes[f.name] = img.width
        assert img.height == img.width
    assert sizes == {
        "icon_16x16.png": 16,
        "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32,
        "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128,
        "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256,
        "icon_256x256@2x.png": 512,
        "icon_512x512.png": 512,
        "icon_512x512@2x.png": 1024,
    }
    big = png.decode((iconset / "icon_512x512@2x.png").read_bytes())
    assert big.pixel(0, 0)[3] == 0  # the margin
    r, g, b, a = big.pixel(512, 512)
    assert a == 255 and abs(r - RED[0]) <= 2 and abs(g - RED[1]) <= 2 and abs(b - RED[2]) <= 2
