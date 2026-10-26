"""Command line of the recompiler: ``python -m recomp <command>`` from the repository root.

    load [--slice PATH] [--out FILE]
        load the slice and write the loader manifest (default work/recomp/loader.json, schema
        in recomp/loader.py); prints the counts and any problems (exit status 1 if there are)
    discover [--slice PATH] [--out FILE] [--report FILE] [--ghidra FILE]
        discover every function (#32) and write work/recomp/functions.json (schema in
        recomp/discover.py) and work/recomp/discovery-report.md (failures, seeds, tables,
        noreturn, the cross-check with Ghidra's work/ghidra/functions.json if present);
        exit status 1 if more than 1% of the functions differ from Ghidra unexplained
    lift [--slice PATH] [--only PTR,PTR,...] [--out DIR] [--units N] [--compile] [--jobs N]
         [--coverage DIR]
        lift functions (default: every LC_FUNCTION_STARTS function) into DIR
        (default work/gen/), split into at most N translation units (default 64,
        ADR 0021) plus gen.cmake for the build; prints one line per function with
        problems, then a summary. Without --only it also writes the coverage report
        (coverage.json and coverage.md in --coverage DIR, default work/recomp/; #40) and
        ends with its summary line; --compile measures compiling too (as `coverage`)
    all [--slice PATH] [--units N] [--compile] [--jobs N]
        the full pipeline into work/: load, discover, lift and the coverage report (exit
        status 1 if any stage failed or fewer than 99% of the functions lift cleanly, or with
        --compile, lift and compile)
    coverage [--slice PATH] [--out DIR] [--units N] [--no-compile] [--jobs N] [--coverage DIR]
        discover and lift into DIR (default work/gen/), compile every generated unit with the
        host C compiler (strict flags; errors charged to functions) and write
        work/recomp/coverage.json and coverage.md (#40, schema in recomp/coverage.py): counts
        discovered / lifted / compiled by #10 tag, every failure with its reason and
        address, quality signals; exit status 1 unless 99% of the functions are lifted and
        compiled with no program problem
    prototype [--slice PATH] [--out DIR]
        lift the ADR 0014 prototype functions into work/gen/proto/
    census [--slice PATH] [--out FILE] [--forms FILE] [--vfp-forms FILE]
        discover and lift every function and write work/recomp/census.json, every
        instruction form with its count and how many instances trap (schema in
        recomp/census.py); --forms / --vfp-forms also write the integer / VFP and NEON
        form lists the snippet suite must cover (tests/recomp/integer_forms.txt,
        tests/recomp/vfp_forms.txt); exit status 1 if any instruction is unsupported
    difftest [--slice PATH] [--seed N] [--inputs N] [--jobs N] [--only PTR,...] [--case N]
        differential test of the leaf functions against Unicorn (#41, ADR 0017; needs the
        difftest group: scripts/difftest); writes work/recomp/difftest.json and .md; with
        --only PTR --case N re-runs one case and prints every difference; exit status 1 if
        any case mismatches
    exceptions [--slice PATH] [--out FILE]
        find every SjLj function context, parse its LSDA and evaluate its landing-pad
        dispatch (#37), and write work/recomp/exceptions.json (schema in
        recomp/exceptions.py); exit status 1 if a context or LSDA has a problem

Every whole-program run (``lift`` without ``--only``, ``all``, ``coverage``) ends with the
coverage summary line of ``recomp/coverage.py``.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from recomp import REPO, discovery_report, loader
from recomp import coverage as coverage_mod
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
    _coverage_args(p_lift, compile_default=False)
    p_all = sub.add_parser("all", help="load, discover, lift and report coverage into work/")
    p_all.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_all.add_argument("--units", type=int, default=DEFAULT_UNITS)
    _coverage_args(p_all, compile_default=False)
    p_cov = sub.add_parser("coverage", help="lift, compile and write work/recomp/coverage.md")
    p_cov.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_cov.add_argument("--out", type=Path, default=REPO / "work" / "gen")
    p_cov.add_argument("--units", type=int, default=DEFAULT_UNITS)
    _coverage_args(p_cov, compile_default=True)
    p_proto = sub.add_parser("prototype", help="lift the ADR 0014 prototype functions")
    p_proto.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_proto.add_argument("--out", type=Path)
    p_census = sub.add_parser("census", help="write work/recomp/census.json")
    p_census.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_census.add_argument("--out", type=Path)
    p_census.add_argument("--forms", type=Path, help="also write the integer form list")
    p_census.add_argument("--vfp-forms", type=Path, help="also write the VFP/NEON form list")
    p_diff = sub.add_parser("difftest", help="differential test against Unicorn")
    p_diff.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_diff.add_argument("--seed", type=int, default=41)
    p_diff.add_argument("--inputs", type=int, default=1000, help="returning cases per function")
    p_diff.add_argument("--max-attempts", type=int, default=10_000)
    p_diff.add_argument("--jobs", type=int, default=4)
    p_diff.add_argument("--only", type=_ptrs, help="comma-separated function pointers (hex)")
    p_diff.add_argument("--case", type=int, help="with one --only pointer: re-run this case")
    p_exc = sub.add_parser("exceptions", help="write work/recomp/exceptions.json")
    p_exc.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_exc.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    if args.command == "exceptions":
        return _exceptions(args.slice, args.out)
    if args.command == "difftest":
        return _difftest(args)
    if args.command == "census":
        return _census(args.slice, args.out, args.forms, args.vfp_forms)
    if args.command == "load":
        return _load(args.slice, args.out)
    if args.command == "discover":
        return _discover(args.slice, args.out, args.report, args.ghidra)[0]
    if args.command == "all":
        status = _load(args.slice, loader.MANIFEST_OUT)
        image = Image(Path(args.slice).read_bytes())
        found_status, found = _discover(
            args.slice,
            discovery_report.FUNCTIONS_OUT,
            discovery_report.REPORT_OUT,
            discovery_report.GHIDRA_FUNCTIONS,
            image,
        )
        return status | found_status | _lift(args, image, REPO / "work" / "gen", True, found)
    if args.command == "coverage":
        return _lift(args, Image.load(args.slice), args.out, gate=True)
    if args.command == "lift" and not args.only:
        _lift(args, Image.load(args.slice), args.out, gate=False)
        return 0

    from recomp import prototype

    t0 = time.perf_counter()
    if args.command == "prototype":
        lifted, sources = prototype.build(args.slice, args.out or prototype.OUT_DIR)
    else:
        image = Image.load(args.slice)
        lifted = prototype.lift(image, args.only)
        span = (image.text.addr, image.text.end)
        imports = ImportTable.from_image(image)
        sources = write_program(args.out, lifted, imports, args.units, span)
    _print_lifted(lifted, sources, time.perf_counter() - t0)
    return 0


def _coverage_args(p: argparse.ArgumentParser, compile_default: bool) -> None:
    if compile_default:
        p.add_argument("--no-compile", dest="compile", action="store_false",
                       help="do not compile the generated C (compiled: not measured)")  # fmt: skip
    else:
        p.add_argument("--compile", action="store_true",
                       help="also compile the generated C for the coverage report")  # fmt: skip
    p.add_argument("--jobs", type=int, help="parallel compiler processes (default: CPUs)")
    p.add_argument("--coverage", type=Path, default=coverage_mod.COVERAGE_JSON.parent,
                   help="directory of coverage.json and coverage.md")  # fmt: skip


def _print_lifted(lifted, sources, seconds: float) -> None:
    for f in lifted:
        for problem in f.problems:
            print(f"{f.name}: {problem}")
        for addr, text in f.unsupported:
            print(f"{f.name}: unsupported instruction at 0x{addr:X}: {text}")
    clean = sum(f.clean for f in lifted)
    out_dir = sources[0].parent if sources else "-"
    print(
        f"recomp: {len(lifted)} functions, {clean} lifted cleanly, "
        f"{len(lifted) - clean} with problems; C in {out_dir} ({seconds:.1f} s)"
    )


def _lift(args, image: Image, out: Path, gate: bool, found=None) -> int:
    """Lift every LC_FUNCTION_STARTS function into ``out`` (``found``: the discovery, if
    already made), then write the coverage report (compiling if ``args.compile``) and print
    its summary line. With ``gate``, the exit status is 1 unless the coverage target is met."""
    from mtport import ghidra_common as gc
    from recomp import census, exceptions, prototype
    from recomp.discover import discover

    t0 = time.perf_counter()
    found = found or discover(image)
    lifted = prototype.lift(image, list(image.function_starts), discovery=found)
    span = (image.text.addr, image.text.end)
    imports = ImportTable.from_image(image)
    sources = write_program(out, lifted, imports, args.units, span)
    _print_lifted(lifted, sources, time.perf_counter() - t0)
    if args.compile:
        cells = coverage_mod.unit_functions(lifted, args.units, span)
        compiled = coverage_mod.compile_program(out, sources, cells, jobs=args.jobs)
    else:
        compiled = coverage_mod.not_measured("not asked for: --compile")
    traps = {addr for f in lifted for addr, _ in f.unsupported}
    signals = {
        "forms": coverage_mod.form_signal(census.take(found.functions.values(), traps)),
        "difftest": coverage_mod.difftest_signal(coverage_mod.DIFFTEST_JSON, found.functions),
    }
    cov = coverage_mod.collect(
        gc.source_info(Path(args.slice), image.data, REPO),
        found,
        lifted,
        compiled,
        exceptions=exceptions.report(image, found),
        imports=imports,
        tags=coverage_mod.load_tags(),
        signals=signals,
    )
    json_out, md_out = args.coverage / "coverage.json", args.coverage / "coverage.md"
    coverage_mod.write(cov, json_out, md_out)
    for ptr in cov.not_clean():
        for f in cov.by_ptr.get(ptr, ()):
            if f.stage in ("compile", "sjlj"):  # the lifter's problems are printed above
                print(f"f_{ptr:08X}: {f.stage}: {f.reason}")
    for f in cov.program_problems:
        print(f"problem: {f.stage}: {f.reason}")
    print(coverage_mod.summary_line(cov, md_out) + f" ({time.perf_counter() - t0:.1f} s)")
    if not gate:
        return 0
    met = cov.met if args.compile else cov.lift_met
    return 0 if met else 1


def _discover(
    slice_path: Path, out: Path, report: Path, ghidra_path: Path, image: Image | None = None
):
    """Discover, write functions.json and the report; returns (exit status, the discovery)."""
    from mtport import ghidra_common as gc
    from recomp.discover import discover

    t0 = time.perf_counter()
    image = image or Image(Path(slice_path).read_bytes())
    data = image.data
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
    return status, found


def _census(
    slice_path: Path, out: Path | None, forms: Path | None, vfp_forms: Path | None = None
) -> int:
    from recomp import census

    t0 = time.perf_counter()
    out = out or census.CENSUS_OUT
    found = census.run(slice_path, out)
    if forms is not None:
        forms.write_text(census.integer_form_list(found), encoding="utf-8")
        print(f"recomp: wrote {forms}")
    if vfp_forms is not None:
        vfp_forms.write_text(census.vfp_form_list(found), encoding="utf-8")
        print(f"recomp: wrote {vfp_forms}")
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
    return 1 if c["unsupported_instructions"] else 0


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


def _exceptions(slice_path: Path, out: Path | None) -> int:
    from recomp import exceptions

    t0 = time.perf_counter()
    out = out or exceptions.EXCEPTIONS_OUT
    rep = exceptions.write(slice_path, out)
    for ctx in rep.contexts:
        for problem in ctx.problems:
            print(f"f_{ctx.ptr:08X}: {problem}")
    c = rep.counts()
    raisers = ", ".join(f"{k} ({len(v)} functions)" for k, v in sorted(rep.raisers.items()))
    print(
        f"recomp: {c['contexts']} SjLj contexts, {c['lsdas']} LSDAs ({c['lsda_bytes']} bytes, "
        f"{c['call_sites']} call sites: {c.get('cleanup', 0)} cleanup, {c.get('catch', 0)} "
        f"catch, {c.get('catch-all', 0)} catch-all, {c.get('spec', 0)} exception spec); "
        f"raised only by {raisers or 'nothing'}"
    )
    print(f"recomp: wrote {out} ({time.perf_counter() - t0:.1f} s)")
    return 1 if c["problems"] else 0


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
