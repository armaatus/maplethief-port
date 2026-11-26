"""Run coverage (#295, ADR 0160): how much of the game's code and data the automated runs executed.

A build with the entry counters (the ``mac-coverage`` preset: ``MAPLETHIEF_TRACE_ENTRY=ON``)
run with ``MAPLETHIEF_COVERAGE=PATH`` writes one coverage file per process at exit (``%p`` in
PATH is the process id). The runtime writes it (``mt_coverage_format`` and ``mt_hits_format``,
``maplethief/diag.h``)::

    # maplethief coverage 1: 2 guest functions entered (pointer, entries, name)
    0x00002001 1 CGame::Update
    0x000225E5 3 f_000225E5
    hit import 12 _memcpy
    hit selector 4 -[EAGLView drawView]
    hit script 1 script/common/portal200.out
    hit native 2 38 changeMap
    hit map 1 2.8

A merged file (``merge``) is the same format, with a ``# run NAME FUNCTIONS`` comment for each
run it holds, so a report keeps saying which run entered how many functions.

Commands::

    python3 -m mtport.runcoverage merge OUT INPUT...       sum coverage files (or directories)
    python3 -m mtport.runcoverage report INPUT... [--out DIR] [--baseline OLD.json]
    python3 -m mtport.runcoverage run [--tests REGEX ...] [--preset mac-coverage]

``report`` writes ``run-coverage.md`` and ``run-coverage.json`` (default ``work/coverage/``):
the functions executed and the total per function tag (``work/re/function-tags.json``, #10),
the game code without Chartboost, the unexecuted functions with address and name, the scripts
read (of the bundle's ``script/``), the natives called (of 65), the maps entered (of the
bundle's ``map/*.map``), the imports and the (class, selector) pairs, each with its count. With
``--baseline`` it adds what this report executes that the old one did not.

``run`` configures and builds the coverage preset, then runs the chosen CTest tests (default:
the baseline set, :data:`BASELINE_TESTS`) one at a time in CTest's order, each with the fixture
setups it needs before it, every test into ``work/coverage/runs/<test>/``, and writes the
merged file and the report. Everything is derived from the IPA and stays in git-ignored
``work/``.

Exit status: 0 done, 1 failed (a file that does not parse, a test that failed), 2 bad usage.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from .scriptout_isa import NATIVES

REPO = Path(__file__).resolve().parents[2]
WORK = REPO / "work"
DEFAULT_OUT = WORK / "coverage"
TAGS = WORK / "re" / "function-tags.json"
IMPORTS = WORK / "re" / "imports.json"
APP = WORK / "ipa" / "Payload" / "MapleThief.app"

HEADER = re.compile(r"^# maplethief coverage 1: (\d+) guest functions entered")
FUNCTION_LINE = re.compile(r"^0x([0-9A-Fa-f]{8}) (\d+) (.+)$")
HIT_LINE = re.compile(r"^hit ([a-z]+) (\d+) (.+)$")
RUN_LINE = re.compile(r"^# run (\S+) (\d+)$")
UNNAMED = re.compile(r"^(f|FUN)_[0-9A-Fa-f]+$")
KINDS = ("import", "selector", "script", "native", "map")
# The tag whose functions never run by design (docs/re/libraries.md).
NOT_GAME = "lib:chartboost"
# The baseline (#295): chapter 1's quests, the regression replay, the menus and the input.
BASELINE_TESTS = (r"play\.chapter_1_[a-m]", r"play\.regress", r"menus\..*", r"input\..*")


# --- the coverage file ----------------------------------------------------------------------


@dataclass
class Coverage:
    """One or more runs: function entries by pointer, hits by kind and key, the runs' names."""

    functions: dict[int, int] = field(default_factory=dict)
    names: dict[int, str] = field(default_factory=dict)
    hits: dict[str, dict[str, int]] = field(default_factory=lambda: {k: {} for k in KINDS})
    runs: dict[str, int] = field(default_factory=dict)  # run -> functions it entered

    def add(self, other: Coverage) -> None:
        for ptr, n in other.functions.items():
            self.functions[ptr] = self.functions.get(ptr, 0) + n
        for ptr, name in other.names.items():
            if ptr not in self.names or UNNAMED.match(self.names[ptr]):
                self.names[ptr] = name
        for kind, keys in other.hits.items():
            mine = self.hits.setdefault(kind, {})
            for key, n in keys.items():
                mine[key] = mine.get(key, 0) + n
        for run, n in other.runs.items():
            self.runs[run] = max(self.runs.get(run, 0), n)


