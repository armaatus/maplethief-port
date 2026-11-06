"""A synthetic knowledge base for the quests section's tests (#223).

Every name, line and number is made up; nothing comes from the game. One NPC (Old Fen) runs
a conversation that offers a side quest (Accept or Refuse), finishes it, and later shows a
menu of two rows to a hero below level 5; a map script (the gate) repeats one of Fen's
lines and ends the chapter.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _c(term: str, op: str, value: int) -> dict:
    return {"term": term, "op": op, "value": value}


def _path(cond, do, box=0, nxt=None, function="main") -> dict:
    return {"if": cond, "do": do, "box": box, "next": nxt, "loop": None, "function": function}


TEXT = {
    "Fen.q5.title": _n("Shells for Fen", "펜의 껍질"),
    "Fen.q5.desc": _n("Fen wants four shells.", "펜은 껍질 네 개를 원한다."),
    "Fen.m1.title": _n("The Gate", "문"),
    "Fen.m1.desc": _n("Open the gate.", "문을 열어라."),
    "Fen.offer.1": _n("Shells, I need shells.", "껍질이 필요해."),
    "Fen.offer.2": _n("Four of them.", "네 개."),
    "Fen.accept.1": _n("Thank you kindly.", "고마워."),
    "Fen.refuse.1": _n("Pity.", "아쉽군."),
    "Fen.reward.1": _n("Lovely shells.", "좋은 껍질이야."),
    "Fen.wait.1": {"en": "Still waiting.", "ko": None},
    "Fen.hello.1": _n("Hello again.", "또 왔군."),
    "Fen.menu.ask": _n("What now?", "이제 뭘?"),
    "Fen.menu.a": _n("Ask about the gate", "문에 대해"),
    "Fen.menu.b": _n("Leave", "떠나기"),
    "Fen.menu.a.reply": _n("The gate opens at dusk.", "문은 해질녘에 열린다."),
    "Fen.menu.b.reply": _n("Farewell, Fen.", "잘 있어."),
    "Fen.unused.1": _n("A line no path shows.", "보이지 않는 줄."),
}
GATE_TEXT = {
    "Fen.hello.1": TEXT["Fen.hello.1"],
    "Gate.closed.1": _n("The gate is shut.", "문이 닫혔다."),
}

FEN_NODES = {
    "n0": {
        "function": "main",
        "answer": None,
        "answers": [],
        "paths": [
            _path(
                [[_c("getQuestState(1, 5)", "==", 0), _c("getLevel()", ">=", 5)]],
                [
                    {"npc": "Fen.offer.1"},
                    {"npc": "Fen.offer.2"},
                    {"call": "setQuest", "args": [1, 5, "Fen.q5.title", "Fen.q5.desc"]},
                    {"call": "regRequireItem", "args": [1, 3, 4]},
                ],
                box=1,
                nxt="n1",
            ),
            _path(
                [[_c("getQuestState(1, 5)", "==", 1), _c("checkRequire(1, 5)", "==", 1)]],
                [
                    {"npc": "Fen.reward.1"},
                    {"call": "regMsgReward", "args": [1, 5]},
                    {"call": "getReward", "args": [1, 5]},
                ],
            ),
            _path(
                [[_c("getQuestState(1, 5)", "==", 1), _c("checkRequire(1, 5)", "!=", 1)]],
                [{"npc": "Fen.wait.1"}],
            ),
            _path(
                [[_c("getLevel()", "<", 5)]],
                [
                    {"npc": "Fen.hello.1"},
                    {"npc": "Fen.menu.ask"},
                    {"choice": "Fen.menu.a", "value": 1},
                    {"choice": "Fen.menu.b", "value": 2},
                ],
                box=2,
                nxt="n2",
            ),
        ],
    },
    "n1": {
        "function": "afterOffer",
        "answer": 1,
        "answers": [0, 1],
        "paths": [
            _path(
                [[_c("answer#1", "==", 1)]],
                [{"npc": "Fen.accept.1"}, {"call": "startQuest", "args": []}],
                function="afterOffer",
            ),
            _path([[_c("answer#1", "==", 0)]], [{"npc": "Fen.refuse.1"}], function="afterOffer"),
        ],
    },
    "n2": {
        "function": "afterMenu",
        "answer": 1,
        "answers": [1, 2],
        "paths": [
            _path([[_c("answer#1", "==", 1)]], [{"npc": "Fen.menu.a.reply"}], function="afterMenu"),
            _path(
                [[_c("answer#1", "==", 2)]],
                [{"hero": "Fen.menu.b.reply", "face": 0}],
                function="afterMenu",
            ),
        ],
    },
}
GATE_NODES = {
    "n0": {
        "function": "main",
        "answer": None,
        "answers": [],
        "paths": [
            _path(
                [[_c("getQuestState(0, 1)", "==", 1)]],
                [{"npc": "Fen.hello.1"}, {"call": "clearAll", "args": []}],
            ),
            _path([[_c("getQuestState(0, 1)", "!=", 1)]], [{"npc": "Gate.closed.1"}]),
        ],
    }
}


def _quest(qid: str, kind: str, number: int, name: dict, **kw: Any) -> dict:
    q = {
        "id": qid,
        "kind": "quest",
        "name": name,
        "chapter": 1,
        "quest_kind": kind,
        "kind_no": 0 if kind == "main" else 1,
        "number": number,
        "main_story": kind == "main",
        "title_key": "",
        "description_key": "",
        "description": None,
        "scripts": ["chapter1-Fen"],
        "functions": ["main"],
        "requirements": [],
        "rewards": [],
        "variants": [],
        "prerequisites": [[]],
        "started": True,
        "reachable": True,
        "completion_scripts": [],
        "given_by": [{"kind": "npc", "id": "7", "map": "1-1", "script": "chapter1-Fen"}],
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


KB: dict[str, list[dict]] = {
    "area": [{"id": "1", "kind": "area", "name": _n("Mossy Hollow", "이끼골"), "confidence": {}}],
    "map": [
        {
            "id": "1-1",
            "kind": "map",
            "name": _n("Hollow Edge", "골짜기끝"),
            "area": [{"kind": "area", "id": "1"}],
            "spawns": [{"kind": "monster", "id": "1", "count": 4}],
            "confidence": {},
        }
    ],
    "monster": [
        {
            "id": "1",
            "kind": "monster",
            "name": _n("Puddle Slug", "웅덩이민달팽이"),
            "level": 2,
            "spawns_on": [{"kind": "map", "id": "1-1", "count": 4}],
            "drops": [{"kind": "item", "id": "etc:3", "chance": 600, "per": 1000, "percent": 60.2}],
            "confidence": {},
        }
    ],
    "item": [
        {
            "id": "etc:3",
            "kind": "item",
            "name": _n("Slug Shell", "민달팽이껍질"),
            "dropped_by": [
                {"kind": "monster", "id": "1", "chance": 600, "per": 1000, "percent": 60.2}
            ],
            "confidence": {},
        }
    ],
    "npc": [
        {
            "id": "7",
            "kind": "npc",
            "name": _n("Old Fen", "늙은펜"),
            "stands_on": [{"kind": "map", "id": "1-1", "script": "Fen", "tile": [3, 4]}],
            "confidence": {},
        }
    ],
    "script": [
        {
            "id": "chapter1-Fen",
            "kind": "script",
            "name": _n("chapter1/Fen", "chapter1/Fen"),
            "approximate_entries": ["state"],
            "confidence": {},
        },
        {
            "id": "chapter1-Gate",
            "kind": "script",
            "name": _n("chapter1/Gate", "chapter1/Gate"),
            "approximate_entries": [],
            "confidence": {},
        },
    ],
    "event": [
        {
            "id": "chapter1-Gate-chapter_end",
            "kind": "event",
            "name": _n("Chapter end", "Chapter end"),
            "type": "chapter_end",
            "args": [],
            "chapters": [1],
            "confidence": {},
        }
    ],
    "dialogue": [
        {
            "id": "chapter1-Fen",
            "kind": "dialogue",
            "name": _n("Old Fen (chapter 1)", "늙은펜 (1장)"),
            "chapters": [1],
            "roles": ["npc"],
            "tree": "n0",
            "nodes": FEN_NODES,
            "state": [],
            "text": TEXT,
            "missing_text": ["Fen.wait.1"],
            "npcs": [{"kind": "npc", "id": "7", "map": "1-1"}],
            "script": [{"kind": "script", "id": "chapter1-Fen"}],
            "confidence": {},
        },
        {
            "id": "chapter1-Gate",
            "kind": "dialogue",
            "name": _n("Gate (chapter 1)", "문 (1장)"),
            "chapters": [1],
            "roles": ["map"],
            "tree": "n0",
            "nodes": GATE_NODES,
            "state": [],
            "text": GATE_TEXT,
            "missing_text": [],
            "npcs": [],
            "script": [{"kind": "script", "id": "chapter1-Gate"}],
            "confidence": {},
        },
    ],
    "quest": [
        _quest(
            "1-main-1",
            "main",
            1,
            _n("The Gate", "문"),
            title_key="Fen.m1.title",
            description_key="Fen.m1.desc",
            description=TEXT["Fen.m1.desc"],
            prerequisites=[[_c("getQuestState(1, 5)", "==", 2) | {"quest": [1, 5]}]],
            requires=[{"kind": "quest", "id": "1-sub-5", "op": "==", "value": 2}],
            requirements=[{"type": "mob", "monster": "1", "count": 3}],
            rewards=[{"type": "exp", "amount": 1200}],
        ),
        _quest(
            "1-sub-5",
            "sub",
            5,
            _n("Shells for Fen", "펜의 껍질"),
            title_key="Fen.q5.title",
            description_key="Fen.q5.desc",
            description=TEXT["Fen.q5.desc"],
            prerequisites=[[_c("getLevel()", ">=", 5)]],
            requirements=[{"type": "item", "item": "etc:3", "count": 4}],
            rewards=[{"type": "exp", "amount": 50}, {"type": "meso", "amount": 1500}],
            completed_by=[{"kind": "npc", "id": "7", "map": "1-1", "script": "chapter1-Fen"}],
            unlocks=[{"kind": "quest", "id": "1-main-1", "op": "==", "value": 2}],
        ),
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
