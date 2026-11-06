"""Ghidra names for the game's rules: damage, hit and miss, drops, items, skills (#214).

``docs/re/mechanics.md`` has the evidence and the rules in words. The fight functions that
other notes already name keep those names: ``CMob::TakeHit`` and ``CMob::Die``
(``mtport.hudextras``), ``CHero::TakeDamage``, ``CHero::LevelUp``, ``CHero::setExp``,
``CHero::Die``, ``CHero::isSpear``, ``CSkill::setActiveSkill`` and
``CStateGame::resultMonCa`` (``mtport.flow``), ``CHero::UseItem`` (``mtport.autosave``) and
the four ``Rand_*`` functions (``mtport.timing``).

Command line::

    python3 -m mtport.mechanics names [OUT]            # work/re/names/mechanics.json
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/mechanics.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    # Damage, evade, miss, critical, avoid.
    (
        "0x64DA1",
        "function",
        "CHero::RollDamage",
        "(hero, skill, star): x = A*(A/30+3)/5 (+star ATT), skill % and constant, "
        "(x + Rand_Range(-x/3, x/5)) * 110/100; a claw punch is 1",
    ),
    ("0x6039D", "function", "CHero::RollCritical", "Rand_Percent(hero +0xB4)"),
    (
        "0x603DD",
        "function",
        "CHero::RollAvoid",
        "Rand_Percent((avoidability +0xAC - mob level) * 4) on contact",
    ),
    (
        "0x72639",
        "function",
        "CMob::RollEvade",
        "Rand_Percent(mob level +0x98 - hero accuracy +0xB0); MISS",
    ),
    (
        "0x6F591",
        "function",
        "CMob::RollDamage",
        "d = L*L/30 + 2L + 20 - 7D/3; d + Rand_Range(-2-d/4, d/4+2) (d < 0: 1..L/2), * 90/100",
    ),
    (
        "0x73B51",
        "function",
        "CMob::LoadStats",
        "mob.dat hp, level, speed +0x20, movement kind +0xAC; Monster Carnival level scaling",
    ),
    # Drops.
    (
        "0x5CAD9",
        "function",
        "RollMobDrops",
        "CMob::Die: DropEquip, DropEtc, DropMeso (not in Monster Carnival)",
    ),
    (
        "0x5C8ED",
        "function",
        "DropEquip",
        "Rand_Chance(hero level + chance, 100000), really /32768; fixed-stat item",
    ),
    (
        "0x5C699",
        "function",
        "DropEtc",
        "3 slots, each Rand_Chance(hero level/10 + chance, 1000)",
    ),
    (
        "0x5C3C1",
        "function",
        "DropMeso",
        "Rand_Range(m*8/10, m*12/10), 80 %; bag item_etc 0 (<100), 1 (<200), 2",
    ),
    ("0x5C201", "function", "SpreadDrops", "places a death's drops side by side"),
    # The hero's stats and items.
    ("0x63C3D", "function", "CHero::MaxHpFromTable", "sum of hero.dat 0x05 over levels 1..L"),
    ("0x63BB9", "function", "CHero::MaxMpFromTable", "sum of hero.dat 0x07 over levels 1..L"),
    ("0x5F89D", "function", "CHero::AddEquipStats", "item's six stats onto the hero; speed +0x160"),
    ("0x5F979", "function", "CHero::RemoveEquipStats", "the reverse; HP/MP clamped to the max"),
    ("0x5F181", "function", "CHero::AddHP", "HP + n, capped at max HP"),
    ("0x6140D", "function", "CHero::Revive", "full HP and MP"),
    (
        "0x67005",
        "function",
        "CHero::UseStars",
        "takes n stars from the first stack that holds n; empty stacks stay",
    ),
    (
        "0x691E5",
        "function",
        "ScrollTargetSlot",
        "scroll item_etc id -> equipped slot (19/134 weapon, 20/133 overall, ...)",
    ),
    (
        "0x69511",
        "function",
        "CItem::ApplyScroll",
        "uses a slot; success % from [rolled*7 + scrolls_used]; type 8 always",
    ),
    ("0x69145", "function", "CItem::AddScrollStats", "a scroll's six stats onto the item"),
    (
        "0x69B5D",
        "function",
        "CItem::InitEquipFixed",
        "item_equip row: table minimums, 5 slots (shops, drops, getItem)",
    ),
    (
        "0x69BB9",
        "function",
        "CItem::InitEquipRolled",
        "each stat Rand_Range(min, max) + bonus_value, 7 slots, rolled = 1",
    ),
    ("0x699A1", "function", "CItem::InitEtc", "item_etc row"),
    (
        "0x7C2D9",
        "function",
        "Quest_MakeEquipItem",
        "script makeEquipItem: takes the quest's items and meso, gives rolled equips",
    ),
    ("0x7B5A9", "function", "Quest_CountKill", "a mob kill counts toward hunting quests"),
    # Skills.
    ("0x7D251", "function", "CSkill::MpCost", "mp_base + (lvl-1) * mp_per_level"),
    ("0x7D325", "function", "CSkill::EtcValue", "(etc_base + (lvl-1) * etc_per_level) / 10"),
    ("0x7D551", "function", "CSkill::DamageValue", "damage_base + (lvl-1) * damage_per_level"),
    ("0x7EC3D", "function", "CSkill::Duration", "buffs: time_base + (lvl-1) * per, ticks"),
    (
        "0x7D781",
        "function",
        "CSkill::SpawnAttack",
        "the hit object per action: boxes, star counts, targets",
    ),
    ("0x7F749", "function", "CSkill::CastBuff", "pays MP; recast refreshes the timer"),
    ("0x7F669", "function", "CSkill::TickBuffs", "counts the buffs at +0xD64 down"),
    ("0x7F581", "function", "CSkill::EndBuff", "removes a buff and its stats"),
    ("0x7ED51", "function", "CSkill::IsBuffEnding", "the last 30 ticks: the icon blinks"),
    ("0x7F99D", "function", "CSkill::LearnLevels", "passives' stats when points are learned"),
    ("0x7D4A5", "function", "CSkill::Drain", "heal dmg * @etc / 100, capped"),
    ("0x7EB35", "function", "CSkill::Pickpocket", "Rand_Percent(@etc): meso m/18-5..m/18"),
    ("0x7EAB1", "function", "CSkill::MesoGuard", "pays (dmg/2) * @etc / 100 meso to halve"),
    ("0x7EB8D", "function", "CSkill::ShadowWeb", "binds up to 6 mobs for @etc ticks"),
    # Monster Carnival.
    (
        "0x82271",
        "function",
        "CStateGame::StartMonsterCarnival",
        "initMonCa: +0x404 on, mode +0x40C, 180 s at +0x430",
    ),
)


def names_json() -> str:
    return gc.names_json("mechanics", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.mechanics", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    args = parser.parse_args(argv)
    try:
        out = Path(args.out)
        gc.write_text(out, names_json())
        print(f"wrote {out} ({len(NAMES)} names)")
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
