"""Real-asset checks for the map viewer: load and render all 90 shipped maps.

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import htmlpage as hp
from mtport import mapview as mv
from mtport import png

pytestmark = pytest.mark.real_assets

# Picked for Joris's in-game comparison (#26): a town, a field and a boss map.
TOWN, FIELD, BOSS = "00", "01", "105"


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


def test_batch_all_90_maps_without_problems(app):
    n, problems = mv.batch(app)
    assert problems == []
    assert n == 90


def test_only_orphan_is_106_att(app):
    ids, problems, orphans = mv.discover(app)
    assert len(ids) == 90 and problems == [] and orphans == ["106.att"]


def test_build_and_check(app, tmp_path):
    out = tmp_path / "maps"
    files, problems = mv.render(app)
    assert problems == []
    assert sum(1 for f in files if f.endswith(".html")) == 91  # 90 maps + index
    assert len([f for f in files if f.startswith("back/")]) == 10  # 11 areas share 10 backdrops
    hp.write_dir(out, files, mv.VIEWER)
    assert mv.main(["check", "--app", str(app), "--out", str(out)]) == 0


def test_comparison_maps(app):
    loaded, problems, _ = mv.load_all(app, f"{TOWN},{FIELD},{BOSS}")
    assert problems == []
    by = {lm.stem: lm for lm in loaded}
    town, field, boss = by[TOWN], by[FIELD], by[BOSS]
    cats = {s: [o["category"] for o in lm.objects] for s, lm in by.items()}
    # Town: shops and NPCs, no mobs.
    assert cats[TOWN].count("mob") == 0 and cats[TOWN].count("npc") == 6
    assert {"Equip00", "Etc00"} <= {o["name"] for o in town.objects}
    # Field: mob spawns, no NPCs.
    assert cats[FIELD].count("npc") == 0 and cats[FIELD].count("mob") == 19
    # Boss: the Balrog map script and its single spawn (mob 69, a boss in mob.dat).
    assert boss.obj.map_script == "BalogMap"
    mobs = [o for o in boss.objects if o["category"] == "mob"]
    assert [(o["mob_id"], o["mob_name"]) for o in mobs] == [(69, "Balrog")]
    assert (town.pixel_size, field.pixel_size, boss.pixel_size) == (
        (640, 448),
        (720, 560),
        (480, 320),
    )


def test_export_is_deterministic(app, tmp_path):
    a, b = tmp_path / "a.png", tmp_path / "b.png"
    assert mv.export(app, TOWN, a, att=True, objects=True) == []
    assert mv.export(app, TOWN, b, att=True, objects=True) == []
    assert a.read_bytes() == b.read_bytes()
    img = png.decode(a.read_bytes())
    assert (img.width, img.height) == (640, 448)
