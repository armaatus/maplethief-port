"""Differential testing of lifted functions against an emulator (#41, ADR 0017).

The game's leaf functions (:mod:`recomp.difftest.select`) run twice on the same seeded
random inputs (:mod:`recomp.difftest.inputs`): as machine code inside Unicorn
(:mod:`recomp.difftest.emulator`) and as our recompiled C (:mod:`recomp.difftest.host`).
Per function, :func:`test_function` generates cases until :data:`INPUTS` of them return in
Unicorn (at most ``max_attempts``), runs every case that returned or faulted through both C
variants, and compares:

- how the run ended: a return, or a memory fault at the same address and PC;
- r0-r12, sp, lr (at a return);
- NZCV, for the flags-in/flags-out variant of a function without calls (a call leaves the
  flags undefined, AAPCS); the FPSCR NZCV bits (both variants);
- d0-d31 (every function: a stray VFP write shows too);
- every byte of guest memory that changed, on either side.

A mismatch is reported with the function, the case seed and the first difference.
``python -m recomp difftest`` (``scripts/difftest``) writes ``work/recomp/difftest.json``
and ``work/recomp/difftest.md``; ``--only PTR --case N`` re-runs one case and prints every
difference. It needs the game binary and the ``difftest`` group, so it runs locally only;
``tests/test_recomp_difftest_real.py`` runs it as a ``real_assets`` test.

The input is untrusted data: guest code runs only inside Unicorn and as recompiled C.
"""

from __future__ import annotations

import json
import multiprocessing
import time
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from recomp import REPO
from recomp.difftest import host, inputs
from recomp.difftest.select import CRITERIA, Leaf, Selection, select_leaves

INPUTS = 1000  # cases per function that must return in Unicorn
MAX_ATTEMPTS = 10_000  # cases per function at most
SEED = 41
OUT_DIR = REPO / "work" / "recomp" / "difftest"
REPORT_JSON = REPO / "work" / "recomp" / "difftest.json"
REPORT_MD = REPO / "work" / "recomp" / "difftest.md"
DRIVER_TIMEOUT = 300.0  # seconds per function
KEEP_MISMATCHES = 5  # per function in the report
_REG_NAMES = [f"r{n}" for n in range(13)] + ["sp", "lr"]


@dataclass(frozen=True)
class Target:
    """What a worker needs about one function (picklable)."""

    index: int  # in the build's function table
    ptr: int
    pointer_regs: frozenset[int]
    vfp: bool
    has_calls: bool
    instructions: int


@dataclass
class FunctionResult:
    ptr: int
    instructions: int
    vfp: bool
    has_calls: bool
    attempts: int = 0
    statuses: dict[str, int] = field(default_factory=dict)  # Unicorn: return/fault/...
    returned: int = 0  # cases that returned in Unicorn and were compared
    faulted: int = 0  # cases that faulted in Unicorn and were compared
    mismatches: int = 0
    examples: list[dict] = field(default_factory=list)  # first mismatches
    errors: list[str] = field(default_factory=list)  # first emulator error messages
    driver_error: str = ""
    seconds: float = 0.0

    def passed(self, wanted: int = INPUTS) -> bool:
        return self.returned >= wanted and not self.mismatches and not self.driver_error


def target(index: int, leaf: Leaf) -> Target:
    fn = leaf.fn
    return Target(index, fn.ptr, leaf.pointer_regs, leaf.vfp, leaf.has_calls, len(fn.insns))


