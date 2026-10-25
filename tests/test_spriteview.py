"""Unit tests for the sprite viewer (mtport.spriteview) on synthetic sprite projects."""

from __future__ import annotations

import dataclasses
import json
import re
from pathlib import Path

import pytest

from mtport import htmlpage as hp
from mtport import png, pngdat, sprprj
from mtport import spriteview as sv

RED, BLUE = (255, 0, 0, 255), (0, 0, 255, 255)
CLEAR = (0, 0, 0, 0)

# A: 2x1, red then blue, opaque.
A = png.Image(2, 1, bytes((*RED, *BLUE)))
# T: 2x1, a transparent pixel with junk colour, then half-transparent green.
T = png.Image(2, 1, bytes((9, 9, 9, 0, 0, 255, 0, 128)))


def layer(image, x, y, alpha=255, hflip=False, visible=True):
    return sprprj.Layer(image, alpha, hflip, visible, x, y)


def frame(*layers, delay=1):
    return sprprj.Frame(delay, tuple(layers))


def action(*frames):
    return sprprj.Action(tuple(frames))


def px(render: sv.ActionRender, flip: bool, i: int, x: int, y: int):
    o = (y * render.width + x) * 4
    return tuple(render.frames[flip][i][o : o + 4])


def at(render: sv.ActionRender, flip: bool, i: int, dx: int, dy: int):
    """The pixel at (dx, dy) relative to the anchor."""
    return px(render, flip, i, render.anchor_x + dx, render.anchor_y + dy)


# --- Compositing ------------------------------------------------------------------------------


def test_offsets_draw_one_pixel_down_and_flip_mirrors_about_the_anchor():
    images = [png.Image(A.width, A.height, sv.clean(A.rgba))]
    r = sv.render_action(action(frame(layer(0, 3, -4))), images)
    # Normal: [3, 5) x [-3, -2); flipped: x = -3 - (2 - 1) = -4 -> [-4, -2); plus the anchor.
    assert (r.width, r.height, r.anchor_x, r.anchor_y) == (9, 4, 4, 3)
    # y + 1 (0x1F119): the layer's row -4 is drawn on row -3.
    assert at(r, False, 0, 3, -3) == RED
    assert at(r, False, 0, 4, -3) == BLUE
    assert sum(1 for i in range(0, len(r.frames[False][0]), 4) if r.frames[False][0][i + 3]) == 2
    # Flipped action: at x - layer.x - (w - 1), mirrored.
    assert at(r, True, 0, -4, -3) == BLUE
    assert at(r, True, 0, -3, -3) == RED
    assert sum(1 for i in range(0, len(r.frames[True][0]), 4) if r.frames[True][0][i + 3]) == 2
    assert sv.check_action("t", action(frame(layer(0, 3, -4))), images, r) == []


def test_layer_hflip_and_action_flip_cancel():
    act = action(frame(layer(0, 0, 0, hflip=True)))
    r = sv.render_action(act, [A])
    assert (at(r, False, 0, 0, 1), at(r, False, 0, 1, 1)) == (BLUE, RED)  # mirrored in place
    assert (at(r, True, 0, -1, 1), at(r, True, 0, 0, 1)) == (RED, BLUE)  # flip + hflip: normal
    assert sv.check_action("t", act, [A], r) == []


def test_layers_blend_back_to_front_in_straight_alpha():
    t = png.Image(2, 1, sv.clean(T.rgba))
    act = action(frame(layer(0, 0, 0), layer(1, 0, 0)))
    r = sv.render_action(act, [A, t])
    assert at(r, False, 0, 0, 1) == RED  # T's transparent pixel leaves red alone
    # Green alpha 128 over opaque blue: D = 128*255 + 255*127 -> alpha 255,
    # g = (255*128*255 + D//2) // D = 128, b = (255*255*127 + D//2) // D = 127.
    assert at(r, False, 0, 1, 1) == (0, 128, 127, 255)
    assert sv.blend((0, 0, 255, 255), (0, 255, 0), 128) == (0, 128, 127, 255)
    assert sv.blend((10, 20, 30, 0), (1, 2, 3), 7) == (1, 2, 3, 7)


