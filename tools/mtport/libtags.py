"""Tag every ``LC_FUNCTION_STARTS`` function of the armv7 slice as game or library code (#10).

Each of the 2,655 functions gets exactly one tag:

* ``app-objc``: the app's Objective-C shell, i.e. the IMPs of the 18 app classes, ``main``,
  and code linked inside an app class's ``.m`` file.
* ``engine``: the game's C/C++ code, the part that gets recompiled.
* ``lib:<name>``: statically linked third-party code (``lib:chartboost``, ``lib:crt1``, ...).
* ``unknown``: no rule fired.

Tags come from an ordered list of rules (:func:`default_rules`, documented in
:data:`RULE_DOCS`); the first rule that claims a function wins, and every assignment records
the rule and a reason that names addresses. Strongest first:

1. ``objc-imp``: ObjC metadata (``work/re/objc.json``, #8). App class IMPs -> ``app-objc``;
   Chartboost class IMPs (:func:`mtport.objc.is_chartboost`) -> ``lib:chartboost``; category
   IMPs by :data:`CATEGORY_LIBRARIES` or, failing that, by where they are linked.
2. ``entry-point``: the ``LC_UNIXTHREAD`` entry is crt1's ``start`` -> ``lib:crt1``. The
   function it calls that calls ``UIApplicationMain`` is ``main`` -> ``app-objc``.
3. ``export``: the export trie is not stripped; exported C functions with a known library
   prefix (:data:`EXPORT_LIBRARIES`) get that library.
4. ``signature``: library markers (:data:`SIGNATURES`: byte strings, table words, 32-bit
   constants built with ``movw``/``movt``) found in a function or in data a function
   references. A *strong* marker tags its function; a *weak* one is only reported.
5. ``vtable``: functions in the vtables of C++ classes with RTTI names. ``std::``/
   ``__gnu_cxx::`` classes -> ``lib:stl``, every other class -> ``engine``. The vtables come
   from :func:`rtti_vtables` (#9's :mod:`mtport.rtti`); any provider with the same result
   type can be passed instead (``default_rules(engine_evidence=...)``).
6. ``static-init``: ``__mod_init_func`` entries (C++ static constructors) -> ``engine``.
7. ``objc-file``: a function referenced only from one ObjC class's code and linked right
   before, among or right after that class's IMPs is part of its ``.m`` file (blocks and
   their copy/dispose helpers, static helpers, ``CGRectMake``-style inlines emitted at -O0)
   -> the class's tag.
8. ``closure``: a function all of whose referrers carry the same ``lib:*`` tag joins that
   library. Referrers are every function that calls, jumps to or takes the address of it,
   including through a data object (below), so it stops at shared functions.
9. ``reach``: functions reachable from ``app-objc`` and ``engine`` functions over the same
   reference graph, without passing through library code -> ``engine``.
10. ``layout``: object files are linked contiguously, so an untagged function whose nearest
    tagged neighbours on both sides share a tag gets that tag. Weak evidence.
11. ``unknown``: everything left.

Reference graph: an edge F -> G for every ``xrefs.json`` reference from inside F (or from a
literal pool between F and the next start) to G's entry. Data references are Ghidra's plus
every aligned word outside code and ObjC metadata that equals a function pointer. A
reference from a data address P to G becomes edges F -> G for every function F that
references the *data object* containing P: the nearest address at or below P, in the same
section and at most :data:`DATA_OBJECT_MAX` bytes away, that code references. This catches
block descriptors and literals, function-pointer tables and vtables.

Inputs: the slice (``work/slices/MapleThief.armv7``), ``work/re/objc.json`` (#8) and the
Ghidra exports ``work/ghidra/{functions,xrefs}.json`` (#7). Outputs:
``work/re/function-tags.json`` (git-ignored) and the generated ``docs/re/libraries.md``.

Command line::

    python3 -m mtport.libtags generate      # write both outputs (checks exact coverage)
    python3 -m mtport.libtags check [FILE]  # coverage of FILE against the slice's starts
    python3 -m mtport.libtags show ADDR...  # tag, rule and reason of functions

Python API for other analyses (#11, #12)::

    from mtport import libtags
    tags = libtags.load()                   # work/re/function-tags.json
    tags.tag_of(0x2A3C1)                    # "engine"; Thumb bit optional, str or int
    tags.functions_with("lib:chartboost")   # sorted function pointers
    tags.is_library(0xC2F25)                # True for any lib:* tag
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from mtport.libtags_doc import (
    FINDINGS,
    TAG_DOCS,
    FunctionTags,
    _sort_tags,
    ghidra_plan,
    is_scheme_tag,
    load,
    readback_json,
    readback_problems,
    tag_comments,
    to_json,
    to_markdown,
)
from mtport.libtags_program import (
    APP_OBJC,
    CATEGORY_LIBRARIES,
    CHARTBOOST,
    CRT1,
    DATA_OBJECT_MAX,
    ENGINE,
    FUNCTIONS_JSON,
    GENERATE_STEP,
    JSON_OUT,
    LIB_PREFIX,
    MARKDOWN_OUT,
    OBJC_JSON,
    OBJC_STEP,
    READBACK_JSON,
    READBACK_SCHEMA,
    RULE_DOCS,
    SCHEMA_VERSION,
    STL,
    UNKNOWN,
    XREFS_JSON,
    Function,
    Program,
    VTable,
    build_program,
    rtti_vtables,
)
from mtport.libtags_rules import (
    CRT1_EXPORTS,
    EXPORT_LIBRARIES,
    SIGNATURES,
    STL_IMPORT_MARKERS,
    Assignment,
    CoverageError,
    EngineEvidence,
    Signature,
    Tagger,
    _const32_hits,
    check_coverage,
    check_doc,
    default_rules,
    make_vtable_rule,
    rule_closure,
    rule_entry,
    rule_exports,
    rule_layout,
    rule_objc,
    rule_objc_file,
    rule_reach,
    rule_signatures,
    rule_static_init,
    rule_stl_imports,
    rule_unknown,
    tag_program,
)

from . import ghidra_common as gc
from . import macho, objc

# mtport.libtags is the public face of its inputs (libtags_program), rules (libtags_rules) and
# outputs (libtags_doc): it re-exports their names and keeps the command line.
__all__ = [
    "APP_OBJC",
    "Assignment",
    "CATEGORY_LIBRARIES",
    "CHARTBOOST",
    "CRT1",
    "CRT1_EXPORTS",
    "CoverageError",
    "DATA_OBJECT_MAX",
    "ENGINE",
    "EXPORT_LIBRARIES",
    "EngineEvidence",
    "FINDINGS",
    "FUNCTIONS_JSON",
    "Function",
    "FunctionTags",
    "GENERATE_STEP",
    "JSON_OUT",
    "LIB_PREFIX",
    "MARKDOWN_OUT",
    "OBJC_JSON",
    "OBJC_STEP",
    "Program",
    "READBACK_JSON",
    "READBACK_SCHEMA",
    "RULE_DOCS",
    "SCHEMA_VERSION",
    "SIGNATURES",
    "STL",
    "STL_IMPORT_MARKERS",
    "Signature",
    "TAG_DOCS",
    "Tagger",
    "UNKNOWN",
    "VTable",
    "XREFS_JSON",
    "_const32_hits",
    "build_program",
    "check_coverage",
    "check_doc",
    "default_rules",
    "ghidra_plan",
    "is_scheme_tag",
    "load",
    "make_vtable_rule",
    "readback_json",
    "readback_problems",
    "rtti_vtables",
    "rule_closure",
    "rule_entry",
    "rule_exports",
    "rule_layout",
    "rule_objc",
    "rule_objc_file",
    "rule_reach",
    "rule_signatures",
    "rule_static_init",
    "rule_stl_imports",
    "rule_unknown",
    "tag_comments",
    "tag_program",
    "to_json",
    "to_markdown",
]

# -- command line --------------------------------------------------------------------------


def generate(repo: Path, slice_path: Path | None = None) -> dict:
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    data = path.read_bytes()
    image = macho.MachO(data)
    prog = build_program(
        image,
        gc.load_export(repo / FUNCTIONS_JSON),
        gc.load_export(repo / XREFS_JSON),
        gc.load_json(
            repo / OBJC_JSON, objc.SCHEMA_VERSION, key="schema_version", producer=OBJC_STEP
        ),
    )
    t = tag_program(prog)
    source = {"arch": "armv7", "sha256": hashlib.sha256(data).hexdigest()}
    text = to_json(t, source)
    doc = json.loads(text)
    check_doc(doc, image)
    gc.write_text(repo / JSON_OUT, text)
    print(f"wrote {JSON_OUT}")
    gc.write_text(repo / MARKDOWN_OUT, to_markdown(doc))
    print(f"wrote {MARKDOWN_OUT}")
    return doc["counts"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.libtags", description=__doc__.splitlines()[0]
    )
    repo_default = Path(__file__).resolve().parents[2]
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate", help=f"write {JSON_OUT} and {MARKDOWN_OUT}")
    gen.add_argument("--repo", type=Path, default=repo_default)
    gen.add_argument("--slice", type=Path, help="armv7 slice (default: from scripts/bootstrap)")
    chk = sub.add_parser("check", help="check that FILE tags exactly the LC_FUNCTION_STARTS")
    chk.add_argument("file", type=Path, nargs="?", default=repo_default / JSON_OUT)
    chk.add_argument("--slice", type=Path)
    show = sub.add_parser("show", help="print tag, rule and reason for addresses")
    show.add_argument("address", nargs="+")
    show.add_argument("--file", type=Path, default=repo_default / JSON_OUT)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "generate":
            for tag, c in generate(args.repo, args.slice).items():
                print(f"  {tag:<28} {c['functions']:>5} functions {c['bytes']:>8} bytes")
        elif args.cmd == "check":
            from mtport import bootstrap

            sl = args.slice or repo_default / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
            counts = check_doc(gc.load_json(args.file, producer=GENERATE_STEP), macho.load(sl))
            print(f"OK: {sum(counts.values())} functions, exactly the LC_FUNCTION_STARTS")
            for tag in _sort_tags(counts):
                print(f"  {tag:<28} {counts[tag]:>5}")
        else:
            tags = load(args.file)
            for a in args.address:
                r = tags.record(a)
                print(
                    f"{a}: " + (f"{r['tag']} [{r['rule']}] {r['reason']}" if r else "no function")
                )
    except (OSError, macho.MachOError, CoverageError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