def parse(text: str, run: str = "") -> Coverage:
    """A coverage file (the runtime's or a merged one). Raises ValueError with the line."""
    lines = text.splitlines()
    if not lines or not (m := HEADER.match(lines[0])):
        raise ValueError("not a maplethief coverage file (no '# maplethief coverage 1' header)")
    cov = Coverage()
    for i, line in enumerate(lines[1:], 2):
        if not line.strip():
            continue
        if f := FUNCTION_LINE.match(line):
            ptr = int(f[1], 16)
            cov.functions[ptr] = cov.functions.get(ptr, 0) + int(f[2])
            cov.names[ptr] = f[3]
        elif h := HIT_LINE.match(line):
            if h[1] not in KINDS:
                raise ValueError(f"line {i}: unknown hit kind {h[1]!r}")
            keys = cov.hits[h[1]]
            keys[h[3]] = keys.get(h[3], 0) + int(h[2])
        elif r := RUN_LINE.match(line):
            cov.runs[r[1]] = int(r[2])
        else:
            raise ValueError(f"line {i}: not a function, hit or run line: {line!r}")
    if len(cov.functions) != int(m[1]):
        raise ValueError(f"the header says {m[1]} functions, the file lists {len(cov.functions)}")
    if run and not cov.runs:
        cov.runs[run] = len(cov.functions)
    return cov


def render(cov: Coverage) -> str:
    """The coverage file of ``cov``: the header, its runs, the functions by pointer, the hits."""
    out = [
        f"# maplethief coverage 1: {len(cov.functions)} guest functions entered "
        "(pointer, entries, name)"
    ]
    out += [f"# run {run} {n}" for run, n in sorted(cov.runs.items())]
    for ptr in sorted(cov.functions):
        out.append(f"0x{ptr:08X} {cov.functions[ptr]} {cov.names.get(ptr, f'f_{ptr:08X}')}")
    for kind in KINDS:
        for key in sorted(cov.hits.get(kind, {})):
            out.append(f"hit {kind} {cov.hits[kind][key]} {key}")
    return "\n".join(out) + "\n"


def run_name(path: Path, root: Path) -> str:
    """A file's run: its directory below ``root`` (``runs/<test>/coverage-123.txt``: the
    test), or ``root``'s own name when it sits in ``root`` itself (``runs/<test>``)."""
    rel = path.relative_to(root) if path.is_relative_to(root) else Path(path.name)
    return rel.parent.as_posix() if rel.parent != Path(".") else root.name


def coverage_files(inputs: Iterable[Path]) -> list[tuple[Path, str]]:
    """(file, run) for every input: a file, or every coverage file below a directory."""
    out: list[tuple[Path, str]] = []
    for p in inputs:
        if p.is_dir():
            for f in sorted(p.rglob("*")):
                if f.is_file() and _is_coverage(f):
                    out.append((f, run_name(f, p)))
        else:
            out.append((p, p.stem))
    return out


def _is_coverage(path: Path) -> bool:
    try:
        with path.open("rb") as f:
            return f.read(22) == b"# maplethief coverage "
    except OSError:
        return False


def load(inputs: Iterable[Path]) -> Coverage:
    """Every input summed. Raises ValueError naming the file that does not parse."""
    total = Coverage()
    for path, run in coverage_files(inputs):
        try:
            total.add(parse(path.read_text(encoding="utf-8"), run))
        except (OSError, UnicodeDecodeError, ValueError) as e:
            raise ValueError(f"{path}: {e}") from e
    return total


# --- what there is ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Function:
    ptr: int
    tag: str
    name: str