def test_layer_alpha_and_transparent_pixels():
    t = png.Image(2, 1, sv.clean(T.rgba))
    act = action(frame(layer(0, 0, 0, alpha=102)), frame(layer(0, 0, 0)), frame(layer(0, 0, 0, 0)))
    r = sv.render_action(act, [t])
    assert at(r, False, 0, 0, 1) == CLEAR
    assert at(r, False, 0, 1, 1) == (0, 255, 0, (128 * 102 + 127) // 255)  # 51
    assert at(r, False, 1, 0, 1) == CLEAR  # junk colour of a transparent pixel is dropped
    assert at(r, False, 1, 1, 1) == (0, 255, 0, 128)
    assert set(r.frames[False][2]) == {0}  # alpha 0 layer draws nothing
    assert sv.check_action("t", act, [t], r) == []


def test_hidden_layers_and_empty_frames():
    act = action(frame(layer(0, 5, 5, visible=False)), frame())
    r = sv.render_action(act, [A])
    assert (r.width, r.height, r.anchor_x, r.anchor_y) == (1, 1, 0, 0)  # just the anchor
    assert r.boxes == {False: (None, None), True: (None, None)}
    assert sv.check_action("t", act, [A], r) == []


def test_every_frame_and_facing_shares_one_canvas_and_anchor():
    act = action(frame(layer(0, -10, -3)), frame(layer(0, 4, 2)), frame(layer(0, 0, -20)))
    r = sv.render_action(act, [A])
    for flip in sv.FACINGS:
        assert all(len(f) == r.width * r.height * 4 for f in r.frames[flip])
    # The bounds sit where the pixels are, relative to the same anchor.
    assert r.boxes[False][0] == sv.Box(-10, -2, -8, -1).shift(r.anchor_x, r.anchor_y)
    assert r.boxes[True][1] == sv.Box(-5, 3, -3, 4).shift(r.anchor_x, r.anchor_y)
    assert at(r, True, 1, -5, 3) == BLUE
    assert sv.check_action("t", act, [A], r) == []


# --- Anchor check -----------------------------------------------------------------------------


def shifted(rgba: bytes, w: int, dx: int) -> bytes:
    """``rgba`` moved ``dx`` pixels right on its own canvas (a jittering frame)."""
    rows = [rgba[y * w * 4 : (y + 1) * w * 4] for y in range(len(rgba) // (w * 4))]
    return b"".join((bytes(4 * dx) + row)[: w * 4] for row in rows)


@pytest.fixture
def walk():
    act = action(frame(layer(0, -1, -1)), frame(layer(0, -1, -2)), frame(layer(0, 0, -1)))
    return act, sv.render_action(act, [A])


def test_check_reports_a_frame_that_moves_off_its_offsets(walk):
    act, r = walk
    frames = list(r.frames[False])
    frames[1] = shifted(frames[1], r.width, 1)
    bad = dataclasses.replace(r, frames={**r.frames, False: tuple(frames)})
    errors = sv.check_action("mob/9 action 0", act, [A], bad)
    assert len(errors) == 2  # pixels differ from the offsets and leave the bounds
    assert errors[0].startswith("mob/9 action 0 frame 1 normal: pixels at [0, 2) x [-1, 0)")
    assert "layer offsets give [-1, 1) x [-1, 0)" in errors[0]


def test_check_reports_swapped_facings_and_cropping(walk):
    act, r = walk
    swapped = dataclasses.replace(r, frames={False: r.frames[True], True: r.frames[False]})
    assert len(sv.check_action("t", act, [A], swapped)) >= 3
    cropped = sv.ActionRender(
        1, r.height, 0, r.anchor_y, {f: (bytes(4 * r.height),) * 3 for f in sv.FACINGS}, r.boxes
    )
    errors = sv.check_action("t", act, [A], cropped)
    assert any("cropped by the canvas" in e for e in errors)


def test_check_reports_frames_off_the_canvas(walk):
    act, r = walk
    short = dataclasses.replace(r, frames={**r.frames, True: r.frames[True][:2]})
    assert sv.check_action("t", act, [A], short) == ["t flipped: 2 frames rendered, 3"]


# --- Images -----------------------------------------------------------------------------------

PLTE = bytes((10, 20, 30, 40, 50, 60))


def indexed_png() -> bytes:
    return pngdat.encode_indexed(pngdat.Indexed(2, 1, (bytes([0, 1]),), PLTE, b"\x00"))


def test_palette_replaces_plte_and_keeps_trns():
    data = indexed_png()
    assert sv.decode_image(data, None, "p").rgba == bytes((0, 0, 0, 0, 40, 50, 60, 255))
    swapped = sv.decode_image(data, bytes((1, 2, 3, 200, 100, 50)), "p")
    assert swapped.rgba == bytes((0, 0, 0, 0, 200, 100, 50, 255))
    with pytest.raises(sv.ViewerError, match="palette of 3 bytes for a PLTE of 6"):
        sv.decode_image(data, b"\1\2\3", "p")
    rgba = png.encode_rgba(1, 1, bytes(4))
    with pytest.raises(sv.ViewerError, match="indexed"):
        sv.decode_image(rgba, b"\1\2\3", "p")


# --- Whole projects, pages and the command line ------------------------------------------------


def write_project(app: Path, folder: str, n: int, project: sprprj.SpriteProject) -> None:
    d = app / "sprite" / folder
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{n}.sprprj").write_bytes(sprprj.build(project))


def make_app(tmp_path: Path) -> Path:
    app = tmp_path / "MapleThief.app"
    res = app / "sprite" / "mob" / "res"
    res.mkdir(parents=True)
    (res / "a.png").write_bytes(png.encode_rgba(2, 1, A.rgba))
    (res / "p.png").write_bytes(indexed_png())
    (res / "<b>.png").write_bytes(png.encode_rgba(2, 1, T.rgba))
    images = (
        sprprj.Image("a.png"),
        sprprj.Image("p.png", bytes((1, 2, 3, 200, 100, 50))),
        sprprj.Image("<b>.png"),
    )
    walk = action(
        frame(layer(0, -1, -2), delay=2),
        frame(layer(1, -1, -3), delay=0),
        frame(layer(0, -1, -2), layer(2, 0, -2, alpha=153), delay=3),
        frame(layer(0, 0, -2, hflip=True), delay=-1),
    )
    stand = action(frame(layer(0, -1, -2)))
    write_project(app, "mob", 0, sprprj.SpriteProject(images, (walk, stand)))
    write_project(app, "mob", 2, sprprj.SpriteProject((), ()))  # the shipped empty files
    write_project(app, "hero", 0, sprprj.SpriteProject((), (action(frame()),)))
    return app


def test_render_writes_frames_json_and_pages(tmp_path):
    app = make_app(tmp_path)
    files, errors = sv.render(app)
    assert errors == []
    assert sorted(f for f in files if not f.endswith(".png")) == [
        "hero-0.html",
        "hero-0.json",
        "index.html",
        "mob-0.html",
        "mob-0.json",
    ]
    assert sum(f.startswith("mob-0/a0-f") for f in files) == 8  # 4 frames x 2 facings
    data = json.loads(files["mob-0.json"])
    walk = data["actions"][0]
    assert [f["delay"] for f in walk["frames"]] == [2, 0, 3, -1]
    assert [f["ticks"] for f in walk["frames"]] == [2, 1, 3, 1]
    assert walk["loop_ticks"] == walk["ended_after_ticks"] == 7
    assert data["actions"][1]["ended_after_ticks"] == 0  # a 1-frame action is ended at once
    assert data["tick_ms"] == [65, 50, 40] and data["default_tick_ms"] == 50
    assert walk["frames"][1]["files"] == {
        "normal": "mob-0/a0-f1.png",
        "flipped": "mob-0/a0-f1-flip.png",
    }
    # Frame PNGs decode to the rendered pixels; the palette swap shows in frame 1.
    w, h = walk["width"], walk["height"]
    ax, ay = walk["anchor"]
    img = png.decode(files["mob-0/a0-f1.png"])
    assert (img.width, img.height) == (w, h)
    assert img.pixel(ax, ay - 2) == (200, 100, 50, 255)
    assert img.pixel(ax - 1, ay - 2) == CLEAR
    assert walk["frames"][1]["bounds"]["normal"] == [ax - 1, ay - 2, ax + 1, ay - 1]


def test_pages_are_static_escaped_and_carry_the_data(tmp_path):
    files, _ = sv.render(make_app(tmp_path))
    page = files["mob-0.html"].decode()
    assert "&lt;b&gt;.png" in page and "<b>.png" not in page
    assert not re.search(r"(src|href)=\"(https?:)?//", page)
    inline = re.search(r'<script type="application/json" id="sprite-data">(.*?)</script>', page)
    assert "<" not in inline.group(1)
    assert json.loads(inline.group(1)) == json.loads(files["mob-0.json"])
    assert 'id="a0"' in page and 'id="a1"' in page
    index = files["index.html"].decode()
    assert 'data-projects="3"' in index
    assert 'href="mob-0.html#a1"' in index
    assert "empty" in index


def test_output_is_deterministic_and_owned(tmp_path):
    app = make_app(tmp_path)
    out = tmp_path / "out"
    files, errors = sv.build(app, out)
    assert errors == []
    again, _ = sv.render(app)
    assert again == files
    assert hp.diff_dir(out, again) == []
    assert (out / hp.MARKER).read_text() == "sprites\n"


def test_select_projects(tmp_path):
    app = make_app(tmp_path)
    files, errors = sv.render(app, ["hero"])
    assert errors == [] and "hero-0.html" in files and "mob-0.html" not in files
    _, errors = sv.render(app, ["mob/7"])
    assert errors == ["mob/7: no such sprite project or folder"]


def test_batch_counts_and_passes(tmp_path, capsys):
    app = make_app(tmp_path)
    assert sv.main(["batch", "--app", str(app)]) == 0
    out = capsys.readouterr().out
    assert out == "projects=3 empty=1 actions=3 frames=6 errors=0\n"


def test_batch_prints_one_line_per_error(tmp_path, capsys):
    app = make_app(tmp_path)
    mob = app / "sprite" / "mob"
    (mob / "res" / "p.png").unlink()  # missing image
    (mob / "1.sprprj").write_bytes(b"\x01\x05ab")  # truncated
    write_project(app, "mob", 3, sprprj.SpriteProject((sprprj.Image("a.png", b"\0\0\0"),), ()))
    (mob / "x.sprprj").write_bytes(b"\0\0")  # not <n>.sprprj
    assert sv.main(["batch", "--app", str(app)]) == 1
    lines = capsys.readouterr().out.splitlines()
    errors = [line for line in lines if line.startswith("error: ")]
    assert lines[-1] == f"projects=5 empty=1 actions=2 frames=2 errors={len(errors)}"
    text = "\n".join(errors)
    assert "error: sprite/mob/x.sprprj: not <folder>/<n>.sprprj" in text
    assert "error: mob/0: image 1: res/p.png: missing" in text
    assert "error: mob/0 action 0: not rendered (an image failed to load)" in text
    assert re.search(r"error: mob/1: .*", text)
    assert "error: mob/3: image 0: a.png: a palette swap needs an indexed" in text
    assert len(errors) == 5


def test_strip_lays_frames_out_on_the_action_canvas(tmp_path, capsys):
    app = make_app(tmp_path)
    out = tmp_path / "s" / "walk.png"
    assert sv.main(["strip", "mob/0", "0", "--out", str(out), "--gap", "2"]) == 0
    data = json.loads(sv.render(app)[0]["mob-0.json"])["actions"][0]
    w, h = data["width"], data["height"]
    img = png.load(out)
    assert (img.width, img.height) == (4 * w + 3 * 2, h)
    ax, ay = data["anchor"]
    assert img.pixel(ax - 1, ay - 1) == RED  # frame 0: a.png at (-1, -2), drawn 1 px down
    assert img.pixel(w + 2 + ax, ay - 2) == (200, 100, 50, 255)  # frame 1, after the gap
    assert img.pixel(w, 0) == CLEAR
    assert "4 frames of" in capsys.readouterr().out


def test_strip_guides_background_flip_and_scale(tmp_path):
    app = make_app(tmp_path)
    out = tmp_path / "walk.png"
    args = ["strip", "mob/0", "1", "--out", str(out), "--guides", "--background", "102030"]
    assert sv.main([*args, "--flip", "--scale", "3"]) == 0
    img = png.load(out)
    r = sv.render_project(sv.load_project(app, sv.ProjectId("mob", 0))).actions[1]
    assert (img.width, img.height) == (3 * r.width, 3 * r.height)
    assert img.pixel(0, 3 * r.anchor_y + 2) == (255, 0, 255, 255)  # horizontal guide
    assert img.pixel(3 * r.anchor_x + 1, 3 * r.anchor_y) == (255, 0, 255, 255)  # vertical
    # a.png at (-1, -2), flipped: x = 1 - (2 - 1) = 0, mirrored, so red lands at x = 1.
    assert img.pixel(3 * (r.anchor_x + 1), 3 * (r.anchor_y - 1)) == RED
    assert sv.main(["strip", "mob/0", "9", "--out", str(out), "--app", str(app)]) == 1


@pytest.fixture(autouse=True)
def _default_app(monkeypatch, tmp_path):
    """Point the CLI's default --app at the synthetic tree."""
    monkeypatch.setattr(sv, "DEFAULT_APP", tmp_path / "MapleThief.app")
    yield


def test_cli_rejects_a_missing_app(tmp_path, capsys):
    assert sv.main(["batch", "--app", str(tmp_path / "nope")]) == 1
    assert "run scripts/bootstrap" in capsys.readouterr().err
