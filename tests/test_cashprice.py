"""Unit tests for the cash shop's price curve (#156, ADR 0072), synthetic data only."""

import re
from types import SimpleNamespace

import pytest

from mtport import cashprice as cp
from repo_paths import REPO

PRICING_CPP = REPO / "runtime" / "src" / "cashshop" / "pricing.cpp"


def mob(exp, meso, kind=cp.NORMAL_MOB):
    return SimpleNamespace(exp=exp, meso=meso, unknown_24=kind)


def test_the_knobs_are_the_runtimes_defaults():
    # k_defaults in pricing.cpp: meso_per_exp_milli, shares, resale, step, max steps, wealth
    # floor, min price, in mt_cash_knobs order.
    text = PRICING_CPP.read_text(encoding="utf-8")
    block = re.search(r"constexpr mt_cash_knobs k_defaults = \{(.*?)\n\};", text, re.S)
    assert block is not None
    numbers = [int(n) for n in re.findall(r"\d+", block.group(1))]
    n = len(cp.PRODUCTS)
    assert len(numbers) == 1 + 2 * n + 4
    d = cp.DEFAULTS
    assert numbers[0] == d.meso_per_exp_milli
    assert numbers[1 : 1 + n] == [d.share_pct.get(p, 0) for p in cp.PRODUCTS]
    assert numbers[1 + n : 1 + 2 * n] == [d.resale.get(p, 0) for p in cp.PRODUCTS]
    assert numbers[1 + 2 * n :] == [d.step_pct, d.max_steps, d.wealth_floor_pct, d.min_price]


def test_price_is_mt_cash_price():
    # The same numbers as tests/cpp/cashshop_test.cpp's table.
    assert cp.level_income(23200) == 38048
    assert cp.level_income(-5) == 0
    assert cp.price("powpackage", 24060) == 39460
    assert cp.price("ComebackPack", 24060) == 78920
    assert cp.price("SPMarbleSP1Plus", 24060) == 49330
    assert cp.price("InitializeSP", 24060) == 10000
    assert cp.price("exppack1", 10) == 100
    assert cp.price("exppack1", 200) == 330
    assert cp.price("exppack1", 71740) == 117660
    assert cp.price("SPMarbleSP1Plus", 5350) == 10970
    assert cp.price("mesopack6", 24060) == -1
    assert cp.price("nothing", 24060) == -1
    steps = [cp.price("powpackage", 23200, steps=s) for s in range(7)]
    assert steps == [38050, 57080, 85610, 128420, 192620, 192620, 192620]
    assert cp.price("powpackage", 23200, meso=1_000_000) == 100000
    assert cp.price("exppack1", 200, meso=-50) == 330


def test_meso_per_exp_is_the_median_over_normal_mobs_times_the_drop_chance():
    mobs = [mob(10, 20), mob(10, 10), mob(10, 30), mob(100, 9999, kind=2), mob(0, 50)]
    assert cp.meso_per_exp(mobs) == pytest.approx(2.0 * 0.8)
    assert len(cp.normal_mobs(mobs)) == 3
    with pytest.raises(ValueError):
        cp.meso_per_exp([mob(10, 10, kind=2)])


def test_resale_floors_from_item_prices():
    items = [
        SimpleNamespace(name="Initialize SP", price=10000),
        SimpleNamespace(name="SP Marble SP+1", price=5000),
        SimpleNamespace(name="Power Elixir", price=300),
        SimpleNamespace(name="Red Potion", price=5),
    ]
    assert cp.resale_floors(items) == {
        "InitializeSP": 10000,
        "SPMarbleSP1Plus": 5000,
        "powpackage": 9000,
        "ComebackPack": 9000,
    }
    with pytest.raises(ValueError):
        cp.resale_floors(items[:2])


def test_curve_and_anchor_level():
    levels = [SimpleNamespace(level=lv, exp=e) for lv, e in ((1, 10), (58, 23200), (59, 24060))]
    rows = cp.curve(levels)
    assert [r["level"] for r in rows] == [1, 58, 59]
    assert rows[2]["income"] == 39458
    assert rows[2]["powpackage"] == 39460
    assert set(cp.SOLD) <= set(rows[0])
    assert "mesopack3" not in rows[0]
    assert cp.anchor_level(levels, 39000) == 59
    assert cp.anchor_level(levels, 0) == 1
