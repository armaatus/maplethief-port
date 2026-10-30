"""Real-asset checks for the save checks' limits (#148, ADR 0078): every number in
tools/mtport/savecheck.py and runtime/include/maplethief/saves.h (test_savecheck.py keeps the two
equal) against the shipped tables and maps, and every quest key the game has passes the key
rule. Skipped when work/ipa/ is absent (CI)."""

import plistlib

import pytest

from mtport import datatables as dt
from mtport import savecheck as sc
from mtport import savedata as sd

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def tables(app):
    return dt.load_dir(app / "English" / "data")


def test_item_ids_levels_and_skills_match_the_tables(tables) -> None:
    assert len(tables["item_equip"].records) == sc.EQUIP_IDS
    assert [r.id for r in tables["item_equip"].records] == list(range(sc.EQUIP_IDS))
    assert len(tables["item_etc"].records) == sc.ETC_IDS
    assert [r.id for r in tables["item_etc"].records] == list(range(sc.ETC_IDS))
    assert [r.level for r in tables["hero"].records] == list(range(1, sc.MAX_LEVEL + 1))
    skills = tables["skill"].records
    assert max(r.max_level for r in skills) == sc.MAX_SKILL_LEVEL
    assert max(r.tier for r in skills) == sc.MAX_JOB_TIER
    assert max(r.branch for r in skills) == sc.MAX_BRANCH


def test_areas_and_maps_match_the_shipped_files(app) -> None:
    folder = app / "map"
    shipped = {p.stem for p in folder.glob("*.map")}
    expected = {f"{a}{m}" for a, n in enumerate(sc.MAPS_IN_AREA) for m in range(n)}
    assert shipped == expected
    assert {p.stem for p in folder.glob("*.mapprj")} == {
        str(a) for a in range(len(sc.MAPS_IN_AREA))
    }


def test_every_quest_key_of_the_game_passes_the_key_rule(app) -> None:
    for lang in ("en", "ko"):
        keys = plistlib.loads((app / f"{lang}.lproj" / "Scripts.strings").read_bytes())
        assert keys
        for key in keys:
            raw = key.encode("utf-8")
            assert len(raw) < 64, key  # fits the title field with its NUL
            assert sc._key_bad(sd.fixed(raw, 64)) is None, key


def test_a_new_character_from_the_shipped_tables_passes(tables) -> None:
    level1 = next(r for r in tables["hero"].records if r.level == 1)
    files = {
        "theif0": sd.encode_character(sd.new_character(level1.hp_gain, level1.mp_gain)),
        "quest0": sd.encode_quest(sd.QuestSave()),
        "config": sd.encode_config(sd.Config()),
    }
    assert sc.set_verdict(sc.check_set(files)) == "ok"
