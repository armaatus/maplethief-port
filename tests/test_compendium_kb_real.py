"""mtport.compendium.kb (#213) on the real bundle (local only: needs work/ipa/).

Counts, zero dangling references, every drop and listing on an existing item, every NPC and
spawn on a map, every map named (or listed with the reason), and the name tables of
mtport.compendium.world checked against the armv7 slice (work/slices/, when present).
"""

from __future__ import annotations

import struct
from pathlib import Path

import pytest

from mtport import macho
from mtport.compendium import kb as kbm
from mtport.compendium import tables, world
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"


def test_counts(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    assert len(e["monster"]) == 69
    assert len(e["npc"]) == 39
    assert len(e["item"]) == 212
    assert sum(i["table"] == "equip" for i in e["item"].values()) == 74
    assert len(e["skill"]) == 24
    assert len(e["map"]) == 90
    assert len(e["area"]) == 11
    assert len(e["listing"]) == 202
    assert len(e["level"]) == 100
    assert sum(len(s["listings"]) for s in e["shop"].values()) == 202


def test_zero_dangling_references(real_kb: kbm.KB, tmp_path: Path) -> None:
    assert real_kb.problems == []
    assert kbm.check_kb(real_kb.entities) == []
    out = tmp_path / "kb"
    kbm.write(real_kb, out)
    assert kbm.check_kb(kbm.load_kb(out), kbm.load_relations(out)) == []


def test_drops_and_listings_point_at_items(real_kb: kbm.KB, ipa_dir: Path) -> None:
    e = real_kb.entities
    items = e["item"]
    for mob in e["monster"].values():
        for d in mob["drops"]:
            assert d["id"] in items
    for listing in e["listing"].values():
        assert len(listing["item"]) == 1 and listing["item"][0]["id"] in items
    # every drop with a chance of 1 or more made it into the KB
    en = kbm.Sources(ipa_dir / "Payload" / "MapleThief.app").tables["en"]
    expected = 0
    for m in en["mob"].records:
        chances = [
            m.equip_drop_chance,
            m.etc_drop_1_chance,
            m.etc_drop_2_chance,
            m.etc_drop_3_chance,
        ]
        expected += sum(c > 0 for c in chances)
    assert sum(len(m["drops"]) for m in e["monster"].values()) == expected
    assert e["listing"][str(tables.SLIP_ROW)]["note"]


def test_npcs_and_spawns_sit_on_maps(real_kb: kbm.KB) -> None:
    e = real_kb.entities
    placements = sum(len(m["npcs"]) for m in e["map"].values())
    assert placements == 83  # docs/formats/obj.md: 83 NPC records
    spawns = sum(s["count"] for m in e["map"].values() for s in m["spawns"])
    assert spawns == 772  # 772 mob spawn records
    for npc in e["npc"].values():
        assert all(r["id"] in e["map"] for r in npc["stands_on"])
    for mob in e["monster"].values():
        assert all(r["id"] in e["map"] for r in mob["spawns_on"])
    portals = sum(len(m["portals"]) for m in e["map"].values())
    assert portals == 160  # 153 portals + 7 hidden
    named = [p for m in e["map"].values() for p in m["portals"] if p["script"]]
    assert all(p["targets"] for p in named)  # every named portal has a known target


def test_every_map_has_a_name_or_a_reason(real_kb: kbm.KB) -> None:
    maps = real_kb.entities["map"]
    for ident, m in maps.items():
        assert m["name"]["en"] and m["name"]["ko"], ident
        if m["name_source"] != "spot":
            assert m["name_note"], ident
    defaults = sorted(
        (m for m in maps.values() if m["name_source"] != "spot"),
        key=lambda m: kbm.natural_key(m["id"]),
    )
    assert [m["id"] for m in defaults] == [
        "0-5",
        "2-6",
        "2-7",
        "2-8",
        "2-9",
        "2-10",
        "2-11",
        "4-18",
        "6-1",
        "6-2",
        "6-3",
        "7-6",
        "8-6",
        "9-9",
        "9-10",
    ]
    assert len(world.name_report(real_kb.entities)) == len(defaults)
    assert maps["0-0"]["name"] == {"en": "Kerning City Village", "ko": "커닝시티 마을"}
    assert maps["10-5"]["name"]["en"] == "Hellfire Altar"
    assert real_kb.entities["area"]["9"]["name"]["en"] == "El Nath"


def test_spot_names_are_all_used(real_kb: kbm.KB) -> None:
    """Every m_strMiddleSpotName is the name of at least one map."""
    keys = {m["name_spot"]["key"] for m in real_kb.entities["map"].values()}
    assert keys == set(range(1, 68))


@pytest.mark.skipif(not SLICE.is_file(), reason="work/slices/MapleThief.armv7 absent")
def test_name_tables_match_the_binary() -> None:
    m = macho.load(SLICE)
    # CDataMgr::MapSpotIndex 0x36F3D: ldr of a PC-relative literal at 0x36F70 -> the table
    assert m.u32(0x36F70) + 0x36F44 == world.SPOT_TABLE_ADDR
    rows, cols = world.SPOT_TABLE_SHAPE
    raw = m.read(world.SPOT_TABLE_ADDR, rows * cols * 4)
    for area in range(rows):
        row = struct.unpack_from(f"<{cols}i", raw, area * cols * 4)
        want = world.SPOT_TABLE[area]
        assert row[: len(want)] == want and not any(row[len(want) :]), area