def differences(end, res: host.Result, variant: int, flags: bool) -> list[str]:
    """Every difference between Unicorn's end state ``end`` and the C run ``res``."""
    out: list[str] = []
    if end.status != res.status:
        detail = ", ".join(f"0x{x:X}" for x in res.detail)
        return [f"status: unicorn {end.status}, C {res.status} ({detail})"]
    if end.status == "fault":
        addr, pc = end.fault
        if (res.detail[0], res.detail[2]) != (addr, pc):
            c_addr, c_pc = res.detail[0], res.detail[2]
            out.append(f"fault: unicorn 0x{addr:X} at 0x{pc:X}, C 0x{c_addr:X} at 0x{c_pc:X}")
    else:
        for n, name in enumerate(_REG_NAMES):
            if end.regs[n] != res.regs[n]:
                out.append(f"{name}: unicorn 0x{end.regs[n]:08X}, C 0x{res.regs[n]:08X}")
        if variant == 1 and flags and end.cpsr != res.cpsr & 0xF0000000:
            out.append(f"nzcv: unicorn {end.cpsr >> 28:04b}, C {res.cpsr >> 28:04b}")
        if end.fpscr & 0xF0000000 != res.fpscr & 0xF0000000:
            out.append(f"fpscr.nzcv: unicorn {end.fpscr >> 28:04b}, C {res.fpscr >> 28:04b}")
        for n in range(32):
            if end.d[n] != res.d[n]:
                out.append(f"d{n}: unicorn 0x{end.d[n]:016X}, C 0x{res.d[n]:016X}")
    for addr in sorted(end.mem.keys() | res.mem.keys()):
        x, y = end.mem.get(addr), res.mem.get(addr)
        if x != y:
            ux = "unchanged" if x is None else f"0x{x:02X}"
            cy = "unchanged" if y is None else f"0x{y:02X}"
            out.append(f"mem 0x{addr:X}: unicorn {ux}, C {cy}")
            break  # the first byte is enough
    return out


# -- workers ------------------------------------------------------------------------------

_W: dict = {}


def _init_worker(pristine_path: str, exe: str, work: str, seed: int, data, text) -> None:
    from recomp.difftest.emulator import Emulator

    pristine = Path(pristine_path).read_bytes()
    emu = Emulator(pristine, text)
    _W.update(emu=emu, pristine=pristine_path, exe=Path(exe), work=Path(work))
    _W.update(seed=seed, data=data)


def test_function(
    t: Target, wanted: int = INPUTS, max_attempts: int = MAX_ATTEMPTS, only_case: int | None = None
) -> tuple[FunctionResult, list[tuple[str, list[str]]]]:
    """Run one function (in a worker). Returns its result and, for ``only_case``, every
    difference of that case per variant."""
    t0 = time.perf_counter()
    emu, seed, data = _W["emu"], _W["seed"], _W["data"]
    r = FunctionResult(t.ptr, t.instructions, t.vfp, t.has_calls)
    pool = inputs.heap_pool(seed, t.ptr)
    statuses: Counter[str] = Counter()
    valid = []
    returned = 0
    indices = [only_case] if only_case is not None else range(max_attempts)
    for index in indices:
        case = inputs.make_case(seed, t.ptr, index, t.pointer_regs, t.vfp, data)
        end = emu.run(t.ptr, case, pool)
        statuses[end.status] += 1
        r.attempts += 1
        if end.status in ("return", "fault"):
            valid.append((index, case, end))
            returned += end.status == "return"
            if returned >= wanted:
                break
        elif end.status == "error" and len(r.errors) < 3:
            r.errors.append(f"{case.seed}: {end.error}")
    r.statuses = dict(statuses)
    detail: list[tuple[str, list[str]]] = []
    if valid:
        stem = _W["work"] / f"cases_{t.ptr:08X}"
        cases_path, results_path = stem.with_suffix(".in"), stem.with_suffix(".out")
        host.write_cases(cases_path, t.index, pool, [(i, c) for i, c, _ in valid])
        r.driver_error = host.run_driver(
            _W["exe"], Path(_W["pristine"]), cases_path, results_path, DRIVER_TIMEOUT
        )
        if not r.driver_error:
            results = host.read_results(results_path)
            for index, case, end in valid:
                bad = False
                for variant in (0, 1):
                    diffs = differences(end, results[(index, variant)], variant, not t.has_calls)
                    if only_case is not None:
                        detail.append((f"{'fg'[variant]}_{t.ptr:08X}", diffs))
                    if diffs and not bad:
                        bad = True
                        r.mismatches += 1
                        if len(r.examples) < KEEP_MISMATCHES:
                            name = f"{'fg'[variant]}_{t.ptr:08X}"
                            r.examples.append(
                                {"seed": case.seed, "function": name, "first": diffs[0]}
                            )
                if not bad:
                    if end.status == "return":
                        r.returned += 1
                    else:
                        r.faulted += 1
        for path in (cases_path, results_path):
            path.unlink(missing_ok=True)
    r.seconds = time.perf_counter() - t0
    return r, detail