@dataclass
class Universe:
    """What a run could execute: the tagged functions and the bundle's scripts, maps, natives
    and imports. Each part may be empty when its source is missing."""

    functions: dict[int, Function] = field(default_factory=dict)
    scripts: list[str] = field(default_factory=list)
    maps: list[str] = field(default_factory=list)
    natives: list[str] = field(default_factory=list)  # "<index> <name>"
    imports: list[str] = field(default_factory=list)  # the game's function imports


def map_key(stem: str) -> str | None:
    """``map/<area><map>.map`` as ``"<area>.<map>"``. The names are not zero-padded
    (docs/formats/map.md): three digits are area 10 when they start with 10, else a one-digit
    area and a two-digit map (210, 410-418, 910)."""
    if not stem.isdigit() or len(stem) not in (2, 3):
        return None
    if len(stem) == 2:
        return f"{int(stem[0])}.{int(stem[1])}"
    if stem.startswith("10"):
        return f"10.{int(stem[2])}"
    return f"{int(stem[0])}.{int(stem[1:])}"


def load_universe(
    tags: Path | None = TAGS, app: Path | None = APP, imports: Path | None = IMPORTS
) -> Universe:
    u = Universe(natives=[f"{n.index} {n.name}" for n in NATIVES])
    if tags is not None and tags.is_file():
        doc = json.loads(tags.read_text(encoding="utf-8"))
        for f in doc["functions"]:
            ptr = int(f["address"], 16)
            u.functions[ptr] = Function(ptr, f["tag"], f.get("name") or f"FUN_{ptr & ~1:08x}")
    if app is not None and (app / "script").is_dir():
        u.scripts = sorted(
            p.relative_to(app).as_posix() for p in (app / "script").rglob("*.out") if p.is_file()
        )
    if app is not None and (app / "map").is_dir():
        keys = (map_key(p.stem) for p in (app / "map").glob("*.map"))
        u.maps = sorted({k for k in keys if k is not None}, key=_map_order)
    if imports is not None and imports.is_file():
        doc = json.loads(imports.read_text(encoding="utf-8"))
        u.imports = sorted(
            i["name"]
            for i in doc["imports"]
            if i.get("kind") != "address" and i.get("class") in ("game", "both")
        )
    return u


def _map_order(key: str) -> tuple[int, int]:
    a, m = key.split(".")
    return int(a), int(m)


def _native_order(key: str) -> tuple[int, str]:
    head = key.split(" ", 1)[0]
    return (int(head), key) if head.isdigit() else (1 << 30, key)


# --- the report ------------------------------------------------------------------------------


def _pct(n: int, total: int) -> str:
    return f"{100.0 * n / total:.1f}%" if total else "-"


def build_report(cov: Coverage, u: Universe, baseline: dict | None = None) -> dict:
    """The report as JSON-ready data (``run-coverage.json``)."""
    tags: dict[str, dict[str, int]] = {}
    for f in u.functions.values():
        t = tags.setdefault(f.tag, {"executed": 0, "total": 0})
        t["total"] += 1
        t["executed"] += 1 if f.ptr in cov.functions else 0
    untagged = sorted(p for p in cov.functions if p not in u.functions)
    game = [f for f in u.functions.values() if f.tag != NOT_GAME]
    game_run = sum(1 for f in game if f.ptr in cov.functions)

    def name_of(ptr: int) -> str:
        seen = cov.names.get(ptr)
        if seen and not UNNAMED.match(seen):
            return seen
        return u.functions[ptr].name if ptr in u.functions else (seen or f"f_{ptr:08X}")

    unexecuted = [
        {"address": f"0x{f.ptr:X}", "tag": f.tag, "name": name_of(f.ptr)}
        for f in sorted(u.functions.values(), key=lambda f: f.ptr)
        if f.ptr not in cov.functions
    ]

    def part(kind: str, universe: list[str], order=None) -> dict:
        hits = cov.hits.get(kind, {})
        keys = sorted(hits, key=order) if order else sorted(hits)
        out: dict = {"used": len(hits), "hits": {k: hits[k] for k in keys}}
        if universe:
            known = set(universe)
            out["total"] = len(universe)
            out["used_known"] = sum(1 for k in hits if k in known)
            out["unused"] = [k for k in universe if k not in hits]
        return out

    report: dict = {
        "tool": "mtport.runcoverage",
        "schema": 1,
        "runs": dict(sorted(cov.runs.items())),
        "functions": {
            "executed": len(cov.functions),
            "total": len(u.functions),
            "game_executed": game_run,
            "game_total": len(game),
            "by_tag": dict(sorted(tags.items())),
            "untagged_executed": [f"0x{p:X}" for p in untagged],
            "entries": sum(cov.functions.values()),
        },
        "unexecuted": unexecuted,
        "scripts": part("script", u.scripts),
        "natives": part("native", u.natives, _native_order),
        "maps": part("map", u.maps, _map_order),
        "imports": part("import", u.imports),
        "selectors": part("selector", []),
    }
    if baseline is not None:
        report["since_baseline"] = compare(baseline, report)
    return report


