"""Screen comparison for device goldens (mtport.goldencmp, #60): normalisation of the forms a
device saves screenshots in, masks, the tolerance maths and the CLI. Synthetic images only."""

from __future__ import annotations

from pathlib import Path

import pytest

from mtport import goldencmp as gc
from mtport import png
from mtport.goldencmp import Rect, Tolerance


def solid(w: int, h: int, rgba: tuple[int, int, int, int] = (10, 20, 30, 255)) -> png.Image:
    return png.Image(w, h, bytes(rgba) * (w * h))


def asymmetric(w: int, h: int) -> png.Image:
    """No two pixels alike, and no symmetry: R = x, G = y, B = a mix, opaque."""
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((x % 256, y % 256, (x * 7 + y * 13) % 256, 255))
    return png.Image(w, h, bytes(px))


def set_px(img: png.Image, x: int, y: int, rgba: tuple[int, int, int, int]) -> png.Image:
    b = bytearray(img.rgba)
    o = (y * img.width + x) * 4
    b[o : o + 4] = bytes(rgba)
    return png.Image(img.width, img.height, bytes(b))


# --- normalisation ---------------------------------------------------------------------------


def test_landscape_480x320_is_kept():
    img = asymmetric(480, 320)
    n = gc.normalise(img)
    assert n.image == img and n.steps == [] and n.scale == 1


def test_portrait_follows_adr_0037_mapping():
    # screen_from_window: window point (x, y) -> screen point (480 - y, x). A marker at window
    # pixel (px, py) covers the point (px + 0.5, py + 0.5) -> screen (479.5 - py, px + 0.5),
    # i.e. screen pixel (479 - py, px).
    portrait = solid(320, 480, (0, 0, 0, 255))
    marks = {
        (0, 0): (255, 0, 0, 255),
        (319, 0): (0, 255, 0, 255),
        (0, 479): (0, 0, 255, 255),
        (100, 7): (9, 9, 9, 255),
    }
    for (x, y), c in marks.items():
        portrait = set_px(portrait, x, y, c)
    screen = gc.normalise(portrait)
    assert (screen.image.width, screen.image.height) == (480, 320)
    for (px, py), c in marks.items():
        assert screen.image.pixel(479 - py, px) == c
    # The window's top-left corner lands at the screen's top right (LandscapeLeft).
    assert screen.image.pixel(479, 0) == (255, 0, 0, 255)
    assert screen.steps and "portrait" in screen.steps[0]


def test_portrait_round_trip_both_directions():
    land = asymmetric(480, 320)
    portrait = gc.screen_to_portrait(land)
    assert (portrait.width, portrait.height) == (320, 480)
    assert gc.portrait_to_screen(portrait) == land
    port = asymmetric(320, 480)
    assert gc.screen_to_portrait(gc.portrait_to_screen(port)) == port
    # A mirrored or upside-down mapping would not be the identity on an asymmetric image.
    assert gc.portrait_to_screen(port) != gc.portrait_to_screen(
        gc.portrait_to_screen(gc.portrait_to_screen(port))
    )


