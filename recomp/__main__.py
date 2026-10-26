"""Command line of the recompiler: ``python -m recomp <command>`` from the repository root.

    load [--slice PATH] [--out FILE]
        load the slice and write the loader manifest (default work/recomp/loader.json, schema
        in recomp/loader.py); prints the counts and any problems (exit status 1 if there are)
    discover [--slice PATH] [--out FILE] [--report FILE] [--ghidra FILE]
        discover every function (#32) and write work/recomp/functions.json (schema in
        recomp/discover.py) and work/recomp/discovery-report.md (failures, seeds, tables,
        noreturn, the cross-check with Ghidra's work/ghidra/functions.json if present);
        exit status 1 if more than 1% of the functions differ from Ghidra unexplained
    lift [--slice PATH] [--only PTR,PTR,...] [--out DIR]
        lift functions (default: every LC_FUNCTION_STARTS function) into DIR
        (default work/gen/); prints one line per function with problems, then a summary
    prototype [--slice PATH] [--out DIR]
        lift the ADR 0014 prototype functions into work/gen/proto/
    census [--slice PATH] [--out FILE] [--forms FILE]
        discover and lift every function and write work/recomp/census.json, every
        instruction form with its count and how many instances trap (schema in
        recomp/census.py); --forms also writes the integer form list the snippet suite
        must cover (tests/recomp/integer_forms.txt); exit status 1 if an integer
        instruction is unsupported
    exceptions [--slice PATH] [--out FILE]
        find every SjLj function context, parse its LSDA and evaluate its landing-pad
        dispatch (#37), and write work/recomp/exceptions.json (schema in
        recomp/exceptions.py); exit status 1 if a context or LSDA has a problem

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
from recomp.output import write_program


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
    p_proto = sub.add_parser("prototype", help="lift the ADR 0014 prototype functions")
    p_proto.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_proto.add_argument("--out", type=Path)
    p_census = sub.add_parser("census", help="write work/recomp/census.json")
    p_census.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_census.add_argument("--out", type=Path)
    p_census.add_argument("--forms", type=Path, help="also write the integer form list")
    p_exc = sub.add_parser("exceptions", help="write work/recomp/exceptions.json")
    p_exc.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_exc.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    if args.command == "exceptions":
        return _exceptions(args.slice, args.out)
    if args.command == "census":
        return _census(args.slice, args.out, args.forms)
    if args.command == "load":
        return _load(args.slice, args.out)
    if args.command == "discover":
        return _discover(args.slice, args.out, args.report, args.ghidra)

    from recomp import prototype

    t0 = time.perf_counter()
    if args.command == "prototype":
        lifted, sources = prototype.build(args.slice, args.out or prototype.OUT_DIR)
    else:
        image = Image.load(args.slice)
        ptrs = args.only or list(image.function_starts)
        lifted = prototype.lift(image, ptrs)
        sources = write_program(args.out, lifted, ImportTable.from_image(image))
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
