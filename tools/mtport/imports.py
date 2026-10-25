"""Import table analysis of the armv7 slice: every imported symbol, where it is used, by whom.

The HLE work list (#11). Inputs: the armv7 slice (``scripts/bootstrap``) and the Ghidra exports
``work/ghidra/{functions,xrefs}.json`` (``tools/ghidra/import``). Steps:

1. **Import table.** The 333 undefined ``nlist`` symbols with their library (two-level
   namespace ordinal), and every place dyld binds them: ``__symbolstub1`` stubs and
   ``__lazy_symbol`` pointers (LC_DYSYMTAB indirect table, checked against each stub's own
   ``ldr pc, [pc, #n]`` and against the lazy-bind stream), ``__nl_symbol_ptr`` pointers, and
   the plain binds into data (``__objc_classrefs``, class structures, CFStrings, vtables).
2. **Code sites** (:mod:`mtport.armscan`, Capstone): ``bl``/``blx``/``b`` to a stub, and loads
   of a pointer slot (``__nl_symbol_ptr``, ``__objc_classrefs``, a bound ``__data`` word).
   Cross-checked against Ghidra's xrefs.
3. **Data uses.** A bind inside a data structure (a class's superclass, a CFString's isa, a
   vtable entry) is used by whoever uses that structure: the class's group, the functions
   that reference the CFString or block, and otherwise the game (C++ vtables and RTTI).
4. **Reachability** (:func:`classify_functions`, first pass, until #10 tags libraries): a
   function is Chartboost's when it is an IMP of a Chartboost class, or of a category method
   whose selector only Chartboost code sends, or is reachable only from those in the Ghidra
   call graph. Everything else reachable from a root that is not Chartboost's is the game's.
5. **Classes.** Each import is *game*, *chartboost* (only Chartboost code uses it), *both*,
   or *unreferenced*. The HLE layer must implement *game* + *both*.
6. **ObjC sends.** At each ``objc_msgSend``-family call, the selector (the ``__objc_selrefs``
   slot loaded into r1/r2) and, where determinable, the receiving class: a class loaded from
   ``__objc_classrefs``, the result of ``+alloc``/``-init…`` on a known class, or the
   superclass for ``objc_msgSendSuper2``.

Command line::

    python3 -m mtport.imports generate [--repo DIR] [--slice PATH] [--ghidra DIR]
        # writes work/re/imports.json, work/re/names/imports.json and docs/re/imports.md
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import macho
from mtport.imports_analysis import (
    BOTH,
    CHARTBOOST,
    CLASSES,
    DISPATCHERS,
    GAME,
    GHIDRA_DIR,
    JSON_OUT,
    MARKDOWN_OUT,
    NAMES_OUT,
    POINTER_DATA,
    SCHEMA_VERSION,
    SLOT_SECTIONS,
    UNREFERENCED,
    Analysis,
    Ghidra,
    Import,
    analyse,
    call_graph,
    chartboost_entries,
    check_stubs,
    classify_functions,
    framework_name,
    ghidra_cross_check,
    import_class,
    import_table,
    load_ghidra,
    resolve_sends,
    run,
    sides,
)
from mtport.imports_doc import (
    counts,
    import_rows,
    message_rows,
    names_json,
    to_json,
    to_markdown,
)

# mtport.imports is the public face of the analysis (imports_analysis) and its outputs
# (imports_doc): it re-exports their names and keeps the command line.
__all__ = [
    "Analysis",
    "BOTH",
    "CHARTBOOST",
    "CLASSES",
    "DISPATCHERS",
    "GAME",
    "GHIDRA_DIR",
    "Ghidra",
    "Import",
    "JSON_OUT",
    "MARKDOWN_OUT",
    "NAMES_OUT",
    "POINTER_DATA",
    "SCHEMA_VERSION",
    "SLOT_SECTIONS",
    "UNREFERENCED",
    "analyse",
    "call_graph",
    "chartboost_entries",
    "check_stubs",
    "classify_functions",
    "counts",
    "framework_name",
    "ghidra_cross_check",
    "import_class",
    "import_rows",
    "import_table",
    "load_ghidra",
    "message_rows",
    "names_json",
    "resolve_sends",
    "run",
    "sides",
    "to_json",
    "to_markdown",
]

# -- command line -------------------------------------------------------------------------


def generate(repo: Path, slice_path: Path | None = None, ghidra_dir: Path | None = None) -> dict:
    """Write the three outputs below ``repo``; return the counts."""
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    gdir = ghidra_dir or repo / GHIDRA_DIR
    data = path.read_bytes()
    ghidra = load_ghidra(gdir)
    a = run(macho.MachO(data), ghidra)
    source = gc.source_info(path, data, repo)
    outputs = {
        JSON_OUT: gc.dump_json(to_json(a, source)),
        NAMES_OUT: gc.dump_json(names_json(a, ghidra)),
        MARKDOWN_OUT: to_markdown(a, source),
    }
    gc.write_outputs(repo, outputs)
    return counts(a)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.imports", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate", help=f"write {JSON_OUT}, {NAMES_OUT} and {MARKDOWN_OUT}")
    gen.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    gen.add_argument("--slice", type=Path, help="armv7 slice (default: from scripts/bootstrap)")
    gen.add_argument("--ghidra", type=Path, help=f"Ghidra exports (default: {GHIDRA_DIR})")
    args = parser.parse_args(argv)
    try:
        for key, value in generate(args.repo, args.slice, args.ghidra).items():
            print(f"  {key:<38} {value}")
    except (OSError, ValueError, macho.MachOError) as e:
        print(f"error: {e}", file=sys.stderr)
        if isinstance(e, FileNotFoundError) and "ghidra" in str(e):
            print("run tools/ghidra/import first", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