def test_retina_2x_is_averaged_per_block():
    big = bytearray()
    for y in range(640):
        for x in range(960):
            # Each 2x2 block holds 0, 1, 2, 3 (+ the block index): the mean rounds half up.
            v = (x % 2) + 2 * (y % 2)
            big += bytes((v, 100 + v, (x // 2) % 200, 255))
    n = gc.normalise(png.Image(960, 640, bytes(big)))
    assert (n.image.width, n.image.height) == (480, 320) and n.scale == 2
    assert n.image.pixel(0, 0) == (2, 102, 0, 255)  # (0+1+2+3+2)//4 = 2
    assert n.image.pixel(17, 5) == (2, 102, 17, 255)


def test_retina_portrait_is_turned_and_averaged():
    land = asymmetric(480, 320)
    up = bytearray()
    for y in range(640):
        row = land.rgba[(y // 2) * 480 * 4 : (y // 2 + 1) * 480 * 4]
        up += b"".join(row[x * 4 : x * 4 + 4] * 2 for x in range(480))
    portrait2x = gc.screen_to_portrait(png.Image(960, 640, bytes(up)))
    assert (portrait2x.width, portrait2x.height) == (640, 960)
    n = gc.normalise(portrait2x)
    assert n.image == land and n.scale == 2 and len(n.steps) == 2


def test_four_inch_letterbox_is_cropped():
    land = asymmetric(480, 320)
    up = bytearray()
    for y in range(640):
        row = land.rgba[(y // 2) * 480 * 4 : (y // 2 + 1) * 480 * 4]
        up += (
            bytes(88 * 4) + b"".join(row[x * 4 : x * 4 + 4] * 2 for x in range(480)) + bytes(88 * 4)
        )
    wide = png.Image(1136, 640, bytes(up))
    wide = png.Image(1136, 640, gc._opaque(wide.rgba))
    n = gc.normalise(wide)
    assert n.image == land and "4-inch" in n.steps[0]
    tall = gc.screen_to_portrait(wide)
    assert gc.normalise(tall).image == land
    # Bars that are not black: not a letterboxed 3.5-inch game.
    with pytest.raises(gc.NormaliseError, match="bars are not black"):
        gc.normalise(set_px(wide, 3, 3, (200, 200, 200, 255)))


@pytest.mark.parametrize("size", [(480, 321), (1024, 768), (2048, 1536), (750, 1334), (1, 1)])
def test_other_sizes_are_rejected(size):
    with pytest.raises(gc.NormaliseError, match="expected 480x320, 320x480"):
        gc.normalise(solid(*size))


def test_alpha_is_made_opaque():
    n = gc.normalise(solid(480, 320, (1, 2, 3, 0)))
    assert n.image.pixel(5, 5) == (1, 2, 3, 255) and n.steps == ["made it opaque"]


# --- comparison --------------------------------------------------------------------------------


def test_identical_images_match_exactly():
    img = asymmetric(48, 32)
    r = gc.compare(img, img)
    assert r.matches and r.differing == 0 and r.compared == 48 * 32 and r.max_delta == 0
    # Matching pixels are the expected pixel's luma dimmed to 64..127 grey, as in golden.h.
    assert r.diff.pixel(0, 0) == (64, 64, 64, 255)


def test_channel_tolerance_and_ratio():
    exp = solid(10, 10, (100, 100, 100, 255))
    act = set_px(exp, 0, 0, (110, 100, 100, 255))  # off by 10
    act = set_px(act, 1, 0, (100, 103, 100, 255))  # off by 3
    r = gc.compare(act, exp, Tolerance(channel=5, ratio=0.0))
    assert not r.matches and r.differing == 1 and r.max_delta == 10
    assert r.diff.pixel(0, 0) == gc.DIFF_DIFFERING and r.diff.pixel(1, 0) == gc.DIFF_WITHIN
    assert gc.compare(act, exp, Tolerance(channel=5, ratio=0.01)).matches  # 1 of 100 allowed
    assert not gc.compare(act, exp, Tolerance(channel=2, ratio=0.01)).matches  # 2 of 100
    assert gc.compare(act, exp, Tolerance(channel=10)).matches  # "more than" channel: 10 is fine
    # Alpha counts, as in golden.h.
    assert not gc.compare(set_px(exp, 5, 5, (100, 100, 100, 254)), exp).matches


def test_masks_leave_pixels_out_and_change_the_ratio_base():
    exp = solid(10, 10)
    act = exp
    for x in range(10):
        act = set_px(act, x, 0, (255, 255, 255, 255))  # the whole top row differs
    assert not gc.compare(act, exp).matches
    r = gc.compare(act, exp, Tolerance(), [Rect(0, 0, 10, 1)])
    assert (
        r.matches and r.masked == 10 and r.compared == 90 and r.diff.pixel(3, 0) == gc.DIFF_MASKED
    )
    # Ratio over the compared pixels: 1 differing of 90 (1.11 %), not of 100 (1 %).
    act2 = set_px(act, 5, 5, (0, 0, 0, 255))
    r2 = gc.compare(act2, exp, Tolerance(ratio=0.0112), [Rect(0, 0, 10, 1)])
    assert r2.differing == 1 and r2.compared == 90 and r2.matches
    assert not gc.compare(act2, exp, Tolerance(ratio=0.0105), [Rect(0, 0, 10, 1)]).matches
    # Masks are clipped to the image.
    assert gc.compare(act, exp, Tolerance(), [Rect(-5, -5, 100, 6)]).matches


def test_size_mismatch_never_matches():
    r = gc.compare(solid(10, 10), solid(12, 10), Tolerance(channel=255, ratio=1.0))
    assert not r.matches and not r.size_matches
    assert r.differing == 20 and r.diff.pixel(11, 3) == gc.DIFF_OUTSIDE


def test_rect_and_tolerance_validation():
    assert Rect.parse("1,2,3,4") == Rect(1, 2, 3, 4)
    for bad in ("1,2,3", "a,b,c,d", "0,0,0,5"):
        with pytest.raises(ValueError):
            Rect.parse(bad)
    with pytest.raises(ValueError):
        Tolerance(channel=256)
    with pytest.raises(ValueError):
        Tolerance(ratio=1.5)


# --- the command line --------------------------------------------------------------------------


def test_cli_compares_a_portrait_screenshot(tmp_path: Path, capsys):
    ours = asymmetric(480, 320)
    png.save(tmp_path / "ours.png", ours)
    png.save(tmp_path / "shot.png", gc.screen_to_portrait(ours))
    diff = tmp_path / "out" / "diff.png"
    norm = tmp_path / "norm.png"
    assert (
        gc.main(
            [
                str(tmp_path / "ours.png"),
                str(tmp_path / "shot.png"),
                "--diff",
                str(diff),
                "--normalised",
                str(norm),
            ]
        )
        == 0
    )
    out = capsys.readouterr().out
    assert "turned the portrait screenshot" in out and "match:" in out
    assert png.load(norm) == ours and png.load(diff).width == 480


def test_cli_exit_statuses(tmp_path: Path, capsys):
    a = solid(480, 320)
    b = set_px(a, 10, 10, (200, 0, 0, 255))
    png.save(tmp_path / "a.png", a)
    png.save(tmp_path / "b.png", b)
    png.save(tmp_path / "odd.png", solid(100, 100))
    assert gc.main([str(tmp_path / "a.png"), str(tmp_path / "b.png")]) == 1
    assert gc.main([str(tmp_path / "a.png"), str(tmp_path / "b.png"), "--mask", "8,8,4,4"]) == 0
    assert gc.main([str(tmp_path / "a.png"), str(tmp_path / "b.png"), "--channel", "255"]) == 0
    assert gc.main([str(tmp_path / "a.png"), str(tmp_path / "odd.png")]) == 2
    assert gc.main([str(tmp_path / "a.png"), str(tmp_path / "missing.png")]) == 2
    assert "no screenshot of the 480x320 game" in capsys.readouterr().err
