"""Ghidra names for the scenes' camera shake and the camera calls it goes through (#158).

``docs/re/screen-shake.md`` has the evidence: the scene runner ``0x83C71`` turns the shake on
(command 19) and off (command 20), ``CStateGame::Paint`` ``0x87501`` draws a random offset per
frame from the amplitude at ``CStateGame +0x488`` and scrolls the map's camera by it, and the
port's ``screen_shake`` hook (``runtime/src/hooks/builtin.cpp``) scales that amplitude.
``CStateGame::Paint`` itself, the scene runner, ``Rand_Range`` and the scene argument readers
are named by ``mtport.flow``, ``mtport.timing`` and ``mtport.audio``.

Command line::

    python3 -m mtport.screenshake names [OUT]            # work/re/names/screenshake.json
    python3 -m mtport.screenshake scenes [SLICE]         # the scenes' shake commands
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from mtport import ghidra_common as gc

NAMES_OUT = "work/re/names/screenshake.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    (
        "0x814E1",
        "function",
        "CStateGame::ResetSceneCamera",
        "+0x47C-0x47E, +0x1E9 and +0x480-0x494, +0x4C4 zeroed: no scene camera, no shake",
    ),
    ("0xC069", "function", "CMMap::ClampCamera", "(map): +0x70/+0x74 to 0 .. map - screen"),
    (
        "0xC175",
        "function",
        "CMMap::FollowPoint",
        "(map, x, y, smooth): the camera to p - screen / 2, at once or by a third",
    ),
    (
        "0xC225",
        "function",
        "CMMap::ScrollCamera",
        "(map, dx, dy): +0x70 += dx, +0x74 += dy, then ClampCamera",
    ),
)

# The CStateGame fields of the shake (docs/re/screen-shake.md), as the hook reads them.
SHAKE_ON = 0x47E
SHAKE_AMPLITUDE = 0x488
SHAKE_OFFSET_X = 0x48C
SHAKE_OFFSET_Y = 0x490
# The scene commands (Scene_AddCommand's `command`; the runner's switch on command - 1).
COMMAND_SHAKE_ON = 19
COMMAND_SHAKE_OFF = 20


def names_json() -> str:
    return gc.names_json("screenshake", NAMES)


@dataclass(frozen=True, order=True)
class ShakeCommand:
    function: int  # the scene builder (CAniManager::makeAni*), function pointer
    step: int | None
    command: int  # COMMAND_SHAKE_ON or COMMAND_SHAKE_OFF
    amplitude: int | None  # p2 of command 19 (screen points); 0 for command 20
    site: int


def shake_commands(scan) -> list[ShakeCommand]:
    """Every shake command the scene builders add, from an :func:`mtport.armscan.scan` of the
    slice run with the scene adders as dispatchers (as :func:`mtport.audio_analysis.collect`)."""
    from mtport.audio_analysis import SCENE_ADD, SCENE_ADD_P2, SCENE_ADDERS, _const

    adders = {a & ~1 for a in SCENE_ADDERS}
    out = []
    for s in scan.sends:
        if s.stub not in adders or s.function in SCENE_ADDERS:
            continue
        command = _const(s.regs[2])
        if command not in (COMMAND_SHAKE_ON, COMMAND_SHAKE_OFF):
            continue
        if s.stub == SCENE_ADD & ~1:
            p2 = _const(s.stack[0]) if s.stack else None
        elif s.stub == SCENE_ADD_P2 & ~1:
            p2 = _const(s.regs[3])
        else:
            p2 = 0
        out.append(ShakeCommand(s.function, _const(s.regs[1]), command, p2, s.site))
    return sorted(out)


def scenes(slice_path: Path) -> list[ShakeCommand]:
    """:func:`shake_commands` of the armv7 slice (``work/slices/MapleThief.armv7``)."""
    from mtport import armscan, macho
    from mtport.audio_analysis import SCENE_ADDERS

    return shake_commands(armscan.scan(macho.load(slice_path), [a & ~1 for a in SCENE_ADDERS]))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.screenshake", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    sc = sub.add_parser("scenes", help="list the scene builders' shake commands")
    sc.add_argument("slice", nargs="?", default="work/slices/MapleThief.armv7")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "scenes":
            for c in scenes(Path(args.slice)):
                what = f"shake {c.amplitude}" if c.command == COMMAND_SHAKE_ON else "stop"
                print(f"0x{c.function:X} step {c.step}: {what} (0x{c.site:X})")
            return 0
        out = Path(args.out)
        gc.write_text(out, names_json())
        print(f"wrote {out} ({len(NAMES)} names)")
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
