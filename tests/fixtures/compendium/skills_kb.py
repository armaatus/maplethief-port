"""A synthetic knowledge base for the Skills and jobs section (#222).

Every name and number is made up; nothing comes from the game. The skills' ``levels`` are
filled the way the knowledge base's tables stage fills them, so the section's own check
(its per-level values against the base and per-level columns) has something to compare.
"""

from __future__ import annotations

from typing import Any

from mtport.compendium import tables


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def skill(
    eid: str,
    name: str,
    branch: str,
    tier: int,
    typ: str,
    max_level: int,
    base: dict[str, int] | None = None,
    per: dict[str, int] | None = None,
    detail: str = "",
) -> dict[str, Any]:
    keys = ("damage", "time", "etc", "mp")
    base = {k: (base or {}).get(k, 0) for k in keys}
    per = {k: (per or {}).get(k, 0) for k in keys}
    levels = []
    for lv in range(1, max_level + 1):
        v = {k: tables.skill_value(base[k], per[k], lv) for k in keys}
        shown = {"dmg": v["damage"], "time": v["time"], "etc": tables.etc_value(v["etc"])}
        shown["mp"] = v["mp"]
        levels.append(
            {
                "level": lv,
                **v,
                "etc_value": shown["etc"],
                "detail": {"en": tables.fill_detail(detail, shown), "ko": ""},
            }
        )
    return {
        "id": eid,
        "kind": "skill",
        "name": _n(name, name + " (ko)"),
        "branch": branch,
        "tier": tier,
        "type": typ,
        "max_level": max_level,
        "learnable": max_level > 0,
        "base": base,
        "per_level": per,
        "description": _n(f"About {name}.", ""),
        "detail": _n(detail, ""),
        "levels": levels,
        "confidence": {},
    }


SKILLS = [
    skill("0", "Poke", "Rogue", 1, "basic attack", 0),
    skill(
        "2",
        "Twin Pebbles",
        "Rogue",
        1,
        "active attack",
        4,
        {"damage": 30, "mp": 6},
        {"damage": 11, "mp": 2},
        "Spend MP @mp, @dmg% Damage",
    ),
    skill(
        "4",
        "Light Feet",
        "Rogue",
        1,
        "passive",
        3,
        {"etc": 12},
        {"etc": 12},
        "Increase accuracy @etc",
    ),
    skill(
        "10",
        "Long Pebble",
        "Assassin",
        2,
        "active attack",
        3,
        {"damage": 65, "etc": 20, "mp": 30},
        {"damage": 31, "etc": 15, "mp": 3},
        "Spend MP @mp, @dmg% Damage, Piercing @etc",
    ),
    skill(
        "12",
        "Focus",
        "Assassin",
        3,
        "buff",
        2,
        {"damage": 10, "etc": 80, "mp": 220, "time": 500},
        {"damage": 6, "etc": 40, "mp": 25, "time": 50},
        "Spend MP @mp, Duration @time, Increase attack @dmg, Increase accuracy @etc",
    ),
    skill(
        "16",
        "Flurry",
        "Bandit",
        2,
        "active attack",
        3,
        {"damage": 5, "etc": 35, "mp": 20},
        {"damage": 6, "etc": 2, "mp": 3},
        "Spend MP @mp, @dmg% Damage, @etc combo",
    ),
]

LEVELS = []
_total = _hp = 0
for _lv in range(1, 13):
    _exp = 10 * _lv * _lv
    _hp += 10 + _lv
    LEVELS.append(
        {
            "id": str(_lv),
            "kind": "level",
            "name": _n(f"Level {_lv}", f"레벨 {_lv}"),
            "level": _lv,
            "exp": _exp,
            "exp_total": _total,
            "hp_gain": 10 + _lv,
            "mp_gain": 10 + _lv,
            "max_hp": _hp,
            "max_mp": _hp,
            "stats": {"strength": 4, "dexterity": 4, "intelligence": 4, "luck": 4 + _lv},
            "attack": _lv,
            "defense": _lv + 1,
            "accuracy": _lv + 2,
            "avoidability": _lv + 3,
            "unknown_9": 1,
            "confidence": {"unknown_9": "unknown"},
        }
    )
    _total += _exp


def _cond(native: str, args: list, op: str, value: int, text: str, **kw: Any) -> dict:
    return {
        "native": native,
        "args": args,
        "op": op,
        "value": value,
        "term": native,
        "text": text,
        **kw,
    }


