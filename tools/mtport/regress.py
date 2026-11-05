"""The regression run (#91, ADR 0104): a recorded input replay played on the real game, its
checkpoints compared run with run, and with a baseline.

``play_real_test regress OUT_DIR`` (tests/cpp/play_real_test.cpp) boots the recompiled game on
the virtual clock (ADR 0044) from a fresh data directory and plays the replay ``MT_REPLAY``
(``tests/regress/title_to_combat.replay``: the title, Play, the intro, the first map and the
tutorial to the first fight) through the frontend's own input layer. At each checkpoint it hashes
the composited screen, the audio so far and reads a few facts from the game; the replay's
``expect`` lines must hold. It writes ``OUT_DIR/checkpoints.txt`` and prints its digest (FNV-1a
64 of that text).

This runner plays it ``--repeat`` times, each in ``<out>/run_NN``, and fails unless every run
passed and gave the same digest. ``--settings LIST`` / ``--preset NAME`` set
``MAPLETHIEF_SETTINGS`` (``preset=original`` ...), so a preset's run can be compared with
another build's: ``--save-baseline FILE`` writes the first run's checkpoints, ``--baseline FILE``
compares with them and names the first checkpoint that differs. Baselines hold hashes of our
renders and stay local (``work/regress/``, never committed).

The last line is ``regress: digest <16 hex>``: the run in one word, for two builds to compare.
Exit status: 0 passed, 1 failed, 77 skipped (no generated code, no game files), 2 bad usage.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

SKIP = 77
FNV_START = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
DIGEST_LINE = re.compile(r"^regress: .*digest ([0-9a-f]{16})$", re.MULTILINE)
SETTINGS_ENV = "MAPLETHIEF_SETTINGS"
PRESETS = ("original", "original_controller", "enhanced", "custom")


def fnv1a64(data: bytes, h: int = FNV_START) -> int:
    """FNV-1a 64 (replay.h's fnv1a64)."""
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def digest(text: str) -> str:
    """A checkpoint log's digest: FNV-1a 64 of its text, 16 hex digits (CheckpointLog::digest)."""
    return f"{fnv1a64(text.encode('utf-8')):016x}"


def checkpoint_name(line: str) -> str:
    return line.split(" ", 1)[0] if line else ""


@dataclass
class Difference:
    index: int
    a: str
    b: str

    def describe(self, a_name: str, b_name: str) -> str:
        name = checkpoint_name(self.a) or checkpoint_name(self.b)
        return (
            f"first difference at checkpoint {self.index + 1} ({name}):\n"
            f"  {a_name}: {self.a or '(none: the log ends)'}\n"
            f"  {b_name}: {self.b or '(none: the log ends)'}"
        )


def first_difference(a: str, b: str) -> Difference | None:
    """The first checkpoint line that differs between two logs (compare_logs), or None."""
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else ""
        y = lb[i] if i < len(lb) else ""
        if x != y:
            return Difference(i, x, y)
    return None


def settings_list(settings: str, preset: str) -> str:
    """MAPLETHIEF_SETTINGS for a run: the preset first, then the list (which wins)."""
    parts = []
    if preset:
        parts.append(f"preset={preset}")
    if settings:
        parts.append(settings)
    return ",".join(parts)


@dataclass
class RunResult:
    status: str  # "PASS", "FAIL", "SKIP"
    why: str
    digest: str = ""
    log: str = ""
    seconds: float = 0.0


def run_once(
    binary: Path,
    replay: Path,
    out: Path,
    settings: str,
    timeout: float,
    environ: dict[str, str] | None = None,
) -> RunResult:
    """One play of the replay in `out` (made afresh)."""
    shutil.rmtree(out, ignore_errors=True)
    out.parent.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ if environ is None else environ)
    env.update(
        {
            "SDL_VIDEO_DRIVER": "dummy",
            "SDL_AUDIO_DRIVER": "dummy",
            "MAPLETHIEF_LANGUAGE": "en",
            "MT_REPLAY": str(replay.resolve()),
        }
    )
    if settings:
        env[SETTINGS_ENV] = settings
    else:
        env.pop(SETTINGS_ENV, None)
    start = time.monotonic()
    try:
        proc = subprocess.run(
            [str(binary), "regress", str(out)],
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return RunResult("FAIL", f"timed out after {timeout:g} s", seconds=timeout)
    except OSError as e:
        return RunResult("FAIL", f"cannot run {binary}: {e}")
    seconds = time.monotonic() - start
    out.mkdir(parents=True, exist_ok=True)
    (out / "stdout.txt").write_text(proc.stdout, encoding="utf-8")
    (out / "stderr.txt").write_text(proc.stderr, encoding="utf-8")
    if proc.returncode == SKIP:
        last = proc.stdout.strip().splitlines()[-1:] or ["skipped"]
        return RunResult("SKIP", last[0], seconds=seconds)
    fails = [ln for ln in proc.stdout.splitlines() if ln.startswith("FAIL")]
    log_path = out / "checkpoints.txt"
    log = log_path.read_text(encoding="utf-8") if log_path.is_file() else ""
    if proc.returncode != 0:
        why = "; ".join(fails[:5]) or f"exit status {proc.returncode}"
        return RunResult("FAIL", why, digest(log) if log else "", log, seconds)
    if not log:
        return RunResult("FAIL", "no checkpoints.txt", seconds=seconds)
    d = digest(log)
    printed = DIGEST_LINE.findall(proc.stdout)
    if not printed or printed[-1] != d:
        return RunResult(
            "FAIL",
            f"the program's digest {printed[-1] if printed else '(none)'} is not its log's {d}",
            d,
            log,
            seconds,
        )
    return RunResult("PASS", f"{len(log.splitlines())} checkpoints", d, log, seconds)


def regress(
    binary: Path,
    replay: Path,
    out: Path,
    repeat: int,
    settings: str,
    timeout: float,
    baseline: Path | None = None,
    save_baseline: Path | None = None,
    environ: dict[str, str] | None = None,
) -> int:
    print(
        f"regress: {replay} x{repeat}, {SETTINGS_ENV}={settings or '(none: the defaults)'}, "
        f"output in {out}"
    )
    results: list[RunResult] = []
    for i in range(repeat):
        r = run_once(binary, replay, out / f"run_{i + 1:02d}", settings, timeout, environ)
        results.append(r)
        print(
            f"run {i + 1}/{repeat}: {r.status} {r.digest or '-' * 16} ({r.why}; {r.seconds:.1f} s)",
            flush=True,
        )
        if r.status == "SKIP":
            print(f"SKIP   {r.why}")
            return SKIP
    failed = sum(1 for r in results if r.status != "PASS")
    first = results[0]
    for i, r in enumerate(results[1:], start=2):
        if r.log and first.log and r.digest != first.digest:
            failed += 1
            d = first_difference(first.log, r.log)
            if d is not None:
                print(f"FAIL   run {i} differs from run 1: " + d.describe("run 1", f"run {i}"))
    if repeat > 1 and failed == 0:
        print(f"PASS   deterministic: {repeat} runs, one digest {first.digest}")
    if baseline is not None and first.log:
        if not baseline.is_file():
            print(f"FAIL   no baseline at {baseline} (--save-baseline writes one)")
            failed += 1
        else:
            base = baseline.read_text(encoding="utf-8")
            d = first_difference(base, first.log)
            if d is None:
                print(f"PASS   the baseline {baseline}: identical ({digest(base)})")
            else:
                failed += 1
                print(
                    f"FAIL   the baseline {baseline} ({digest(base)}) differs: "
                    + d.describe("baseline", "this run")
                )
    if save_baseline is not None and first.status == "PASS":
        save_baseline.parent.mkdir(parents=True, exist_ok=True)
        save_baseline.write_text(first.log, encoding="utf-8")
        print(f"SAVED  the baseline {save_baseline} ({first.digest})")
    passed = sum(1 for r in results if r.status == "PASS")
    print(f"regress: {passed} of {repeat} runs passed")
    print(f"regress: digest {first.digest or '-'}")
    return 0 if failed == 0 else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.regress", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument(
        "--binary", type=Path, required=True, help="play_real_test, built with the generated code"
    )
    parser.add_argument("--replay", type=Path, default=Path("tests/regress/title_to_combat.replay"))
    parser.add_argument("--out", type=Path, default=Path("build/test-output/regress"))
    parser.add_argument("--repeat", type=int, default=1, help="play it N times (one digest)")
    parser.add_argument("--settings", default="", help=f"{SETTINGS_ENV}: key=value,key=value")
    parser.add_argument("--preset", default="", choices=("",) + PRESETS, help="preset=NAME first")
    parser.add_argument("--baseline", type=Path, help="compare the first run's checkpoints")
    parser.add_argument("--save-baseline", type=Path, help="write the first run's checkpoints")
    parser.add_argument("--timeout", type=float, default=1800.0, help="seconds per run")
    args = parser.parse_args(argv)
    if args.repeat < 1:
        print("regress: --repeat needs 1 or more", file=sys.stderr)
        return 2
    if not args.replay.is_file():
        print(f"regress: no replay at {args.replay}", file=sys.stderr)
        return 2
    if not args.binary.is_file():
        print(f"SKIP   {args.binary}: no such program (build play_real_test with work/gen)")
        return SKIP
    return regress(
        args.binary,
        args.replay,
        args.out,
        args.repeat,
        settings_list(args.settings, args.preset),
        args.timeout,
        args.baseline,
        args.save_baseline,
    )


if __name__ == "__main__":
    sys.exit(main())
