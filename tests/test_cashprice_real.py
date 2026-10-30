"""Real-asset checks of the cash shop's pricing knobs (#156, ADR 0072) against the shipped tables.

The defaults in runtime/src/cashshop/pricing.cpp (mirrored by mtport.cashprice.DEFAULTS) are
derived from mob.dat, hero.dat and item_etc.dat; this checks they still are. Skipped when
work/ipa/ is absent (CI); run scripts/bootstrap first.
"""

import pytest

from mtport import cashprice as cp
from mtport import datatables as dt

pytestmark = pytest.mark.real_assets


@pytest.fixture(scope="module")
def data_dir(ipa_dir):
    return ipa_dir / "Payload" / "MapleThief.app" / "English" / "data"


@pytest.fixture(scope="module")
def tables(data_dir):
    return dt.load_dir(data_dir)


def test_meso_per_exp_knob_matches_mob_dat(tables):
    farmed = cp.normal_mobs(tables["mob"].records)
    assert len(farmed) == 66
    derived = cp.meso_per_exp(tables["mob"].records)
    assert derived == pytest.approx(2.0485 * 0.8, abs=1e-3)
    assert round(derived * 100) * 10 == cp.DEFAULTS.meso_per_exp_milli


def test_resale_floors_match_item_etc(tables):
    assert cp.resale_floors(tables["item_etc"].records) == cp.DEFAULTS.resale


def test_the_anchors_sit_at_level_59(tables):
    levels = tables["hero"].records
    assert cp.anchor_level(levels, 39000) == 59
    exp59 = next(r.exp for r in levels if r.level == 59)
    # ADR 0072: POW! Package about 39,000, Comeback Pack about 79,000 at the reference level.
    assert abs(cp.price("powpackage", exp59) - 39000) < 1000
    assert abs(cp.price("ComebackPack", exp59) - 79000) < 1000
    # The curve never falls with the level.
    rows = cp.curve(levels)
    for product in cp.SOLD:
        prices = [r[product] for r in rows]
        assert prices == sorted(prices), product


def test_the_command_prints_the_curve(data_dir, capsys):
    assert cp.main([str(data_dir), "--every", "10"]) == 0
    out = capsys.readouterr().out
    assert "over 66 normal monsters" in out
    assert "level 59" in out