def advancement(eid: str, chapter: int, adv: int, job: int | None, conds: list) -> dict:
    names = {0: "Rogue", 1: "Assassin", 2: "Bandit"}
    return {
        "id": eid,
        "kind": "job_advancement",
        "name": _n(f"Step {adv}", f"{adv}단계"),
        "chapter": chapter,
        "advancement": adv,
        "job": job,
        "job_name": names.get(job) if job is not None else None,
        "keeps_job": job is None,
        "script": "chapter1-Mentor",
        "conditions": [conds],
        "given_by": [{"kind": "npc", "id": "3", "map": "0-0"}],
        "confidence": {},
    }


ADVANCEMENTS = [
    advancement(
        "1-1-0",
        1,
        1,
        0,
        [
            _cond("getLevel", [], ">=", 10, "level >= 10"),
            _cond("getQuestState", [0, 21], "==", 1, "main quest 21 is in progress", quest=[0, 21]),
            _cond("isInvenAvailable", [0, 1], "==", 1, "isInvenAvailable(0, 1) == 1"),
        ],
    ),
    advancement(
        "1-2-1",
        1,
        2,
        1,
        [
            _cond("getLevel", [], ">=", 30, "level >= 30"),
            _cond("getClassUpCount", [], "==", 1, "advancements == 1"),
        ],
    ),
    advancement("1-2-2", 1, 2, 2, [_cond("getLevel", [], ">=", 30, "level >= 30")]),
    advancement(
        "2-3-same",
        2,
        3,
        None,
        [
            _cond("getLevel", [], ">=", 60, "level >= 60"),
            _cond("someNative", [7], "==", 2, "someNative(7) == 2"),
        ],
    ),
]


def _weapon(eid: str, name: str, kind: str, level: int, att: tuple[int, int], shop: str) -> dict:
    return {
        "id": eid,
        "kind": "item",
        "name": _n(name, name + " (ko)"),
        "category": "weapon",
        "weapon_type": kind,
        "req_level": level,
        "speed": 2,
        "stats": {"attack": {"min": att[0], "max": att[1]}},
        "sold_by": [{"kind": "shop", "id": shop, "price": 500}],
        "confidence": {},
    }


KB: dict[str, list[dict[str, Any]]] = {
    "skill": SKILLS,
    "level": LEVELS,
    "job_advancement": ADVANCEMENTS,
    "area": [{"id": "0", "kind": "area", "name": _n("Mossy Hollow", "이끼골"), "confidence": {}}],
    "map": [
        {
            "id": "0-0",
            "kind": "map",
            "name": _n("Hollow Square", "골짜기광장"),
            "area": [{"kind": "area", "id": "0"}],
            "confidence": {},
        }
    ],
    "npc": [
        {
            "id": "3",
            "kind": "npc",
            "name": _n("Old Tamsin", "탐신"),
            "stands_on": [{"kind": "map", "id": "0-0"}],
            "confidence": {},
        }
    ],
    "quest": [
        {
            "id": "1-main-21",
            "kind": "quest",
            "name": _n("The Trial", "시험"),
            "chapter": 1,
            "confidence": {},
        }
    ],
    "shop": [
        {
            "id": "2",
            "kind": "shop",
            "name": _n("Shop 2", "상점 2"),
            "opened_by": [{"kind": "npc", "id": "3", "map": "0-0", "chapters": [2, 3]}],
            "unopened_reason": None,
            "confidence": {},
        },
        {
            "id": "0",
            "kind": "shop",
            "name": _n("Shop 0", "상점 0"),
            "unopened_reason": "no script opens it",
            "confidence": {},
        },
    ],
    "item": [
        _weapon("equip:36", "Tin Claw", "claw", 10, (3, 6), "2"),
        _weapon("equip:24", "Tin Knife", "dagger", 10, (8, 12), "0"),
        {
            "id": "etc:11",
            "kind": "item",
            "name": _n("Pebble", "조약돌"),
            "category": "throwing star",
            "attack": 1,
            "max_stack": 200,
            "confidence": {},
        },
    ],
}

PROSE = {
    "skills/index.md": "The jobs of the synthetic world.\n\n## How skills work\n\nPoints.\n",
    "skills/assassin.md": "The claw job.\n\n## Strengths\n\nRange, {{skill:10}}.\n",
    "skill/2.md": "Throws two pebbles.\n\n## Strategy\n\nUse it early.\n",
}
