"""The playthrough (#306, docs/testing/regression-replay.md): the save-chained whole-game runs as a
regression suite, with ADR 0104's checkpoint digest at the end of every part.

The chain is CTest's: ``play.chapter_1_a`` to ``_m``, chapter 2's main parts, chapter 3's parts,
the ending (watched and skipped) and the post-game (tests/CMakeLists.txt), each part going on from
the save of the part before it (MT_FROM_SAVE, a fixture). This runner has CTest play the parts it
selects (``--quick``: the first ones only; CTest's fixtures add the parts and the start states a
part needs) with ``MT_CHECKPOINTS`` set, so each part (``play_real_test``) hashes the screen, the
audio and the state every ``--every`` steps and at its end into its OUT_DIR's
``checkpoints_MODE.txt``. It then prints each part's status, time and digest, and the chain's
digest: FNV-1a 64 of the chain's log, every part's checkpoint lines prefixed by its test's name.

``--repeat N`` plays it N times and fails unless every run gives the same log.
``--save-baseline FILE`` writes the first run's log; ``--baseline FILE`` compares each part that
ran with the baseline's and names the first checkpoint that differs (a quick run compares with a
full baseline: the parts it did not play are left out). Baselines hold hashes of our renders and
stay local (``work/regress/``, never committed). ``--nudge-tick STEP`` sets ``MT_NUDGE_TICK``: the
virtual clock moves one game tick more at that step of every part, a deliberate one-tick change
the comparison must catch.

The last line is ``playthrough: digest <16 hex>``. Exit status: 0 passed, 1 failed, 77 skipped
(no generated code or no game files), 2 bad usage.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport.regress import SKIP, digest

# The chain's parts: chapter 1's main line, chapter 2's main parts (not its side quests, crafts
# and shops, which branch off it), chapter 3's parts with the ending and the post-game.
FULL = r"^play\.chapter_(1_[a-m]|2_[a-f](_synthetic)?|3_.+)$"
# The quick subset: the chain's first parts, the title and the tutorial to Perion's first quests
# (under 15 minutes in Release on an M-series Mac; the times are in the doc).
QUICK = r"^play\.chapter_1_[a-c]$"
EVERY = 60000  # a checkpoint every virtual minute (steps of 1 ms)
STATUSES = ("PASS", "FAIL", "SKIP", "NOT RUN")


@dataclass
class Part:
    """A part of the chain: its CTest name, its play_real_test mode and output directory."""

    test: str
    mode: str = ""
    out: Path | None = None

    @property
    def log_path(self) -> Path | None:
        return self.out / f"checkpoints_{self.mode}.txt" if self.out and self.mode else None


@dataclass
class PartResult:
    test: str
    status: str
    seconds: float = 0.0
    log: str = ""

    @property
    def digest(self) -> str:
        return digest(self.log) if self.log else "-" * 16


@dataclass
class Run:
    parts: list[PartResult] = field(default_factory=list)
    seconds: float = 0.0
    why: str = ""

    def log(self) -> str:
        """The chain's log: each part's checkpoint lines prefixed by its test's name."""
        return "".join(f"{p.test} {line}\n" for p in self.parts for line in p.log.splitlines())

    def passed(self) -> bool:
        return bool(self.parts) and all(p.status == "PASS" for p in self.parts) and not self.why


def parts_from_ctest_json(text: str) -> dict[str, Part]:
    """The play tests of ``ctest --show-only=json-v1``: test name -> mode and OUT_DIR."""
    found: dict[str, Part] = {}
    for t in json.loads(text).get("tests", []):
        command = t.get("command") or []
        name = t.get("name", "")
        if len(command) >= 3 and Path(command[0]).stem == "play_real_test":
            found[name] = Part(name, command[1], Path(command[2]))
        else:
            found[name] = Part(name)
    return found


def parse_junit(text: str) -> list[tuple[str, str, float]]:
    """``ctest --output-junit``'s test cases (in the tests' order): name, status, seconds.

    A test that exits with its SKIP_RETURN_CODE is SKIP; one whose fixture failed is NOT RUN.
    """
    out = []
    for case in ET.fromstring(text).iter("testcase"):
        status = case.get("status", "")
        skipped = case.find("skipped")
        if case.find("failure") is not None or status == "fail":
            s = "FAIL"
        elif skipped is not None or status in ("notrun", "disabled"):
            why = skipped.get("message", "") if skipped is not None else ""
            s = "SKIP" if why.startswith("SKIP_RETURN_CODE") else "NOT RUN"
        else:
            s = "PASS"
        out.append((case.get("name", ""), s, float(case.get("time", "0") or 0)))
    return out


