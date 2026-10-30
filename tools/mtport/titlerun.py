"""The title for a minute (#74): the boot to the title, 60 s on it, and the guest functions it ran.

The game has no title screen of its own: the "title" of the golden tests is the main menu with
its promo banner after the 2 s slide-in ([ANALYSIS.md, Program flow](../../docs/ANALYSIS.md),
ADR 0044). Two commands:

``soak`` runs ``MapleThief --fixed-step 16.667 --seconds 60`` headless (SDL's dummy drivers)
on a fresh data directory, with composited shots (``--shot``) during the slide-in and every
10 s after it, and the unimplemented report (``MAPLETHIEF_REPORT``) written to a file. It
fails unless:

- the run ends cleanly at 60 s of virtual time, ended by the host (no trap, no guest crash,
  exit status 0);
- the unimplemented report reads ``unimplemented report: none`` (#53);
- the GL counters show no misuse and no unsupported or invalid draw;
- the game's frame loop keeps going for the whole minute: between two shots the GL frame
  count grows by at least :data:`MIN_GL_FRAMES_PER_SECOND` a second (one ``Paint`` and
  ``presentRenderbuffer:`` a tick, 20 ticks a second by default);
- it animates: the slide-in's shots all differ from each other and from the still title;
- the still title stays put and drawn: every shot from 2.5 s to 60 s is the same picture, and
  not black.

``coverage`` configures a build of its own with ``MAPLETHIEF_TRACE_ENTRY=ON`` (the
function-entry coverage of ADR 0030), builds MapleThief and the guest symbol table, runs the
same soak with ``MAPLETHIEF_COVERAGE`` and ``MAPLETHIEF_SYMBOLS``, and writes
``title.coverage`` (the runtime's list: pointer, entries, name) and ``title-coverage.md`` (a
summary: functions entered of those discovered, by class, the most entered). The fixed-step
run is deterministic, so the report is the same on every run of the same code. Everything goes
to ``--out`` (default ``build/title-coverage/report``), which is derived from the IPA and never
committed.

Exit status: 0 passed, 1 failed, 77 skipped (no program, or the program cannot run the game:
no generated code or no ``work/ipa``), 2 bad usage.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from . import png

SKIP = 77
# MapleThief's exit status when it cannot run the game (game.h: kExitNoGame).
EXIT_NO_GAME = 4
SECONDS = 60.0
FIXED_STEP_MS = "16.667"
# (name, virtual seconds after the first run-loop turn).
# Shot names are [A-Za-z0-9_-] (MapleThief --shot): the time in milliseconds. Until about
# 0.75 s the screen shows the main menu's background and the banner; the logo, the character and
# Play then slide in until about 1.3 s, and from there nothing moves.
SLIDE_SHOTS = (
    ("slide_500ms", 0.5),
    ("slide_900ms", 0.9),
    ("slide_1000ms", 1.0),
    ("slide_1100ms", 1.1),
    ("slide_1200ms", 1.2),
)
STILL_SHOTS = (
    ("title_2500ms", 2.5),
    ("title_10s", 10.0),
    ("title_20s", 20.0),
    ("title_30s", 30.0),
    ("title_40s", 40.0),
    ("title_50s", 50.0),
    ("title_60s", 60.0),
)
# The slowest speed setting ticks every 65 ms (15.4 a second): below that the loop has stalled.
MIN_GL_FRAMES_PER_SECOND = 14.0
REPORT_NONE = "unimplemented report: none"

SHOT_LINE = re.compile(
    r"game: shot (?P<name>\S+) at (?P<at>[0-9.]+) s \(turn (?P<turn>\d+), GL frame (?P<gl>\d+)\)"
)
END_LINE = re.compile(
    r"game: (?P<outcome>\w+) after [0-9.]+ s: (?P<turns>\d+) turns, .*?(?P<gl>\d+) GL frames"
)
VIRTUAL_LINE = re.compile(r"game: virtual time at the end: (?P<t>[0-9.]+) s")
DRAWS_LINE = re.compile(
    r"game: GL draws: (?P<drawn>\d+) drawn, (?P<skipped>\d+) skipped, (?P<misuses>\d+) misuses; "
    r"mt_gfx: .*?(?P<unsupported>\d+) unsupported, (?P<invalid>\d+) invalid"
)
STOP_LINE = re.compile(r"game: stop reason: (?P<reason>.*)")
ERROR_LINES = (
    re.compile(r"=== guest crash"),
    re.compile(r"game: trap kind"),
    re.compile(r"game: exit status"),
)


@dataclass(frozen=True)
class Shot:
    name: str
    at: float
    turn: int
    gl_frame: int


@dataclass
class RunLog:
    """What the checks need from MapleThief's log."""

    shots: dict[str, Shot] = field(default_factory=dict)
    outcome: str = ""
    gl_frames: int = -1
    virtual_end: float = -1.0
    draws: dict[str, int] = field(default_factory=dict)
    stop_reason: str = ""
    errors: list[str] = field(default_factory=list)