def compare(old: dict, new: dict) -> dict:
    """What ``new`` executes that ``old`` did not: functions, scripts, natives, maps, imports
    and selectors, plus the change of the game-code count."""
    old_unrun = {f["address"] for f in old.get("unexecuted", [])}
    new_unrun = {f["address"] for f in new.get("unexecuted", [])}
    out: dict = {
        "game_executed": new["functions"]["game_executed"]
        - old.get("functions", {}).get("game_executed", 0),
        "functions": sorted(old_unrun - new_unrun, key=lambda a: int(a, 16)),
    }
    for part in ("scripts", "natives", "maps", "imports", "selectors"):
        before = set(old.get(part, {}).get("hits", {}))
        out[part] = [k for k in new[part]["hits"] if k not in before]
    return out


def render_markdown(r: dict, top: int = 40) -> str:
    fn = r["functions"]
    lines = [
        "# Run coverage (#295)",
        "",
        "What the automated runs executed of the game's code and data, from the coverage files "
        "of the `mac-coverage` build (`python3 -m mtport.runcoverage`, ADR 0160). Derived from "
        "the IPA: never committed.",
        "",
        f"- **Game code: {fn['game_executed']:,} of {fn['game_total']:,} functions executed "
        f"({_pct(fn['game_executed'], fn['game_total'])})**, Chartboost not counted "
        "(it never runs by design).",
        f"- All functions: {fn['executed']:,} of {fn['total']:,}; {fn['entries']:,} entries.",
    ]
    for label, part in (
        ("Scripts read", "scripts"),
        ("Natives called", "natives"),
        ("Maps entered", "maps"),
        ("Game imports called", "imports"),
    ):
        p = r[part]
        of = f" of {p['total']:,} ({_pct(p['used_known'], p['total'])})" if "total" in p else ""
        lines.append(f"- {label}: {p.get('used_known', p['used']):,}{of}.")
    lines.append(f"- (class, selector) pairs called: {r['selectors']['used']:,}.")
    if fn["untagged_executed"]:
        lines.append(f"- Executed functions without a tag: {len(fn['untagged_executed'])}.")
    lines += ["", "## Runs", "", "| Run | Functions entered |", "|---|---:|"]
    lines += [f"| `{run}` | {n:,} |" for run, n in r["runs"].items()]
    lines += [
        "",
        "## Functions by tag",
        "",
        "| Tag | Executed | Total | % |",
        "|---|---:|---:|---:|",
    ]
    for tag, t in fn["by_tag"].items():
        lines.append(
            f"| `{tag}` | {t['executed']:,} | {t['total']:,} | {_pct(t['executed'], t['total'])} |"
        )
    if "since_baseline" in r:
        s = r["since_baseline"]
        lines += [
            "",
            "## Since the baseline",
            "",
            f"- Game functions executed: {s['game_executed']:+,}; newly executed: "
            f"{len(s['functions']):,}.",
        ]
        for part in ("scripts", "natives", "maps", "imports", "selectors"):
            lines.append(f"- New {part}: {len(s[part]):,}" + _inline(s[part], 20) + ".")
    lines += _hits_section("Scripts read", r["scripts"], "script")
    lines += _hits_section("Natives called", r["natives"], "native")
    lines += _hits_section("Maps entered", r["maps"], "map")
    lines += _hits_section("Imports called", r["imports"], "import", top=top)
    lines += _hits_section("(Class, selector) pairs called", r["selectors"], "selector", top=top)
    game_unrun = [f for f in r["unexecuted"] if f["tag"] != NOT_GAME]
    lines += [
        "",
        f"## Unexecuted game functions ({len(game_unrun):,})",
        "",
        "Chartboost's are left out; `run-coverage.json` lists every one.",
        "",
        "| Address | Tag | Name |",
        "|---|---|---|",
    ]
    lines += [f"| `{f['address']}` | `{f['tag']}` | `{f['name']}` |" for f in game_unrun]
    return "\n".join(lines) + "\n"


