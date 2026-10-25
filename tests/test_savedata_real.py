"""Real-asset checks for the save-file parser (docs/formats/save.md).

No save ships with the game, so these check the new-character save that
``CDataMgr::LoadSaveData`` builds against the shipped tables and maps. Skipped when
work/ipa/ is absent (CI). Two optional extras, each skipped on its own:

- ``MAPLETHIEF_SAVE_DIR``: a copy of a real device's ``Documents/`` (see save.md); every save
  file in it must parse and re-encode byte-exactly.
- ``work/ghidra/functions.json`` (tools/ghidra/import): every function in the names table is a
  function entry with that Thumb bit.
"""

import json
import os
from pathlib import Path

import pytest

from mtport import datatables as dt
from mtport import savedata as sd
from repo_paths import REPO

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def app(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app"


@pytest.fixture(scope="module")
def tables(app):
    return dt.load_dir(app / "English" / "data")


def test_new_character_from_the_shipped_tables(app, tables):
    level1 = next(r for r in tables["hero"].records if r.level == 1)
    save = sd.new_character(level1.hp_gain, level1.mp_gain)
    data = sd.encode_character(save)
    assert sd.parse_character(data) == save
    equip = {r.id: r for r in tables["item_equip"].records}
    etc = {r.id: r for r in tables["item_etc"].records}
    weapon = save.equipped[1]
    assert equip[weapon.id].name == "Wooden Club"
    assert equip[weapon.id].slot == 1  # worn items are indexed by item_equip.slot
    stocked = [e for e in save.etc_inventory if not e.empty]
    assert [etc[e.id].name for e in stocked] == ["Red Potion", "Blue Potion"]
    assert all(etc[e.id].type == 1 for e in stocked)  # potions
    for s in save.shortcuts:
        assert s.kind == 0 or s.id in {e.id for e in stocked}
    assert (app / "map" / f"{save.area}{save.map}.map").is_file()
    assert (app / "map" / f"{save.area}.mapprj").is_file()


def test_skill_slots_fill_exactly_thirteen(tables):
    skills = tables["skill"].records
    for branch in (1, 2):
        slots = sd.skill_slots(branch, 4, skills)
        assert -1 not in slots and len(set(slots)) == 13
        assert all(skills[i].branch in (0, branch) for i in slots)
    assert sd.skill_slots(0, 1, skills)[:4] == [2, 3, 4, 5]
    assert sd.skill_slots(0, 0, skills) == [-1] * 13


def test_default_caption_matches_script_extra(app):
    import plistlib

    with open(app / "en.lproj" / "ScriptExtra.strings", "rb") as f:
        strings = plistlib.load(f)
    assert strings["CDataMgr.LoadSaveData.1"] == sd.DEFAULT_CAPTION
    assert strings["CDataMgr.SaveSaveData.1"] == sd.DEFAULT_CAPTION


def test_pulled_device_saves_round_trip():
    folder = os.environ.get("MAPLETHIEF_SAVE_DIR")
    if not folder:
        pytest.skip("MAPLETHIEF_SAVE_DIR not set (copy of a device's Documents/)")
    count = 0
    for path in sorted(Path(folder).iterdir()):
        try:
            sd.kind_of(path.name)
        except sd.SaveDataError:
            continue
        data = path.read_bytes()
        assert sd.encode_file(path.name, sd.parse_file(path.name, data)) == data, path.name
        count += 1
    assert count, f"no save files in {folder}"


def test_names_are_function_entries():
    exported = REPO / "work" / "ghidra" / "functions.json"
    if not exported.is_file():
        pytest.skip("work/ghidra/functions.json absent (run tools/ghidra/import)")
    entries = {f["address"] for f in json.loads(exported.read_text(encoding="utf-8"))["functions"]}
    for address, kind, name, _ in sd.NAMES:
        if kind == "function":
            assert address in entries, name
