"""The bot's world tables (#297) from the shipped bundle's knowledge base. Local only."""

import pytest

from mtport import botworld as bw
from mtport import objfile

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def tables(real_kb):
    return bw.parse(bw.generate(real_kb.entities))


def test_chapter_1_links_and_gates(tables):
    links = tables.links
    chapter_1 = [lk for lk in links if lk.chapter == 1]
    # The 53 links and 4 gates the chapter-1 bot had by hand before #297.
    assert len(chapter_1) == 53
    assert {(lk.area, lk.map, lk.portal): lk.gate for lk in chapter_1 if lk.gate} == {
        (2, 0, 1): ((bw.Term((0, 9), "==", 2),),),
        (2, 1, 2): ((bw.Term((1, 25), "==", 1),),),
        (2, 4, 2): ((bw.Term((1, 7), "==", 2),),),
        (3, 7, 1): ((bw.Term((0, 18), "==", 1),),),
    }
    assert {lk.area for lk in chapter_1} == {0, 1, 2, 3}


def test_every_link_leaves_from_a_portal_object(tables, ipa_dir):
    links = tables.links
    maps = ipa_dir / "Payload" / "MapleThief.app" / "map"
    for lk in links:
        obj = objfile.load(maps / f"{lk.area}{lk.map}.obj")
        kinds = {o.kind for o in obj.objects if o.category in ("portal", "hidden_portal")}
        assert lk.portal in kinds, lk
        assert (maps / f"{lk.to_area}{lk.to_map}.map").is_file(), lk


def test_every_shop_npc_stands_on_its_map(tables, ipa_dir):
    shops = tables.shops
    maps = ipa_dir / "Payload" / "MapleThief.app" / "map"
    assert shops
    for s in shops:
        obj = objfile.load(maps / f"{s.area}{s.map}.obj")
        assert s.npc in {o.name for o in obj.objects}, s
    # The town shops of chapter 1 sell the Red (3) and Blue (6) Potions the bot restocks.
    for area, npc in ((0, "Etc00"), (1, "Etc10"), (2, "Etc20"), (3, "Etc30")):
        sold = {s.item for s in shops if (s.chapter, s.area, s.npc, s.table) == (1, area, npc, 1)}
        assert {3, 6} <= sold, npc


def test_every_npc_object_and_recipe_crafter_stands_where_the_obj_file_puts_it(tables, ipa_dir):
    maps = ipa_dir / "Payload" / "MapleThief.app" / "map"
    assert len({n.id for n in tables.npcs}) >= 30
    for n in tables.npcs:
        obj = objfile.load(maps / f"{n.area}{n.map}.obj")
        assert n.object in {o.name for o in obj.objects}, n
    placed = {(n.area, n.map, n.object) for n in tables.npcs}
    # The 48 recipes of the compendium (#220), each at its crafter.
    assert len(tables.recipes) == 48
    for rc in tables.recipes:
        assert (rc.area, rc.map, rc.npc) in placed, rc
