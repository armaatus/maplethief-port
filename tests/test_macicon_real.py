"""The macOS app icon from the real IPA (tools/mtport/macicon.py, #81, ADR 0064).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first. The build itself needs
``sips`` and ``iconutil`` (macOS). The icon is written to pytest's temporary directory only.
"""

from __future__ import annotations

import shutil
import subprocess
import sys

import pytest

from mtport import macicon, png
from repo_paths import IPA_DIR

pytestmark = pytest.mark.real_assets


def test_the_source_is_the_1024_pixel_store_artwork():
    source = macicon.find_source(IPA_DIR)
    assert source == IPA_DIR / "iTunesArtwork"
    assert source.read_bytes()[:3] == b"\xff\xd8\xff"  # a JPEG in version 1.5


def test_without_the_artwork_the_largest_plist_icon_is_icon_at_2x(tmp_path):
    # The same bundle without iTunesArtwork: link Payload/ into an IPA directory of its own.
    ipa = tmp_path / "ipa"
    ipa.mkdir()
    (ipa / "Payload").symlink_to(IPA_DIR / "Payload", target_is_directory=True)
    source = macicon.find_source(ipa)
    assert source.name == "Icon@2x.png"
    f = png.read(source.read_bytes(), source.name)
    assert (f.width, f.height, f.cgbi) == (114, 114, True)


@pytest.mark.skipif(
    sys.platform != "darwin" or not (shutil.which("sips") and shutil.which("iconutil")),
    reason="sips and iconutil: macOS only",
)
def test_the_icon_builds_and_iconutil_reads_it(tmp_path):
    out = tmp_path / "MapleThief.icns"
    assert macicon.main(["--ipa", str(IPA_DIR), "--out", str(out)]) == 0
    iconset = tmp_path / "MapleThief.iconset"
    subprocess.run(["iconutil", "-c", "iconset", str(out), "-o", str(iconset)], check=True)
    big = png.decode((iconset / "icon_512x512@2x.png").read_bytes())
    assert (big.width, big.height) == (1024, 1024)
    assert big.pixel(0, 0)[3] == 0 and big.pixel(512, 512)[3] == 255
    assert len(list(iconset.glob("*.png"))) == 10