def _work(args) -> FunctionResult:
    t, wanted, max_attempts = args
    return test_function(t, wanted, max_attempts)[0]


# -- the run ------------------------------------------------------------------------------


@dataclass
class Run:
    selection: Selection
    results: list[FunctionResult]
    seed: int
    wanted: int
    max_attempts: int
    seconds: dict[str, float]

    @property
    def passed(self) -> list[FunctionResult]:
        return [r for r in self.results if r.passed(self.wanted)]

    @property
    def mismatched(self) -> list[FunctionResult]:
        return [r for r in self.results if r.mismatches or r.driver_error]

    def summary(self) -> dict:
        res = self.results
        return {
            "leaf_functions": len(self.selection.leaves),
            "tested": len(res),
            "passed": len(self.passed),
            "with_mismatches": len(self.mismatched),
            "not_bounded": sum(
                r.returned < self.wanted and not r.mismatches and not r.driver_error for r in res
            ),
            "cases_returned": sum(r.returned for r in res),
            "cases_faulted": sum(r.faulted for r in res),
            "mismatched_cases": sum(r.mismatches for r in res),
            "vfp_passed": sum(r.vfp for r in self.passed),
            "with_calls_passed": sum(r.has_calls for r in self.passed),
        }

    def to_json(self) -> str:
        doc = {
            "schema": 1,
            "seed": self.seed,
            "inputs_per_function": self.wanted,
            "max_attempts": self.max_attempts,
            "selection": self.selection.counts,
            "summary": self.summary(),
            "seconds": {k: round(v, 1) for k, v in self.seconds.items()},
            "functions": [
                {
                    "ptr": f"0x{r.ptr:X}",
                    "instructions": r.instructions,
                    "vfp": r.vfp,
                    "calls": r.has_calls,
                    "passed": r.passed(self.wanted),
                    "attempts": r.attempts,
                    "unicorn": r.statuses,
                    "returned": r.returned,
                    "faulted": r.faulted,
                    "mismatches": r.mismatches,
                    "examples": r.examples,
                    "errors": r.errors,
                    "driver_error": r.driver_error,
                    "seconds": round(r.seconds, 2),
                }
                for r in sorted(self.results, key=lambda r: r.ptr)
            ],
        }
        return json.dumps(doc, indent=1) + "\n"

    def to_markdown(self) -> str:
        s = self.summary()
        sel = self.selection.counts
        lines = [
            "# Differential test: lifted leaf functions against Unicorn",
            "",
            f"Generated by `python -m recomp difftest` (#41, ADR 0017); seed {self.seed}, "
            f"{self.wanted} returning inputs per function, at most {self.max_attempts} attempts.",
            "",
            "## Selection",
            "",
            "| criterion | functions passing |",
            "|---|---|",
            *(f"| {c} | {sel.get(c, 0)} |" for c in CRITERIA),
            "",
            "## Result",
            "",
            f"- **{s['passed']}** of {s['tested']} leaf functions passed {self.wanted} inputs "
            f"with zero mismatches ({s['vfp_passed']} with VFP code, {s['with_calls_passed']} "
            "calling other leaves).",
            f"- {s['with_mismatches']} functions with mismatches, {s['not_bounded']} without "
            f"{self.wanted} bounded returning runs.",
            f"- {s['cases_returned']:,} returning and {s['cases_faulted']:,} faulting cases "
            "compared.",
            "- Wall time: " + ", ".join(f"{k} {v:.1f} s" for k, v in self.seconds.items()) + ".",
            "",
        ]
        bad = sorted(self.mismatched, key=lambda r: r.ptr)
        if bad:
            lines += [
                "## Mismatches",
                "",
                "| function | cases | first | seed |",
                "|---|---|---|---|",
            ]
            for r in bad:
                if r.driver_error:
                    lines.append(f"| 0x{r.ptr:X} | - | {r.driver_error} | - |")
                for ex in r.examples[:1]:
                    lines.append(
                        f"| {ex['function']} | {r.mismatches} | {ex['first']} | `{ex['seed']}` |"
                    )
            lines.append("")
        return "\n".join(lines)


