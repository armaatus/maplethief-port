"""Command line of the recompiler: ``python -m recomp <command>`` from the repository root.

    load [--slice PATH] [--out FILE]
        load the slice and write the loader manifest (default work/recomp/loader.json, schema
        in recomp/loader.py); prints the counts and any problems (exit status 1 if there are)
    lift [--slice PATH] [--only PTR,PTR,...] [--out DIR]
        lift functions (default: every LC_FUNCTION_STARTS Thumb function) into DIR
        (default work/gen/); prints one line per function with problems, then a summary
    prototype [--slice PATH] [--out DIR]
        lift the ADR 0014 prototype functions into work/gen/proto/

Later stages add their commands here (discover #32, census #33, coverage #40).
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from recomp import REPO, loader
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
    p_lift = sub.add_parser("lift", help="lift functions to C")
    p_lift.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_lift.add_argument("--only", type=_ptrs, help="comma-separated function pointers (hex)")
    p_lift.add_argument("--out", type=Path, default=REPO / "work" / "gen")
    p_proto = sub.add_parser("prototype", help="lift the ADR 0014 prototype functions")
    p_proto.add_argument("--slice", type=Path, default=DEFAULT_SLICE)
    p_proto.add_argument("--out", type=Path)
    args = ap.parse_args(argv)
    if args.command == "load":
        return _load(args.slice, args.out)

    from recomp import prototype

    t0 = time.perf_counter()
    if args.command == "prototype":
        lifted, sources = prototype.build(args.slice, args.out or prototype.OUT_DIR)
    else:
        image = Image.load(args.slice)
        ptrs = args.only or [p for p in image.function_starts if p & 1]
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
