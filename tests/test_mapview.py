"""Unit tests for the map viewer (mtport.mapview) on a synthetic .app tree built in code."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from mtport import attfile, mapfile, mapprj, objfile, png
from mtport import datatables as dt
from mtport import htmlpage as hp
from mtport import mapview as mv

E = mapfile.EMPTY
RED = (200, 10, 10, 255)
GREEN = (10, 200, 10, 255)
BLUE = (10, 10, 200, 255)
BACK_A = (1, 2, 3, 255)
BACK_B = (4, 5, 6, 255)


def solid(color, n: int) -> bytes:
    return bytes(color) * n


def sheet_png() -> bytes:
    """A 48x16 sheet with three 16x16 tiles.

    tile 0: red, except a transparent pixel at (0, 0);
    tile 1: left half transparent, right half green;
    tile 2: blue with alpha 128 (partially transparent).
    """
    rows = []
    for y in range(16):
        t0 = solid(RED, 16)
        if y == 0:
            t0 = bytes(4) + t0[4:]
        t1 = bytes(8 * 4) + solid(GREEN, 8)
        t2 = solid(BLUE[:3] + (128,), 16)
        rows.append(t0 + t1 + t2)
    return png.encode_rgba(48, 16, b"".join(rows))


def back_png() -> bytes:
    """A 2x1 backdrop: two colours, so the repetition is visible."""
    return png.encode_rgba(2, 1, bytes(BACK_A) + bytes(BACK_B))


def mob_record(**values):
    base = {c.name: "" if c.kind == "str" else 0 for c in dt.layout(dt.Mob)}
    base.update(values)
    return dt.Mob(**base)


# 2 x 1 tiles (32 x 16 px), two layers: layer 0 tile 0 in both cells, layer 1 tile 1 in cell 0.
LAYERS = ((0, 0), (1, E))
# 4 x 2 attribute cells: foothold, ladder, wall, air / foothold+wall, ladder top, air, foothold.
ATT = bytes([0x02, 0x04, 0x10, 0x01, 0x12, 0x08, 0x01, 0x02])
OBJECTS = (
    objfile.Record(-1, 0xFF, "TestMap"),
    objfile.Record(0, 0, "portal000"),
    objfile.Record(1, 4, "Npc"),
    objfile.Record(1, 60, ""),
)


def make_app(
    root: Path,
    layers=LAYERS,
    att: bytes = ATT,
    objects=OBJECTS,
    width: int = 2,
    height: int = 1,
    stem: str = "00",
) -> Path:
    """A minimal .app tree with one area (0) and one map; returns the .app directory."""
    app = root / "Test.app"
    (app / "map" / "tile").mkdir(parents=True, exist_ok=True)
    (app / "map" / "back").mkdir(parents=True, exist_ok=True)
    project = mapprj.MapProject(16, 16, ("sheet.png",))
    (app / "map" / "0.mapprj").write_bytes(mapprj.encode(project))
    (app / "map" / "tile" / "sheet.png").write_bytes(sheet_png())
    (app / "map" / "back" / "0.png").write_bytes(back_png())
    m = mapfile.MapFile(width, height, tuple(tuple(layer) for layer in layers))
    (app / "map" / f"{stem}.map").write_bytes(mapfile.encode(m))
    (app / "map" / f"{stem}.att").write_bytes(attfile.encode(attfile.AttFile(att)))
    (app / "map" / f"{stem}.obj").write_bytes(objfile.encode(objfile.ObjFile(tuple(objects))))
    for kind in (0, 4, 60):
        (app / "sprite" / "mob").mkdir(parents=True, exist_ok=True)
        (app / "sprite" / "mob" / f"{kind}.sprprj").write_bytes(b"")
    (app / "script" / "common").mkdir(parents=True, exist_ok=True)
    (app / "script" / "chapter1").mkdir(parents=True, exist_ok=True)
    (app / "script" / "common" / "portal000.out").write_bytes(b"")
    (app / "script" / "chapter1" / "Npc.out").write_bytes(b"")
    (app / "script" / "common" / "TestMap.out").write_bytes(b"")
    (app / "English" / "data").mkdir(parents=True, exist_ok=True)
    mobs = [mob_record(id=1, name="Blue Snail", sprite=60)]
    (app / "English" / "data" / "mob.dat").write_bytes(dt.encode("mob", mobs))
    return app


@pytest.fixture
def app(tmp_path):
    return make_app(tmp_path)


def load(app: Path, stem: str = "00") -> mv.LoadedMap:
    loaded, problems, _ = mv.load_all(app, stem)
    assert problems == []
    return loaded[0]


def px(buf, width: int, x: int, y: int) -> tuple[int, int, int, int]:
    i = (y * width + x) * 4
    return tuple(buf[i : i + 4])


# --- Rendering ------------------------------------------------------------------------------


def test_tile_layers_bake_in_order_with_an_alpha_test(app):
    lm = load(app)
    assert lm.problems == []
    assert lm.pixel_size == (32, 16)
    buf = mv.render_tiles(lm)
    assert len(buf) == 32 * 16 * 4
    # Cell 0: layer 0 red; layer 1 covers its right half with green.
    assert px(buf, 32, 0, 0) == (0, 0, 0, 0)  # transparent in tile 0, nothing below
    assert px(buf, 32, 1, 0) == RED
    assert px(buf, 32, 7, 5) == RED  # layer 1 transparent there: layer 0 shows
    assert px(buf, 32, 8, 5) == GREEN  # layer 1 opaque: it wins
    assert px(buf, 32, 15, 15) == GREEN
    # Cell 1: layer 0 only (layer 1 empty).
    assert px(buf, 32, 16, 0) == (0, 0, 0, 0)
    assert px(buf, 32, 17, 0) == RED
    assert px(buf, 32, 31, 15) == RED


def test_single_layer_renders_alone(app):
    lm = load(app)
    layer1 = mv.render_tiles(lm, [1])
    assert px(layer1, 32, 7, 5) == (0, 0, 0, 0)
    assert px(layer1, 32, 8, 5) == GREEN
    assert px(layer1, 32, 20, 5) == (0, 0, 0, 0)


def test_partial_alpha_overwrites_like_the_game(tmp_path):
    # The bake copies any pixel with non-zero alpha as it is (no blending): a half-transparent
    # tile over an opaque one leaves the half-transparent pixel.
    app = make_app(tmp_path, layers=((0, 0), (2, E)))
    buf = mv.render_tiles(load(app))
    assert px(buf, 32, 5, 5) == BLUE[:3] + (128,)


def test_backdrop_repeats_from_the_top_left(app):
    lm = load(app)
    back = mv.render_back(lm)
    assert [px(back, 32, x, 0) for x in range(4)] == [BACK_A, BACK_B, BACK_A, BACK_B]
    assert px(back, 32, 31, 15) == BACK_B


def test_export_composites_tiles_over_the_backdrop(app):
    lm = load(app)
    out = mv.export_rgba(lm)
    assert px(out, 32, 0, 0) == BACK_A  # transparent tile pixel: backdrop shows
    assert px(out, 32, 16, 0) == BACK_A
    assert px(out, 32, 1, 0) == RED
    assert px(out, 32, 8, 5) == GREEN
    no_back = mv.export_rgba(lm, back=False)
    assert px(no_back, 32, 0, 0) == (0, 0, 0, 0)


def test_attribute_overlay_colours(app):
    lm = load(app)
    att = mv.render_att(lm)
    expected = {
        (0, 0): 0x02,
        (8, 0): 0x04,
        (16, 0): 0x10,
        (24, 0): 0x01,
        (0, 8): 0x12,
        (8, 8): 0x08,
        (16, 8): 0x01,
        (24, 8): 0x02,
    }
    for (x0, y0), value in expected.items():
        color = mv.ATT_STYLES[value][0]
        for dx, dy in ((0, 0), (7, 0), (0, 7), (7, 7)):
            assert px(att, 32, x0 + dx, y0 + dy) == color, (value, dx, dy)
    assert mv.ATT_STYLES[0x01][0][3] == 0  # air is not drawn
    assert len({style[0] for style in mv.ATT_STYLES.values()}) == len(mv.ATT_STYLES)


def test_unknown_attribute_value_is_coloured_listed_and_reported(tmp_path):
    att = bytes([0x20, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01])
    app = make_app(tmp_path, att=att)
    loaded, _, _ = mv.load_all(app)
    lm = loaded[0]
    assert lm.problems == ["map 00: 00.att: unknown attribute value 0x20 (0x20) in 1 cells"]
    assert px(mv.render_att(lm), 32, 3, 3) == mv.UNKNOWN_COLOR
    legend = mv._legend(lm)
    assert "<code>0x20</code>" in legend and "unknown value" in legend


def test_over_blends_only_partial_pixels():
    dst = bytearray(bytes((100, 100, 100, 255)) * 2 + bytes((0, 0, 0, 0)))
    src = bytes((200, 0, 0, 255)) + bytes((0, 0, 255, 51)) + bytes((10, 20, 30, 128))
    mv.over(dst, src)
    assert tuple(dst[0:4]) == (200, 0, 0, 255)
    # 51/255 = 0.2 of blue over grey: (100*204 + 0*51) / 255 = 80, (255*51 + 100*204)/255 = 131
    assert tuple(dst[4:8]) == (80, 80, 131, 255)
    # Over a transparent pixel the colour is kept and the alpha copied.
    assert tuple(dst[8:12]) == (10, 20, 30, 128)


def test_export_with_attributes_blends_the_overlay(app):
    lm = load(app)
    out = mv.export_rgba(lm, att=True)
    r, g, b, a = mv.ATT_STYLES[0x02][0]
    blend = tuple(
        (c * a + d * (255 - a) + 127) // 255 for c, d in zip((r, g, b), RED[:3], strict=True)
    )
    assert px(out, 32, 1, 1) == blend + (255,)
    assert px(out, 32, 24, 0) == RED  # air: nothing drawn over the tiles


def test_object_markers(app):
    lm = load(app)
    assert [o["label"] for o in lm.objects] == [
        "portal 0 portal000",
        "NPC 0 Npc",
        "mob 1 Blue Snail",
    ]
    assert lm.objects[0]["anchor"] == [8, 15]
    assert lm.objects[2]["sprite"] == "sprite/mob/60.sprprj"
    out = mv.export_rgba(lm, back=False, objects=True)
    assert px(out, 32, 8, 15) == (0, 0, 0, 255)  # border on the anchor row
    assert px(out, 32, 8, 12) == mv.OBJ_COLORS["portal"]  # inside the square
    assert px(out, 32, 24, 12) == mv.OBJ_COLORS["mob"]  # drawn last on cell 1
    assert px(out, 32, 8, 8) == GREEN  # above the 7-px square: the tiles
    assert px(out, 32, 4, 12) == RED  # left of it


# --- Problems -------------------------------------------------------------------------------


def test_missing_tile_is_reported_once_per_index(tmp_path, capsys):
    app = make_app(tmp_path, layers=((0, 7), (7, 9)))
    n, problems = mv.batch(app)
    assert n == 1
    assert problems == [
        "map 00: layer 0: missing tile 7 (tile 7 is outside sheet 0 (3 tiles)): "
        "1 cells, first at tile (1, 0)",
        "map 00: layer 1: missing tile 7 (tile 7 is outside sheet 0 (3 tiles)): "
        "1 cells, first at tile (0, 0)",
        "map 00: layer 1: missing tile 9 (tile 9 is outside sheet 0 (3 tiles)): "
        "1 cells, first at tile (1, 0)",
    ]
    assert mv.main(["batch", "--app", str(app)]) == 1
    captured = capsys.readouterr()
    assert captured.err.count("error: map 00: layer") == 3
    assert "1 maps loaded and rendered, 3 errors" in captured.out


def test_missing_files_and_assets_are_reported(tmp_path):
    app = make_app(tmp_path)
    (app / "map" / "00.obj").unlink()
    (app / "map" / "back" / "0.png").unlink()
    _, problems = mv.batch(app)
    assert problems == ["map 00: missing map/00.obj", "map 00: missing map/back/0.png (backdrop)"]
    (app / "map" / "tile" / "sheet.png").unlink()
    _, problems = mv.batch(app)
    assert "map 00: missing map/tile/sheet.png (tile sheet of map/0.mapprj)" in problems


def test_missing_sprite_script_and_mob_are_reported(tmp_path):
    objects = (
        objfile.Record(-1, 0xFF, "NoSuchMapScript"),
        objfile.Record(0, 5, "NoScript"),
        objfile.Record(1, 70, ""),
    )
    app = make_app(tmp_path, objects=objects)
    _, problems = mv.batch(app)
    assert problems == [
        "map 00: map script 'NoSuchMapScript': no script/<chapter|common>/*.out",
        "map 00: object 0 (NPC 1 NoScript): no script/<chapter|common>/NoScript.out",
        "map 00: object 0 (NPC 1 NoScript): missing sprite/mob/5.sprprj",
        "map 00: object 1: mob id 11 (kind 70) is not in English/data/mob.dat",
    ]


def test_att_grid_must_match_the_map(tmp_path):
    app = make_app(tmp_path, att=ATT[:4])
    _, problems = mv.batch(app)
    assert problems == ["map 00: 00.att has 4 cells, the map needs 4x2"]


def test_parse_error_is_reported(tmp_path):
    app = make_app(tmp_path)
    (app / "map" / "00.map").write_bytes(b"\x02\x00")
    n, problems = mv.batch(app)
    assert n == 1 and len(problems) == 1 and problems[0].startswith("map 00: 00.map")


def test_orphans_and_selection(tmp_path):
    app = make_app(tmp_path)
    (app / "map" / "06.att").write_bytes(b"\x01")
    ids, problems, orphans = mv.discover(app)
    assert [m.stem for m in ids] == ["00"] and problems == [] and orphans == ["06.att"]
    loaded, problems, _ = mv.load_all(app, "00,99")
    assert [lm.stem for lm in loaded] == ["00"]
    assert problems == ["no map '99' in map/"]


# --- Pages ----------------------------------------------------------------------------------


def two_map_app(tmp_path) -> Path:
    app = make_app(tmp_path)
    make_app(tmp_path, stem="01", objects=(objfile.Record(1, 7, '<b>&"x'),))
    return app


def test_build_is_deterministic_and_complete(tmp_path):
    app = two_map_app(tmp_path)
    files, problems = mv.render(app)
    again, _ = mv.render(app)
    assert files == again
    assert sorted(files) == [
        "00.html",
        "00/att.png",
        "00/layer0.png",
        "00/layer1.png",
        "00/objects.json",
        "01.html",
        "01/att.png",
        "01/layer0.png",
        "01/layer1.png",
        "01/objects.json",
        "back/0.png",
        "index.html",
    ]
    assert problems == [
        'map 01: object 0 (NPC 3 <b>&"x): no script/<chapter|common>/<b>&"x.out',
        'map 01: object 0 (NPC 3 <b>&"x): missing sprite/mob/7.sprprj',
    ]
    layer1 = png.decode(files["00/layer1.png"])
    assert (layer1.width, layer1.height) == (32, 16)
    assert layer1.pixel(8, 5) == GREEN and layer1.pixel(7, 5) == (0, 0, 0, 0)
    assert png.decode(files["00/att.png"]).pixel(0, 0) == mv.ATT_STYLES[0x02][0]
    assert png.decode(files["back/0.png"]).pixel(1, 0) == BACK_B
    objects = json.loads(files["00/objects.json"])
    assert [o["category"] for o in objects] == ["portal", "npc", "mob"]
    assert objects[2]["mob_name"] == "Blue Snail"


def test_map_page(tmp_path):
    app = two_map_app(tmp_path)
    files, _ = mv.render(app)
    page = files["00.html"].decode()
    assert '<a rel="next" href="01.html">' in page and 'rel="prev"' not in page
    for layer in ("back", "l0", "l1", "att", "obj", "labels"):
        assert f'data-layer="{layer}"' in page
    assert 'src="00/layer0.png"' in page and 'src="00/att.png"' in page
    assert "background-image:url(back/0.png)" in page
    assert 'style="left:8px;top:15px"' in page and "portal 0 portal000" in page
    assert "TestMap" in page and "No problems." in page
    other = files["01.html"].decode()
    assert '<a rel="prev" href="00.html">' in other
    assert "&lt;b&gt;&amp;&quot;x" in other and '<b>&"x' not in other
    index = files["index.html"].decode()
    assert 'href="00.html"' in index and 'rel="first"' in index and "2 maps, 2 problems" in index


def test_build_and_check_cli(tmp_path, capsys):
    app = make_app(tmp_path)
    out = tmp_path / "out"
    assert mv.main(["build", "--app", str(app), "--out", str(out)]) == 0
    assert (out / "index.html").is_file() and (out / hp.MARKER).is_file()
    assert mv.main(["check", "--app", str(app), "--out", str(out)]) == 0
    (out / "00" / "att.png").write_bytes(b"stale")
    assert mv.main(["check", "--app", str(app), "--out", str(out)]) == 1
    assert "00/att.png: stale" in capsys.readouterr().err


def test_export_cli(tmp_path):
    app = make_app(tmp_path)
    out = tmp_path / "map.png"
    assert mv.main(["export", "00", "--app", str(app), "--out", str(out), "--att"]) == 0
    img = png.decode(out.read_bytes())
    assert (img.width, img.height) == (32, 16)
    assert img.rgba == bytes(mv.export_rgba(load(app), att=True))
    first = out.read_bytes()
    mv.main(["export", "00", "--app", str(app), "--out", str(out), "--att"])
    assert out.read_bytes() == first
    assert mv.main(["export", "42", "--app", str(app), "--out", str(out)]) == 1


def test_missing_app_dir(tmp_path, capsys):
    assert mv.main(["batch", "--app", str(tmp_path / "nope")]) == 1
    assert "not a directory" in capsys.readouterr().err


def test_a_map_that_does_not_parse_gets_no_page(tmp_path):
    app = two_map_app(tmp_path)
    (app / "map" / "00.map").write_bytes(b"\x02\x00")
    files, problems = mv.render(app)
    assert "00.html" not in files and "01.html" in files
    assert problems[0].startswith("map 00: 00.map")
    index = files["index.html"].decode()
    assert 'href="00.html"' not in index and 'rel="first" href="01.html"' in index
    assert hp.esc(problems[0]) in index
    assert 'rel="prev"' not in files["01.html"].decode()
