"""Real-asset checks for the .mapprj projects and the map/tile, map/back folders
(docs/formats/mapprj.md).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import mapfile, pnginfo
from mtport import mapprj as mp

pytestmark = pytest.mark.real_assets

# area -> (tileset, sheet size, highest tile index used by the area's maps), from the spec.
EXPECTED = {
    0: ("Kerningcity.png", (320, 128), 159),
    1: ("Perion.png", (320, 112), 139),
    2: ("Henesys.png", (320, 128), 159),
    3: ("Dungeon.png", (320, 112), 136),
    4: ("Orvis.png", (320, 176), 219),
    5: ("Orvis-dungeon-2.png", (320, 96), 117),
    6: ("Magatia.png", (320, 192), 231),
    7: ("Magatia-field-2.png", (320, 128), 151),
    8: ("Magatia-field-1.png", (320, 64), 79),
    9: ("elnas.png", (320, 128), 159),
    10: ("Zakum.png", (320, 144), 176),
}


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def projects(app):
    paths = sorted((app / "map").glob("*.mapprj"))
    return {int(p.stem): (p, mp.load(p)) for p in paths}


def test_all_11_projects_parse_exactly(projects):
    assert sorted(projects) == list(range(11))
    for area, (path, p) in projects.items():
        assert path.name == mp.project_path(area).split("/")[-1]
        # decode() is strict: no truncation, no trailing byte, sane names.
        assert (p.tile_width, p.tile_height) == (mp.BAKED_TILE_SIZE, mp.BAKED_TILE_SIZE)
        assert p.tilesets == (EXPECTED[area][0],), path.name
        assert mp.encode(p) == path.read_bytes(), path.name


def test_every_referenced_png_exists(app, projects):
    referenced_tiles = set()
    for _, p in projects.values():
        for rel in p.tileset_paths():
            assert (app / rel).is_file(), rel
            referenced_tiles.add(rel.split("/")[-1])
    referenced_backs = set()
    for area in projects:
        rel = mp.back_path(area)
        assert (app / rel).is_file(), rel
        referenced_backs.add(rel.split("/")[-1])
    # Every sheet in map/tile/ is used; map/back/7.png is the only unreferenced backdrop.
    assert referenced_tiles == {p.name for p in (app / mp.TILE_DIR).glob("*.png")}
    shipped_backs = {p.name for p in (app / mp.BACK_DIR).glob("*.png")}
    assert referenced_backs <= shipped_backs
    assert shipped_backs - referenced_backs == {"7.png"}


def test_map_folder_pngs_are_plain_pngs(app):
    pngs = sorted((app / mp.TILE_DIR).glob("*.png")) + sorted((app / mp.BACK_DIR).glob("*.png"))
    assert len(pngs) == 22
    for path in pngs:
        info = pnginfo.load_info(path)
        assert not info.cgbi, path.name  # standard PNGs, unlike the bundle-root UI art
        if path.parent.name == "back":
            assert (info.width, info.height) == (480, 320), path.name  # one landscape screen


def test_every_map_tile_lies_inside_its_area_tileset(app, projects):
    layouts = {}
    for area, (_, p) in projects.items():
        sizes = [
            (i.width, i.height)
            for i in map(pnginfo.load_info, (app / r for r in p.tileset_paths()))
        ]
        assert sizes == [EXPECTED[area][1]]
        layouts[area] = mp.layout(p, sizes)

    maps = sorted((app / "map").glob("*.map"))
    assert len(maps) == 90
    highest = dict.fromkeys(projects, mapfile.EMPTY)
    per_area: dict[int, list[int]] = {area: [] for area in projects}
    for path in maps:
        area, map_no = mp.split_map_stem(path.stem, frozenset(projects))
        per_area[area].append(map_no)
        m = mapfile.load(path)
        lay = layouts[area]
        for layer in m.layers:
            for v in set(layer) - {mapfile.EMPTY}:
                lay.tile_rect(v)  # raises IndexError if outside the sheet
        highest[area] = max(highest[area], m.max_tile())
    assert highest == {area: e[2] for area, e in EXPECTED.items()}
    # Map numbers are contiguous from 0 in every area.
    assert all(sorted(nums) == list(range(len(nums))) for nums in per_area.values())


def test_worked_example(app, projects):
    data = (app / "map" / "0.mapprj").read_bytes()
    assert data.hex(" ") == "10 00 10 00 01 0f " + b"Kerningcity.png".hex(" ")
    # 00.map's first tile, 0x8D = 141, is column 1, row 7 of Kerningcity.png.
    lay = mp.layout(projects[0][1], [(320, 128)])
    assert lay.tile_rect(0x8D) == mp.TileSource(0, 16, 112, 16, 16)
