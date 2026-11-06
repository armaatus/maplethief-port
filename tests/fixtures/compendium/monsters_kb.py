"""A synthetic knowledge base for the Monsters section tests (#218).

Every name and number is made up; nothing comes from the game. The shapes follow
docs/compendium/kb.md: a field monster with an equipment and two etc drops, a boss with its
boss map and a used boss event, a boss whose event no map runs, a bomb, and a monster no
map places.
"""

from __future__ import annotations


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _drop(item: str, chance: int, per: int, percent: float, slot: int) -> dict:
    return {
        "kind": "item",
        "id": item,
        "chance": chance,
        "per": per,
        "percent": percent,
        "nominal_percent": chance * 100 / per,
        "slot": slot,
    }


MONSTERS = [
    {
        "id": "1",
        "kind": "monster",
        "name": _n("Puddle Slug", "웅덩이민달팽이"),
        "level": 20,
        "hp": 300,
        "exp": 30,
        "meso": {"amount": 50, "min": 40, "max": 60, "percent": 80},
        "class": "normal",
        "boss_bar": False,
        "movement_kind": 1,
        "walk_speed": 2,
        "name_fills_field": True,
        "drops": [
            _drop("equip:1", 300, 100000, 0.9155, 0),
            _drop("etc:3", 930, 1000, 93.1641, 1),
            _drop("etc:4", 40, 1000, 4.0283, 2),
        ],
        "meso_drops": [{"kind": "item", "id": "etc:0", "min": 40, "max": 60, "percent": 80}],
        "spawns_on": [
            {"kind": "map", "id": "1-1", "count": 4},
            {"kind": "map", "id": "3-7", "count": 2},
        ],
        "hunted_by_quests": [{"kind": "quest", "id": "0-main-5", "count": 3}],
    },
    {
        "id": "9",
        "kind": "monster",
        "name": _n("Puddle Slug", "웅덩이민달팽이"),
        "level": 70,
        "hp": 3000,
        "exp": 80,
        "meso": None,
        "class": "normal",
        "boss_bar": False,
        "movement_kind": 1,
        "walk_speed": 3,
        "drops": [],
    },
    {
        "id": "64",
        "kind": "monster",
        "name": _n("Grim Lantern", "음산한등불"),
        "level": 60,
        "hp": 15000,
        "exp": 2000,
        "meso": {"amount": 5000, "min": 4000, "max": 6000, "percent": 80},
        "class": "boss",
        "boss_bar": True,
        "movement_kind": 2,
        "walk_speed": 4,
        "drops": [_drop("etc:4", 30, 1000, 3.0212, 3)],
        "meso_drops": [{"kind": "item", "id": "etc:0", "min": 4000, "max": 6000, "percent": 80}],
        "spawns_on": [{"kind": "map", "id": "3-8", "count": 1}],
        "hunted_by_quests": [{"kind": "quest", "id": "1-main-18", "count": 1}],
    },
    {
        "id": "65",
        "kind": "monster",
        "name": _n("Lantern Spark", "등불불꽃"),
        "level": 25,
        "hp": 1,
        "exp": 0,
        "meso": None,
        "class": "bomb",
        "boss_bar": True,
        "movement_kind": 3,
        "walk_speed": 0,
        "drops": [],
    },
    {
        "id": "68",
        "kind": "monster",
        "name": _n("Frost Hulk", "서리거인"),
        "level": 121,
        "hp": 32000,
        "exp": 5000,
        "meso": None,
        "class": "normal",
        "boss_bar": True,
        "movement_kind": 1,
        "walk_speed": 3,
        "drops": [],
        "spawns_on": [{"kind": "map", "id": "9-5", "count": 1}],
        "hunted_by_quests": [{"kind": "quest", "id": "2-main-8", "count": 1}],
    },
]

ITEMS = [
    {"id": "etc:0", "kind": "item", "name": _n("Coin Pouch", "동전주머니")},
    {"id": "equip:1", "kind": "item", "name": _n("Tin Dagger", "양철단검")},
    {
        "id": "etc:3",
        "kind": "item",
        "name": _n("Slug Shell", "민달팽이껍질"),
        "needed_by_quests": [{"kind": "quest", "id": "1-sub-3", "count": 8}],
        "material_of": [{"kind": "recipe", "id": "1-equip:1", "count": 3}],
        "traded_away_in": [{"kind": "trade", "id": "1-etc:3-etc:4", "count": 50}],
    },
    {"id": "etc:4", "kind": "item", "name": _n("Murky Tonic", "탁한물약")},
]

MAPS = [
    {
        "id": "1-1",
        "kind": "map",
        "name": _n("Hollow Edge", "골짜기끝"),
        "area": [{"kind": "area", "id": "1"}],
    },
    {
        "id": "3-7",
        "kind": "map",
        "name": _n("Shrine Steps", "신전계단"),
        "area": [{"kind": "area", "id": "3"}],
        "spawns": [{"kind": "monster", "id": "1", "count": 2}],
    },
    {
        "id": "3-8",
        "kind": "map",
        "name": _n("Lantern Shrine", "등불신전"),
        "area": [{"kind": "area", "id": "3"}],
        "boss_map": True,
        "entrances": [{"kind": "map", "id": "3-7", "portal": 1}],
    },
    {
        "id": "9-5",
        "kind": "map",
        "name": _n("Frost Hollow", "서리골"),
        "area": [{"kind": "area", "id": "9"}],
        "boss_map": True,
        "entrances": [],
    },
]

