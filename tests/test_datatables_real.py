"""Real-asset checks for the data table parser against docs/formats/data-tables.md.

Parses every shipped ``<Language>/data/*.dat``. Skipped when work/ipa/ is absent (CI); run
scripts/bootstrap first.
"""

import re

import pytest

from mtport import datatables as dt

pytestmark = pytest.mark.real_assets

HANGUL = re.compile("[가-힣]")
ROWS = {
    "hero": 100,
    "mob": 69,
    "npc": 39,
    "item_equip": 74,
    "item_etc": 138,
    "skill": 24,
    "market": 220,
}


def data_dir(ipa_dir, language):
    return ipa_dir / "Payload" / "MapleThief.app" / language / "data"


@pytest.fixture(scope="module")
def tables(ipa_dir):
    return {lang: dt.load_dir(data_dir(ipa_dir, lang)) for lang in dt.LANGUAGES}


def test_every_file_parses_with_no_leftover_bytes(ipa_dir, tables):
    # parse() rejects any file whose size is not exactly 6 + rows * stride.
    count = 0
    for lang in dt.LANGUAGES:
        files = sorted(p.stem for p in data_dir(ipa_dir, lang).glob("*.dat"))
        assert files == sorted([*dt.TABLES, "buildup"])
        for name, table in tables[lang].items():
            assert len(table.records) == ROWS[name], (lang, name)
            assert 6 + len(table.records) * table.stride == (
                (data_dir(ipa_dir, lang) / f"{name}.dat").stat().st_size
            )
            count += 1
        assert dt.parse_buildup((data_dir(ipa_dir, lang) / "buildup.dat").read_bytes()) == 1
        count += 1
    assert count == 24


def test_japanese_is_a_copy_of_english(ipa_dir):
    for name in [*dt.TABLES, "buildup"]:
        en = (data_dir(ipa_dir, "English") / f"{name}.dat").read_bytes()
        assert (data_dir(ipa_dir, "Japanese") / f"{name}.dat").read_bytes() == en, name


def test_english_and_korean_agree_on_everything_but_text(tables):
    for name, spec in dt.TABLES.items():
        en, ko = tables["English"][name], tables["Korean"][name]
        assert len(en.records) == len(ko.records), name
        numeric = [c.name for c in dt.layout(spec.record) if c.kind != "str"]
        for a, b in zip(en.records, ko.records, strict=True):
            assert [getattr(a, f) for f in numeric] == [getattr(b, f) for f in numeric], name


def test_korean_text_is_valid_hangul(tables):
    strings = 0
    for name, spec in dt.TABLES.items():
        text_cols = [c for c in dt.layout(spec.record) if c.kind == "str"]
        for record in tables["Korean"][name].records:
            for c in text_cols:
                value = getattr(record, c.name)
                assert "�" not in value
                value.encode("euc_kr")  # the game decodes with kCFStringEncodingEUC_KR
                if value:
                    assert HANGUL.search(value), (name, c.name)
                    strings += 1
    assert strings == 69 + 39 + 74 + 2 * 138 + 3 * 24 - 4  # 4 skill texts are empty


def test_english_text_is_ascii(tables):
    for name, spec in dt.TABLES.items():
        for record in tables["English"][name].records:
            for c in dt.layout(spec.record):
                if c.kind == "str":
                    assert getattr(record, c.name).isascii()


def test_lookup_keys_are_unique(tables):
    # The game finds rows by the first u8 column (0x3990D returns the first match).
    for name in ("hero", "mob", "npc", "item_equip", "item_etc", "skill"):
        keys = [dt.dataclasses.astuple(r)[0] for r in tables["English"][name].records]
        assert len(set(keys)) == len(keys), name


def test_cross_references_resolve(tables):
    en = tables["English"]
    equip = {r.id for r in en["item_equip"].records}
    etc = {r.id for r in en["item_etc"].records}
    for mob in en["mob"].records:
        if mob.equip_drop_chance:
            assert mob.equip_drop in equip
        for item, chance in (
            (mob.etc_drop_1, mob.etc_drop_1_chance),
            (mob.etc_drop_2, mob.etc_drop_2_chance),
            (mob.etc_drop_3, mob.etc_drop_3_chance),
        ):
            if chance:
                assert item in etc
    for listing in en["market"].records:
        if listing.price:
            assert listing.item in (equip if listing.table == 0 else etc)
    assert [h.level for h in en["hero"].records] == list(range(1, 101))
