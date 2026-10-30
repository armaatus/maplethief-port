"""The key pad's hiding through MapleThief's capture path (mtport.keypadhide, #71): the
comparison of the shown and hidden screens on synthetic images, and a whole run against a
stand-in for MapleThief (a small Python program that writes synthetic shots and GL frames and
draws a "key pad" only when it is not hidden), so the flow is tested without the game."""

from __future__ import annotations

import sys

from mtport import devicegolden as dg
from mtport import keypadhide as kh
from mtport import png
from repo_paths import REPO

SCREENS = REPO / "tests" / "golden" / "device_screens.json"


def image(fill: tuple[int, int, int, int] = (10, 20, 30, 255)) -> png.Image:
    return png.Image(480, 320, bytes(fill) * (480 * 320))


def paint(img: png.Image, rects: list[tuple[int, int, int, int]], c: int) -> png.Image:
    b = bytearray(img.rgba)
    for x0, y0, w, h in rects:
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                o = 4 * (y * 480 + x)
                b[o : o + 4] = bytes((c, c, c, 255))
    return png.Image(480, 320, bytes(b))


def test_diff_counts_inside_and_outside_the_pad() -> None:
    a = image()
    b = paint(a, [(10, 200, 5, 5), (420, 290, 2, 2), (240, 100, 3, 1)], 200)
    d = kh.diff(a, b)
    assert d == kh.Diff(all=32, outside=3, dpad=25, attack=4)
    assert kh.diff(a, a) == kh.Diff()


def test_check_screen_wants_pad_only_changes_on_the_field() -> None:
    shown = paint(image(), [kh.DPAD, kh.ATTACK], 255)
    hidden = image()
    ok, what = kh.check_screen("first_map", shown, hidden)
    assert ok, what
    # A change outside the pad's rects fails.
    ok, what = kh.check_screen("first_map", shown, paint(hidden, [(240, 100, 1, 1)], 7))
    assert not ok
    # No change at all on the field fails too (the pad was not drawn when shown).
    assert not kh.check_screen("first_map", hidden, hidden)[0]
    # Elsewhere the screens must be identical.
    assert kh.check_screen("popup", hidden, hidden)[0]
    assert not kh.check_screen("popup", shown, hidden)[0]


# The stand-in: for each --shot, a screen with a "key pad" painted over the d-pad and attack
# rects on first_map unless --hide-keypad always, and the same GL frame either way.
FAKE = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
from mtport import png
args = sys.argv[2:]
mode = args[args.index("--hide-keypad") + 1]
assert "--shot-gl" in args
if os.environ.get("FAKE_MODE") == "nogame":
    sys.exit(4)
cap = args[args.index("--capture") + 1]
print("input: hide_keypad %s%s" % (mode, ": the key pad is hidden" if mode == "always" else ""))
for i, a in enumerate(args):
    if a == "--shot":
        name = args[i + 1].split(",")[1]
        b = bytearray(bytes((10, 20, 30, 255)) * (480 * 320))
        if name == "first_map" and mode != "always":
            for x0, y0, w, h in ((0, 180, 168, 136), (405, 277, 70, 34)):
                for y in range(y0, y0 + h):
                    for x in range(x0, x0 + w):
                        b[4 * (y * 480 + x)] = 250
        if os.environ.get("FAKE_MODE") == "leak" and mode == "always":
            b[4 * (100 * 480 + 240)] = 1
        png.save(os.path.join(cap, name + ".png"), png.Image(480, 320, bytes(b)))
        gl = png.Image(480, 320, bytes((5, 6, 7, 255)) * (480 * 320))
        png.save(os.path.join(cap, name + ".gl.png"), gl)
"""


def run_fake(tmp_path, monkeypatch, mode: str = "") -> dg.Report:
    fake = tmp_path / "fake.py"
    fake.write_text(FAKE, encoding="utf-8")
    if mode:
        monkeypatch.setenv("FAKE_MODE", mode)
    golden = tmp_path / "golden"
    spec = dg.load_spec(SCREENS)
    return kh.run_all(
        spec, [sys.executable, str(fake), str(REPO / "tools")], golden, tmp_path / "out", 120
    )


def test_a_whole_run_against_a_stand_in(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch)
    text = "\n".join(report.lines)
    assert report.failed == 0, text
    assert "first_map: hidden vs shown: " in text
    assert "outside the key pad's rects" in text
    assert "popup: hidden vs shown: identical" in text
    assert "the game's GL frame is identical" in text
    assert report.skipped > 0  # no device golden renders here


def test_a_change_outside_the_pad_fails(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "leak")
    assert report.failed > 0


def test_no_game_skips(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "nogame")
    assert report.no_game
    assert report.status() == kh.SKIP
