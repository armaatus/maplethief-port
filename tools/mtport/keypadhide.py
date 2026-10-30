"""The key pad's hiding through MapleThief's own capture path (#71, ADR 0052).

The device golden list's "fresh" run (``tests/golden/device_screens.json``: a new game into the
first map, then the cash shop's popup) runs twice with ``MapleThief --fixed-step``, once with
``--hide-keypad never`` and once with ``--hide-keypad always``, and every screen is shot with
``--shot`` and ``--shot-gl`` (the composited screen and the GL frame under it). Then, per screen:

- **GL**: the game's own frame is identical, hidden as shown. Hiding is the compositor's alone.
- **screen**: the composited screens differ only inside the game pad's rects (panel 0 of
  ``docs/controls/keypad.md``: the d-pad, jump, attack, the short keys, Map, Bag, Cash). On the
  first map, where the game shows the pad, they do differ inside the d-pad and the attack key.
  Everywhere else (the menus, the intro, the popup, where the game itself has the pad's panel
  off) they're identical.
- **ours**: the run with the pad shown matches the device golden's recorded render
  (``<golden dir>/ours/<name>.png``, ``platform.device_golden``), when one is there.
- **log**: ``always`` says the pad is hidden from the start and never shows it; ``never``
  never hides it.

Everything goes to ``--out`` (CTest: ``build/test-output/<preset>/keypad_hide/``). Exit status:
0 nothing failed, 1 something failed, 77 skipped (the game could not start), 2 bad usage.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import devicegolden as dg
from . import png

SKIP = 77
RUN = "fresh"
MODES = ("never", "always")

# Panel 0's elements in screen points (x, y, w, h): docs/controls/keypad.md, "Panel 0".
DPAD = (0, 180, 168, 136)
ATTACK = (405, 277, 70, 34)
PAD_RECTS: tuple[tuple[int, int, int, int], ...] = (
    DPAD,
    (335, 277, 70, 34),  # jump (and its icon)
    ATTACK,
    (446, 224, 34, 31),  # short keys 1-4
    (446, 178, 34, 31),
    (446, 132, 34, 31),
    (446, 86, 34, 31),
    (0, 0, 52, 52),  # Map
    (383, 0, 50, 52),  # Bag
    (433, 0, 47, 52),  # Cash
)
# The screens where the game shows the pad: hidden must differ there.
PAD_SHOWN = ("first_map",)


def _inside(rect: tuple[int, int, int, int], x: int, y: int) -> bool:
    rx, ry, rw, rh = rect
    return rx <= x < rx + rw and ry <= y < ry + rh


@dataclass(frozen=True)
class Diff:
    """Where two screens differ: pixels outside the pad's rects, in the d-pad, in attack."""

    all: int = 0
    outside: int = 0
    dpad: int = 0
    attack: int = 0


def diff(a: png.Image, b: png.Image) -> Diff:
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"sizes differ: {a.width}x{a.height} and {b.width}x{b.height}")
    n = outside = dpad = attack = 0
    for i in range(a.width * a.height):
        o = 4 * i
        if a.rgba[o : o + 4] == b.rgba[o : o + 4]:
            continue
        x, y = i % a.width, i // a.width
        n += 1
        dpad += _inside(DPAD, x, y)
        attack += _inside(ATTACK, x, y)
        outside += not any(_inside(r, x, y) for r in PAD_RECTS)
    return Diff(n, outside, dpad, attack)


def check_screen(name: str, shown: png.Image, hidden: png.Image) -> tuple[bool, str]:
    """The composited screens of one shot, the pad shown and hidden."""
    d = diff(shown, hidden)
    if name in PAD_SHOWN:
        ok = d.outside == 0 and d.dpad > 0 and d.attack > 0
        what = (
            f"{d.all} pixels differ, {d.outside} outside the key pad's rects, {d.dpad} in the "
            f"d-pad, {d.attack} in the attack key"
        )
    else:
        ok = d.all == 0
        what = "identical (the game has the pad's panel off)" if ok else f"{d.all} pixels differ"
    return ok, what


