"""A synthetic knowledge base for the walkthrough section's tests (#227).

Every name and number is made up; nothing comes from the game. Two chapters:

- Chapter 1 in Fernvale: Old Fen's two main quests (a hunt, then shells), a side quest of
  Captain Brine whose reward is the only Ferry Token, and Brine's passage, which needs the
  token and ends the chapter (a ``clearAll`` script that warps to chapter 2's pier). One
  optional side quest (the lost button) is missable.
- Chapter 2 on the clouds: a greeting, a boss behind a portal that opens only while its
  quest is in progress, and a last quest that never ends. The boss's fight is the ending.

The quest ids follow the game's scheme, so the walkthrough's parts pick them up: 1-main-7
and 1-main-8 (Henesys), 1-main-21 (Kerning City), 2-main-0 to 2-main-2 (Orbis).
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _state(kind: int, number: int, op: str, value: int) -> dict:
    return {
        "args": [kind, number],
        "native": "getQuestState",
        "op": op,
        "quest": [kind, number],
        "term": f"getQuestState({kind}, {number})",
        "text": f"quest {kind}/{number} {op} {value}",
        "value": value,
    }


def _ref(kind: str, i: str, **kw: Any) -> dict:
    return {"kind": kind, "id": i, **kw}


def _exit(to: str, script: str, gated: bool = False) -> dict:
    return {"kind": "map", "id": to, "gated": gated, "portal": 0, "script": script}


def _map(mid: str, en: str, area: str, exits: list[dict], **kw: Any) -> dict:
    m = {
        "id": mid,
        "kind": "map",
        "name": _n(en, en),
        "area": [_ref("area", area)],
        "boss_map": False,
        "exits": exits,
        "entrances": [],
        "spawns": [],
        "npcs": [],
        "confidence": {},
    }
    m.update(kw)
    return m


def _quest(qid: str, name: str, **kw: Any) -> dict:
    ch, kind, number = qid.split("-")
    q = {
        "id": qid,
        "kind": "quest",
        "name": _n(name, name),
        "chapter": int(ch),
        "quest_kind": kind,
        "kind_no": 0 if kind == "main" else 1,
        "number": int(number),
        "main_story": kind == "main",
        "title_key": "",
        "description_key": "",
        "description": None,
        "scripts": [],
        "functions": ["main"],
        "requirements": [],
        "rewards": [],
        "variants": [],
        "prerequisites": [[]],
        "started": True,
        "reachable": True,
        "completion_scripts": [],
        "given_by": [],
        "completed_by": [],
        "needs_items": [],
        "needs_kills": [],
        "reward_items": [],
        "requires": [],
        "unlocks": [],
        "confidence": {},
    }
    q.update(kw)
    return q


FEN = [_ref("npc", "7", map="1-0", script="chapter1-Fen")]
BRINE_1 = [_ref("npc", "8", map="1-0", script="chapter1-Brine")]
BRINE_2 = [_ref("npc", "8", map="2-0", script="chapter2-Brine")]


def _after(prev: str, kind: int, number: int, value: int = 2) -> dict:
    return {
        "requires": [_ref("quest", prev, op="==", value=value)],
        "prerequisites": [[_state(kind, number, "==", value)]],
    }


KB: dict[str, list[dict]] = {
    "area": [
        {"id": "1", "kind": "area", "name": _n("Fernvale", "Fernvale"), "confidence": {}},
        {"id": "2", "kind": "area", "name": _n("Cloudreach", "Cloudreach"), "confidence": {}},
    ],
    "map": [
        _map(
            "1-0",
            "Fernvale Square",
            "1",
            [_exit("1-1", "common/portal100")],
            entrances=[_exit("1-1", "common/portal110")],
            npcs=[_ref("npc", "7", tile=[3, 4]), _ref("npc", "8", tile=[9, 4])],
        ),
        _map(
            "1-1",
            "Mossy Field",
            "1",
            [_exit("1-0", "common/portal110")],
            entrances=[_exit("1-0", "common/portal100")],
            spawns=[_ref("monster", "1", count=6)],
        ),
        _map(
            "2-0",
            "Cloud Pier",
            "2",
            [_exit("2-1", "common/portal200")],
            entrances=[_exit("2-1", "common/portal210")],
            npcs=[_ref("npc", "8", tile=[5, 4])],
        ),
        _map(
            "2-1",
            "Cloud Path",
            "2",
            [_exit("2-0", "common/portal210"), _exit("2-2", "chapter2/portal211", gated=True)],
            entrances=[_exit("2-0", "common/portal200"), _exit("2-2", "common/portal220")],
            spawns=[_ref("monster", "3", count=4)],
        ),
        _map(
            "2-2",
            "Storm Nest",
            "2",
            [_exit("2-1", "common/portal220")],
            entrances=[_exit("2-1", "chapter2/portal211", gated=True)],
            spawns=[_ref("monster", "2", count=1)],
            boss_map=True,
        ),
    ],
    "monster": [
        {
            "id": "1",
            "kind": "monster",
            "name": _n("Puddle Slug", "Puddle Slug"),
            "level": 3,
            "exp": 4,
            "hp": 30,
            "spawns_on": [_ref("map", "1-1", count=6)],
            "confidence": {},
        },
        {
            "id": "2",
            "kind": "monster",
            "name": _n("Gale Wyrm", "Gale Wyrm"),
            "level": 20,
            "exp": 900,
            "hp": 5000,
            "boss_bar": True,
            "class": "boss",
            "spawns_on": [_ref("map", "2-2", count=1)],
            "hunted_by_quests": [_ref("quest", "2-main-1", count=1)],
            "confidence": {},
        },
        {
            "id": "3",
            "kind": "monster",
            "name": _n("Puff Moth", "Puff Moth"),
            "level": 12,
            "exp": 40,
            "hp": 300,
            "spawns_on": [_ref("map", "2-1", count=4)],
            "confidence": {},
        },
    ],
    "item": [
        {
            "id": "etc:3",
            "kind": "item",
            "name": _n("Slug Shell", "Slug Shell"),
            "dropped_by": [_ref("monster", "1", chance=600, per=1000, percent=60.2)],
            "confidence": {},
        },
        {
            "id": "etc:9",
            "kind": "item",
            "name": _n("Ferry Token", "Ferry Token"),
            "rewarded_by_quests": [_ref("quest", "1-sub-3")],
            "confidence": {},
        },
    ],
    "npc": [
        {
            "id": "7",
            "kind": "npc",
            "name": _n("Old Fen", "Old Fen"),
            "stands_on": [_ref("map", "1-0", script="Fen", tile=[3, 4])],
            "confidence": {},
        },
        {
            "id": "8",
            "kind": "npc",
            "name": _n("Captain Brine", "Captain Brine"),
            "stands_on": [
                _ref("map", "1-0", script="Brine", tile=[9, 4]),
                _ref("map", "2-0", script="Brine", tile=[5, 4]),
            ],
            "confidence": {},
        },
    ],
    "level": [
        {
            "id": str(lv),
            "kind": "level",
            "name": _n(f"Level {lv}", f"Level {lv}"),
            "level": lv,
            "accuracy": lv + 2,
            "exp_total": 10 * lv * (lv - 1),
            "confidence": {},
        }
        for lv in range(1, 31)
    ],
    "event": [
        {
            "id": "chapter1-Brine-chapter_end",
            "kind": "event",
            "name": _n("chapter1/Brine: end of chapter", "chapter1/Brine: end of chapter"),
            "type": "chapter_end",
            "chapters": [1],
            "args": [],
            "conditions": [[_state(0, 21, ">=", 1)]],
            "maps": [_ref("map", "1-0")],
            "script": [_ref("script", "chapter1-Brine")],
            "trigger": ["npc"],
            "used": True,
            "confidence": {},
        },
        {
            "id": "chapter2-NestMap-boss",
            "kind": "event",
            "name": _n("chapter2/NestMap: boss", "chapter2/NestMap: boss"),
            "type": "boss",
            "chapters": [2],
            "args": [],
            "conditions": [[]],
            "maps": [_ref("map", "2-2")],
            "script": [_ref("script", "chapter2-NestMap")],
            "trigger": ["map"],
            "used": True,
            "confidence": {},
        },
    ],
    "warp": [
        {
            "id": "chapter1-Brine-1",
            "kind": "warp",
            "name": _n("chapter1/Brine to 2-0", "chapter1/Brine to 2-0"),
            "chapter_end": True,
            "chapters": [1],
            "conditional": True,
            "conditions": [[_state(0, 21, ">=", 1)]],
            "from": [{"map": "1-0", "npc": "8", "portal": None, "role": "npc"}],
            "script": [_ref("script", "chapter1-Brine")],
            "target_map": "2-0",
            "target": [_ref("map", "2-0", portal=0)],
            "confidence": {},
        },
        {
            "id": "chapter2-portal211-1",
            "kind": "warp",
            "name": _n("chapter2/portal211 to 2-2", "chapter2/portal211 to 2-2"),
            "chapter_end": False,
            "chapters": [2],
            "conditional": True,
            "conditions": [[_state(0, 1, "==", 1)]],
            "from": [{"map": "2-1", "npc": None, "portal": 1, "role": "portal"}],
            "script": [_ref("script", "chapter2-portal211")],
            "target_map": "2-2",
            "target": [_ref("map", "2-2", portal=0)],
            "confidence": {},
        },
    ],
    "quest": [
        _quest(
            "1-main-7",
            "Fen's Errand",
            given_by=FEN,
            completed_by=FEN,
            requirements=[{"type": "mob", "monster": "1", "count": 5}],
            rewards=[{"type": "exp", "amount": 60}],
        ),
        _quest(
            "1-main-8",
            "Shells for Fen",
            given_by=FEN,
            completed_by=FEN,
            requirements=[{"type": "item", "item": "etc:3", "count": 4}],
            rewards=[{"type": "exp", "amount": 80}, {"type": "meso", "amount": 500}],
            **_after("1-main-7", 0, 7),
        ),
        _quest(
            "1-sub-3",
            "Brine's Chore",
            given_by=BRINE_1,
            completed_by=BRINE_1,
            requirements=[{"type": "mob", "monster": "1", "count": 3}],
            rewards=[{"type": "item", "item": "etc:9", "count": 1}],
            **_after("1-main-7", 0, 7),
        ),
        _quest(
            "1-sub-4",
            "The Lost Button",
            given_by=FEN,
            completed_by=FEN,
            requirements=[{"type": "mob", "monster": "1", "count": 9}],
            rewards=[{"type": "exp", "amount": 40}],
        ),
        _quest(
            "1-main-21",
            "Brine's Passage",
            given_by=BRINE_1,
            requirements=[{"type": "item", "item": "etc:9", "count": 1}],
            **_after("1-main-8", 0, 8),
        ),
        _quest("2-main-0", "Cloud Welcome", given_by=BRINE_2, completed_by=BRINE_2),
        _quest(
            "2-main-1",
            "The Wyrm",
            given_by=BRINE_2,
            completed_by=BRINE_2,
            requirements=[{"type": "mob", "monster": "2", "count": 1}],
            rewards=[{"type": "exp", "amount": 3000}],
            unlocks=[_ref("quest", "2-main-2", op="==", value=1)],
            **_after("2-main-0", 0, 0),
        ),
        _quest("2-main-2", "Calm Skies", given_by=BRINE_2, **_after("2-main-1", 0, 1)),
    ],
}


def kb() -> dict[str, list[dict]]:
    return copy.deepcopy(KB)


def write_kb(root: Path, data: dict | None = None) -> Path:
    d = root / "kb"
    d.mkdir(parents=True, exist_ok=True)
    data = data or KB
    for kind, rows in data.items():
        doc = {"schema": 1, "kind": kind, "entities": rows}
        (d / f"{kind}.json").write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
    return d
