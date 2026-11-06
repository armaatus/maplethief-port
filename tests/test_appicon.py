"""The port's own app icon (mtport.appicon, platform/icon/, ADR 0125).

The derived icon files are committed; these tests check that they decode to exactly what the
script draws, and that the content guard's allowlist (mtport.release.ICON_ALLOWLIST) pins
their real SHA-256.
"""

from __future__ import annotations

import hashlib
import struct

from mtport import appicon, png
from mtport import release as rel
from repo_paths import REPO

ICON_DIR = REPO / "platform" / "icon"


def test_committed_files_match_the_script():
    assert appicon.check(ICON_DIR) == []


def test_allowlist_pins_the_committed_files():
    sources = {
        "MapleThief.app/Contents/Resources/MapleThief.icns": ICON_DIR / "MapleThief.icns",
        "icon.png": ICON_DIR / "icon.png",
    }
    assert set(rel.ICON_ALLOWLIST) == set(sources)
    for path, source in sources.items():
        assert rel.ICON_ALLOWLIST[path] == hashlib.sha256(source.read_bytes()).hexdigest(), path


def test_render_shapes():
    img = appicon.render(64)
    assert (img.width, img.height) == (64, 64)
    assert img.pixel(0, 0)[3] == 0  # outside the rounded square: transparent
    assert img.pixel(8, 32)[3] == 255  # the plate, beside the star's left point
    r, g, b, a = img.pixel(32, 24)  # the star's upper arm
    assert a == 255 and (r, g, b) == appicon.STAR
    centre = img.pixel(32, 32)  # the hole shows the plate again
    assert centre[3] == 255 and centre[:3] != appicon.STAR


def test_render_is_deterministic():
    assert appicon.render(48).rgba == appicon.render(48).rgba


def test_ico_and_icns_containers(tmp_path):
    files = appicon.files()
    ico = files["MapleThief.ico"]
    assert struct.unpack("<HHH", ico[:6]) == (0, 1, len(appicon.ICO_SIZES))
    assert sorted(appicon.embedded_pngs("MapleThief.ico", ico)) == sorted(appicon.ICO_SIZES)
    icns = files["MapleThief.icns"]
    assert icns[:4] == b"icns" and struct.unpack(">I", icns[4:8])[0] == len(icns)
    assert sorted(appicon.embedded_pngs("MapleThief.icns", icns)) == sorted(appicon.ICNS_SIZES)
    assert png.decode(files["icon.png"]).width == appicon.PNG_SIZE
    assert files["MapleThief.svg"].startswith(b"<svg ")


def test_check_reports_a_changed_file(tmp_path):
    for name, data in appicon.files().items():
        (tmp_path / name).write_bytes(data)
    assert appicon.check(tmp_path) == []
    other = appicon.render(appicon.PNG_SIZE)
    flipped = png.Image(other.width, other.height, bytes(reversed(other.rgba)))
    (tmp_path / "icon.png").write_bytes(
        png.encode_rgba(flipped.width, flipped.height, flipped.rgba)
    )
    (tmp_path / "MapleThief.svg").unlink()
    problems = appicon.check(tmp_path)
    assert any("icon.png" in p and "differs" in p for p in problems)
    assert any("MapleThief.svg" in p and "missing" in p for p in problems)
