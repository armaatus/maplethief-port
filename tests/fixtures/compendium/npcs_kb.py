"""A synthetic knowledge base for the NPCs and shops section (#220).

Every name and number is made up; nothing comes from the game. It has a town with two
keepers, a field one exit away, a map nothing leads to, a crafter with two recipes, a trader,
a decoration-only NPC, an NPC on no map, an opened and an unopened shop, and a listing marked
as a data slip in the middle of another shop's rows.
"""

from __future__ import annotations

import copy


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _q(term: str, op: str, value: int) -> dict:
    return {"term": term, "op": op, "value": value}


def _call(name: str, *args) -> dict:
    return {"call": name, "args": list(args)}


SMITH_NODES = {
    "n1": {
        "function": "makeIt",
        "answer": 0,
        "answers": [0, 1],
        "paths": [
            {
                "if": [[_q("checkRequire(1, 77)", "==", 1)]],
                "function": "makeIt",
                "do": [_call("startQuest", 1, 77), _call("makeEquipItem", 1, 77)],
                "box": 0,
                "next": None,
                "loop": None,
            }
        ],
    },
    "n2": {
        "function": "pickItem",
        "answer": 1,
        "answers": [1, 2],
        "paths": [
            {
                "if": [[_q("answer#1", "==", 1)]],
                "function": "pickItem",
                "do": [
                    _call("setQuest", 1, 77, "K.title", "K.desc"),
                    _call("regRequireItem", 1, 3, 2),
                    _call("regRequireMeso", 50),
                    _call("regRewardItem", 0, 1, 1),
                ],
                "box": 1,
                "next": "n1",
                "loop": None,
            },
            {
                "if": [[_q("answer#1", "==", 2)]],
                "function": "pickItem",
                "do": [
                    _call("setQuest", 1, 77, "K.title", "K.desc"),
                    _call("regRequireItem", 1, 3, 5),
                    _call("regRequireMeso", 90),
                    _call("regRewardItem", 0, 2, 1),
                ],
                "box": 1,
                "next": "n1",
                "loop": None,
            },
        ],
    },
    "n3": {
        "function": "main",
        "answer": None,
        "answers": None,
        "paths": [
            {
                "if": [
                    [_q("getQuestState(0, 5)", ">=", 1), _q("getQuestState(1, 2)", "==", 2)],
                    [_q("getQuestState(0, 5)", ">=", 1), _q("getQuestState(1, 2)", "!=", 2)],
                ],
                "function": "main",
                "do": [],
                "box": 2,
                "next": "n2",
                "loop": None,
            },
            {
                "if": [[_q("getQuestState(0, 5)", "<", 1)]],
                "function": "main",
                "do": [{"npc": "K.busy"}],
                "box": 0,
                "next": None,
                "loop": None,
            },
        ],
    },
}

TRADER_NODES = {
    "t1": {
        "function": "main",
        "answer": None,
        "answers": None,
        "paths": [
            {
                "if": [[]],
                "function": "main",
                "do": [
                    _call("setQuest", 1, 78, "K.trade", "K.trade2"),
                    _call("regRequireItem", 1, 3, 50),
                    _call("regRewardItem", 1, 4, 3),
                ],
                "box": 0,
                "next": None,
                "loop": None,
            }
        ],
    }
}


def _npc(nid: str, en: str, ko: str, **rel) -> dict:
    base = {
        "id": nid,
        "kind": "npc",
        "name": _n(en, ko),
        "sprite": int(nid) + 4,
        "stands_on": [],
        "opens_shop": [],
        "gives_quests": [],
        "completes_quests": [],
        "crafts": [],
        "trades": [],
        "dialogues": [],
        "advancements": [],
    }
    base.update(rel)
    return base


def _listing(row: int, shop: str, item: str, price: int, note: str | None = None) -> dict:
    return {
        "id": str(row),
        "kind": "listing",
        "name": _n(f"Ware {row}", f"물건 {row}"),
        "row": row,
        "price": price,
        "note": note,
        "item": [{"kind": "item", "id": item, "price": price}],
        "shop": [{"kind": "shop", "id": shop, "price": price}],
    }


def _script(sid: str, base: str, chapters: list[int], maps: list[tuple[str, str]]) -> dict:
    return {
        "id": sid,
        "kind": "script",
        "name": _n(sid, sid),
        "base_name": base,
        "chapters": chapters,
        "used": True,
        "maps": [
            {"kind": "map", "id": m, "npc": n, "role": "npc", "portal": None} for m, n in maps
        ],
    }


