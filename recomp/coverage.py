"""Recompiler coverage (#40): which functions are discovered, lifted cleanly and compiled, by
#10 tag, and every function that is not, with the reason.

``python -m recomp coverage`` discovers and lifts the whole image into ``work/gen/``, compiles
every generated translation unit with the host C compiler, and writes
``work/recomp/coverage.json`` and ``work/recomp/coverage.md``. ``python -m recomp lift`` (the
whole program) and ``python -m recomp all`` write the same report without compiling, unless
they get ``--compile``, so a normal run stays at a few seconds (ADR 0002's 60 s budget).
Every one of these runs ends with :func:`summary_line`.

**What each status means.** The functions are the union of the discovered and the lifted
ones (on the armv7 slice both are the 2,655 ``LC_FUNCTION_STARTS`` functions).

- *Discovered*: whole-image discovery (#32) knows the function and recorded no problem.
- *Lifted* (cleanly): the lifter produced its C, and no stage reported a problem for it:
  discovery, the lifter (``Lifted.problems``, e.g. a flag live across a call), an
  unsupported instruction (``Lifted.unsupported``, a trap in place) or the SjLj context
  census (``recomp.exceptions``: context, LSDA or landing-pad dispatch).
- *Compiled*: measured, never assumed. :func:`compile_program` compiles each ``fn_NNN.c``,
  ``imports.c`` and ``dispatch.c`` to an object file with the flags of
  ``cmake/GeneratedCode.cmake`` plus warnings as errors and ``-O0`` (:meth:`Compiler.command`).
  A unit that fails is compiled again one function at a time (``mt_gen.h`` plus that
  function's C), so each error is charged to the function it is in. A unit that fails while
  each of its functions compiles alone charges all of them. ``imports.c`` or ``dispatch.c``
  failing is a program problem.
- *OK*: lifted and compiled. The M3 exit criterion is :data:`TARGET`, 99% of the functions.

Every function that is not OK has at least one :class:`Failure`: its stage
(:data:`STAGES`), the instruction address where one is known, and the reason. Problems that
belong to no function (the import table, the dispatch table, ``imports.c``) are program
problems: they fail the run however many functions are OK.

The report also carries quality signals that cost little: the dispatch index, the import
and SjLj counts, the census forms that are missing from the snippet form lists
(``tests/recomp/integer_forms.txt``, ``vfp_forms.txt``; CI checks that each listed form has
a passing snippet), and the pass count of the last differential test
(``work/recomp/difftest.json``, ``python -m recomp difftest``) if there is one. Tags come
from ``work/re/function-tags.json`` (``python3 -m mtport.libtags generate``, #10); without it
every function is ``untagged``.

``coverage.json``, schema 1 (``ghidra_common.dumps`` layout, functions in pointer order, no
timings, so equal inputs give equal files)::

    {"schema": 1, "source": {"path", "arch", "sha256"}, "target": 0.99,
     "met": true,                      # null if compiling was not measured
     "counts": {"functions", "discovered", "lifted", "compiled", "ok", "failures",
                "program_problems"},  # compiled and ok are null if not measured
     "by_tag": {"engine": {"functions", "discovered", "lifted", "compiled", "ok"}, ...},
     "compile": {"measured": true, "compiler": "clang", "flags": [...], "units": 62,
                 "failed_units": []}  # or {"measured": false, "why": "..."}
     "program_problems": [{"stage": "imports", "reason": "..."}],
     "signals": {"dispatch": {...}, "imports": {...}, "sjlj": {...},
                 "forms": {...} | null, "difftest": {...} | null},
     "functions": [
      {"ptr": "0x27E0", "tag": "lib:crt1", "name": "start", "discovered": true,
       "lifted": true, "compiled": true,           # null if not measured
       "failures": [{"stage": "lift", "address": "0x2A3C4", "reason": "..."}]}]}

The input is untrusted data: it is decoded and compiled, never executed.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterable, Mapping, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

from recomp import REPO
from recomp.lift.core import Lifted, c_name
from recomp.output import partition, unit_name

COVERAGE_JSON = REPO / "work" / "recomp" / "coverage.json"
COVERAGE_MD = REPO / "work" / "recomp" / "coverage.md"
TAGS_JSON = REPO / "work" / "re" / "function-tags.json"
DIFFTEST_JSON = REPO / "work" / "recomp" / "difftest.json"
FORM_LISTS = (
    REPO / "tests" / "recomp" / "integer_forms.txt",
    REPO / "tests" / "recomp" / "vfp_forms.txt",
)
INCLUDE = REPO / "runtime" / "include"
TARGET = 0.99
UNTAGGED = "untagged"
# Stages a failure can come from, in pipeline order. The first five make a function not
# lifted cleanly; "compile" makes it not compiled.
STAGES = ("discover", "lift", "unsupported", "sjlj", "imports", "output", "compile")
LIFT_STAGES = frozenset({"discover", "lift", "unsupported", "sjlj", "imports"})
COMPILE_TIMEOUT = 900.0  # seconds per translation unit
REASON_CHARS = 300


@dataclass(frozen=True)
class Failure:
    """Why a function (``ptr``; None for a program problem) is not clean."""

    ptr: int | None
    stage: str
    reason: str
    address: int | None = None

    def to_json(self) -> dict:
        out: dict = {"stage": self.stage}
        if self.address is not None:
            out["address"] = f"0x{self.address:X}"
        out["reason"] = self.reason
        return out


# -- compiling ----------------------------------------------------------------------------------


@dataclass(frozen=True)
class Compiler:
    """A host C compiler: ``kind`` is ``gnu`` (clang, gcc), ``msvc`` (cl) or ``clang-cl``."""

    path: str
    kind: str

    @property
    def name(self) -> str:
        return Path(self.path).stem

    def flags(self) -> list[str]:
        """The flags of ``cmake/GeneratedCode.cmake`` plus warnings as errors, unoptimized."""
        if self.kind == "gnu":
            return ["-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-O0",
                    "-ffp-contract=off"]  # fmt: skip
        fp = "/clang:-ffp-contract=off" if self.kind == "clang-cl" else "/fp:precise"
        return ["/nologo", "/std:c17", "/W4", "/WX", "/Od", "/utf-8", fp, "/bigobj"]

    def command(self, src: Path, obj: Path, include: Sequence[Path]) -> list[str]:
        if self.kind == "gnu":
            inc = [f"-I{p}" for p in include]
            return [self.path, *self.flags(), *inc, "-c", str(src), "-o", str(obj)]
        inc = [f"/I{p}" for p in include]
        return [self.path, *self.flags(), *inc, "/c", str(src), f"/Fo{obj}"]


def find_compiler() -> Compiler | None:
    """``$MAPLETHIEF_CC`` or ``$CC``, else MSVC ``cl`` on Windows when it is on PATH, else
    ``clang``, ``gcc`` or ``cc``; None if there is none."""
    for env in ("MAPLETHIEF_CC", "CC"):
        path = os.environ.get(env)
        if path:
            return Compiler(path, _kind(path))
    if sys.platform == "win32" and shutil.which("cl"):
        return Compiler(shutil.which("cl") or "cl", "msvc")
    for name in ("clang", "gcc", "cc"):
        path = shutil.which(name)
        if path:
            return Compiler(path, "gnu")
    return None


def _kind(path: str) -> str:
    name = Path(path).stem.lower()
    if name == "clang-cl":
        return "clang-cl"
    return "msvc" if name == "cl" else "gnu"


@dataclass
class CompileResult:
    """What :func:`compile_program` measured. ``compiled``: the function pointers whose C
    compiled; ``failures``: per function (``ptr`` set) and per program (``ptr`` None)."""

    measured: bool
    compiler: str | None = None
    flags: list[str] = field(default_factory=list)
    units: int = 0
    failed_units: list[str] = field(default_factory=list)
    compiled: set[int] = field(default_factory=set)
    failures: list[Failure] = field(default_factory=list)
    why: str = ""
    seconds: float = 0.0

    def to_json(self) -> dict:
        if not self.measured:
            return {"measured": False, "why": self.why}
        return {
            "measured": True,
            "compiler": self.compiler,
            "flags": self.flags,
            "units": self.units,
            "failed_units": self.failed_units,
        }


def not_measured(why: str) -> CompileResult:
    return CompileResult(False, why=why)


def unit_functions(
    lifted: Sequence[Lifted], units: int, span: tuple[int, int] | None
) -> dict[str, list[Lifted]]:
    """``fn_NNN.c`` -> its functions, as :func:`recomp.output.write_program` partitions them."""
    by_ptr = {f.ptr: f for f in lifted}
    cells = partition(by_ptr, units, span)
    return {unit_name(u): [by_ptr[p] for p in ptrs] for u, ptrs in cells.items()}


def _first_error(output: str, dirs: Iterable[Path]) -> str:
    """The first error line of a compiler's output, with the build directories cut off."""
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    pick = next((line for line in lines if "error" in line.lower()), None)
    text = pick or (lines[0] if lines else "failed with no output")
    for d in dirs:  # the deepest first, each with its separator first
        for form in (str(d) + os.sep, str(d) + "/", str(d)):
            text = text.replace(form, "")
    return text[:REASON_CHARS]