def parse_log(text: str) -> RunLog:
    log = RunLog()
    for line in text.splitlines():
        if m := SHOT_LINE.search(line):
            log.shots[m["name"]] = Shot(m["name"], float(m["at"]), int(m["turn"]), int(m["gl"]))
        elif m := END_LINE.search(line):
            log.outcome = m["outcome"]
            log.gl_frames = int(m["gl"])
        elif m := VIRTUAL_LINE.search(line):
            log.virtual_end = float(m["t"])
        elif m := DRAWS_LINE.search(line):
            log.draws = {k: int(v) for k, v in m.groupdict().items()}
        elif m := STOP_LINE.search(line):
            log.stop_reason = m["reason"].strip()
        if any(p.search(line) for p in ERROR_LINES):
            log.errors.append(line.strip())
    return log


def game_args(capture: Path, seconds: float = SECONDS) -> list[str]:
    shots = [s for s in (*SLIDE_SHOTS, *STILL_SHOTS) if s[1] <= seconds]
    args = ["--fixed-step", FIXED_STEP_MS, "--seconds", f"{seconds:g}", "--capture", str(capture)]
    for name, at in shots:
        args += ["--shot", f"{at:g},{name}"]
    return args


def _is_black(image: png.Image) -> bool:
    rgba = image.rgba
    return not any(rgba[i] or rgba[i + 1] or rgba[i + 2] for i in range(0, len(rgba), 4))


def check(
    log: RunLog,
    returncode: int,
    report: str | None,
    images: dict[str, png.Image],
    seconds: float = SECONDS,
) -> list[str]:
    """The soak's checks (see the module docstring): the failures, empty if it passed."""
    fails: list[str] = []
    if returncode != 0:
        fails.append(f"MapleThief exited with status {returncode}")
    fails += [f"error in the log: {e}" for e in log.errors]
    if log.outcome != "stopped" or "the host ended the run" not in log.stop_reason:
        fails.append(
            f"the run did not end by the host: outcome {log.outcome or '?'}, "
            f"stop reason {log.stop_reason or '?'}"
        )
    if log.virtual_end < seconds - 0.05:
        fails.append(f"the run ended at {log.virtual_end:.3f} s of virtual time, not {seconds:g} s")
    if report is None:
        fails.append("no unimplemented report was written")
    elif report.strip() != REPORT_NONE:
        fails.append("the unimplemented report is not empty:\n" + report.strip())
    if not log.draws:
        fails.append("no GL draw counters in the log")
    else:
        for key in ("misuses", "unsupported", "invalid"):
            if log.draws[key] != 0:
                fails.append(f"GL draws: {log.draws[key]} {key}")
    expected = [s for s in (*SLIDE_SHOTS, *STILL_SHOTS) if s[1] <= seconds]
    missing = [name for name, _ in expected if name not in log.shots or name not in images]
    if missing:
        fails.append("shots missing: " + ", ".join(missing))
        return fails
    # The frame loop: GL frames keep coming between consecutive shots.
    timeline = sorted((log.shots[name] for name, _ in expected), key=lambda s: s.at)
    for a, b in zip(timeline, timeline[1:], strict=False):
        rate = (b.gl_frame - a.gl_frame) / (b.at - a.at)
        if rate < MIN_GL_FRAMES_PER_SECOND:
            fails.append(
                f"GL frames {a.gl_frame} at {a.at:g} s to {b.gl_frame} at {b.at:g} s: "
                f"{rate:.1f} a second, below {MIN_GL_FRAMES_PER_SECOND:g}"
            )
    # The slide-in animates: every slide shot is a picture of its own.
    slide = [name for name, _ in SLIDE_SHOTS if name in images]
    still = [name for name, _ in STILL_SHOTS if name in images]
    pictures = [*slide, still[0]] if still else slide
    for i, a in enumerate(pictures):
        for b in pictures[i + 1 :]:
            if images[a].rgba == images[b].rgba:
                fails.append(f"{a} and {b} are the same picture: the slide-in does not animate")
    # The still title stays put and drawn.
    for name in still:
        if _is_black(images[name]):
            fails.append(f"{name} is black")
        if images[name].rgba != images[still[0]].rgba:
            fails.append(f"{name} differs from {still[0]}: the still title moved")
    return fails


