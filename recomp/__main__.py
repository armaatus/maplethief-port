"""Command line of the recompiler: ``python -m recomp <command>`` from the repository root.

    load [--slice PATH] [--out FILE]
        load the slice and write the loader manifest (default work/recomp/loader.json, schema
        in recomp/loader.py); prints the counts and any problems (exit status 1 if there are)
    discover [--slice PATH] [--out FILE] [--report FILE] [--ghidra FILE]
        discover every function (#32) and write work/recomp/functions.json (schema in
        recomp/discover.py) and work/recomp/discovery-report.md (failures, seeds, tables,
        noreturn, the cross-check with Ghidra's work/ghidra/functions.json if present);
        exit status 1 if more than 1% of the functions differ from Ghidra unexplained
    lift [--slice PATH] [--only PTR,PTR,...] [--out DIR] [--units N]
        lift functions (default: every LC_FUNCTION_STARTS function) into DIR
        (default work/gen/), split into at most N translation units (default 64,
        ADR 0021) plus gen.cmake for the build; prints one line per function with
        problems, then a summary
    all [--slice PATH] [--units N]
        the full pipeline into work/: load, discover, lift (exit status 1 if any failed)
    prototype [--slice PATH] [--out DIR]
        lift the ADR 0014 prototype functions into work/gen/proto/
    census [--slice PATH] [--out FILE] [--forms FILE]
        discover and lift every function and write work/recomp/census.json, every
        instruction form with its count and how many instances trap (schema in
        recomp/census.py); --forms also writes the integer form list the snippet suite
        must cover (tests/recomp/integer_forms.txt); exit status 1 if an integer
        instruction is unsupported
    difftest [--slice PATH] [--seed N] [--inputs N] [--jobs N] [--only PTR,...] [--case N]
        differential test of the leaf functions against Unicorn (#41, ADR 0017; needs the
        difftest group: scripts/difftest); writes work/recomp/difftest.json and .md; with
        --only PTR --case N re-runs one case and prints every difference; exit status 1 if
        any case mismatches

Later stages add their commands here (coverage #40).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from recomp import REPO, discovery_report, loader
from recomp.imports import ImportTable
from recomp.loader import DEFAULT_SLICE, Image
from recomp.output import DEFAULT_UNITS, write_program


def _ptrs(text: str) -> list[int]:
    return [int(p, 16) for p in text.split(",") if p.strip()]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="python -m recomp", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    p_load = sub.add_parser("load", help="write the loader manifest work/recomp/loader.json")
    p_load.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_load.add_argument("--out", type=Path, default=loader.MANIFEST_OUT)
    p_disc = sub.add_parser("discover", help="write work/recomp/functions.json and the report")
    p_disc.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_disc.add_argument("--out", type=Path, default=discovery_report.FUNCTIONS_OUT)
    p_disc.add_argument("--report", type=Path, default=discovery_report.REPORT_OUT)
    p_disc.add_argument("--ghidra", type=Path, default=discovery_report.GHIDRA_FUNCTIONS)
    p_lift = sub.add_parser("lift", help="lift functions to C")
    p_lift.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_lift.add_argument("--only", type=_ptrs, help="comma-separated function pointers (hex)")
    p_lift.add_argument("--out", type=Path, default=REPO / "work" / "gen")
    p_lift.add_argument("--units", type=int, default=DEFAULT_UNITS)
    p_all = sub.add_parser("all", help="load, discover and lift into work/")
    p_all.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_all.add_argument("--units", type=int, default=DEFAULT_UNITS)
    p_proto = sub.add_parser("prototype", help="lift the ADR 0014 prototype functions")
    p_proto.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_proto.add_argument("--out", type=Path)
    p_census = sub.add_parser("census", help="write work/recomp/census.json")
    p_census.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_census.add_argument("--out", type=Path)
    p_census.add_argument("--forms", type=Path, help="also write the integer form list")
    p_diff = sub.add_parser("difftest", help="differential test against Unicorn")
    p_diff.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_diff.add_argument("--seed", type=int, default=41)
    p_diff.add_argument("--inputs", type=int, default=1000, help="returning cases per function")
    p_diff.add_argument("--max-attempts", type=int, default=10_000)
    p_diff.add_argument("--jobs", type=int, default=4)
    p_diff.add_argument("--only", type=_ptrs, help="comma-separated function pointers (hex)")
    p_diff.add_argument("--case", type=int, help="with one --only pointer: re-run this case")
    args = ap.parse_args(argv)
    if args.command == "difftest":
        return _difftest(args)
    if args.command == "census":
        return _census(args.slice, args.out, args.forms)
    if args.command == "load":
        return _load(args.slice, args.out)
    if args.command == "discover":
        return _discover(args.slice, args.out, args.report, args.ghidra)
    if args.command == "all":
        status = _load(args.slice, loader.MANIFEST_OUT)
        status |= _discover(
            args.slice,
            discovery_report.FUNCTIONS_OUT,
            discovery_report.REPORT_OUT,
            discovery_report.GHIDRA_FUNCTIONS,
        )
        return status | main(["lift", "--slice", str(args.slice), "--units", str(args.units)])

    from recomp import prototype

    t0 = time.perf_counter()
    if args.command == "prototype":
        lifted, sources = prototype.build(args.slice, args.out or prototype.OUT_DIR)
    else:
        image = Image.load(args.slice)
        ptrs = args.only or list(image.function_starts)
        lifted = prototype.lift(image, ptrs)
        span = (image.text.addr, image.text.end)
        imports = ImportTable.from_image(image)
        sources = write_program(args.out, lifted, imports, args.units, span)
    for f in lifted:
        for problem in f.problems:
            print(f"{f.name}: {problem}")
    clean = sum(f.clean for f in lifted)
    out_dir = sources[0].parent if sources else "-"
    print(
        f"recomp: {len(lifted)} functions, {clean} lifted cleanly, "
        f"{len(lifted) - clean} with problems; C in {out_dir} ({time.perf_counter() - t0:.1f} s)"
    )
    return 0


def _discover(slice_path: Path, out: Path, report: Path, ghidra_path: Path) -> int:
    from mtport import ghidra_common as gc
    from recomp.discover import discover

    t0 = time.perf_counter()
    data = Path(slice_path).read_bytes()
    image = Image(data)
    found = discover(image)
    ghidra = discovery_report.write(
        found, image, gc.source_info(Path(slice_path), data, REPO), out, report, ghidra_path
    )
    c = found.counts()
    for ptr, problems in found.failures():
        for problem in problems:
            print(f"f_{ptr:08X}: {problem}")
    print(
        f"recomp: {c['functions']} functions ({c['failed']} not finished), "
        f"{c['instructions']:,} instructions, {c['blocks']:,} blocks, {c['tables']} jump tables, "
        f"{c['noreturn_functions']} noreturn functions; discovery {found.seconds:.1f} s"
    )
    status = 0
    if ghidra is not None:
        share = ghidra.unexplained_share()
        print(
            f"recomp: Ghidra cross-check: {len(ghidra.diffs)} of {ghidra.compared} functions "
            f"differ, {len(ghidra.unexplained)} unexplained ({share:.2%})"
        )
        status = int(share >= discovery_report.UNEXPLAINED_LIMIT)
    print(f"recomp: wrote {out} and {report} ({time.perf_counter() - t0:.1f} s)")
    return status


def _census(slice_path: Path, out: Path | None, forms: Path | None) -> int:
    from recomp import census

    t0 = time.perf_counter()
    out = out or census.CENSUS_OUT
    found = census.run(slice_path, out)
    if forms is not None:
        forms.write_text(census.integer_form_list(found), encoding="utf-8")
        print(f"recomp: wrote {forms}")
    c = found.counts()
    for f in sorted(found.forms.values(), key=lambda f: (f.cls, f.form)):
        if f.unsupported:
            print(f"unsupported: {f.cls} {f.form!r}: {f.unsupported} of {f.count}")
    print(
        f"recomp: {c['instructions']:,} instructions in {c['functions']} functions, "
        f"{c['forms']} forms ({c['integer_forms']} integer, {c['vfp_forms']} vfp, "
        f"{c['neon_forms']} neon, {c['undefined_forms']} undefined); "
        f"{c['unsupported_instructions']} unsupported instructions "
        f"({c['unsupported_integer_instructions']} integer)"
    )
    print(f"recomp: wrote {out} ({time.perf_counter() - t0:.1f} s)")
    return 1 if c["unsupported_integer_instructions"] else 0


def _difftest(args) -> int:
    from recomp import difftest

    if args.case is not None:
        if not args.only or len(args.only) != 1:
            print("recomp: --case needs exactly one --only pointer")
            return 2
        r, detail = difftest.run_case(args.slice, args.only[0], args.case, args.seed)
        print(f"case {args.seed}:0x{args.only[0]:X}:{args.case}: unicorn {r.statuses}")
        for name, diffs in detail:
            print(f"{name}: " + ("; ".join(diffs) if diffs else "no difference"))
        return 1 if r.mismatches or r.driver_error else 0

    def progress(done: int, total: int, r) -> None:
        if r.mismatches or r.driver_error:
            first = r.examples[0] if r.examples else {"first": r.driver_error, "seed": "-"}
            print(f"MISMATCH 0x{r.ptr:X}: {r.mismatches} cases; {first['first']} ({first['seed']})")
        if done % 50 == 0 or done == total:
            print(f"recomp: difftest {done}/{total} functions", flush=True)

    run = difftest.run(
        args.slice, args.seed, args.inputs, args.max_attempts, args.jobs, args.only,
        progress=progress,
    )  # fmt: skip
    difftest.write_report(run)
    s = run.summary()
    print(
        f"recomp: {s['passed']} of {s['tested']} leaf functions passed {args.inputs} inputs with "
        f"zero mismatches; {s['with_mismatches']} with mismatches, {s['not_bounded']} not "
        f"bounded; {s['cases_returned']:,} returning + {s['cases_faulted']:,} faulting cases "
        f"({run.seconds['total']:.1f} s)"
    )
    print(f"recomp: wrote {difftest.REPORT_JSON} and {difftest.REPORT_MD}")
    return 1 if s["with_mismatches"] else 0


def _load(slice_path: Path, out: Path) -> int:
    t0 = time.perf_counter()
    image = loader.write_manifest(slice_path, out)
    for key, value in image.summary().items():
        print(f"  {key:<16} {value}")
    problems = image.problems()
    for problem in problems:
        print(f"problem: {problem}")
    print(f"recomp: wrote {out} ({time.perf_counter() - t0:.1f} s), {len(problems)} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
