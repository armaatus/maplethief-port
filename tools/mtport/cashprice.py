"""The offline cash shop's prices (#156, ADR 0072) from the shipped data tables.

With ``cash_shop = meso`` the port sells the cash shop's items and the death popup's revives for
the hero's meso (``runtime/src/cashshop/pricing.cpp``, ``maplethief/cashshop.h``). This module
derives the pricing's inputs from the game's own tables and prints the resulting price curve,
so that the knobs can be checked against the data and tuned in a playthrough:

* ``meso_per_exp``: the median meso a normal monster drops per EXP it gives (``mob.dat``; a
  drop is 80 % likely, of 80..120 % of the monster's meso), so ``exp_to_level x meso_per_exp``
  is what a hero earns over one level (``hero.dat``'s EXP column, ``CHero::ExpToLevelUp``).
* the resale floors: what the items a product grants sell back for (``item_etc.dat``'s price,
  the item's +0x9C): Initialize SP, SP Marble SP+1, and the revives' 30 Power Elixirs.

:func:`price` is the runtime's ``mt_cash_price`` in the same integer arithmetic; the tests
check the two agree (``tests/test_cashprice.py``, ``tests/cpp/cashshop_test.cpp``).

    python3 -m mtport.cashprice work/ipa/Payload/MapleThief.app/English/data
"""

from __future__ import annotations

import argparse
import statistics
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import datatables as dt

# The products in mt_cash_product order (maplethief/cashshop.h).
PRODUCTS: tuple[str, ...] = (
    "InitializeSP",
    "SPMarbleSP1Plus",
    "mesopack3",
    "mesopack4",
    "mesopack5",
    "mesopack6",
    "exppack1",
    "exppack2",
    "powpackage",
    "ComebackPack",
)
SOLD: tuple[str, ...] = tuple(p for p in PRODUCTS if not p.startswith("mesopack"))

MESO_DROP_CHANCE = 0.8  # mob.dat: a meso drop is 80 % likely (docs/formats/data-tables.md)
NORMAL_MOB = 1  # mob.dat unknown_24: 1 normal, 2 boss, 3 Malady Bomb
POWER_ELIXIRS = 30  # a POW! Package or a Comeback Pack: a revive and 30 Power Elixirs

# The items whose sell-back price floors a product, and how many of each it grants.
RESALE_ITEMS: dict[str, tuple[str, int]] = {
    "InitializeSP": ("Initialize SP", 1),
    "SPMarbleSP1Plus": ("SP Marble SP+1", 1),
    "powpackage": ("Power Elixir", POWER_ELIXIRS),
    "ComebackPack": ("Power Elixir", POWER_ELIXIRS),
}


@dataclass(frozen=True)
class Knobs:
    """mt_cash_knobs; the defaults are runtime/src/cashshop/pricing.cpp's k_defaults."""

    meso_per_exp_milli: int = 1640
    share_pct: dict[str, int] = field(
        default_factory=lambda: {
            "InitializeSP": 25,
            "SPMarbleSP1Plus": 125,
            "exppack1": 100,
            "exppack2": 200,
            "powpackage": 100,
            "ComebackPack": 200,
        }
    )
    resale: dict[str, int] = field(
        default_factory=lambda: {
            "InitializeSP": 10000,
            "SPMarbleSP1Plus": 5000,
            "powpackage": 9000,
            "ComebackPack": 9000,
        }
    )
    step_pct: int = 150
    max_steps: int = 4
    wealth_floor_pct: int = 10
    min_price: int = 100


DEFAULTS = Knobs()


def level_income(exp_to_level: int, knobs: Knobs = DEFAULTS) -> int:
    """mt_cash_level_income: the meso a hero earns over a level that needs this much EXP."""
    return max(exp_to_level, 0) * knobs.meso_per_exp_milli // 1000


