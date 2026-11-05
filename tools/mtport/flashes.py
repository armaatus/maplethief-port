"""Ghidra names for the game's full-screen flashes and the fills they go through (#186).

``docs/re/flashes.md`` has the evidence: the hero's draw ``CHero::Paint`` ``0x61A19`` fills the
whole screen white, then black, on frames of a revive and of effects 12 (a level-up), 13 and 15
(``CHero::StartEffect`` ``0x6049D`` starts them) through ``Canvas_FillRectColor`` ``0x23D95``;
``CStateGame::Paint`` ``0x87501`` (named by ``mtport.flow``) fades a scene in from a colour
through ``Canvas_FillRect`` ``0x23D1D``. Both end in ``FillRect`` ``0x8079``. The port's
``flashes`` hooks (``runtime/src/hooks/flashes.cpp``) scale those fills' alpha.

Command line::

    python3 -m mtport.flashes names [OUT]            # work/re/names/flashes.json
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/flashes.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    (
        "0x8079",
        "function",
        "FillRect",
        "(surface, x, y, w, h, graphics context at sp + 4): a solid rectangle in the context's "
        "colour (+0x14) at its alpha (+0x20, -1 opaque); leaves the colour current in GL",
    ),
    (
        "0x22479",
        "function",
        "Canvas_SetAlpha",
        "the draw alpha the canvas fills and image draws use while the alpha flag is set",
    ),
    (
        "0x23D1D",
        "function",
        "Canvas_FillRect",
        "(x, y, w, h): FillRect with the canvas's graphics context (scene fades, flashes)",
    ),
    (
        "0x23D95",
        "function",
        "Canvas_FillRectColor",
        "(x, y, w, h, colour at sp): sets the context's colour, then Canvas_FillRect",
    ),
    (
        "0x23DC1",
        "function",
        "Canvas_FillScreen",
        "(colour): the whole 480 x 320 screen, the title and menu screens' backgrounds",
    ),
    (
        "0x6049D",
        "function",
        "CHero::StartEffect",
        "(hero, id): the hero's effect id (global 0x11A9A4) from frame 0; LevelUp starts 12",
    ),
    (
        "0x61A19",
        "function",
        "CHero::Paint",
        "the hero's draw: its sprite, effects and the full-screen flash frames (#186)",
    ),
)

# The return addresses of the flash fills (docs/re/flashes.md), as the hook matches them.
HERO_FLASH_RETURNS = (0x61CDF, 0x61D33, 0x6200F, 0x62059, 0x621DF, 0x62213)
FADE_IN_RETURN = 0x8A6EF
# The flash colours: rows of the colour table (50 colours a row, 4 bytes each).
COLOUR_TABLE = 0x117150
WHITE_ROW = 2
BLACK_ROW = 3


def names_json() -> str:
    return gc.names_json("flashes", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.flashes", description=__doc__.splitlines()[0]
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