def _find_program(build_dir: Path) -> Path | None:
    for rel in (
        "platform/MapleThief.app/Contents/MacOS/MapleThief",
        "platform/MapleThief",
        "platform/MapleThief.exe",
    ):
        if (build_dir / rel).is_file():
            return build_dir / rel
    return None


def run_soak(
    command: Sequence[str],
    out: Path,
    *,
    seconds: float = SECONDS,
    coverage: Path | None = None,
    symbols: Path | None = None,
    timeout: float = 1800.0,
) -> int:
    """Runs the soak into ``out`` and prints its verdict; returns the exit status."""
    if out.exists():
        shutil.rmtree(out)
    data = out / "data"
    data.mkdir(parents=True)
    report_path = out / "unimplemented-report.txt"
    env = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        MAPLETHIEF_DATA_DIR=str(data),
        MAPLETHIEF_REPORT=str(report_path),
    )
    for var in ("MAPLETHIEF_COVERAGE", "MAPLETHIEF_SYMBOLS"):
        env.pop(var, None)
    if coverage is not None:
        env["MAPLETHIEF_COVERAGE"] = str(coverage)
    if symbols is not None:
        env["MAPLETHIEF_SYMBOLS"] = str(symbols)
    argv = [*command, *game_args(out, seconds)]
    log_path = out / "log.txt"
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(" ".join(argv) + "\n")
        f.flush()
        try:
            p = subprocess.run(argv, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"FAIL   title: MapleThief timed out after {timeout:g} s (log: {log_path})")
            return 1
        except OSError as e:
            print(f"SKIP   title: cannot start {argv[0]}: {e}")
            return SKIP
    if p.returncode == EXIT_NO_GAME:
        print(f"SKIP   title: MapleThief cannot run the game (log: {log_path})")
        return SKIP
    log = parse_log(log_path.read_text(encoding="utf-8", errors="replace"))
    report = report_path.read_text(encoding="utf-8") if report_path.is_file() else None
    images: dict[str, png.Image] = {}
    for name in log.shots:
        try:
            images[name] = png.load(out / f"{name}.png")
        except (OSError, ValueError):
            pass
    fails = check(log, p.returncode, report, images, seconds)
    for fail in fails:
        print(f"FAIL   title: {fail}")
    if fails:
        print(f"title: {len(fails)} checks failed (log: {log_path})")
        return 1
    last = max(log.shots.values(), key=lambda s: s.at)
    print(
        f"PASS   title: {seconds:g} s of virtual time without errors: {log.gl_frames} GL frames "
        f"({last.gl_frame} by {last.at:g} s), {len(SLIDE_SHOTS)} slide-in shots all different, "
        f"{len(STILL_SHOTS)} title shots identical and not black; {REPORT_NONE} "
        f"(shots and log in {out})"
    )
    return 0