def price(
    product: str, exp_to_level: int, meso: int = 0, steps: int = 0, knobs: Knobs = DEFAULTS
) -> int:
    """mt_cash_price: the product's price in meso, or -1 if it is not sold."""
    share = knobs.share_pct.get(product, 0)
    if share == 0:
        return -1
    milli = max(exp_to_level, 0) * knobs.meso_per_exp_milli * share // 100
    for _ in range(min(steps, knobs.max_steps)):
        milli = milli * knobs.step_pct // 100
    value = (milli + 999) // 1000
    value = max(value, knobs.resale.get(product, 0))
    value = max(value, max(meso, 0) * knobs.wealth_floor_pct // 100)
    value = max(value, knobs.min_price)
    return (value + 9) // 10 * 10


def normal_mobs(mobs: Iterable[Any]) -> list[Any]:
    """The monsters a hero farms: normal ones (no bosses) that give EXP."""
    return [m for m in mobs if m.unknown_24 == NORMAL_MOB and m.exp > 0]


def meso_per_exp(mobs: Iterable[Any]) -> float:
    """The median expected meso per EXP over the normal monsters."""
    farmed = normal_mobs(mobs)
    if not farmed:
        raise ValueError("no normal monster gives EXP")
    return statistics.median(m.meso / m.exp for m in farmed) * MESO_DROP_CHANCE


def resale_floors(items: Iterable[Any]) -> dict[str, int]:
    """What each floored product's items sell back for, by item_etc's (English) names."""
    by_name = {i.name: i.price for i in items}
    floors = {}
    for product, (name, count) in RESALE_ITEMS.items():
        if name not in by_name:
            raise ValueError(f"item_etc has no {name!r}")
        floors[product] = by_name[name] * count
    return floors


def anchor_level(levels: Sequence[Any], target: int, knobs: Knobs = DEFAULTS) -> int:
    """The level whose income is nearest `target` meso (ADR 0072's anchors: a POW! Package at
    39,000 = one level's income, a Comeback Pack at 79,000 = two)."""
    best = min(levels, key=lambda row: abs(level_income(row.exp, knobs) - target))
    return int(best.level)


def curve(levels: Sequence[Any], knobs: Knobs = DEFAULTS) -> list[dict[str, int]]:
    """One row per level: its EXP, income and every sold product's base price."""
    rows = []
    for row in levels:
        entry = {"level": row.level, "exp": row.exp, "income": level_income(row.exp, knobs)}
        for product in SOLD:
            entry[product] = price(product, row.exp, knobs=knobs)
        rows.append(entry)
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.cashprice", description=__doc__.splitlines()[0]
    )
    parser.add_argument("data_dir", help="a <Language>/data directory (English: item names)")
    parser.add_argument("--every", type=int, default=5, help="print every Nth level (5)")
    args = parser.parse_args(argv)
    try:
        tables = dt.load_dir(Path(args.data_dir))
        mobs = tables["mob"].records
        derived = meso_per_exp(mobs)
        floors = resale_floors(tables["item_etc"].records)
    except (OSError, dt.DataTableError, ValueError) as e:
        print(f"cashprice: {e}", file=sys.stderr)
        return 1
    levels = tables["hero"].records
    print(
        f"meso per EXP: {derived:.4f} over {len(normal_mobs(mobs))} normal monsters "
        f"(knob {DEFAULTS.meso_per_exp_milli / 1000:.3f})"
    )
    print("resale floors: " + ", ".join(f"{p} {v}" for p, v in floors.items()))
    print(
        f"a level's income nearest 39,000 (the POW! Package's anchor): level "
        f"{anchor_level(levels, 39000)}"
    )
    print("level    exp   income " + " ".join(f"{p[:12]:>12}" for p in SOLD))
    for row in curve(levels):
        if row["level"] % args.every == 0 or row["level"] in (1, 59):
            cells = " ".join(f"{row[p]:>12,}" for p in SOLD)
            print(f"{row['level']:>5} {row['exp']:>6} {row['income']:>8,} {cells}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
