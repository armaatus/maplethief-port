"""The device golden harness (#60, ADR 0044): our frames of the listed screens against device
screenshots and against our own recorded renders.

``tests/golden/device_screens.json`` lists the screens (title, main menu, NPC dialog, ...), the
runs that reach them and how: taps at fixed times of virtual time, run by ``MapleThief
--fixed-step`` so that every run reaches the same frames bit for bit. For each run the harness
starts the game headless (SDL's dummy video driver) with a data directory of its own (empty,
or a copy of what an earlier run left: the second launch of the character select), and
collects the composited 480x320 screens (``--shot``). Then, per screen:

- **ours** (``<golden dir>/ours/<name>.png``, our own earlier render): must be identical
  (tolerance "ours"). With ``MAPLETHIEF_UPDATE_GOLDEN=1`` (or ``--update``) a missing or
  different one is (re)written; without one the case is skipped. This catches regressions in
  our renderer before any device screenshot exists.
- **device** (``<golden dir>/device/<name>.png``, a screenshot from the device): normalised
  (``mtport.goldencmp``: rotated, downsampled), compared with the screen's masks and the
  tolerance for its scale ("device_1x" or "device_2x"). Missing: skipped, "not captured yet".
  Never written by the harness.

Everything it writes goes to ``--out`` (CTest: ``build/test-output/<preset>/device_golden/``):
``runs/<run>/`` (the game's log, shots and data directory), and per screen
``<name>.actual.png``, ``<name>.ours.diff.png``, ``<name>.device.png`` (the normalised
screenshot) and ``<name>.device.diff.png``. The golden directory is ``work/golden/``: both
kinds of golden are game art and are never committed.

``--repeat N`` runs every run N times and fails unless the shots are identical each time (the
determinism check). Exit status: 0 nothing failed, 1 something failed, 77 skipped (the game
could not start, e.g. no ``work/ipa``, or every case was skipped), 2 bad usage or list.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from . import goldencmp, png
from .goldencmp import Rect, Tolerance

SKIP = 77
NAME = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
TOLERANCES = ("ours", "device_1x", "device_2x")
UPDATE_ENV = "MAPLETHIEF_UPDATE_GOLDEN"
# MapleThief's exit status when it cannot run the game (game.h: kExitNoGame).
EXIT_NO_GAME = 4


class SpecError(ValueError):
    """A screen list that does not hold together."""


@dataclass(frozen=True)
class Tap:
    at: float
    x: float
    y: float
    what: str


@dataclass(frozen=True)
class Run:
    name: str
    data: str  # "empty" or the name of an earlier run whose data directory it starts from
    taps: tuple[Tap, ...]


@dataclass(frozen=True)
class Mask:
    rect: Rect
    why: str


@dataclass(frozen=True)
class Screen:
    name: str
    run: str
    at: float
    what: str
    device: str
    masks: tuple[Mask, ...]


@dataclass(frozen=True)
class Spec:
    fixed_step_ms: float
    tolerance: dict[str, Tolerance]
    runs: tuple[Run, ...]
    screens: tuple[Screen, ...]

    def screens_of(self, run: str) -> list[Screen]:
        return [s for s in self.screens if s.run == run]


def _need(cond: bool, what: str) -> None:
    if not cond:
        raise SpecError(what)


def _number(value: object, what: str) -> float:
    _need(isinstance(value, (int, float)) and not isinstance(value, bool), f"{what}: not a number")
    return float(value)  # type: ignore[arg-type]


def _text(value: object, what: str) -> str:
    _need(isinstance(value, str) and value.strip() != "", f"{what}: needs a text")
    return value  # type: ignore[return-value]


def parse_spec(doc: object) -> Spec:
    """Checks and reads a screen list (the JSON of device_screens.json)."""
    _need(isinstance(doc, dict), "the screen list is not a JSON object")
    assert isinstance(doc, dict)
    _need(doc.get("version") == 1, "version: only 1 is known")
    step = _number(doc.get("fixed_step_ms"), "fixed_step_ms")
    _need(0.001 <= step <= 1000, "fixed_step_ms: 0.001 to 1000")
    tol = doc.get("tolerance")
    _need(
        isinstance(tol, dict) and set(tol) == set(TOLERANCES),
        f"tolerance: needs exactly {TOLERANCES}",
    )
    assert isinstance(tol, dict)
    tolerance = {}
    for key in TOLERANCES:
        t = tol[key]
        _need(isinstance(t, dict), f"tolerance.{key}: not an object")
        channel = t.get("channel")
        _need(
            isinstance(channel, int) and not isinstance(channel, bool),
            f"tolerance.{key}.channel: not an integer",
        )
        try:
            tolerance[key] = Tolerance(channel, _number(t.get("ratio"), f"tolerance.{key}.ratio"))
        except ValueError as e:
            raise SpecError(f"tolerance.{key}: {e}") from None
        _text(t.get("why"), f"tolerance.{key}.why")

    runs: list[Run] = []
    _need(isinstance(doc.get("runs"), list) and doc["runs"], "runs: needs a list")
    for i, r in enumerate(doc["runs"]):
        where = f"runs[{i}]"
        _need(isinstance(r, dict), f"{where}: not an object")
        name = r.get("name")
        _need(
            isinstance(name, str) and bool(NAME.match(name)), f"{where}.name: [A-Za-z0-9_-]{{1,64}}"
        )
        _need(all(x.name != name for x in runs), f"{where}: run {name!r} twice")
        data = r.get("data")
        _need(
            data == "empty" or any(x.name == data for x in runs),
            f"{where}.data: 'empty' or the name of an earlier run, got {data!r}",
        )
        _text(r.get("why"), f"{where}.why")
        taps = []
        _need(isinstance(r.get("taps"), list), f"{where}.taps: needs a list")
        for j, t in enumerate(r["taps"]):
            tw = f"{where}.taps[{j}]"
            _need(isinstance(t, dict), f"{tw}: not an object")
            tap = Tap(
                _number(t.get("at"), f"{tw}.at"),
                _number(t.get("x"), f"{tw}.x"),
                _number(t.get("y"), f"{tw}.y"),
                _text(t.get("what"), f"{tw}.what"),
            )
            _need(tap.at >= 0, f"{tw}.at: negative")
            _need(
                0 <= tap.x < goldencmp.SCREEN_WIDTH and 0 <= tap.y < goldencmp.SCREEN_HEIGHT,
                f"{tw}: ({tap.x}, {tap.y}) is off the 480x320 screen",
            )
            _need(not taps or tap.at > taps[-1].at, f"{tw}.at: taps must be in time order")
            taps.append(tap)
        runs.append(Run(name, data, tuple(taps)))

    screens: list[Screen] = []
    _need(isinstance(doc.get("screens"), list) and doc["screens"], "screens: needs a list")
    for i, s in enumerate(doc["screens"]):
        where = f"screens[{i}]"
        _need(isinstance(s, dict), f"{where}: not an object")
        name = s.get("name")
        _need(
            isinstance(name, str) and bool(NAME.match(name)), f"{where}.name: [A-Za-z0-9_-]{{1,64}}"
        )
        _need(all(x.name != name for x in screens), f"{where}: screen {name!r} twice")
        run = s.get("run")
        _need(any(r.name == run for r in runs), f"{where}.run: no run {run!r}")
        at = _number(s.get("at"), f"{where}.at")
        _need(at >= 0, f"{where}.at: negative")
        masks = []
        _need(isinstance(s.get("masks"), list), f"{where}.masks: needs a list")
        for j, m in enumerate(s["masks"]):
            mw = f"{where}.masks[{j}]"
            _need(isinstance(m, dict), f"{mw}: not an object")
            try:
                rect = Rect.of(m.get("rect"))
            except (TypeError, ValueError) as e:
                raise SpecError(f"{mw}.rect: {e}") from None
            _need(
                rect.inside(goldencmp.SCREEN_WIDTH, goldencmp.SCREEN_HEIGHT),
                f"{mw}.rect: off the screen",
            )
            masks.append(Mask(rect, _text(m.get("why"), f"{mw}.why")))
        screens.append(
            Screen(
                name,
                str(run),
                at,
                _text(s.get("what"), f"{where}.what"),
                _text(s.get("device"), f"{where}.device"),
                tuple(masks),
            )
        )
    for r in runs:
        _need(
            bool([s for s in screens if s.run == r.name]) or any(x.data == r.name for x in runs),
            f"run {r.name!r}: no screen and no later run uses it",
        )
    return Spec(step, tolerance, tuple(runs), tuple(screens))


def load_spec(path: Path) -> Spec:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise SpecError(f"{path}: {e}") from None
    return parse_spec(doc)


def _num(v: float) -> str:
    return f"{v:g}"


def game_args(spec: Spec, run: Run, capture_dir: Path) -> list[str]:
    """MapleThief's arguments for a run: the fixed step, its taps and its screens' shots.

    skip_intros is pinned off (#174, ADR 0096): it runs the guest clock x8 through scenes, so the
    timed taps and shots would land on other frames; a suite run with
    MAPLETHIEF_SETTINGS="skip_intros = on" must still pass, and the flag wins over it.
    """
    args = ["--fixed-step", _num(spec.fixed_step_ms), "--skip-intros", "off"]
    args += ["--capture", str(capture_dir)]
    for t in run.taps:
        args += ["--tap", f"{_num(t.at)},{_num(t.x)},{_num(t.y)}"]
    for s in spec.screens_of(run.name):
        args += ["--shot", f"{_num(s.at)},{s.name}"]
    return args


@dataclass
class Report:
    lines: list[str]
    failed: int = 0
    passed: int = 0
    skipped: int = 0
    no_game: bool = False

    def add(self, verdict: str, text: str) -> None:
        if verdict == "FAIL":
            self.failed += 1
        elif verdict == "SKIP":
            self.skipped += 1
        else:
            self.passed += 1
        self.lines.append(f"{verdict:6} {text}")
        print(f"{verdict:6} {text}", flush=True)

    def status(self) -> int:
        if self.failed:
            return 1
        if self.no_game or self.passed == 0:
            return SKIP
        return 0


def _run_game(
    command: Sequence[str],
    spec: Spec,
    run: Run,
    run_dir: Path,
    data_from: Path | None,
    timeout: float,
    extra: Sequence[str] = (),
    env: Mapping[str, str] | None = None,
    headless: bool = True,
) -> tuple[int, str]:
    """Runs the game once into run_dir. ``env`` adds variables. ``headless=False`` keeps the
    platform's video driver instead of SDL's dummy one: a window opens (the GPU backend needs a
    real video driver, #120)."""
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    data = run_dir / "data"
    if data_from is not None:
        shutil.copytree(data_from, data)
    else:
        data.mkdir()
    run_env = dict(os.environ, MAPLETHIEF_DATA_DIR=str(data), **(env or {}))
    if headless:
        run_env["SDL_VIDEO_DRIVER"] = "dummy"
    else:
        run_env.pop("SDL_VIDEO_DRIVER", None)
    argv = [*command, *game_args(spec, run, run_dir), *extra]
    with open(run_dir / "log.txt", "w", encoding="utf-8") as log:
        log.write(" ".join(argv) + "\n")
        log.flush()
        try:
            p = subprocess.run(
                argv, env=run_env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout
            )
        except subprocess.TimeoutExpired:
            return -1, f"timed out after {timeout:g} s"
        except OSError as e:
            return -1, f"cannot start {argv[0]}: {e}"
    return p.returncode, f"exit status {p.returncode}"


def _same_pixels(a: Path, b: Path) -> bool:
    try:
        return png.load(a).rgba == png.load(b).rgba
    except (OSError, ValueError):
        return False


def run_all(
    spec: Spec,
    command: Sequence[str],
    golden_dir: Path,
    out: Path,
    *,
    only: Sequence[str] = (),
    repeat: int = 1,
    update: bool = False,
    timeout: float = 1800.0,
) -> Report:
    """Runs the game for every run that a selected screen needs, then compares each screen."""
    report = Report([])
    selected = [s for s in spec.screens if not only or s.name in only]
    unknown = [n for n in only if all(s.name != n for s in spec.screens)]
    if unknown:
        raise SpecError(f"no screen {', '.join(unknown)}")
    # The runs needed: those of the selected screens and the runs whose data they start from.
    needed: set[str] = set()
    for s in selected:
        name: str | None = s.run
        while name is not None and name != "empty":
            needed.add(name)
            name = next(r.data for r in spec.runs if r.name == name)
    out.mkdir(parents=True, exist_ok=True)
    run_dirs: dict[str, Path] = {}
    broken: dict[str, str] = {}
    for run in spec.runs:
        if run.name not in needed:
            continue
        data_from = None
        if run.data != "empty":
            if run.data in broken:
                broken[run.name] = f"its data comes from run {run.data}, which failed"
                continue
            data_from = run_dirs[run.data] / "data"
        dirs = []
        for k in range(repeat):
            run_dir = out / "runs" / (run.name if k == 0 else f"{run.name}.{k + 1}")
            status, what = _run_game(command, spec, run, run_dir, data_from, timeout)
            print(f"run {run.name}{'' if k == 0 else f' (repeat {k + 1})'}: {what}", flush=True)
            if status == EXIT_NO_GAME:
                report.no_game = True
                report.add(
                    "SKIP",
                    f"run {run.name}: the game did not start ({what}: no generated "
                    "code, no work/ipa bundle or no slice; see runs/*/log.txt)",
                )
                return report
            if status != 0:
                broken[run.name] = f"{what} (log: {run_dir / 'log.txt'})"
                break
            dirs.append(run_dir)
        if run.name in broken:
            continue
        run_dirs[run.name] = dirs[0]
        for s in spec.screens_of(run.name):
            if repeat > 1 and s in selected:
                same = all(
                    _same_pixels(dirs[0] / f"{s.name}.png", d / f"{s.name}.png") for d in dirs[1:]
                )
                report.add(
                    "PASS" if same else "FAIL",
                    f"{s.name}: deterministic: {'identical' if same else 'NOT identical'} "
                    f"over {repeat} runs",
                )

    for s in selected:
        if s.run in broken:
            report.add("FAIL", f"{s.name}: run {s.run} failed: {broken[s.run]}")
            continue
        shot = run_dirs[s.run] / f"{s.name}.png"
        try:
            actual = png.load(shot)
        except (OSError, ValueError) as e:
            report.add("FAIL", f"{s.name}: no shot from run {s.run}: {e}")
            continue
        goldencmp.save(out / f"{s.name}.actual.png", actual)
        _compare_ours(report, spec, s, actual, golden_dir / "ours" / f"{s.name}.png", out, update)
        _compare_device(report, spec, s, actual, golden_dir / "device" / f"{s.name}.png", out)
    return report


def _compare_ours(
    report: Report, spec: Spec, s: Screen, actual: png.Image, golden: Path, out: Path, update: bool
) -> None:
    if not golden.exists():
        if update:
            golden.parent.mkdir(parents=True, exist_ok=True)
            goldencmp.save(golden, actual)
            report.add("UPDATE", f"{s.name} ours: recorded {golden}")
        else:
            report.add(
                "SKIP", f"{s.name} ours: no recorded render at {golden} ({UPDATE_ENV}=1 records it)"
            )
        return
    try:
        expected = png.load(golden)
    except (OSError, ValueError) as e:
        report.add("FAIL", f"{s.name} ours: {golden}: {e}")
        return
    result = goldencmp.compare(actual, expected, spec.tolerance["ours"])
    diff = out / f"{s.name}.ours.diff.png"
    if result.matches:
        diff.unlink(missing_ok=True)
        report.add("PASS", f"{s.name} ours: {result.summary()}")
    elif update:
        goldencmp.save(golden, actual)
        report.add("UPDATE", f"{s.name} ours: re-recorded {golden} (was: {result.summary()})")
    else:
        goldencmp.save(diff, result.diff)
        report.add("FAIL", f"{s.name} ours: {result.summary()}; diff {diff}")


def _compare_device(
    report: Report, spec: Spec, s: Screen, actual: png.Image, golden: Path, out: Path
) -> None:
    if not golden.exists():
        report.add("SKIP", f"{s.name} device: not captured yet ({golden}; tests/golden/README.md)")
        return
    try:
        n = goldencmp.normalise(png.load(golden))
    except (OSError, ValueError) as e:
        report.add("FAIL", f"{s.name} device: {golden}: {e}")
        return
    goldencmp.save(out / f"{s.name}.device.png", n.image)
    tolerance = spec.tolerance["device_2x" if n.scale == 2 else "device_1x"]
    result = goldencmp.compare(actual, n.image, tolerance, [m.rect for m in s.masks])
    diff = out / f"{s.name}.device.diff.png"
    goldencmp.save(diff, result.diff)
    steps = f" [{'; '.join(n.steps)}]" if n.steps else ""
    report.add(
        "PASS" if result.matches else "FAIL",
        f"{s.name} device ({n.source_size[0]}x{n.source_size[1]}){steps}: "
        f"{result.summary()}; diff {diff}",
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.devicegolden", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument(
        "--binary", type=Path, required=True, help="MapleThief, built with the generated code"
    )
    parser.add_argument("--screens", type=Path, default=Path("tests/golden/device_screens.json"))
    parser.add_argument("--golden-dir", type=Path, default=Path("work/golden"))
    parser.add_argument("--out", type=Path, default=Path("build/test-output/device_golden"))
    parser.add_argument("--only", default="", help="comma-separated screen names")
    parser.add_argument(
        "--repeat", type=int, default=1, help="run each run N times; the shots must be identical"
    )
    parser.add_argument("--arch", help="run the binary under `arch -ARCH` (macOS, e.g. x86_64)")
    parser.add_argument(
        "--update", action="store_true", help=f"record our renders (same as {UPDATE_ENV}=1)"
    )
    parser.add_argument(
        "--timeout", type=float, default=1800.0, help="seconds per run (default 1800)"
    )
    args = parser.parse_args(argv)
    try:
        spec = load_spec(args.screens)
    except SpecError as e:
        print(f"devicegolden: {e}", file=sys.stderr)
        return 2
    if args.repeat < 1:
        print("devicegolden: --repeat needs 1 or more", file=sys.stderr)
        return 2
    if not args.binary.is_file():
        print(f"SKIP   {args.binary}: no such program (build MapleThief with work/gen)")
        return SKIP
    command = [str(args.binary)]
    if args.arch:
        command = ["arch", f"-{args.arch}", *command]
    update = args.update or os.environ.get(UPDATE_ENV) == "1"
    only = [n for n in args.only.split(",") if n]
    try:
        report = run_all(
            spec,
            command,
            args.golden_dir,
            args.out,
            only=only,
            repeat=args.repeat,
            update=update,
            timeout=args.timeout,
        )
    except SpecError as e:
        print(f"devicegolden: {e}", file=sys.stderr)
        return 2
    print(
        f"device_golden: {report.passed} passed, {report.failed} failed, {report.skipped} skipped "
        f"(output in {args.out})"
    )
    return report.status()


if __name__ == "__main__":
    sys.exit(main())
