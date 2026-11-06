"""The compendium images over the shipped game (issue #216).

Skipped when work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

from __future__ import annotations

import struct

import pytest

from mtport import macho, png
from mtport import mapview_render as mvr
from mtport import spriteview_render as svr
from mtport.compendium import images as ci
from repo_paths import IPA_DIR, REPO

pytestmark = pytest.mark.real_assets

APP = IPA_DIR / "Payload" / "MapleThief.app"
SLICE = REPO / "work" / "slices" / "MapleThief.armv7"


@pytest.fixture(scope="module")
def export(tmp_path_factory):
    out = tmp_path_factory.mktemp("compendium") / "img"
    ex, src = ci.build(APP)
    ci.write(ex, out)
    return ex, src, out


def test_every_entity_has_its_image_and_the_manifest_lists_every_file(export):
    ex, src, out = export
    assert ex.errors == []
    assert ex.missing == {}
    ents = ci.entities(src)
    assert {k: len(v) for k, v in ents.items()} == {
        "monster": 69,
        "npc": 39,
        "item": 212,
        "skill": 24,
        "map": 90,
    }
    assert ci.counts(ex) == {"item": 212, "map": 90, "monster": 69, "npc": 39, "skill": 24}
    errors, notes = ci.check(out, ents)
    assert (errors, notes) == ([], [])


def test_worn_images_for_every_equip_with_a_look(export):
    ex, src, _ = export
    looks = {r.id for r in src.rows("item_equip") if r.look >= 0}
    worn = {
        int(k.split(":")[2]) for k, v in ex.images.items() if any(e["role"] == "worn" for e in v)
    }
    assert worn == looks and len(looks) == 50  # 24 equips are shoes or earrings (look -1)


def test_mob_actions_and_animation_delays(export):
    ex, src, _ = export
    snail = {e["path"]: e for e in ex.images["monster:1"]}
    assert sorted(snail) == [
        "monster/1/die.anim.png",
        "monster/1/die.png",
        "monster/1/hit.png",
        "monster/1/move.anim.png",
        "monster/1/move.png",
    ]
    for mob in src.rows("mob"):
        assert any(
            e["path"] == f"monster/{mob.id}/move.png" for e in ex.images[f"monster:{mob.id}"]
        )
    total = sum(1 for v in ex.images.values() for e in v if e["role"] == "anim")
    assert total > 69


def test_stills_are_the_sprite_viewers_frames(export):
    """Compared by eye for five monsters; here, pixel for pixel: the still is the viewer's
    frame 0 (normal facing) cropped around its pixels."""
    ex, src, out = export
    sprites = {r.id: r.sprite for r in src.rows("mob")}
    for mob_id in (1, 6, 18, 66, 69):
        loaded = svr.load_project(APP, svr.ProjectId("mob", sprites[mob_id]))
        r = svr.render_action(loaded.project.actions[0], loaded.images)
        box = svr.union(svr.visible_box(f[3::4], r.width, r.height) for f in r.frames[False])
        still = png.load(out / f"monster/{mob_id}/move.png")
        assert still.rgba == ci.crop(r.frames[False][0], r.width, box)


def test_map_renders_are_the_map_viewers_export(export):
    _, _, out = export
    assets = mvr.Assets(APP)
    ids = {f"{m.area}-{m.map_no}": m for m in mvr.discover(APP)[0]}
    for ident in ("0-0", "1-0", "2-3", "3-8", "10-5"):
        lm = mvr.load_map(assets, ids[ident])
        w, h = lm.pixel_size
        assert png.load(out / f"map/{ident}.png").rgba == bytes(mvr.export_rgba(lm))
        big = png.load(out / f"map/{ident}@2x.png")
        assert (big.width, big.height) == (2 * w, 2 * h)


@pytest.mark.skipif(not SLICE.is_file(), reason="needs work/slices/ (scripts/bootstrap)")
def test_worn_swap_tables_match_the_binary():
    """WORN's hero/0 image indexes are row 0 of the i32 tables 0x62571 copies to its stack."""
    image = macho.load(SLICE)
    for swap in ci.WORN.values():
        n = len(swap.images)
        assert struct.unpack(f"<{n}i", image.read(swap.table, 4 * n)) == swap.images
