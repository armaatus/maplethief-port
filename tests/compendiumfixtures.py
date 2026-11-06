"""A synthetic compendium knowledge base, image manifest and prose for the site tests.

Every name and number is made up; nothing comes from the game.
"""

from __future__ import annotations

import json
from pathlib import Path

PNG_1X1 = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010806000000"
    "1f15c4890000000d49444154789c6360f8cfc0000003010100c9fe92ef0000000049454e44ae426082"
)


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


KB = {
    "monster": [
        {
            "id": "1",
            "kind": "monster",
            "name": _n("Puddle Slug", "웅덩이민달팽이"),
            "level": 2,
            "hp": 15,
            "exp": 3,
            "meso": 5,
            "type": 0,
            "aliases": ["slug"],
            "confidence": {"hp": "read"},
            "drops": [
                {"kind": "item", "id": "etc:3", "chance": 600},
                {"kind": "item", "id": "equip:1", "chance": 25},
            ],
            "maps": [{"kind": "map", "id": "1-1", "count": 4}],
        },
        {
            "id": "2",
            "kind": "monster",
            "name": _n("Grim Lantern", "음산한등불"),
            "level": 30,
            "hp": 9000,
            "exp": 700,
            "meso": 120,
            "type": 2,
            "confidence": {},
            "drops": [{"kind": "item", "id": "etc:3", "chance": 1000}],
        },
    ],
    "item": [
        {
            "id": "equip:1",
            "kind": "item",
            "name": _n("Tin Dagger", "양철단검"),
            "price": 100,
            "description": "A practice blade.",
            "confidence": {},
        },
        {
            "id": "etc:3",
            "kind": "item",
            "name": _n("Slug Shell", "민달팽이껍질"),
            "price": 2,
            "confidence": {},
        },
    ],
    "npc": [
        {
            "id": "7",
            "kind": "npc",
            "name": _n("Old Fen", "늙은펜"),
            "shops": [{"kind": "shop", "id": "1"}],
            "confidence": {},
        }
    ],
    "shop": [
        {
            "id": "1",
            "kind": "shop",
            "name": _n("Fen's stall", "펜의가게"),
            "listings": [{"kind": "listing", "id": "1:0"}, {"kind": "listing", "id": "1:1"}],
            "confidence": {},
        }
    ],
    "listing": [
        {"id": "1:0", "kind": "listing", "name": _n("Tin Dagger", "양철단검"), "price": 150},
        {"id": "1:1", "kind": "listing", "name": _n("Slug Shell", "민달팽이껍질"), "price": 4},
    ],
    "skill": [{"id": "3", "kind": "skill", "name": _n("Quick Step", "재빠른걸음"), "level": 1}],
    "level": [
        {"id": "1", "kind": "level", "name": _n("Level 1", "레벨 1"), "exp": 15},
        {"id": "2", "kind": "level", "name": _n("Level 2", "레벨 2"), "exp": 34},
    ],
    "area": [{"id": "1", "kind": "area", "name": _n("Mossy Hollow", "이끼골")}],
    "map": [
        {
            "id": "1-1",
            "kind": "map",
            "name": _n("Hollow Edge", "골짜기끝"),
            "area": {"kind": "area", "id": "1"},
            "monsters": [{"kind": "monster", "id": "1", "count": 4}],
        }
    ],
}

PROSE = {
    "index.md": "# Welcome\n\nA synthetic compendium for the tests.\n",
    "monsters/index.md": "The monsters of the synthetic world. Start with {{monster:1}}.\n",
    "monster/2.md": "## Strategy\n\nBring {{item:etc:3|a few shells}}.\n",
    "mechanics/drops.md": (
        "# Drop rates\n\n"
        "Each kill rolls every drop of the monster on its own.\n\n"
        "## Rates\n\n"
        "- {{monster:1}} drops {{item:etc:3}} often.\n"
        "- See [the item list](items/index.html) and [the rates](#rates).\n\n"
        "| Monster | Chance |\n|---|---|\n| {{monster:2}} | **100%** |\n\n"
        "> A quote with *emphasis* and `code`.\n\n"
        "1. First\n2. Second\n  continued\n"
    ),
}

MANIFEST = {
    "schema": 1,
    "images": {
        "monster:1": [
            {"path": "monster/1/stand.png", "w": 1, "h": 1, "alt": "Puddle Slug", "role": "still"},
            {
                "path": "monster/1/move.png",
                "w": 1,
                "h": 1,
                "alt": "Puddle Slug moving",
                "role": "anim",
            },
        ],
        "item:etc:3": [
            {"path": "item/etc-3.png", "w": 1, "h": 1, "alt": "Slug Shell", "role": "icon"}
        ],
    },
}


def write_kb(root: Path, kb: dict | None = None) -> Path:
    d = root / "kb"
    d.mkdir(parents=True, exist_ok=True)
    for kind, rows in (kb or KB).items():
        doc = {"schema": 1, "kind": kind, "entities": rows}
        (d / f"{kind}.json").write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
    return d


def write_prose(root: Path, prose: dict | None = None) -> Path:
    d = root / "pages"
    for rel, text in (prose if prose is not None else PROSE).items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    d.mkdir(parents=True, exist_ok=True)
    return d


def write_images(site: Path) -> Path:
    img = site / "img"
    img.mkdir(parents=True, exist_ok=True)
    (img / "manifest.json").write_text(json.dumps(MANIFEST), encoding="utf-8")
    for rows in MANIFEST["images"].values():
        for r in rows:
            p = img / r["path"]
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(PNG_1X1)
    return img / "manifest.json"