# --- coverage -------------------------------------------------------------------------------


@dataclass(frozen=True)
class Covered:
    ptr: int
    entries: int
    name: str


COVERAGE_HEADER = re.compile(r"^# maplethief coverage 1: (\d+) guest functions entered")
COVERAGE_LINE = re.compile(r"^0x([0-9A-Fa-f]{8}) (\d+) (.+)$")
UNNAMED = re.compile(r"^(f|FUN)_[0-9A-Fa-f]+$")


def parse_coverage(text: str) -> list[Covered]:
    """The runtime's coverage file (mt_coverage_format, maplethief/diag.h). Raises ValueError."""
    lines = text.splitlines()
    if not lines or not (m := COVERAGE_HEADER.match(lines[0])):
        raise ValueError("not a maplethief coverage file (no '# maplethief coverage 1' header)")
    out = []
    for i, line in enumerate(lines[1:], 2):
        if not line.strip():
            continue
        c = COVERAGE_LINE.match(line)
        if c is None:
            raise ValueError(f"line {i}: not 'POINTER ENTRIES NAME': {line!r}")
        out.append(Covered(int(c[1], 16), int(c[2]), c[3]))
    if len(out) != int(m[1]):
        raise ValueError(f"the header says {m[1]} functions, the file lists {len(out)}")
    return out


def group_of(name: str) -> str:
    """The class of a symbol: ``-[Class sel]`` and ``Class::method`` give ``Class``; a name
    without one is "(functions)", an unnamed function "(unnamed)"."""
    if UNNAMED.match(name):
        return "(unnamed)"
    if name[:2] in ("-[", "+[") and " " in name:
        return name[2:].split(" ", 1)[0].split("(", 1)[0]
    head = name.split("(", 1)[0]
    if "::" in head:
        return head.rsplit("::", 1)[0]
    return "(functions)"


def summarise(covered: Sequence[Covered], discovered: int | None, top: int = 25) -> str:
    """The Markdown summary of a coverage run."""
    total = sum(c.entries for c in covered)
    named = sum(1 for c in covered if not UNNAMED.match(c.name))
    lines = [
        "# Guest functions on the path to the title (#74)",
        "",
        "Boot, launch, the main menu's slide-in and 60 s on the still title: "
        f"`MapleThief --fixed-step {FIXED_STEP_MS} --seconds {SECONDS:g}` built with "
        "`MAPLETHIEF_TRACE_ENTRY=ON` (`python3 -m mtport.titlerun coverage`). Derived from the "
        "IPA: never committed.",
        "",
    ]
    of = ""
    if discovered:
        of = f" of {discovered:,} discovered ({100.0 * len(covered) / discovered:.1f}%)"
    lines += [
        f"- **{len(covered):,} guest functions entered**{of}, {named:,} of them named.",
        f"- {total:,} entries in all.",
        "",
        "## By class",
        "",
        "| Class | Functions | Entries |",
        "|---|---:|---:|",
    ]
    groups: Counter[str] = Counter()
    entries: Counter[str] = Counter()
    for c in covered:
        groups[group_of(c.name)] += 1
        entries[group_of(c.name)] += c.entries
    for g, n in sorted(groups.items(), key=lambda kv: (-kv[1], kv[0])):
        lines.append(f"| `{g}` | {n:,} | {entries[g]:,} |")
    lines += [
        "",
        f"## The {top} most entered",
        "",
        "| Function | Pointer | Entries |",
        "|---|---|---:|",
    ]
    for c in sorted(covered, key=lambda c: (-c.entries, c.ptr))[:top]:
        lines.append(f"| `{c.name}` | `0x{c.ptr:08X}` | {c.entries:,} |")
    lines += [
        "",
        "The full list (pointer, entries, name) is `title.coverage` beside this file.",
        "",
    ]
    return "\n".join(lines)


