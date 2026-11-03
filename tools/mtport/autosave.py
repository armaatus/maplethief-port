"""Ghidra names for the game's save guards and the facts the port's autosave reads (#150).

``docs/re/autosave.md`` has the evidence: what ``CStateGame::OnKeyPressed`` ``0x8D4C9`` and
``Game_ExitToMainMenu`` ``0x54BF9`` check before the game saves, the game modes, the hero's
actions and flags, and the 16 maps where the game refuses to save. ``NO_SAVE_MAPS`` is the
same list as ``platform/src/autosave.cpp``'s ``kNoSaveMaps``; ``tests/test_autosave_names.py``
keeps the two equal.

Command line::

    python3 -m mtport.autosave names [OUT]            # work/re/names/autosave.json
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/autosave.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x60551", "function", "CHero::SetAction", "(hero, action): +0x10; 0x604D9 on a change"),
    ("0x61329", "function", "CHero::StopForMenu", "stand unless on a ladder; speed 0"),
    (
        "0x61385",
        "function",
        "CHero::StopToStand",
        "unless standing: off a ladder, flags &= ~0x1F; speed 0",
    ),
    ("0x66695", "function", "CHero::UseItem", "(hero, slot, count); a scroll sets dm +0x108"),
    ("0x6D0BD", "function", "CHero::HasMoveFlags", "flags +0xC & 7"),
    ("0x6D0E1", "function", "CHero::IsInAir", "flags +0xC bit 1: a jump or a knock-back"),
    ("0x6D101", "function", "CHero::TestFlag0", "flags +0xC bit 0"),
    ("0x6D119", "function", "CHero::IsAttacking", "flags +0xC bit 3: onAttack to Flush"),
    ("0x6D139", "function", "CHero::IsOnLadder", "flags +0xC bit 2: a ladder or a rope"),
    ("0x80935", "function", "CStateGame::QueueScene", "game mode 10: the scene in +0x1E4"),
    ("0x80965", "function", "CStateGame::QueueBossScene", "+0x1E4 by area, then game mode 10"),
    ("0x853E5", "function", "CStateGame::StartScene", "(state, n): CAniManager::makeAni*"),
    ("0x108C64", "data", "g_popupId", "the open popup, -1 for none"),
    ("0x11A950", "data", "g_pSkyCanvas", "SkyCanvas*: +0xC the engine's state"),
    ("0x11A970", "data", "g_pDataMgr", "CDataMgr*; +0x4E4 the slot in play"),
    ("0x11AA00", "data", "g_gameMode", "0 field, 1 menu, 6-8 loading, 9/10 a scene"),
)

# (area, map, kind): where Save says "Cannot save.", in 0x54BF9's order.
NO_SAVE_MAPS: tuple[tuple[int, int, str], ...] = (
    (2, 8, "tutorial"),
    (2, 9, "tutorial"),
    (3, 8, "boss"),
    (4, 17, "boss"),
    (7, 4, "boss"),
    (9, 5, "boss"),
    (10, 5, "boss"),
    (2, 6, "climb"),
    (2, 7, "climb"),
    (6, 1, "climb"),
    (6, 2, "climb"),
    (9, 9, "climb"),
    (9, 10, "climb"),
    (0, 5, "Monster Carnival"),
    (4, 18, "Monster Carnival"),
    (6, 3, "Monster Carnival"),
)


def names_json() -> str:
    return gc.names_json("autosave", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.autosave", description=__doc__.splitlines()[0]
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
