"""Unit tests for the compendium images (mtport.compendium.images, issue #216) on a synthetic
.app built in code: sprites, icon packs, tables and one map."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from mtport import attfile, mapfile, mapprj, objfile, png, sprprj
from mtport import datatables as dt
from mtport.compendium import images as ci

RED, BLUE, GREEN = (255, 0, 0, 255), (0, 0, 255, 255), (0, 200, 0, 255)
HAT = (250, 10, 120, 255)


def solid(w: int, h: int, color) -> bytes:
    return png.encode_rgba(w, h, bytes(color) * (w * h))


def layer(image, x, y):
    return sprprj.Layer(image, 255, False, True, x, y)


def frame(*layers, delay=1):
    return sprprj.Frame(delay, tuple(layers))


def action(*frames):
    return sprprj.Action(tuple(frames))


def record(cls, **values):
    base = {c.name: "" if c.kind == "str" else 0 for c in dt.layout(cls)}
    base.update(values)
    return cls(**base)


def write_project(app: Path, folder: str, n: int, project: sprprj.SpriteProject | None) -> None:
    d = app / "sprite" / folder
    d.mkdir(parents=True, exist_ok=True)
    data = b"\0\0" if project is None else sprprj.build(project)  # None: a shipped empty file
    (d / f"{n}.sprprj").write_bytes(data)


def make_app(root: Path) -> Path:
    app = root / "MapleThief.app"
    res = app / "sprite" / "mob" / "res"
    res.mkdir(parents=True)
    (res / "r.png").write_bytes(solid(2, 3, RED))
    (res / "b.png").write_bytes(solid(3, 2, BLUE))
    images = (sprprj.Image("r.png"), sprprj.Image("b.png"))
    move = action(frame(layer(0, -1, -3), delay=2), frame(layer(1, -1, -2), delay=0))
    hit = action(frame(layer(1, 0, -2)))
    die = action(frame(layer(0, -1, -3), delay=3), frame(layer(0, 1, -3), delay=1))
    write_project(app, "mob", 60, sprprj.SpriteProject(images, (move, hit, die)))
    write_project(app, "mob", 4, sprprj.SpriteProject(images, (action(frame(layer(1, -1, -2))),)))
    write_project(app, "mob", 5, None)  # NPC 1: empty project
    write_project(app, "mob", 0, None)  # the portals' sprites
    write_project(app, "mob", 3, None)
    # hero/0: 42 images, standing (action 2) draws body 1, hat 29 and weapon 32.
    hres = app / "sprite" / "hero" / "res"
    hres.mkdir(parents=True)
    hero_images = []
    for i in range(42):
        (hres / f"h{i}.png").write_bytes(solid(2, 2, GREEN))
        hero_images.append(sprprj.Image(f"h{i}.png"))
    stand = action(frame(layer(1, -1, -2), layer(29, -1, -4), layer(32, 1, -2)))
    empty = action(frame())
    write_project(app, "hero", 0, sprprj.SpriteProject(tuple(hero_images), (empty, empty, stand)))
    (app / "png" / "cab").mkdir(parents=True)
    (app / "png" / "cab" / "cab_3_0.png").write_bytes(solid(2, 2, HAT))
    (app / "png" / "cab" / "cab_3_1.png").write_bytes(solid(2, 2, HAT))
    # Icon packs: PNGs back to back.
    for rel, colors in (
        ("png/item/equip.dat", (RED, BLUE)),
        ("png/item/etc.dat", (GREEN,)),
        ("png/skill/skillico.dat", (BLUE,)),
    ):
        (app / rel).parent.mkdir(parents=True, exist_ok=True)
        (app / rel).write_bytes(b"".join(solid(2, 2, c[:3] + (128,)) for c in colors))
    data = app / "English" / "data"
    data.mkdir(parents=True)
    tables = {
        "mob": [record(dt.Mob, id=1, name="Blue Snail", sprite=60)],
        "npc": [record(dt.Npc, id=0, name="Mr.$Lee"), record(dt.Npc, id=1, name="Ghost")],
        "item_equip": [
            record(dt.ItemEquip, id=0, name="Red Hat", slot=0, icon=1, look=3),
            record(dt.ItemEquip, id=1, name="Shoes", slot=4, icon=0, look=-1),
            record(dt.ItemEquip, id=2, name="Lost", slot=4, icon=9, look=-1),
        ],
        "item_etc": [record(dt.ItemEtc, id=0, name="Potion", icon=0)],
        "skill": [record(dt.Skill, id=0, name="Lucky Seven", icon=0)],
    }
    for name, rows in tables.items():
        (data / f"{name}.dat").write_bytes(dt.encode(name, rows))
    # One map, area 0 map 0: 2 x 1 tiles; a portal, a hidden portal, NPC 0 and a snail spawn.
    (app / "map" / "tile").mkdir(parents=True)
    (app / "map" / "back").mkdir(parents=True)
    (app / "map" / "0.mapprj").write_bytes(mapprj.encode(mapprj.MapProject(16, 16, ("s.png",))))
    (app / "map" / "tile" / "s.png").write_bytes(solid(16, 16, RED))
    (app / "map" / "back" / "0.png").write_bytes(solid(2, 1, BLUE))
    m = mapfile.MapFile(2, 1, ((0, mapfile.EMPTY),))
    (app / "map" / "00.map").write_bytes(mapfile.encode(m))
    (app / "map" / "00.att").write_bytes(attfile.encode(attfile.AttFile(bytes([2] * 8))))
    objects = (
        objfile.Record(0, 0, ""),
        objfile.Record(1, 3, ""),
        objfile.Record(0, 4, ""),
        objfile.Record(1, 60, ""),
        objfile.Record(1, 60, ""),
    )
    (app / "map" / "00.obj").write_bytes(objfile.encode(objfile.ObjFile(objects)))
    return app


@pytest.fixture
def app(tmp_path):
    return make_app(tmp_path)


def built(app: Path, out: Path) -> ci.Export:
    ex, _ = ci.build(app)
    ci.write(ex, out)
    return ex


EXPECTED_FILES = {
    "item/equip/0.png",
    "item/equip/0@2x.png",
    "item/equip/0.worn.png",
    "item/equip/1.png",
    "item/equip/1@2x.png",
    "item/etc/0.png",
    "item/etc/0@2x.png",
    "map/0-0.png",
    "map/0-0.svg",
    "map/0-0@2x.png",
    "monster/1/die.anim.png",
    "monster/1/die.png",
    "monster/1/hit.png",
    "monster/1/move.anim.png",
    "monster/1/move.png",
    "npc/0.png",
    "skill/0.png",
    "skill/0@2x.png",
}


def test_names_are_stable(app, tmp_path):
    ex = built(app, tmp_path / "img")
    assert ex.errors == []
    assert set(ex.files) == EXPECTED_FILES
    on_disk = {p.relative_to(tmp_path / "img").as_posix() for p in (tmp_path / "img").rglob("*")}
    assert {p for p in on_disk if "." in p} == EXPECTED_FILES | {"manifest.json"}
    assert ex.missing == {
        "item:equip:2": "icon 9: no such image in png/item/equip.dat (2 images)",
        "npc:1": "sprite/mob/5.sprprj has no actions",
    }


def test_animation_frame_delays_match_the_input(app, tmp_path):
    built(app, tmp_path / "img")
    move = (tmp_path / "img" / "monster/1/move.anim.png").read_bytes()
    s = ci.read_summary(move)
    # delays 2 and 0 ticks: a delay below 1 is one tick, one tick is 50 ms (ADR 0087).
    assert (s.frames, s.delays_ms, s.plays) == (2, (100, 50), 0)
    assert ci.read_summary(
        (tmp_path / "img" / "monster/1/die.anim.png").read_bytes()
    ).delays_ms == (
        150,
        50,
    )
    manifest = json.loads((tmp_path / "img" / "manifest.json").read_text())
    entry = next(e for e in manifest["images"]["monster:1"] if e["path"].endswith("move.anim.png"))
    assert entry["delays_ms"] == [100, 50] and entry["role"] == "anim"
    # The default image (IDAT) is frame 0, the still: a decoder without APNG shows it.
    still = png.load(tmp_path / "img" / "monster/1/move.png")
    assert png.decode(move).rgba == still.rgba


def test_frames_are_cropped_to_their_pixels_with_one_anchor(app, tmp_path):
    ex = built(app, tmp_path / "img")
    move = next(e for e in ex.images["monster:1"] if e["path"] == "monster/1/move.png")
    # red 2x3 at (-1, -3) and blue 3x2 at (-1, -2), both drawn 1 px down: x -1..2, y -2..1.
    assert (move["w"], move["h"], move["anchor"]) == (3, 3, [1, 2])
    img = png.load(tmp_path / "img" / "monster/1/move.png")
    assert img.rgba[:4] == bytes(RED) and img.rgba[-4:] == bytes(4)


def test_encode_apng_round_trip_and_errors():
    frames = [bytes(RED) * 2, bytes(BLUE) * 2, bytes(GREEN) * 2]
    data = ci.encode_apng(2, 1, frames, [50, 100, 65535], plays=1)
    assert ci.read_summary(data) == ci.PngSummary(2, 1, 3, (50, 100, 65535), 1)
    assert ci.read_summary(png.encode_rgba(1, 1, bytes(4))) == ci.PngSummary(1, 1, 1, ())
    with pytest.raises(ValueError, match="frames"):
        ci.encode_apng(2, 1, frames, [50])
    with pytest.raises(ValueError, match="65535"):
        ci.encode_apng(2, 1, frames[:1], [70000])
    broken = bytearray(data)
    broken[40] ^= 1
    with pytest.raises(ci.FormatError, match="CRC"):
        ci.read_summary(bytes(broken))


def test_icons_at_1x_and_2x_on_transparency(app, tmp_path):
    built(app, tmp_path / "img")
    one = png.load(tmp_path / "img" / "item/equip/0.png")
    two = png.load(tmp_path / "img" / "item/equip/0@2x.png")
    assert (one.width, two.width) == (2, 4)
    assert one.rgba[:4] == bytes((0, 0, 255, 128))  # icon 1 of the pack: half-transparent blue
    assert two.rgba == ci.upscale(one, 2).rgba


def test_upscale_is_nearest_neighbour():
    img = png.Image(2, 1, bytes((1, 2, 3, 4, 5, 6, 7, 8)))
    big = ci.upscale(img, 3)
    row = bytes((1, 2, 3, 4)) * 3 + bytes((5, 6, 7, 8)) * 3
    assert (big.width, big.height, big.rgba) == (6, 3, row * 3)


def test_worn_equipment_swaps_the_items_images_into_the_standing_hero(app, tmp_path):
    ex = built(app, tmp_path / "img")
    worn = png.load(tmp_path / "img" / "item/equip/0.worn.png")
    colors = {worn.rgba[i : i + 4] for i in range(0, len(worn.rgba), 4)}
    assert bytes(HAT) in colors and bytes(GREEN) in colors
    assert not any(e["role"] == "worn" for e in ex.images["item:equip:1"])  # shoes: no look


def test_map_render_2x_and_overlay(app, tmp_path):
    ex = built(app, tmp_path / "img")
    entries = {e["role"]: e for e in ex.images["map:0-0"]}
    assert {r: (e["w"], e["h"]) for r, e in entries.items()} == {
        "render": (32, 16),
        "render2x": (64, 32),
        "overlay": (32, 16),
    }
    one = png.load(tmp_path / "img" / "map/0-0.png")
    assert one.rgba[:4] == bytes(RED) and one.rgba[16 * 4 : 17 * 4] == bytes(BLUE)  # tile, back
    svg = (tmp_path / "img" / "map/0-0.svg").read_text()
    assert svg.startswith('<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16"')
    assert '<text x="8" y="12">0</text>' in svg  # portal 0 at the bottom centre of tile 0
    assert "hidden portal 3" in svg
    assert ">Mr. Lee</text>" in svg
    assert svg.count("data:image/png;base64,") == 1  # the snail embedded once ...
    assert svg.count('<use href="#m1"') == 2  # ... for both spawns


def test_manifest_lists_every_file_and_check_passes(app, tmp_path):
    out = tmp_path / "img"
    built(app, out)
    manifest = json.loads((out / "manifest.json").read_text())
    listed = {e["path"] for v in manifest["images"].values() for e in v}
    assert listed == EXPECTED_FILES
    for entries in manifest["images"].values():
        assert entries == sorted(entries, key=lambda e: e["path"])
        for e in entries:
            assert e["alt"] and e["w"] > 0 and e["h"] > 0
    ents = ci.entities(ci.load_sources(app))
    errors, notes = ci.check(out, ents)
    assert errors == []
    assert notes == [
        "item:equip:2: no icon image: icon 9: no such image in png/item/equip.dat (2 images)",
        "npc:1: no still image: sprite/mob/5.sprprj has no actions",
    ]


def test_check_reports_missing_extra_and_wrong_files(app, tmp_path):
    out = tmp_path / "img"
    built(app, out)
    (out / "skill" / "0.png").unlink()
    (out / "stray.png").write_bytes(b"")
    (out / "monster/1/hit.png").write_bytes(png.encode_rgba(1, 1, bytes(4)))
    still = (out / "monster/1/move.png").read_bytes()
    (out / "monster/1/move.anim.png").write_bytes(still)
    ents = {"monster": {"1": {}, "2": {}}, "skill": {"0": {}}}
    errors, _ = ci.check(out, ents)
    assert errors == [
        "monster:1 monster/1/hit.png: 1x1, manifest 3x2",
        "monster:1 monster/1/move.anim.png: frame delays [], manifest [100, 50]",
        "skill:0 skill/0.png: listed, no such file",
        "stray.png: on disk, not in manifest.json",
        "monster:2: no still image",
    ]


def test_two_runs_are_byte_identical(app, tmp_path):
    a, b = tmp_path / "a", tmp_path / "b"
    built(app, a)
    built(app, b)
    files_a = sorted(p.relative_to(a) for p in a.rglob("*") if p.is_file())
    files_b = sorted(p.relative_to(b) for p in b.rglob("*") if p.is_file())
    assert files_a == files_b
    for rel in files_a:
        assert (a / rel).read_bytes() == (b / rel).read_bytes(), rel


def test_write_replaces_its_own_directory_only(app, tmp_path):
    out = tmp_path / "img"
    built(app, out)
    (out / "old.png").write_bytes(b"x")
    built(app, out)
    assert not (out / "old.png").exists()
    other = tmp_path / "other"
    other.mkdir()
    (other / "keep.txt").write_text("mine")
    ex, _ = ci.build(app)
    with pytest.raises(FileExistsError):
        ci.write(ex, other)
    assert (other / "keep.txt").read_text() == "mine"


def test_cli_build_and_check(app, tmp_path, capsys):
    out = tmp_path / "img"
    assert ci.main(["build", "--app", str(app), "--out", str(out)]) == 0
    text = capsys.readouterr().out
    assert "npc:1: no image: sprite/mob/5.sprprj has no actions" in text
    assert "18 files for 3 item, 1 map, 1 monster, 1 npc, 1 skill" in text
    assert ci.main(["check", "--app", str(app), "--out", str(out)]) == 0
    assert "0 errors, 2 entities without an image" in capsys.readouterr().out
    (out / "map/0-0.svg").unlink()
    assert ci.main(["check", "--app", str(app), "--out", str(out)]) == 1
