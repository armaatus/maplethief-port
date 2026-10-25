"""The Objective-C shell (#22): what each of the 18 app classes uses and calls.

The mechanical half of ``docs/re/objc-shell.md``. For every app class (``objc.json``, #8) it
collects, from a Capstone sweep (:mod:`mtport.armscan`, receivers resolved as in
:mod:`mtport.imports`) and the Ghidra exports:

* **functions**: the class's IMPs plus its file-local helpers: the ``objc-file`` functions of
  ``libtags`` (#10) and the curated :data:`SHELL_NAMES` (inline ``CGRectMake``-style copies,
  block invoke/copy/dispose helpers, two C helpers);
* **framework classes**: classes loaded from ``__objc_classrefs`` and receivers of sends;
* **framework messages**: sends that reach an implementation outside the image (receiver
  class where it is known), the HLE surface of the class;
* **shell messages**: sends whose selector only app classes implement: the class's own methods
  and edges to other app classes;
* **engine calls** (ObjC → C++): direct calls from the class's functions to functions tagged
  ``engine``, with the constant arguments of the ``Clet_HandleEvent`` key events; and **engine
  sends** (C++ → ObjC): sends from ``engine`` functions that reach this class's methods;
* **selectors passed as arguments** (button and gesture wiring): a selector reference that a
  function loads but does not send, with the send it is passed to;
* **images**: strings the class's functions reference that name a root file of the app bundle
  (``work/ipa/Payload/MapleThief.app``), exactly or without the extension.

It also renders ``MainWindow.nib`` (:mod:`mtport.nib`). The judgement half (what each class is
for, the effort estimate) is hand-written in the same doc; this module only rewrites the blocks
between ``<!-- BEGIN generated:KEY -->`` and ``<!-- END generated:KEY -->``, and fails if the
doc lacks one.

A send's receiver is unknown at most sites (an ivar or an argument). Such a send counts as a
shell message when only app classes implement the selector and no send anywhere in the image
reaches a framework class with it; otherwise it is a framework message with receiver ``?``.

Command line::

    python3 -m mtport.objcshell names [--repo DIR]      # work/re/names/objcshell.json
    python3 -m mtport.objcshell generate [--repo DIR]   # work/re/objcshell.json + doc blocks
    python3 -m mtport.objcshell trace [--repo DIR] FUNCTION...   # one function's events

``names`` needs only the slice; ``generate`` and ``trace`` need the Ghidra export, the function
tags and the extracted bundle (``scripts/re-analyze``). ``trace`` prints a function's events in
site order (calls, sends with their arguments, ivar, class and string references) for a hex
address or every function whose name contains the text. It is a reading aid.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import libtags, macho, names, nib
from mtport.names import hexaddr
from mtport.objcshell_analysis import (
    ALLOC,
    APP,
    APP_OBJC,
    BRIDGES,
    BUNDLE,
    CHARTBOOST,
    CONTROL_EVENTS,
    ENGINE,
    FRAMEWORK,
    GAMECENTER_BLOCKS,
    GHIDRA_DIR,
    JSON_OUT,
    KEY_EVENT,
    LIBTAGS_STEP,
    MARKDOWN,
    NAMES_DIR,
    NAMES_OUT,
    NIB_NAME,
    SCHEMA_VERSION,
    SHELL_NAMES,
    SOURCE,
    TAGS_JSON,
    TOUCH_SELECTORS,
    UNKNOWN,
    Function,
    Program,
    Selectors,
    Shell,
    ShellClass,
    ShellError,
    analyse,
    analyse_class,
    app_classes,
    argument_selectors,
    argument_words,
    assign_functions,
    bridge_name,
    build_names,
    build_program,
    bundle_files,
    classify,
    curated_names,
    load_ghidra_functions,
    load_strings,
    names_json,
    resource_files,
    selector_info,
)
from mtport.objcshell_doc import (
    ADD_BUTTON,
    _keycode,
    blocks,
    class_block,
    images_block,
    nib_block,
    render,
    summary_block,
    to_json,
    trace,
)

# mtport.objcshell is the public face of the analysis (objcshell_analysis) and its outputs
# (objcshell_doc): it re-exports their names and keeps the driver and command line.
__all__ = [
    "ADD_BUTTON",
    "ALLOC",
    "APP",
    "APP_OBJC",
    "BRIDGES",
    "BUNDLE",
    "CHARTBOOST",
    "CONTROL_EVENTS",
    "ENGINE",
    "FRAMEWORK",
    "Function",
    "GAMECENTER_BLOCKS",
    "GHIDRA_DIR",
    "JSON_OUT",
    "KEY_EVENT",
    "LIBTAGS_STEP",
    "MARKDOWN",
    "NAMES_DIR",
    "NAMES_OUT",
    "NIB_NAME",
    "Program",
    "SCHEMA_VERSION",
    "SHELL_NAMES",
    "SOURCE",
    "Selectors",
    "Shell",
    "ShellClass",
    "ShellError",
    "TAGS_JSON",
    "TOUCH_SELECTORS",
    "UNKNOWN",
    "_keycode",
    "analyse",
    "analyse_class",
    "app_classes",
    "argument_selectors",
    "argument_words",
    "assign_functions",
    "blocks",
    "bridge_name",
    "build_names",
    "build_program",
    "bundle_files",
    "class_block",
    "classify",
    "curated_names",
    "images_block",
    "load_ghidra_functions",
    "load_strings",
    "names_json",
    "nib_block",
    "render",
    "resource_files",
    "selector_info",
    "summary_block",
    "to_json",
    "trace",
]

# -- driver -------------------------------------------------------------------------------


def load_program(repo: Path) -> Program:
    from mtport import bootstrap

    slice_path = repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    ghidra = repo / GHIDRA_DIR
    for path in (slice_path, ghidra / "functions.json", repo / TAGS_JSON, repo / BUNDLE):
        if not path.exists():
            raise ShellError(f"{path} is missing (run scripts/bootstrap and scripts/re-analyze)")
    return build_program(
        macho.load(slice_path),
        load_ghidra_functions(ghidra / "functions.json"),
        gc.load_json(repo / TAGS_JSON, libtags.SCHEMA_VERSION, producer=LIBTAGS_STEP),
        load_strings(ghidra / "strings.json"),
        bundle_files(repo / BUNDLE),
    )


def build(repo: Path) -> tuple[Program, Shell, dict, str]:
    """Everything ``generate`` writes: the program, the analysis, the JSON and the doc."""
    from mtport import bootstrap

    p = load_program(repo)
    s = analyse(p)
    n = nib.load(repo / BUNDLE / NIB_NAME)
    slice_path = repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    source = gc.source_info(slice_path, slice_path.read_bytes(), repo)
    doc = render((repo / MARKDOWN).read_text(encoding="utf-8"), blocks(p, s, n))
    return p, s, to_json(p, s, source), doc


def generate(repo: Path) -> Shell:
    _, s, doc_json, doc = build(repo)
    gc.write_text(repo / JSON_OUT, gc.dump_json(doc_json))
    gc.write_text(repo / MARKDOWN, doc)
    print(f"wrote {JSON_OUT} and the generated blocks of {MARKDOWN}")
    return s


def write_names(repo: Path) -> list[dict]:
    from mtport import bootstrap

    image = macho.load(repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH))
    entries = build_names(image, names.other_names(repo / NAMES_DIR, Path(NAMES_OUT).name))
    gc.write_text(repo / NAMES_OUT, names_json(entries))
    print(f"wrote {NAMES_OUT} ({len(entries)} names)")
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.objcshell", description=__doc__.splitlines()[0]
    )
    parser.add_argument("command", choices=("generate", "names", "trace"))
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("functions", nargs="*", help="trace: hex addresses or name substrings")
    args = parser.parse_args(argv)
    try:
        if args.command == "names":
            write_names(args.repo)
        elif args.command == "generate":
            generate(args.repo)
        else:
            p = load_program(args.repo)
            for text in args.functions:
                if re.fullmatch(r"0x[0-9A-Fa-f]+", text):
                    found = [p.index.of(names.parse_hex(text))]
                else:
                    found = [f.pointer for f in p.functions.values() if text in f.name]
                for ptr in found:
                    if ptr is not None:
                        print(f"{hexaddr(ptr)} {p.name(ptr)}")
                        print("\n".join(trace(p, ptr)))
    except (OSError, ShellError, macho.MachOError, names.NamesError, nib.FormatError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
