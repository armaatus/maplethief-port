"""Runs a libFuzzer target of tests/fuzz/ (#322, ADR 0158).

    fuzz_run.py smoke NAME --binary B --seeds DIR --out DIR --seconds N
        CTest's fuzz.NAME (the CI smoke): N seconds from the seeds, with a fresh scratch corpus.
    fuzz_run.py run NAME --binary B --seeds DIR --corpus DIR --out DIR --cpu-hours H
        scripts/fuzz's long run: until the target has used H hours of CPU time, growing the
        corpus DIR (kept between runs). Appends one JSON line per run to OUT/runs.jsonl.

A finding stops the run with a non-zero exit; libFuzzer writes the input to OUT/crash-<sha1>
(or leak-, timeout-, oom-). Reproduce it with `BINARY OUT/crash-...`, turn it into a regression
test in tests/cpp/, fix the parser, and add the input to the seeds if it is worth keeping.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import resource
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_corpus  # noqa: E402

# The asan-base test preset's options (ADR 0157), plus LeakSanitizer: libFuzzer reports a leak
# within one input, which the parsers must not have.
SANITIZER_ENV = {
    "ASAN_OPTIONS": "abort_on_error=1:halt_on_error=1:detect_leaks=1:"
    "detect_stack_use_after_return=1:strict_string_checks=1:check_initialization_order=1",
    "UBSAN_OPTIONS": "halt_on_error=1:abort_on_error=1:print_stacktrace=1:report_error_type=1",
}
# Per input: a parser that takes this long on one input hangs (libFuzzer's -timeout, seconds).
INPUT_TIMEOUT = 25
RSS_LIMIT_MB = 2048


def fuzzer_args(name: str, out: Path) -> list[str]:
    args = [
        f"-max_len={make_corpus.max_len(name)}",
        f"-timeout={INPUT_TIMEOUT}",
        f"-rss_limit_mb={RSS_LIMIT_MB}",
        f"-artifact_prefix={out}/",
        "-print_final_stats=1",
    ]
    dictionary = HERE / f"{name}.dict"
    if dictionary.is_file():
        args.append(f"-dict={dictionary}")
    return args


def env() -> dict[str, str]:
    e = dict(os.environ)
    e.update(SANITIZER_ENV)
    return e


def child_cpu() -> float:
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def run_fuzzer(cmd: list[str], log: Path) -> tuple[int, float, float, str]:
    """Runs libFuzzer, its output to the log and to stderr; (exit code, wall s, cpu s, output)."""
    cpu0, t0 = child_cpu(), time.monotonic()
    with log.open("a", encoding="utf-8") as f:
        f.write(f"$ {' '.join(cmd)}\n")
        f.flush()
        p = subprocess.run(
            cmd, env=env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False
        )
        text = p.stdout.decode("utf-8", "replace")
        f.write(text)
    sys.stderr.write(text[-6000:])
    return p.returncode, time.monotonic() - t0, child_cpu() - cpu0, text


def stat(text: str, key: str) -> int:
    m = re.findall(rf"stat::{key}:\s+(\d+)", text)
    return int(m[-1]) if m else 0


def report_finding(binary: str, out: Path) -> None:
    found = sorted(
        p for p in out.iterdir() if re.match(r"(crash|leak|timeout|oom|slow-unit)-", p.name)
    )
    for p in found:
        print(f"fuzz_run: finding {p}\n  reproduce: {binary} {p}", file=sys.stderr)
    print(
        "fuzz_run: turn each finding into a regression test in tests/cpp/ (tests/fuzz/README.md)",
        file=sys.stderr,
    )


def smoke(a: argparse.Namespace) -> int:
    out = Path(a.out)
    shutil.rmtree(out, ignore_errors=True)
    scratch = out / "corpus"
    scratch.mkdir(parents=True)
    cmd = [
        a.binary,
        *fuzzer_args(a.name, out),
        f"-max_total_time={a.seconds}",
        str(scratch),
        a.seeds,
    ]
    code, wall, cpu, text = run_fuzzer(cmd, out / "fuzz.log")
    print(
        f"fuzz_run: {a.name}: {stat(text, 'number_of_executed_units')} inputs in {wall:.0f} s "
        f"({cpu:.0f} CPU s), exit {code}"
    )
    if code != 0:
        report_finding(a.binary, out)
    return code


def long_run(a: argparse.Namespace) -> int:
    out, corpus = Path(a.out), Path(a.corpus)
    out.mkdir(parents=True, exist_ok=True)
    corpus.mkdir(parents=True, exist_ok=True)
    goal = a.cpu_hours * 3600.0
    started = datetime.now(timezone.utc).isoformat(timespec="seconds")
    log = out / f"run-{started.replace(':', '')}.log"
    cpu = wall = 0.0
    execs, code = 0, 0
    # libFuzzer counts wall time; a busy machine gives a process less than its share of the
    # CPU, so run again until the CPU time adds up.
    while cpu < goal and code == 0:
        left = max(60, int(goal - cpu))
        cmd = [a.binary, *fuzzer_args(a.name, out), f"-max_total_time={left}", str(corpus), a.seeds]
        code, w, c, text = run_fuzzer(cmd, log)
        wall += w
        cpu += c
        execs += stat(text, "number_of_executed_units")
    record = {
        "target": a.name,
        "started": started,
        "finished": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "cpu_seconds": round(cpu),
        "wall_seconds": round(wall),
        "inputs": execs,
        "corpus": sum(1 for _ in corpus.iterdir()),
        "exit": code,
        "log": str(log),
    }
    with (out / "runs.jsonl").open("a", encoding="utf-8") as f:
        f.write(json.dumps(record) + "\n")
    print(
        f"fuzz_run: {a.name}: {cpu / 3600:.2f} CPU h ({wall / 3600:.2f} h wall), "
        f"{execs} inputs, corpus {record['corpus']}, exit {code}"
    )
    if code != 0:
        report_finding(a.binary, out)
    return code


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    for cmd in ("smoke", "run"):
        p = sub.add_parser(cmd)
        p.add_argument("name")
        p.add_argument("--binary", required=True)
        p.add_argument("--seeds", required=True)
        p.add_argument("--out", required=True)
    sub.choices["smoke"].add_argument("--seconds", type=int, required=True)
    sub.choices["run"].add_argument("--corpus", required=True)
    sub.choices["run"].add_argument("--cpu-hours", type=float, required=True)
    sub.add_parser("list")
    a = ap.parse_args(argv)
    if a.cmd == "list":
        print("\n".join(make_corpus.targets()))
        return 0
    return smoke(a) if a.cmd == "smoke" else long_run(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