def run_all(
    spec: dg.Spec, command: Sequence[str], golden_dir: Path, out: Path, timeout: float
) -> dg.Report:
    report = dg.Report([])
    run = next((r for r in spec.runs if r.name == RUN), None)
    if run is None or run.data != "empty":
        raise dg.SpecError(f"the screen list has no run {RUN!r} from an empty data directory")
    screens = spec.screens_of(RUN)
    out.mkdir(parents=True, exist_ok=True)
    logs: dict[str, str] = {}
    for mode in MODES:
        run_dir = out / mode
        status, what = dg._run_game(
            command,
            spec,
            run,
            run_dir,
            None,
            timeout,
            extra=("--hide-keypad", mode, "--shot-gl"),
        )
        print(f"run {RUN} --hide-keypad {mode}: {what}", flush=True)
        if status == dg.EXIT_NO_GAME:
            report.no_game = True
            report.add("SKIP", f"the game did not start ({what}; log: {run_dir / 'log.txt'})")
            return report
        if status != 0:
            report.add("FAIL", f"--hide-keypad {mode}: {what} (log: {run_dir / 'log.txt'})")
            return report
        logs[mode] = (run_dir / "log.txt").read_text(encoding="utf-8", errors="replace")

    always_ok = (
        "hide_keypad always: the key pad is hidden" in logs["always"]
        and "the key pad shows" not in logs["always"]
    )
    report.add(
        "PASS" if always_ok else "FAIL",
        "always: the log says the key pad is hidden from the start and never shows it",
    )
    never_ok = "the key pad hides" not in logs["never"]
    report.add("PASS" if never_ok else "FAIL", "never: the log never hides the key pad")

    for s in screens:
        try:
            shown = png.load(out / "never" / f"{s.name}.png")
            hidden = png.load(out / "always" / f"{s.name}.png")
            gl_shown = png.load(out / "never" / f"{s.name}.gl.png")
            gl_hidden = png.load(out / "always" / f"{s.name}.gl.png")
        except (OSError, ValueError) as e:
            report.add("FAIL", f"{s.name}: a shot is missing: {e}")
            continue
        same_gl = gl_shown == gl_hidden
        report.add(
            "PASS" if same_gl else "FAIL",
            f"{s.name}: the game's GL frame is {'identical' if same_gl else 'DIFFERENT'} "
            "with the key pad hidden",
        )
        ok, what = check_screen(s.name, shown, hidden)
        report.add("PASS" if ok else "FAIL", f"{s.name}: hidden vs shown: {what}")
        golden = golden_dir / "ours" / f"{s.name}.png"
        if golden.exists():
            same = png.load(golden) == shown
            report.add(
                "PASS" if same else "FAIL",
                f"{s.name}: shown = the device golden's render ({golden}): "
                f"{'identical' if same else 'DIFFERENT'}",
            )
        else:
            report.add("SKIP", f"{s.name}: no device golden render at {golden}")
    return report


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.keypadhide", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument(
        "--binary", type=Path, required=True, help="MapleThief, built with the generated code"
    )
    parser.add_argument("--screens", type=Path, default=Path("tests/golden/device_screens.json"))
    parser.add_argument("--golden-dir", type=Path, default=Path("work/golden"))
    parser.add_argument("--out", type=Path, default=Path("build/test-output/keypad_hide"))
    parser.add_argument(
        "--timeout", type=float, default=1800.0, help="seconds per run (default 1800)"
    )
    args = parser.parse_args(argv)
    try:
        spec = dg.load_spec(args.screens)
    except dg.SpecError as e:
        print(f"keypadhide: {e}", file=sys.stderr)
        return 2
    if not args.binary.is_file():
        print(f"SKIP   {args.binary}: no such program (build MapleThief with work/gen)")
        return SKIP
    try:
        report = run_all(spec, [str(args.binary)], args.golden_dir, args.out, args.timeout)
    except dg.SpecError as e:
        print(f"keypadhide: {e}", file=sys.stderr)
        return 2
    print(
        f"keypad_hide: {report.passed} passed, {report.failed} failed, {report.skipped} skipped "
        f"(output in {args.out})"
    )
    if report.failed:
        return 1
    return SKIP if report.no_game or report.passed == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
