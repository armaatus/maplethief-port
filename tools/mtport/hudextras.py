"""Ghidra names for what the field HUD and the mobs draw, and the game's notify lines (#191).

``docs/re/hud-extras.md`` has the evidence: the game already draws a mob's HP bar for 20 ticks
after each hit (``CMob::Paint`` ``0x72685`` while the counter ``+0xD0`` that ``CMob::TakeHit``
``0x73715`` starts runs) and a line for each pick-up and meso gain (``CHero::addInven``
``0x64309`` and ``CHero::setMeso`` ``0x618E1`` through ``CNotifyTextList::Add`` ``0x78339``,
5 s each), so the port draws neither; ``HUD_Paint`` ``0x5A465`` shows the EXP only as a
2-point line (``Gauge_Width`` ``0x36FF1`` over ``CHero::ExpToLevelUp`` ``0x65919``, which reads
hero.dat through ``CDataMgr::FindRow`` ``0x3990D`` and ``CDataMgr::ReadI32`` ``0x385D1``), so
the port's ``exp_percent`` adds the figure (ADR 0109).

Command line::

    python3 -m mtport.hudextras names [OUT]            # work/re/names/hudextras.json
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/hudextras.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    (
        "0x36FF1",
        "function",
        "Gauge_Width",
        "(g, full, max, value): a gauge's fill in points, full * value / max (0 for max 0, "
        "full from value >= max); the HUD's HP / MP / EXP bars and a mob's HP bar",
    ),
    (
        "0x385D1",
        "function",
        "CDataMgr::ReadI32",
        "(mgr, table, offset, row): the i32 at table + 6 + row * stride + offset",
    ),
    (
        "0x3990D",
        "function",
        "CDataMgr::FindRow",
        "(mgr, table, offset, key): the first row whose u8 at offset is key, else -1",
    ),
    (
        "0x5CF6D",
        "function",
        "CMMap_CameraX",
        "(map): the camera's x, CMMap +0x70 (map pixels at the screen's left edge)",
    ),
    (
        "0x5CF81",
        "function",
        "CMMap_CameraY",
        "(map): the camera's y, CMMap +0x74",
    ),
    (
        "0x72685",
        "function",
        "CMob::Paint",
        "the mob's sprite; while its hit counter +0xD0 runs (id < 0x40), its name and a 40 x 2 "
        "HP bar (+0x88 of +0x90) above it",
    ),
    (
        "0x73349",
        "function",
        "CMob::Die",
        "(mob, hero): HP 0, the death state, drops, EXP (CHero::setExp) and quests; a boss "
        "(0x40, 0x42-0x45) starts its scene",
    ),
    (
        "0x73715",
        "function",
        "CMob::TakeHit",
        "(mob, hero, ...): rolls the damage (miss, normal or critical), the damage number, HP "
        "+0x88 down; dies at 0, else the hit counters +0xC8 and +0xD0 start",
    ),
    (
        "0x76D05",
        "function",
        "CNpcs_Paint",
        "(npcs): every mob of the list (CMob::Paint), between three 32-slot object lists",
    ),
    (
        "0x7798D",
        "function",
        "CNotifyText::CNotifyText",
        "(this, text, colour, &lines): the text wrapped at 240 points (+0x04, count +0x08), "
        "colour +0x0C, the time +0x10",
    ),
    (
        "0x77B39",
        "function",
        "CNotifyTextList::Expire",
        "(list): removes the lines older than 5,000 ms",
    ),
    (
        "0x77C19",
        "function",
        "CNotifyTextList::Paint",
        "(list, x, y): the lines, each fading out from 4.5 s to 5 s",
    ),
    (
        "0x78339",
        "function",
        "CNotifyTextList::Add",
        "(list, text, colour): a new line; the oldest goes when the list holds its maximum",
    ),
    (
        "0x788A9",
        "function",
        "CNotifyTextList::CNotifyTextList",
        "(this, max, line height, style, ...): an empty list",
    ),
)


def names_json() -> str:
    return gc.names_json("hudextras", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.hudextras", description=__doc__.splitlines()[0]
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