KB = {
    "area": [
        {"id": "1", "kind": "area", "name": _n("Fernvale", "펀베일")},
        {"id": "2", "kind": "area", "name": _n("Fernvale", "펀베일")},
    ],
    "map": [
        {
            "id": "1-0",
            "kind": "map",
            "name": _n("Fernvale Square", "펀베일광장"),
            "area": [{"kind": "area", "id": "1"}],
            "exits": [{"kind": "map", "id": "1-1", "portal": 0, "gated": False}],
            "entrances": [{"kind": "map", "id": "1-1"}],
        },
        {
            "id": "1-1",
            "kind": "map",
            "name": _n("Mossy Path", "이끼길"),
            "area": [{"kind": "area", "id": "1"}],
            "exits": [{"kind": "map", "id": "1-0", "portal": 0, "gated": True}],
            "entrances": [{"kind": "map", "id": "1-0"}],
        },
        {
            "id": "2-3",
            "kind": "map",
            "name": _n("Fernvale Square", "펀베일광장"),
            "area": [{"kind": "area", "id": "2"}],
            "exits": [],
            "entrances": [],
        },
    ],
    "item": [
        {
            "id": "equip:1",
            "kind": "item",
            "name": _n("Twig Knife", "잔가지칼"),
            "table": "equip",
            "category": "weapon",
            "weapon_type": "dagger",
            "req_level": 10,
            "price": 5,
        },
        {
            "id": "equip:2",
            "kind": "item",
            "name": _n("Reed Hat", "갈대모자"),
            "table": "equip",
            "category": "hat",
            "req_level": 15,
            "price": 6,
        },
        {
            "id": "etc:3",
            "kind": "item",
            "name": _n("Pebble", "조약돌"),
            "table": "etc",
            "category": "etc",
            "price": 1,
        },
        {
            "id": "etc:4",
            "kind": "item",
            "name": _n("Shard", "조각"),
            "table": "etc",
            "category": "etc",
            "price": 2,
        },
    ],
    "npc": [
        _npc(
            "1",
            "Tinker Bo",
            "땜장이보",
            stands_on=[{"kind": "map", "id": "1-0", "script": "Shop10", "tile": [3, 4]}],
            opens_shop=[
                {
                    "kind": "shop",
                    "id": "2",
                    "map": "1-0",
                    "script": "common-Shop10",
                    "chapters": [1, 2],
                }
            ],
            dialogues=[{"kind": "dialogue", "id": "common-Shop10", "map": "1-0"}],
        ),
        _npc(
            "2",
            "Smith Ada",
            "대장장이아다",
            stands_on=[{"kind": "map", "id": "1-1", "script": "forge", "tile": [7, 2]}],
            crafts=[
                {"kind": "recipe", "id": "1-equip:1", "map": "1-1"},
                {"kind": "recipe", "id": "1-equip:2", "map": "1-1"},
            ],
            gives_quests=[
                {"kind": "quest", "id": "1-main-5", "map": "1-1", "script": "chapter1-forge"}
            ],
            completes_quests=[
                {"kind": "quest", "id": "1-main-5", "map": "1-1", "script": "chapter1-forge"}
            ],
            dialogues=[{"kind": "dialogue", "id": "chapter1-forge", "map": "1-1"}],
        ),
        _npc(
            "3",
            "Pedlar Ru",
            "행상루",
            stands_on=[{"kind": "map", "id": "1-0", "script": "barter", "tile": [9, 4]}],
            trades=[{"kind": "trade", "id": "1-etc:3-etc:4", "map": "1-0"}],
            dialogues=[{"kind": "dialogue", "id": "chapter1-barter", "map": "1-0"}],
        ),
        _npc(
            "4",
            "Stone Cat",
            "돌고양이",
            stands_on=[{"kind": "map", "id": "2-3", "script": None, "tile": [1, 1]}],
        ),
        _npc("5", "Lost Owl", "길잃은부엉이"),
    ],
    "shop": [
        {
            "id": "2",
            "kind": "shop",
            "name": _n("Shop 2", "상점 2"),
            "unopened_reason": None,
            "opened_by": [
                {
                    "kind": "npc",
                    "id": "1",
                    "map": "1-0",
                    "script": "common-Shop10",
                    "chapters": [1, 2],
                }
            ],
            "listings": [{"kind": "listing", "id": str(r), "price": 10} for r in (0, 2)],
            "sells": [],
        },
        {
            "id": "0",
            "kind": "shop",
            "name": _n("Shop 0", "상점 0"),
            "unopened_reason": "no script opens it: showMarket is the only way to the shop screen",
            "opened_by": [],
            "listings": [{"kind": "listing", "id": str(r), "price": 10} for r in (1, 3)],
            "sells": [],
        },
    ],
    "listing": [
        _listing(0, "2", "equip:1", 120),
        _listing(1, "0", "equip:2", 1500, note="a probable data slip"),
        _listing(2, "2", "equip:2", 140),
        _listing(3, "0", "equip:1", 99),
    ],
    "script": [
        _script("common-Shop10", "Shop10", [1, 2], [("1-0", "1")]),
        _script("chapter1-forge", "forge", [1], [("1-1", "2")]),
        _script("chapter1-barter", "barter", [1], [("1-0", "3")]),
    ],
    "dialogue": [
        {
            "id": "common-Shop10",
            "kind": "dialogue",
            "name": _n("Tinker Bo (common)", "땜장이보 (공통)"),
            "chapters": [1, 2],
            "tree": None,
            "nodes": {},
            "state": [],
            "npcs": [{"kind": "npc", "id": "1", "map": "1-0"}],
        },
        {
            "id": "chapter1-forge",
            "kind": "dialogue",
            "name": _n("Smith Ada (chapter 1)", "대장장이아다 (1장)"),
            "chapters": [1],
            "tree": "n3",
            "nodes": SMITH_NODES,
            "state": [
                {
                    "if": [
                        [_q("getQuestState(0, 5)", "==", 1)],
                        [_q("getQuestState(0, 5)", "==", 2)],
                    ],
                    "hide": 1,
                    "notify": 2,
                },
                {"if": [[_q("getQuestState(0, 5)", "==", 0)]], "hide": 0, "notify": -1},
            ],
            "npcs": [{"kind": "npc", "id": "2", "map": "1-1"}],
        },
        {
            "id": "chapter1-barter",
            "kind": "dialogue",
            "name": _n("Pedlar Ru (chapter 1)", "행상루 (1장)"),
            "chapters": [1],
            "tree": "t1",
            "nodes": TRADER_NODES,
            "state": [],
            "npcs": [{"kind": "npc", "id": "3", "map": "1-0"}],
        },
    ],
    "quest": [
        {
            "id": "1-main-5",
            "kind": "quest",
            "name": _n("Warm the Forge", "대장간 데우기"),
            "chapter": 1,
            "quest_kind": "main",
            "number": 5,
        }
    ],
    "recipe": [
        {
            "id": "1-equip:1",
            "kind": "recipe",
            "name": _n("Twig Knife", "잔가지칼"),
            "chapter": 1,
            "quest": [1, 77],
            "function": "pickItem",
            "script": "chapter1-forge",
            "material_items": [{"type": "item", "item": "etc:3", "count": 2}],
            "meso": 50,
            "result_item": "equip:1",
            "result_count": 1,
            "crafted_by": [{"kind": "npc", "id": "2", "map": "1-1"}],
        },
        {
            "id": "1-equip:2",
            "kind": "recipe",
            "name": _n("Reed Hat", "갈대모자"),
            "chapter": 1,
            "quest": [1, 77],
            "function": "pickItem",
            "script": "chapter1-forge",
            "material_items": [{"type": "item", "item": "etc:3", "count": 5}],
            "meso": 90,
            "result_item": "equip:2",
            "result_count": 1,
            "crafted_by": [{"kind": "npc", "id": "2", "map": "1-1"}],
        },
    ],
    "trade": [
        {
            "id": "1-etc:3-etc:4",
            "kind": "trade",
            "name": _n("Shard", "조각"),
            "chapter": 1,
            "quest": [1, 78],
            "function": "main",
            "script": "chapter1-barter",
            "cost_items": [{"type": "item", "item": "etc:3", "count": 50}],
            "cost_meso": 0,
            "result_items": [{"type": "item", "item": "etc:4", "count": 3}],
            "result_other": [],
            "offered_by": [{"kind": "npc", "id": "3", "map": "1-0"}],
        }
    ],
}


def kb() -> dict:
    """A fresh copy (tests may change it)."""
    return copy.deepcopy(KB)