def split_log(text: str) -> dict[str, list[str]]:
    """A chain log (a baseline) back into each part's checkpoint lines, in the parts' order."""
    parts: dict[str, list[str]] = {}
    for line in text.splitlines():
        test, _, rest = line.partition(" ")
        parts.setdefault(test, []).append(rest)
    return parts


@dataclass
class Difference:
    test: str
    index: int
    a: str
    b: str

    def describe(self, a_name: str, b_name: str) -> str:
        name = (self.a or self.b).split(" ", 1)[0]
        return (
            f"first difference in {self.test}, at checkpoint {self.index + 1} ({name}):\n"
            f"  {a_name}: {self.a or '(none: the log ends)'}\n"
            f"  {b_name}: {self.b or '(none: the log ends)'}"
        )


def compare(a: dict[str, list[str]], b: dict[str, list[str]], tests: Sequence[str]):
    """The first checkpoint that differs between two chain logs over `tests`, in their order."""
    for test in tests:
        la, lb = a.get(test, []), b.get(test, [])
        for i in range(max(len(la), len(lb))):
            x = la[i] if i < len(la) else ""
            y = lb[i] if i < len(lb) else ""
            if x != y:
                return Difference(test, i, x, y)
    return None


def run_chain(
    ctest: Sequence[str],
    build: Path,
    regex: str,
    out: Path,
    every: int,
    nudge: int | None,
    environ: dict[str, str] | None = None,
) -> Run:
    """One play of the chain through CTest; each part's log copied to `out` (made afresh)."""
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    env = dict(os.environ if environ is None else environ)
    env["MT_CHECKPOINTS"] = str(every)
    if nudge is None:
        env.pop("MT_NUDGE_TICK", None)
    else:
        env["MT_NUDGE_TICK"] = str(nudge)
    listed = subprocess.run(
        [*ctest, "--test-dir", str(build), "--show-only=json-v1"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    if listed.returncode != 0:
        return Run(why=f"ctest --show-only failed: {listed.stderr.strip()[:300]}")
    parts = parts_from_ctest_json(listed.stdout)
    if not any(re.search(regex, name) for name in parts):
        return Run(why=f"no test matches {regex} in {build} (configure it first)")
    junit = out / "ctest.xml"
    start, started = time.monotonic(), time.time() - 1  # a log older than `started` is stale
    with open(out / "ctest.txt", "w", encoding="utf-8") as log:
        subprocess.run(
            [*ctest, "--test-dir", str(build), "-R", regex, "--output-on-failure"]
            + ["--output-junit", str(junit.resolve())],
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            check=False,
        )
    run = Run(seconds=time.monotonic() - start)
    if not junit.is_file():
        run.why = f"ctest wrote no {junit} (see {out / 'ctest.txt'})"
        return run
    for name, status, seconds in parse_junit(junit.read_text(encoding="utf-8")):
        part = parts.get(name, Part(name))
        path = part.log_path
        if path is None:
            continue  # a start state (starts.*): no checkpoints of its own
        result = PartResult(name, status, seconds)
        if path.is_file() and path.stat().st_mtime >= started and status != "NOT RUN":
            result.log = path.read_text(encoding="utf-8")
            (out / f"{name}.txt").write_text(result.log, encoding="utf-8")
        if status == "PASS" and not result.log:
            result.status = "FAIL"
        run.parts.append(result)
        print(f"  {result.status:7} {name:40} {seconds:8.1f} s  {result.digest}", flush=True)
    (out / "chain.txt").write_text(run.log(), encoding="utf-8")
    return run


def playthrough(
    ctest: Sequence[str],
    build: Path,
    out: Path,
    regex: str,
    repeat: int = 1,
    every: int = EVERY,
    nudge: int | None = None,
    baseline: Path | None = None,
    save_baseline: Path | None = None,
    environ: dict[str, str] | None = None,
) -> int:
    print(f"playthrough: {regex} in {build}, x{repeat}, a checkpoint every {every} steps")
    runs: list[Run] = []
    for i in range(repeat):
        print(f"run {i + 1}/{repeat}:", flush=True)
        r = run_chain(ctest, build, regex, out / f"run_{i + 1:02d}", every, nudge, environ)
        runs.append(r)
        if r.why:
            print(f"FAIL   {r.why}")
            return 2 if r.why.startswith("no test matches") else 1
        if r.parts and all(p.status == "SKIP" for p in r.parts):
            print("SKIP   every part skipped (no generated code or no game files)")
            return SKIP
        n = sum(1 for p in r.parts if p.status == "PASS")
        print(
            f"run {i + 1}/{repeat}: {n} of {len(r.parts)} parts passed in {r.seconds:.0f} s, "
            f"digest {digest(r.log())}",
            flush=True,
        )
    failed = sum(1 for r in runs if not r.passed())
    first = runs[0]
    tests = [p.test for p in first.parts]
    for i, r in enumerate(runs[1:], start=2):
        d = compare(split_log(first.log()), split_log(r.log()), tests)
        if d is not None:
            failed += 1
            print(f"FAIL   run {i} differs from run 1: " + d.describe("run 1", f"run {i}"))
    if repeat > 1 and failed == 0:
        print(f"PASS   deterministic: {repeat} runs, one digest {digest(first.log())}")
    if baseline is not None:
        if not baseline.is_file():
            print(f"FAIL   no baseline at {baseline} (--save-baseline writes one)")
            failed += 1
        else:
            base = split_log(baseline.read_text(encoding="utf-8"))
            mine = split_log(first.log())
            missing = [t for t in tests if t not in base]
            if missing:
                failed += 1
                print(f"FAIL   the baseline {baseline} has no {', '.join(missing)}")
            d = compare(base, mine, [t for t in tests if t in base])
            if d is not None:
                failed += 1
                print(
                    f"FAIL   the baseline {baseline} differs: " + d.describe("baseline", "this run")
                )
            elif not missing:
                left = len(base) - len(tests)
                print(
                    f"PASS   the baseline {baseline}: {len(tests)} parts identical"
                    + (f" ({left} more in it not played)" if left > 0 else "")
                )
    if save_baseline is not None and first.passed():
        save_baseline.parent.mkdir(parents=True, exist_ok=True)
        save_baseline.write_text(first.log(), encoding="utf-8")
        print(f"SAVED  the baseline {save_baseline} ({digest(first.log())})")
    print(
        f"playthrough: {sum(1 for r in runs if r.passed())} of {repeat} runs passed, "
        f"{first.seconds:.0f} s the first"
    )
    print(f"playthrough: digest {digest(first.log()) if first.parts else '-'}")
    return 0 if failed == 0 else 1


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.playthrough", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument(
        "--build", type=Path, required=True, help="the configured build tree (play_real_test built)"
    )
    parser.add_argument("--out", type=Path, default=Path("build/test-output/playthrough"))
    parser.add_argument("--quick", action="store_true", help=f"only {QUICK}")
    parser.add_argument("--tests", default="", help="the parts: a CTest regex (overrides --quick)")
    parser.add_argument("--repeat", type=int, default=1, help="play it N times (one digest)")
    parser.add_argument("--every", type=int, default=EVERY, help="steps between checkpoints")
    parser.add_argument("--nudge-tick", type=int, help="MT_NUDGE_TICK: one tick more at STEP")
    parser.add_argument("--baseline", type=Path, help="compare the first run's parts with FILE")
    parser.add_argument("--save-baseline", type=Path, help="write the first run's log to FILE")
    parser.add_argument("--ctest", default="ctest", help="the ctest program")
    args = parser.parse_args(argv)
    if args.repeat < 1 or args.every < 0:
        print("playthrough: --repeat needs 1 or more, --every 0 or more", file=sys.stderr)
        return 2
    regex = args.tests or (QUICK if args.quick else FULL)
    return playthrough(
        args.ctest.split(),
        args.build,
        args.out,
        regex,
        args.repeat,
        args.every,
        args.nudge_tick,
        args.baseline,
        args.save_baseline,
    )


if __name__ == "__main__":
    sys.exit(main())