def _default_preset() -> str:
    system = platform.system()
    return {"Darwin": "mac-debug", "Windows": "win-debug"}.get(system, "linux-debug")


def _discovered(repo: Path) -> int | None:
    try:
        doc = json.loads((repo / "work" / "recomp" / "functions.json").read_text(encoding="utf-8"))
        return int(doc["counts"]["functions"])
    except (OSError, ValueError, KeyError, TypeError):
        return None


def run_coverage(repo: Path, build_dir: Path, preset: str, out: Path, timeout: float) -> int:
    if not (repo / "work" / "gen" / "gen.cmake").is_file():
        print("SKIP   coverage: no work/gen (python3 -m recomp all writes it)")
        return SKIP
    configure = ["cmake", "--preset", preset, "-B", str(build_dir), "-DMAPLETHIEF_TRACE_ENTRY=ON"]
    build = ["cmake", "--build", str(build_dir), "--target", "MapleThief", "maplethief_symbols"]
    for cmd in (configure, build):
        print("+ " + " ".join(cmd), flush=True)
        if subprocess.run(cmd, cwd=repo).returncode != 0:
            print("FAIL   coverage: the build failed")
            return 1
    program = _find_program(build_dir)
    if program is None:
        print(f"FAIL   coverage: no MapleThief in {build_dir}/platform")
        return 1
    out.mkdir(parents=True, exist_ok=True)
    raw = out / "title.coverage"
    raw.unlink(missing_ok=True)
    status = run_soak(
        [str(program)],
        out / "run",
        coverage=raw,
        symbols=build_dir / "maplethief.sym",
        timeout=timeout,
    )
    if status != 0:
        return status
    try:
        covered = parse_coverage(raw.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"FAIL   coverage: {raw}: {e}")
        return 1
    if not covered:
        print(
            f"FAIL   coverage: {raw} lists no function (was it built with MAPLETHIEF_TRACE_ENTRY?)"
        )
        return 1
    summary = out / "title-coverage.md"
    discovered = _discovered(repo)
    summary.write_text(summarise(covered, discovered), encoding="utf-8")
    of = f" of {discovered:,}" if discovered else ""
    print(f"PASS   coverage: {len(covered):,}{of} guest functions entered; {summary}, {raw}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.titlerun", description=__doc__.split("\n\n")[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)
    soak = sub.add_parser("soak", help="60 s on the title, checked")
    soak.add_argument("--binary", type=Path, required=True, help="MapleThief, with generated code")
    soak.add_argument("--out", type=Path, default=Path("build/test-output/title_60s"))
    soak.add_argument("--seconds", type=float, default=SECONDS, help=argparse.SUPPRESS)
    soak.add_argument("--timeout", type=float, default=1800.0, help="seconds (default 1800)")
    cov = sub.add_parser("coverage", help="build with the entry coverage and record the path")
    cov.add_argument("--repo", type=Path, default=Path("."))
    cov.add_argument("--build-dir", type=Path, default=Path("build/title-coverage"))
    cov.add_argument("--preset", default=_default_preset(), help="configure preset to start from")
    cov.add_argument("--out", type=Path, help="default: <build dir>/report")
    cov.add_argument("--timeout", type=float, default=1800.0, help="seconds (default 1800)")
    args = parser.parse_args(argv)
    if args.command == "soak":
        if args.seconds < STILL_SHOTS[0][1] or args.seconds > SECONDS:
            print("titlerun: --seconds must be from 2.5 to 60", file=sys.stderr)
            return 2
        if not args.binary.is_file():
            print(f"SKIP   title: {args.binary}: no such program (build MapleThief with work/gen)")
            return SKIP
        return run_soak([str(args.binary)], args.out, seconds=args.seconds, timeout=args.timeout)
    repo = args.repo.resolve()
    build_dir = args.build_dir if args.build_dir.is_absolute() else repo / args.build_dir
    out = args.out or build_dir / "report"
    return run_coverage(repo, build_dir, args.preset, out.resolve(), args.timeout)


if __name__ == "__main__":
    sys.exit(main())