AREAS = [
    {
        "id": "1",
        "kind": "area",
        "name": _n("Mossy Hollow", "이끼골"),
        "maps": [{"kind": "map", "id": "1-1"}],
    },
    {
        "id": "3",
        "kind": "area",
        "name": _n("Deep Burrow", "깊은굴"),
        "maps": [{"kind": "map", "id": "3-7"}, {"kind": "map", "id": "3-8"}],
    },
    {
        "id": "9",
        "kind": "area",
        "name": _n("Frostfell", "서리땅"),
        "maps": [{"kind": "map", "id": "9-5"}],
    },
]

GIVER = [{"kind": "npc", "id": "7", "map": "1-1", "script": "x"}]

QUESTS = [
    {
        "id": "0-main-5",
        "kind": "quest",
        "name": _n("First Hunt", "첫사냥"),
        "chapter": 0,
        "quest_kind": "main",
        "given_by": GIVER,
        "rewards": [{"type": "exp", "amount": 30}],
    },
    {
        "id": "1-sub-3",
        "kind": "quest",
        "name": _n("Shell Collector", "껍질수집가"),
        "chapter": 1,
        "quest_kind": "sub",
        "given_by": GIVER,
        "rewards": [],
    },
    {
        "id": "1-main-18",
        "kind": "quest",
        "name": _n("Put Out the Lantern", "등불을꺼라"),
        "chapter": 1,
        "quest_kind": "main",
        "given_by": GIVER,
        "rewards": [
            {"type": "exp", "amount": 1000},
            {"type": "meso", "amount": 2500},
            {"type": "item", "item": "etc:4", "count": 2},
        ],
    },
    {
        "id": "2-main-8",
        "kind": "quest",
        "name": _n("Thaw the Hollow", "골짜기녹이기"),
        "chapter": 2,
        "quest_kind": "main",
        "given_by": GIVER,
        "rewards": [{"type": "exp", "amount": 5000}],
    },
]


def _require(kind: int, number: int) -> list:
    return [[{"native": "checkRequire", "args": [kind, number], "op": "==", "value": 0}]]


EVENTS = [
    {
        "id": "chapter1-LanternMap-boss",
        "kind": "event",
        "name": _n("chapter1/LanternMap: boss", "chapter1/LanternMap: 보스"),
        "type": "boss",
        "used": True,
        "chapters": [1],
        "conditions": _require(0, 18),
        "maps": [{"kind": "map", "id": "3-8"}],
    },
    {
        "id": "chapter2-FrostMap-boss",
        "kind": "event",
        "name": _n("chapter2/FrostMap: boss", "chapter2/FrostMap: 보스"),
        "type": "boss",
        "used": False,
        "chapters": [],
        "conditions": _require(0, 8),
        "maps": [],
    },
    {
        "id": "common-Tickets-carnival-0",
        "kind": "event",
        "name": _n("common/Tickets: carnival 0", "common/Tickets: 카니발 0"),
        "type": "carnival",
        "used": False,
        "chapters": [],
        "conditions": [],
        "maps": [],
    },
]

LEVELS = [
    {
        "id": str(lv),
        "kind": "level",
        "name": _n(f"Level {lv}", f"레벨 {lv}"),
        "level": lv,
        "attack": a,
        "defense": df,
        "accuracy": acc,
        "avoidability": av,
    }
    for lv, a, df, acc, av in (
        (20, 26, 27, 25, 22),
        (40, 51, 51, 49, 42),
        (60, 76, 76, 73, 62),
        (25, 32, 33, 31, 27),
        (70, 88, 88, 85, 72),
        (99, 125, 123, 121, 102),
    )
]

KB = {
    "monster": MONSTERS,
    "item": ITEMS,
    "map": MAPS,
    "area": AREAS,
    "quest": QUESTS,
    "event": EVENTS,
    "level": LEVELS,
    "npc": [{"id": "7", "kind": "npc", "name": _n("Old Fen", "늙은펜")}],
    "recipe": [
        {
            "id": "1-equip:1",
            "kind": "recipe",
            "name": _n("Tin Dagger", "양철단검"),
            "crafted_by": [{"kind": "npc", "id": "7", "map": "1-1"}],
        }
    ],
    "trade": [
        {
            "id": "1-etc:3-etc:4",
            "kind": "trade",
            "name": _n("Murky Tonic", "탁한물약"),
            "offered_by": [{"kind": "npc", "id": "7", "map": "1-1"}],
        }
    ],
}

PROSE = {
    "monster/1.md": "{{monster:1}} is a made-up monster of {{area:1}} for the tests.\n",
}
