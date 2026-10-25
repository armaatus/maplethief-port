"""Convert every shipped CgBI PNG and pass every other PNG through (docs/formats/cgbi.md).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first. The ``sips`` checks run
on macOS only: ImageIO is the reference decoder for CgBI.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import zlib
from collections import Counter
from pathlib import Path

import pytest

from mtport import cgbi, png, pngdat, pnginfo
from repo_paths import IPA_DIR

pytestmark = pytest.mark.real_assets

APP = IPA_DIR / "Payload" / "MapleThief.app"
needs_sips = pytest.mark.skipif(
    sys.platform != "darwin" or shutil.which("sips") is None, reason="sips: macOS only"
)


def _pngs() -> list[Path]:
    return sorted(p for p in APP.rglob("*.png") if p.is_file())


@pytest.fixture(scope="module")
def crushed() -> dict[str, bytes]:
    """Bundle-relative path -> original bytes, for the CgBI files."""
    out = {}
    for p in _pngs():
        data = p.read_bytes()
        if cgbi.is_cgbi(data):
            out[p.relative_to(APP).as_posix()] = data
    return out


@pytest.fixture(scope="module")
def converted(crushed, tmp_path_factory) -> Path:
    """All CgBI files converted through the command line, as ``<dir>/<file name>``
    (English.lproj's one file has a name of its own)."""
    out = tmp_path_factory.mktemp("cgbi-out")
    assert cgbi.main(["convert", *(str(APP / rel) for rel in crushed), "--out", str(out)]) == 0
    return out


def test_counts_match_analysis(crushed):
    assert len(crushed) == 113
    folders = Counter(rel.count("/") and rel.split("/")[0] for rel in crushed)
    assert folders == {0: 112, "English.lproj": 1}
    flags = Counter(pnginfo.chunk_at(d, 8)[1] for d in crushed.values())
    assert flags == {b"\x30\x00\x20\x02": 105, b"\x30\x00\x20\x06": 8}


def test_every_cgbi_file_converts_to_a_standard_png(crushed, converted):
    for rel, data in crushed.items():
        out = (converted / Path(rel).name).read_bytes()
        header = png.read(data, rel)
        f = png.read(out, rel)
        assert not f.cgbi and f.chunks == (b"IHDR", b"IDAT", b"IEND"), rel
        assert (f.width, f.height, f.bit_depth, f.color_type) == (
            header.width,
            header.height,
            8,
            png.RGBA,
        ), rel
        # A plain zlib reader (header + Adler-32 checked) inflates the stream ...
        raw = zlib.decompress(f.idat)
        stride = f.width * 4 + 1
        assert len(raw) == f.height * stride, rel
        assert all(raw[y * stride] <= 4 for y in range(f.height)), rel
        # ... and the codec decodes it to the un-crushed pixels.
        assert png.decode(out, rel) == cgbi.decode(data, rel), rel


def test_shipped_cgbi_facts(crushed):
    """docs/formats/cgbi.md: every file is 8-bit BGRA, filtered Sub on every row; flag byte
    3 is 06 exactly for the fully opaque files; no colour exceeds its alpha."""
    for rel, data in crushed.items():
        f = png.read(data, rel)
        assert (f.color_type, f.bit_depth) == (png.RGBA, 8), rel
        raw = png.inflate(f.idat, f.height * (f.stride + 1), rel, raw=True)
        assert {raw[y * (f.stride + 1)] for y in range(f.height)} == {png.FILTER_SUB}, rel
        pre = cgbi.decode(data, rel, premultiplied=True).rgba
        alpha = pre[3::4]
        opaque = alpha.count(255) == len(alpha)
        assert opaque == (data[8 + 8 + 3] == 0x06), rel
        for c in (pre[0::4], pre[1::4], pre[2::4]):
            assert all(v <= a for v, a in zip(c, alpha, strict=True)), rel


def _sips_convert(files: list[Path], out: Path) -> None:
    out.mkdir()
    subprocess.run(
        ["sips", "-s", "format", "png", *map(str, files), "--out", str(out)],
        check=True,
        capture_output=True,
    )


@needs_sips
def test_sips_decodes_the_originals_to_the_same_pixels(crushed, tmp_path):
    """ImageIO reads CgBI natively: its decode of each original equals ours, every pixel."""
    # sips --out DIR flattens names; English.lproj's file does not collide with a root one.
    _sips_convert([APP / rel for rel in crushed], tmp_path / "sips")
    for rel, data in crushed.items():
        ref = png.load(tmp_path / "sips" / Path(rel).name)
        assert ref.rgba == cgbi.decode(data, rel).rgba, rel


@needs_sips
def test_sips_opens_every_converted_file(crushed, converted, tmp_path):
    outputs = [converted / Path(rel).name for rel in crushed]
    _sips_convert(outputs, tmp_path / "again")
    for rel in crushed:
        assert png.load(tmp_path / "again" / Path(rel).name) == png.load(
            converted / Path(rel).name
        ), rel


def test_standard_pngs_pass_through_byte_identical(tmp_path):
    standard = [p for p in _pngs() if not cgbi.is_cgbi(p.read_bytes())]
    assert len(standard) == 1322 - 113
    out = tmp_path / "out"
    assert cgbi.main(["convert", *map(str, standard[:1]), "--out", str(out)]) == 0
    assert (out / standard[0].name).read_bytes() == standard[0].read_bytes()
    for p in standard:
        data = p.read_bytes()
        assert cgbi.convert(data, p.name) is data, p
        png.decode(data, p.name)  # and the codec reads every one of them


def test_every_png_in_the_dat_packs_decodes():
    images = 0
    for pack in sorted((APP / "png").rglob("*.dat")):
        for sub in pngdat.split(pack.read_bytes(), pack.name):
            img = png.decode(sub.data, f"{pack.name}[{sub.index}]")
            assert (img.width, img.height) == (sub.info.width, sub.info.height)
            images += 1
    assert images == 473


def test_check_command_over_the_bundle():
    report = cgbi.check(APP)
    assert (report.cgbi, report.standard, report.failures) == (113, 1209, [])