def _inline(keys: list[str], limit: int) -> str:
    if not keys:
        return ""
    shown = ", ".join(f"`{k}`" for k in keys[:limit])
    more = f" and {len(keys) - limit:,} more" if len(keys) > limit else ""
    return f" ({shown}{more})"


def _hits_section(title: str, p: dict, what: str, top: int | None = None) -> list[str]:
    hits = p["hits"]
    lines = ["", f"## {title} ({len(hits):,})", ""]
    rows = list(hits.items())
    if top is not None and len(rows) > top:
        rows = sorted(rows, key=lambda kv: (-kv[1], kv[0]))[:top]
        lines += [f"The {top} with the most calls; `run-coverage.json` lists all.", ""]
    lines += [f"| {what.capitalize()} | Count |", "|---|---:|"]
    lines += [f"| `{k}` | {n:,} |" for k, n in rows]
    if p.get("unused"):
        unused = ", ".join(f"`{k}`" for k in p["unused"])
        lines += ["", f"Not used ({len(p['unused']):,}): {unused}."]
    return lines


def write_report(r: dict, out: Path) -> tuple[Path, Path]:
    out.mkdir(parents=True, exist_ok=True)
    md, js = out / "run-coverage.md", out / "run-coverage.json"
    md.write_text(render_markdown(r), encoding="utf-8")
    js.write_text(json.dumps(r, indent=1) + "\n", encoding="utf-8")
    return md, js


# --- running the tests -----------------------------------------------------------------------


@dataclass(frozen=True)
class Test:
    name: str
    requires: tuple[str, ...]
    sets_up: tuple[str, ...]


def ctest_tests(doc: dict) -> list[Test]:
    """The tests of ``ctest --show-only=json-v1``, in CTest's order, with their fixtures."""
    out = []
    for t in doc.get("tests", []):
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        out.append(
            Test(
                t["name"],
                tuple(props.get("FIXTURES_REQUIRED", []) or []),
                tuple(props.get("FIXTURES_SETUP", []) or []),
            )
        )
    return out


def select(tests: Sequence[Test], patterns: Sequence[str]) -> list[Test]:
    """The tests whose whole name matches a pattern, plus the fixture setups each needs
    (transitively), every setup before the tests that need it; otherwise CTest's order."""
    regexes = [re.compile(p) for p in patterns]
    by_fixture: dict[str, list[Test]] = {}
    for t in tests:
        for f in t.sets_up:
            by_fixture.setdefault(f, []).append(t)
    chosen: dict[str, Test] = {}
    order: list[Test] = []

    def need(t: Test, path: tuple[str, ...]) -> None:
        if t.name in chosen or t.name in path:
            return
        for f in t.requires:
            for setup in by_fixture.get(f, []):
                need(setup, (*path, t.name))
        chosen[t.name] = t
        order.append(t)

    for t in tests:
        if any(r.fullmatch(t.name) for r in regexes):
            need(t, ())
    return order


def _test_dir_name(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "_", name)


def run_tests(
    build_dir: Path,
    tests: Sequence[Test],
    runs: Path,
    symbols: Path | None,
    runner=subprocess.run,
) -> dict[str, int]:
    """Runs each test alone (``ctest -R ^name$``, no fixture added) with the coverage on, into
    ``runs/<test>/coverage-<pid>.txt``. Returns the exit status of each."""
    status: dict[str, int] = {}
    for t in tests:
        d = (runs / _test_dir_name(t.name)).resolve()  # CTest runs each test elsewhere
        d.mkdir(parents=True, exist_ok=True)
        for old in d.glob("coverage-*.txt"):
            old.unlink()
        env = dict(os.environ)
        env["MAPLETHIEF_COVERAGE"] = str(d / "coverage-%p.txt")
        if symbols is not None and symbols.is_file():
            env["MAPLETHIEF_SYMBOLS"] = str(symbols)
        cmd = [
            "ctest",
            "--test-dir",
            str(build_dir),
            "-R",
            f"^{re.escape(t.name)}$",
            "-FA",
            ".*",
            "--output-on-failure",
        ]
        print("+ " + " ".join(cmd), flush=True)
        status[t.name] = runner(cmd, env=env).returncode
        print(f"{'PASS' if status[t.name] == 0 else 'FAIL'}   {t.name}", flush=True)
    return status