def compile_program(
    gen: Path,
    sources: Sequence[Path],
    cells: Mapping[str, Sequence[Lifted]],
    compiler: Compiler | None = None,
    jobs: int | None = None,
) -> CompileResult:
    """Compile every generated source of ``gen`` (``sources``, as ``write_program`` returns
    them; ``cells``: the functions of each ``fn_NNN.c``) to an object file in a temporary
    directory, and charge every error to a function (see the module docstring)."""
    compiler = compiler or find_compiler()
    if compiler is None:
        return not_measured("no C compiler found (set MAPLETHIEF_CC)")
    t0 = time.perf_counter()
    result = CompileResult(True, compiler.name, compiler.flags(), len(sources))
    include = [INCLUDE, gen]
    with tempfile.TemporaryDirectory(prefix="mt-coverage-") as tmp_name:
        tmp = Path(tmp_name)
        dirs = (tmp / "alone", tmp, gen)

        def run(src: Path, obj: Path) -> str | None:
            cmd = compiler.command(src, obj, include)
            try:
                done = subprocess.run(
                    cmd, capture_output=True, text=True, errors="replace", timeout=COMPILE_TIMEOUT
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                return f"{src.name}: {exc}"[:REASON_CHARS]
            if done.returncode == 0:
                return None
            return _first_error(done.stdout + "\n" + done.stderr, dirs)

        def unit(src: Path) -> tuple[Path, str | None]:
            return src, run(src, tmp / f"{src.stem}.o")

        def alone(f: Lifted) -> tuple[int, str | None]:
            src = tmp / "alone" / f"{f.name}.c"
            src.parent.mkdir(exist_ok=True)
            src.write_text(f'#include "mt_gen.h"\n\n{f.source}', encoding="utf-8")
            return f.ptr, run(src, tmp / "alone" / f"{f.name}.o")

        order = sorted(sources, key=lambda s: -s.stat().st_size if s.is_file() else 0)
        with ThreadPoolExecutor(jobs or os.cpu_count() or 1) as pool:
            failed = {src.name: err for src, err in pool.map(unit, order) if err is not None}
            for name in sorted(failed):
                result.failed_units.append(name)
                if name not in cells:
                    result.failures.append(Failure(None, "compile", f"{name}: {failed[name]}"))
            retry = [f for name in sorted(failed) for f in cells.get(name, ())]
            alone_errors = dict(pool.map(alone, retry))
    written = {s.name for s in sources}
    for name, functions in cells.items():
        if name not in written:
            result.failures += [
                Failure(f.ptr, "compile", f"{name} was not written") for f in functions
            ]
            continue
        if name not in failed:
            result.compiled.update(f.ptr for f in functions)
            continue
        charged = [f for f in functions if alone_errors.get(f.ptr) is not None]
        for f in charged:
            result.failures.append(Failure(f.ptr, "compile", str(alone_errors[f.ptr])))
        if charged:  # the others compile alone, and their unit fails only through these
            result.compiled.update(f.ptr for f in functions if alone_errors.get(f.ptr) is None)
        else:
            reason = f"{name} fails although each of its functions compiles alone: {failed[name]}"
            result.failures += [Failure(f.ptr, "compile", reason) for f in functions]
    result.seconds = time.perf_counter() - t0
    return result


# -- the report ---------------------------------------------------------------------------------


def load_tags(path: Path = TAGS_JSON) -> dict[int, tuple[str, str | None]]:
    """Function pointer -> (tag, name) from ``work/re/function-tags.json``; empty without it."""
    if not path.is_file():
        return {}
    doc = json.loads(path.read_text(encoding="utf-8"))
    return {int(f["address"], 16): (f["tag"], f.get("name")) for f in doc.get("functions", [])}


@dataclass
class Coverage:
    """The coverage of one recompiler run (build it with :func:`collect`)."""

    source: dict
    discovered: set[int]
    lifted: set[int]
    failures: list[Failure]
    compile: CompileResult
    tags: dict[int, tuple[str, str | None]] = field(default_factory=dict)
    signals: dict = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.functions = sorted(self.discovered | self.lifted)
        self.by_ptr: dict[int, list[Failure]] = {}
        for f in self.failures:
            if f.ptr is not None:
                self.by_ptr.setdefault(f.ptr, []).append(f)
        for fs in self.by_ptr.values():
            fs.sort(key=lambda f: (STAGES.index(f.stage), f.address or 0))

    @property
    def program_problems(self) -> list[Failure]:
        return [f for f in self.failures if f.ptr is None]

    def tag(self, ptr: int) -> str:
        return self.tags.get(ptr, (UNTAGGED, None))[0]

    def name(self, ptr: int) -> str | None:
        return self.tags.get(ptr, (UNTAGGED, None))[1]

    def is_discovered(self, ptr: int) -> bool:
        return ptr in self.discovered and all(
            f.stage != "discover" for f in self.by_ptr.get(ptr, ())
        )

    def is_lifted(self, ptr: int) -> bool:
        return ptr in self.lifted and all(
            f.stage not in LIFT_STAGES for f in self.by_ptr.get(ptr, ())
        )

    def is_compiled(self, ptr: int) -> bool | None:
        return ptr in self.compile.compiled if self.compile.measured else None

    def is_ok(self, ptr: int) -> bool | None:
        compiled = self.is_compiled(ptr)
        return None if compiled is None else compiled and self.is_lifted(ptr)

    def _counts(self, ptrs: Sequence[int]) -> dict[str, int | None]:
        measured = self.compile.measured
        return {
            "functions": len(ptrs),
            "discovered": sum(self.is_discovered(p) for p in ptrs),
            "lifted": sum(self.is_lifted(p) for p in ptrs),
            "compiled": sum(bool(self.is_compiled(p)) for p in ptrs) if measured else None,
            "ok": sum(bool(self.is_ok(p)) for p in ptrs) if measured else None,
        }

    def counts(self) -> dict[str, int | None]:
        out = self._counts(self.functions)
        out["failures"] = sum(len(v) for v in self.by_ptr.values())
        out["program_problems"] = len(self.program_problems)
        return out

    def by_tag(self) -> dict[str, dict[str, int | None]]:
        groups: dict[str, list[int]] = {}
        for p in self.functions:
            groups.setdefault(self.tag(p), []).append(p)
        return {t: self._counts(groups[t]) for t in sorted(groups)}

    def share(self, key: str) -> float | None:
        c = self.counts()
        value = c[key]
        if value is None:
            return None
        return value / c["functions"] if c["functions"] else 1.0

    @property
    def lift_met(self) -> bool:
        share = self.share("lifted")
        return share is not None and share >= TARGET and not self.program_problems

    @property
    def met(self) -> bool | None:
        """Whether :data:`TARGET` of the functions are lifted and compiled, with no program
        problem; None if compiling was not measured."""
        share = self.share("ok")
        if share is None:
            return None
        return share >= TARGET and not self.program_problems

    def not_clean(self) -> list[int]:
        """Every function that is not OK (with compiling unmeasured: not lifted cleanly)."""
        return [
            p
            for p in self.functions
            if not self.is_lifted(p) or self.is_compiled(p) is False or p in self.by_ptr
        ]

    def to_json(self) -> str:
        from mtport import ghidra_common as gc

        header = {
            "schema": 1,
            "source": self.source,
            "target": TARGET,
            "met": self.met,
            "counts": self.counts(),
            "by_tag": self.by_tag(),
            "compile": self.compile.to_json(),
            "program_problems": [f.to_json() for f in self.program_problems],
            "signals": self.signals,
        }
        rows = []
        for p in self.functions:
            rows.append(
                {
                    "ptr": f"0x{p:X}",
                    "tag": self.tag(p),
                    "name": self.name(p),
                    "discovered": self.is_discovered(p),
                    "lifted": self.is_lifted(p),
                    "compiled": self.is_compiled(p),
                    "failures": [f.to_json() for f in self.by_ptr.get(p, ())],
                }
            )
        return gc.dumps(header, "functions", rows)

    def to_markdown(self) -> str:
        return _markdown(self)


def _n(value: int | None) -> str:
    return "not measured" if value is None else f"{value:,}"


def _pct(value: int | None, total: int) -> str:
    if value is None:
        return "not measured"
    return f"{value / total:.2%}" if total else "100.00%"


def _cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def _markdown(cov: Coverage) -> str:
    c = cov.counts()
    total = c["functions"]
    src = cov.source
    out = [
        "# Recompiler coverage",
        "",
        "<!-- Generated by `python -m recomp coverage` (recomp/coverage.py, #40). "
        "Never edit; never commit. -->",
        "",
        f"Slice `{src.get('path')}` ({src.get('arch')}, sha256 `{src.get('sha256')}`).",
        "",
    ]
    if cov.met is None:
        out.append(
            f"**{c['lifted']:,} of {total:,} functions ({_pct(c['lifted'], total)}) lift "
            f"cleanly.** Compiling was not measured ({cov.compile.why}); "
            "`python -m recomp coverage` measures it."
        )
    else:
        verdict = "met" if cov.met else "**not met**"
        out.append(
            f"**{c['ok']:,} of {total:,} functions ({_pct(c['ok'], total)}) are lifted "
            f"cleanly and compile.** Target {TARGET:.0%}: {verdict}."
        )
    out += ["", "| | Functions | Share |", "|---|---|---|", f"| Functions | {total:,} | |"]
    for key, label in (
        ("discovered", "Discovered"),
        ("lifted", "Lifted cleanly"),
        ("compiled", "Compiled"),
        ("ok", "Lifted and compiled"),
    ):
        out.append(f"| {label} | {_n(c[key])} | {_pct(c[key], total)} |")
    out.append("")
    if cov.compile.measured:
        cmd = " ".join([cov.compile.compiler or "cc", *cov.compile.flags])
        failed = ", ".join(f"`{u}`" for u in cov.compile.failed_units) or "none"
        out.append(
            f"Compiled with `{cmd}`, one object file per translation unit: "
            f"{cov.compile.units} units, failed: {failed}."
        )
        out.append("")
    out += [
        "## By tag",
        "",
        "Tags from #10 (`work/re/function-tags.json`, [docs/re/libraries.md]"
        "(../../docs/re/libraries.md)).",
        "",
        "| Tag | Functions | Discovered | Lifted cleanly | Compiled | Lifted and compiled |",
        "|---|---|---|---|---|---|",
    ]
    for tag, t in cov.by_tag().items():
        out.append(
            f"| `{tag}` | {t['functions']:,} | {_n(t['discovered'])} | {_n(t['lifted'])} | "
            f"{_n(t['compiled'])} | {_n(t['ok'])} |"
        )
    out.append(
        f"| **total** | **{total:,}** | **{_n(c['discovered'])}** | **{_n(c['lifted'])}** | "
        f"**{_n(c['compiled'])}** | **{_n(c['ok'])}** |"
    )
    out += ["", "## Functions that are not clean", ""]
    bad = cov.not_clean()
    if not bad:
        out.append("None.")
    else:
        out += [
            f"{len(bad):,} functions, {c['failures']:,} failures.",
            "",
            "| Function | Tag | Name | Stage | Address | Reason |",
            "|---|---|---|---|---|---|",
        ]
        for p in bad:
            for f in cov.by_ptr.get(p) or [Failure(p, "?", "no reason recorded")]:
                addr = f"0x{f.address:X}" if f.address is not None else ""
                out.append(
                    f"| `{c_name(p)}` | `{cov.tag(p)}` | {_cell(cov.name(p) or '')} | "
                    f"{f.stage} | {addr} | {_cell(f.reason)} |"
                )
    out += ["", "## Program problems", ""]
    if not cov.program_problems:
        out.append("None.")
    for f in cov.program_problems:
        out.append(f"- {f.stage}: {_cell(f.reason)}")
    out += ["", "## Quality signals", ""]
    out += _signal_lines(cov.signals)
    out += [
        "",
        "Regenerate: `python -m recomp coverage` (lifts into `work/gen/` and compiles); "
        "`python -m recomp lift` and `all` rewrite this report without compiling unless given "
        "`--compile`.",
        "",
    ]
    return "\n".join(out)


def _signal_lines(s: Mapping) -> list[str]:
    out = []
    d = s.get("dispatch")
    if d:
        out.append(
            f"- **Dispatch table** (#35): {d['entries']:,} functions, {d['buckets']:,} "
            f"buckets of {d['bucket_bytes']} bytes, at most {d['max_per_bucket']} per bucket."
        )
    i = s.get("imports")
    if i:
        out.append(
            f"- **Imports** (#36): {i['imports']:,} ({i['functions']:,} functions, "
            f"{i['data']:,} data), {i['binds']:,} binds, {i['thunks']:,} thunks, "
            f"{i['problems']:,} problems."
        )
    j = s.get("sjlj")
    if j:
        out.append(
            f"- **SjLj** (#37): {j['contexts']:,} function contexts, {j['lsdas']:,} LSDAs with "
            f"{j['call_sites']:,} call sites, {j['problems']:,} problems."
        )
    f = s.get("forms")
    if f:
        missing = ", ".join(f"`{m}`" for m in f["missing"]) or "none"
        out.append(
            f"- **Snippet forms** (#33, #34): {f['listed']:,} of the binary's {f['forms']:,} "
            f"instruction forms are in `tests/recomp/integer_forms.txt` / `vfp_forms.txt`, "
            f"where CI checks that each has a passing snippet; missing: {missing}."
        )
    else:
        out.append("- **Snippet forms**: not measured.")
    t = s.get("difftest")
    if t:
        stale = (
            f"; {t['not_in_run']:,} of its functions are not in this run (stale report)"
            if t["not_in_run"]
            else ""
        )
        out.append(
            f"- **Differential test** (#41, `work/recomp/difftest.json`): {t['passed']:,} of "
            f"{t['tested']:,} leaf functions agree with Unicorn on every input, "
            f"{t['with_mismatches']:,} with mismatches, {t['not_bounded']:,} not bounded{stale}."
        )
        if t["mismatched"]:
            out.append("  Mismatching: " + ", ".join(f"`{m}`" for m in t["mismatched"]) + ".")
    else:
        out.append(
            "- **Differential test**: no `work/recomp/difftest.json` (`python -m recomp difftest`)."
        )
    return out


def form_signal(census, form_lists: Sequence[Path] = FORM_LISTS) -> dict | None:
    """Census forms against the snippet form lists; None if a list is missing."""
    from recomp.census import read_form_list

    if not all(p.is_file() for p in form_lists):
        return None
    listed = {f for p in form_lists for f in read_form_list(p.read_text(encoding="utf-8"))}
    forms = sorted(census.forms)
    return {
        "forms": len(forms),
        "listed": sum(f in listed for f in forms),
        "missing": [f for f in forms if f not in listed],
    }


def difftest_signal(path: Path, functions: Iterable[int]) -> dict | None:
    """The summary of the last differential test, if ``path`` exists."""
    if not path.is_file():
        return None
    doc = json.loads(path.read_text(encoding="utf-8"))
    s = doc.get("summary", {})
    rows = doc.get("functions", [])
    known = set(functions)
    return {
        "tested": s.get("tested", len(rows)),
        "passed": s.get("passed", sum(bool(r.get("passed")) for r in rows)),
        "with_mismatches": s.get("with_mismatches", 0),
        "not_bounded": s.get("not_bounded", 0),
        "not_in_run": sum(int(r["ptr"], 16) not in known for r in rows),
        "mismatched": [r["ptr"] for r in rows if r.get("mismatches")],
    }


def collect(
    source: dict,
    discovery,
    lifted: Sequence[Lifted],
    compile_result: CompileResult,
    *,
    exceptions=None,
    imports=None,
    tags: dict[int, tuple[str, str | None]] | None = None,
    signals: dict | None = None,
) -> Coverage:
    """The coverage of one run. ``discovery``: a :class:`recomp.discover.Discovery` (or
    anything with ``functions``: pointer -> object with ``problems``); ``exceptions``: a
    :class:`recomp.exceptions.Report`; ``imports``: a :class:`recomp.imports.ImportTable`."""
    failures: list[Failure] = []
    discovered = set(discovery.functions)
    done = {f.ptr for f in lifted}
    for ptr, fn in sorted(discovery.functions.items()):
        failures += [Failure(ptr, "discover", str(p)) for p in fn.problems]
        if ptr not in done:
            failures.append(Failure(ptr, "lift", "discovered but not lifted"))
    for f in lifted:
        if f.ptr not in discovered:
            failures.append(
                Failure(f.ptr, "discover", "not found by whole-image discovery (lifted alone)")
            )
        failures += [Failure(f.ptr, "lift", p) for p in f.problems]
        failures += [
            Failure(f.ptr, "unsupported", f"unsupported instruction: {text}", addr)
            for addr, text in f.unsupported
        ]
    sig = dict(signals or {})
    if exceptions is not None:
        for ctx in exceptions.contexts:
            failures += [Failure(ctx.ptr, "sjlj", p) for p in ctx.problems]
        ec = exceptions.counts()
        sig["sjlj"] = {k: ec[k] for k in ("contexts", "lsdas", "call_sites", "problems")}
    if imports is not None:
        failures += [Failure(None, "imports", p) for p in imports.problems]
        ic = imports.counts()
        sig["imports"] = {
            "imports": ic["imports"], "functions": ic["functions"], "data": ic["data"],
            "binds": ic["binds"], "thunks": ic["thunks"], "problems": ic["problems"],
        }  # fmt: skip
    sig.setdefault("dispatch", None)
    sig.setdefault("forms", None)
    sig.setdefault("difftest", None)
    if sig["dispatch"] is None and done:
        sig["dispatch"] = _dispatch_signal(done, failures)
    failures += compile_result.failures
    return Coverage(source, discovered, done, failures, compile_result, tags or {}, sig)


def _dispatch_signal(ptrs: set[int], failures: list[Failure]) -> dict | None:
    from recomp.dispatch import DispatchTable

    try:
        table = DispatchTable.build(ptrs)
    except ValueError as exc:
        failures.append(Failure(None, "output", f"dispatch table: {exc}"))
        return None
    return {
        "entries": len(table.entries),
        "buckets": table.buckets,
        "bucket_bytes": 1 << table.shift,
        "max_per_bucket": table.max_per_bucket(),
    }


def write(cov: Coverage, json_out: Path = COVERAGE_JSON, md_out: Path = COVERAGE_MD) -> None:
    from mtport import ghidra_common as gc

    gc.write_text(json_out, cov.to_json())
    gc.write_text(md_out, cov.to_markdown())


def _shown(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO).as_posix()
    except ValueError:
        return str(path)


def summary_line(cov: Coverage, md_out: Path | None = None) -> str:
    """The line every recompiler run ends with."""
    c = cov.counts()
    total = c["functions"]
    parts = [
        f"{total} functions",
        f"{c['discovered']} discovered",
        f"{c['lifted']} lifted cleanly ({_pct(c['lifted'], total)})",
    ]
    if cov.compile.measured:
        parts.append(
            f"{c['compiled']} compiled ({_pct(c['compiled'], total)}, {cov.compile.compiler}, "
            f"{cov.compile.seconds:.1f} s)"
        )
        verdict = "met" if cov.met else "NOT met"
    else:
        parts.append(f"compile not measured ({cov.compile.why})")
        verdict = "met for lifting" if cov.lift_met else "NOT met for lifting"
    parts.append(f"{c['failures']} failures, {c['program_problems']} program problems")
    line = f"recomp: coverage: {', '.join(parts)}; target {TARGET:.0%} {verdict}"
    return line + (f"; wrote {_shown(md_out)}" if md_out is not None else "")


__all__ = [
    "COVERAGE_JSON", "COVERAGE_MD", "CompileResult", "Compiler", "Coverage", "Failure",
    "STAGES", "TARGET", "collect", "compile_program", "difftest_signal", "find_compiler",
    "form_signal", "load_tags", "not_measured", "summary_line", "unit_functions", "write",
]  # fmt: skip
