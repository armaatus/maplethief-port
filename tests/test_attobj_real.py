"""Real-asset checks: every shipped .att and .obj file decodes cleanly (att.md, obj.md).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import attfile as af
from mtport import mapfile as mf
from mtport import objfile as of

pytestmark = pytest.mark.real_assets

SHIPPED_VALUES = {
    af.AIR,
    af.FOOTHOLD,
    af.LADDER,
    af.FOOTHOLD | af.LADDER,
    af.LADDER_TOP,
    af.WALL,
    af.WALL | af.FOOTHOLD,
}


@pytest.fixture(scope="module")
def map_dir(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app" / "map"


@pytest.fixture(scope="module")
def maps(map_dir):
    return {p.stem: mf.load(p) for p in sorted(map_dir.glob("*.map"))}


def test_all_91_att_files_decode(map_dir, maps):
    paths = sorted(map_dir.glob("*.att"))
    assert len(paths) == 91
    values = set()
    packed = 0
    for path in paths:
        data = path.read_bytes()
        att = af.decode_any(data, path.name)
        values |= set(att.cells)
        if path.stem == "106":
            assert not att.packed  # the orphan: see test_106_is_a_raw_44x48_grid
            continue
        packed += 1
        # Strict decode: magic, runs add up to exactly the header count, no odd byte left.
        assert att.packed, path.name
        # The grid is 2w x 2h cells of the matching .map (one cell per 8x8 pixels).
        m = maps[path.stem]
        w, h = af.grid_size(m.width, m.height)
        assert len(att.cells) == w * h, path.name
        # Every byte is accounted for: re-encoding gives the original file back.
        assert af.encode(att) == data, path.name
    assert packed == 90
    assert set(maps) == {p.stem for p in paths} - {"106"}
    assert values == SHIPPED_VALUES


def test_first_att_matches_the_spec(map_dir, maps):
    att = af.load(map_dir / "00.att")
    rows = att.rows(2 * maps["00"].width)
    assert len(rows) == 56
    # 00.att: 255+255+255+237 air cells, then a 4-cell foothold at row 12, columns 42-45.
    assert rows[12][42:46] == bytes([af.FOOTHOLD]) * 4
    assert rows[14][18:20] == bytes([af.FOOTHOLD | af.LADDER]) * 2  # ladder top in a floor
    assert rows[15][18:20] == bytes([af.LADDER]) * 2
    assert rows[50] == bytes([af.FOOTHOLD]) * 80  # the ground


def test_106_is_a_raw_44x48_grid(map_dir, maps):
    att = af.load(map_dir / "106.att")
    assert "106" not in maps and not (map_dir / "106.obj").exists()
    assert len(att.cells) == 2112 == 44 * 48
    rows = att.rows(44)
    # Laid out 44 wide, its ladders are unbroken columns capped by LADDER_TOP cells, the way
    # the area-10 maps draw them, and row 42 is a full-width floor.
    assert rows[2][12:14] == bytes([af.LADDER_TOP]) * 2
    assert all(rows[y][12:14] == bytes([af.LADDER]) * 2 for y in range(3, 21))
    assert rows[42] == bytes([af.FOOTHOLD]) * 44
    assert att.histogram() == {1: 1822, 2: 90, 4: 188, 6: 4, 8: 8}


def test_all_90_obj_files_decode(map_dir, maps):
    paths = sorted(map_dir.glob("*.obj"))
    assert len(paths) == 90
    assert {p.stem for p in paths} == set(maps)
    totals = dict.fromkeys(of.CATEGORIES, 0)
    for path in paths:
        data = path.read_bytes()
        obj = of.decode(data, path.name)  # strict: no truncation, no trailing bytes
        assert of.encode(obj) == data, path.name
        m = maps[path.stem]
        for r in obj.objects:
            assert 0 <= r.cell < m.width * m.height, (path.name, r)
            assert "|" not in r.name, (path.name, r)
            if r.category in ("portal", "hidden_portal") and r.name:
                assert r.name.startswith("portal"), (path.name, r)
            if r.category == "mob":
                assert r.name == "", (path.name, r)
        for r in obj.records:
            if r.is_map_script:
                assert r.kind == 0xFF and r.name.endswith("Map"), (path.name, r)
        for category, n in obj.counts().items():
            totals[category] += n
    assert totals == {"map_script": 6, "portal": 153, "hidden_portal": 7, "npc": 83, "mob": 772}


def test_first_obj_matches_the_spec(map_dir):
    obj = of.load(map_dir / "00.obj")
    assert obj.map_script is None
    assert [(r.cell, r.kind, r.name) for r in obj.records] == [
        (246, 16, "smith"),
        (267, 9, "Alex"),
        (274, 0, "portal000"),
        (636, 7, "Darklord"),
        (762, 5, "Equip00"),
        (767, 6, "Etc00"),
        (971, 8, "JM"),
        (996, 1, "portal001"),
    ]
    portal = obj.records[2]
    assert portal.tile(40) == (34, 6) and portal.position(40) == (552, 111)