def cmd_run(args: argparse.Namespace) -> int:
    build_dir = args.build_dir or REPO / "build" / args.preset
    if not args.no_build:
        for cmd in (
            ["cmake", "--preset", args.preset],
            ["cmake", "--build", "--preset", args.preset],
        ):
            print("+ " + " ".join(cmd), flush=True)
            if subprocess.run(cmd, cwd=REPO).returncode != 0:
                print("FAIL   run coverage: the build failed")
                return 1
    listing = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        capture_output=True,
        text=True,
        check=False,
    )
    if listing.returncode != 0:
        print(f"FAIL   run coverage: ctest cannot list the tests of {build_dir}")
        return 1
    tests = select(ctest_tests(json.loads(listing.stdout)), args.tests or BASELINE_TESTS)
    if not tests:
        print("FAIL   run coverage: no test matches")
        return 1
    runs = args.out / "runs"
    status = run_tests(build_dir, tests, runs, build_dir / "maplethief.sym")
    failed = [n for n, s in status.items() if s != 0]
    try:
        cov = load([runs / _test_dir_name(t.name) for t in tests])
    except ValueError as e:
        print(f"FAIL   run coverage: {e}")
        return 1
    (args.out / "run-coverage.txt").write_text(render(cov), encoding="utf-8")
    md, _ = write_report(build_report(cov, load_universe()), args.out)
    print(f"run coverage: {len(tests)} tests, {len(failed)} failed{_inline(failed, 50)}; {md}")
    return 1 if failed else 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.runcoverage", description=__doc__.split("\n\n")[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("merge", help="sum coverage files into one")
    m.add_argument("out", type=Path)
    m.add_argument("inputs", type=Path, nargs="+", help="coverage files or directories of them")
    r = sub.add_parser("report", help="write run-coverage.md and .json")
    r.add_argument("inputs", type=Path, nargs="+", help="coverage files or directories of them")
    r.add_argument("--out", type=Path, default=DEFAULT_OUT)
    r.add_argument("--tags", type=Path, default=TAGS)
    r.add_argument("--app", type=Path, default=APP, help="the extracted bundle")
    r.add_argument("--imports", type=Path, default=IMPORTS)
    r.add_argument("--baseline", type=Path, help="an earlier run-coverage.json to compare with")
    run = sub.add_parser("run", help="build the coverage preset, run tests, report")
    run.add_argument("--tests", nargs="+", help="CTest name regexes (default: the baseline)")
    run.add_argument("--preset", default="mac-coverage")
    run.add_argument("--build-dir", type=Path)
    run.add_argument("--out", type=Path, default=DEFAULT_OUT)
    run.add_argument("--no-build", action="store_true", help="use the build as it is")
    args = parser.parse_args(argv)

    if args.command == "run":
        return cmd_run(args)
    try:
        cov = load(args.inputs)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if args.command == "merge":
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(render(cov), encoding="utf-8")
        print(f"merged {len(cov.runs)} runs: {len(cov.functions):,} functions; {args.out}")
        return 0
    baseline = None
    if args.baseline is not None:
        try:
            baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"error: {args.baseline}: {e}", file=sys.stderr)
            return 1
    u = load_universe(args.tags, args.app, args.imports)
    report = build_report(cov, u, baseline)
    md, js = write_report(report, args.out)
    fn = report["functions"]
    print(
        f"run coverage: game code {fn['game_executed']:,} of {fn['game_total']:,} functions "
        f"({_pct(fn['game_executed'], fn['game_total'])}); {md}, {js}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