def prepare(slice_path: Path, out_dir: Path, jobs: int):
    """Load, select, build: ``(image, selection, targets, exe, pristine path)``."""
    from recomp.discover import discover
    from recomp.loader import Image

    image = Image.load(slice_path)
    selection = select_leaves(image, discover(image))
    leaves = list(selection.leaves.values())
    exe = host.build(image, leaves, out_dir / "build", jobs=jobs)
    pristine = out_dir / "pristine.bin"
    pristine.write_bytes(inputs.pristine(image))
    targets = [target(i, lf) for i, lf in enumerate(leaves)]
    return image, selection, targets, exe, pristine


def run(
    slice_path: Path,
    seed: int = SEED,
    wanted: int = INPUTS,
    max_attempts: int = MAX_ATTEMPTS,
    jobs: int = 4,
    only: list[int] | None = None,
    out_dir: Path = OUT_DIR,
    progress=None,
) -> Run:
    """Select, build and test every leaf function (or ``only`` these pointers)."""
    t0 = time.perf_counter()
    image, selection, targets, exe, pristine = prepare(slice_path, out_dir, jobs)
    if only:
        missing = set(only) - {t.ptr for t in targets}
        if missing:
            raise ValueError(f"not leaf functions: {', '.join(f'0x{p:X}' for p in missing)}")
        targets = [t for t in targets if t.ptr in set(only)]
    t1 = time.perf_counter()
    init = (str(pristine), str(exe), str(out_dir), seed, inputs.data_range(image),
            inputs.text_range(image))  # fmt: skip
    work = [(t, wanted, max_attempts) for t in targets]
    # Big functions first, so the long ones do not end up last on one worker.
    work.sort(key=lambda w: -w[0].instructions)
    results = []
    ctx = multiprocessing.get_context("spawn")
    with ctx.Pool(max(1, jobs), initializer=_init_worker, initargs=init) as pool:
        for r in pool.imap_unordered(_work, work):
            results.append(r)
            if progress:
                progress(len(results), len(work), r)
    t2 = time.perf_counter()
    seconds = {"prepare": t1 - t0, "test": t2 - t1, "total": t2 - t0}
    return Run(selection, results, seed, wanted, max_attempts, seconds)


def run_case(slice_path: Path, ptr: int, case: int, seed: int = SEED, out_dir: Path = OUT_DIR):
    """Re-run one case of one function; ``(result, [(function, differences)])``."""
    image, _, targets, exe, pristine = prepare(slice_path, out_dir, 4)
    t = next((t for t in targets if t.ptr == ptr), None)
    if t is None:
        raise ValueError(f"0x{ptr:X} is not a leaf function")
    _init_worker(str(pristine), str(exe), str(out_dir), seed, inputs.data_range(image),
                 inputs.text_range(image))  # fmt: skip
    return test_function(t, 1, 1, only_case=case)


def write_report(result: Run, json_path: Path = REPORT_JSON, md_path: Path = REPORT_MD) -> None:
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(result.to_json(), encoding="utf-8")
    md_path.write_text(result.to_markdown(), encoding="utf-8")


__all__ = [
    "INPUTS", "MAX_ATTEMPTS", "SEED", "FunctionResult", "Run", "Target", "differences",
    "prepare", "run", "run_case", "test_function", "write_report",
]  # fmt: skip
