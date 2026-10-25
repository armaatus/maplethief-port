"""C++ RTTI and vtables of a thin 32-bit Mach-O image (the armv7 slice).

The game's C++ engine was built with GCC's Itanium C++ ABI. Every polymorphic class has a
``type_info`` object and one or more vtables; this module finds them from the Mach-O alone
(no Ghidra) and turns them into plain data: classes with their bases, every vtable with its
slots, which class each virtual function belongs to, the destructors and the ``this``-adjusting
thunks.

**type_info objects.** libstdc++ is linked dynamically, so the vptr of every ``type_info`` is a
dyld bind (addend 8, the address point of the imported vtable) to one of the three
``__cxxabiv1`` classes in :data:`TYPEINFO_VTABLES`. Layouts (u32 fields)::

    __class_type_info      { vptr, __name }                               no bases
    __si_class_type_info   { vptr, __name, __base_type* }                 one public base at 0
    __vmi_class_type_info  { vptr, __name, __flags, __base_count,
                             __base_count x { __base_type*, __offset_flags } }

``__name`` points at the mangled type name without the ``_ZTS`` prefix (``6IState``,
``N2ns3FooE``). ``__offset_flags`` is ``offset << 8 | flags`` with ``__virtual_mask`` 1 and
``__public_mask`` 2.

**vtables.** ``{ offset_to_top, typeinfo*, slot 0, slot 1, ... }``; the address point (what an
object's vptr holds) is the first slot. A class with non-primary bases has one secondary
vtable per such base subobject, right after its primary one, with ``offset_to_top`` = minus
the subobject's offset. A vtable is found as a word equal to a ``type_info`` address that is
not part of a ``type_info`` object and follows a plausible ``offset_to_top`` (0 down to
-:data:`MAX_OFFSET_TO_TOP`). Slots run from the address point while a word is either a
``__cxa_pure_virtual`` bind (a pure virtual slot, 0 in the file) or a pointer into
``__TEXT,__text``; :attr:`VTable.end` says what stopped the walk.

**Ownership.** A virtual function belongs to the most-base class whose vtables hold it; it is
named after its slot in that class's primary vtable, so an override keeps its base's number
(``IStateAdaptor::vf4``, ``CStateGame::vf4``). A slot function of class ``C`` that looks up
``C.Method.N`` localization keys (:data:`METHOD_KEY`) names its slot ``Method`` in the class
that declared the slot and in every override. Slots with no such key can be named from the call
sites instead (:data:`KNOWN_SLOTS`, evidence in docs/re/program-flow.md); a key-derived name
always wins over a known one, and a disagreement is reported. Destructors take a pair of slots,
complete object (D1) then deleting (D0) as the Itanium ABI orders them. GCC 4.2 at -O0 emits
both from one body, with ``operator delete`` (``__ZdlPv``) behind a constant flag, so a slot
pair whose two functions both call ``operator delete`` is a destructor pair; derived classes
keep the pair at their primary base's position. A secondary-vtable function that is in no
primary vtable and makes exactly one call is a non-virtual thunk to the function it calls.

Command line::

    python3 -m mtport.rtti generate [--repo DIR] [--slice PATH]
        # writes work/re/vtables.json, work/re/names/rtti.json and docs/re/cpp-classes.md
    python3 -m mtport.rtti json SLICE      # the vtables.json document on stdout
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import macho
from mtport.armscan import pc_relative_values
from mtport.rtti_analysis import (
    KNOWN_SLOTS,
    MAX_BASES,
    MAX_OFFSET_TO_TOP,
    MAX_THUNK_SIZE,
    METHOD_KEY,
    OPERATOR_DELETE,
    PUBLIC_MASK,
    PURE_VIRTUAL,
    TYPEINFO_KIND_NAMES,
    TYPEINFO_VTABLES,
    VIRTUAL_MASK,
    Base,
    Function,
    Image,
    Rtti,
    RttiError,
    Slot,
    TypeInfo,
    VTable,
    analyze,
    call_targets,
    demangle_type,
    find_typeinfos,
    find_vtables,
    layout_problems,
    slot_exceptions,
)
from mtport.rtti_doc import (
    JSON_OUT,
    MARKDOWN_OUT,
    NAMES_OUT,
    NOTES,
    SCHEMA_VERSION,
    counts,
    names_json,
    to_json,
    to_markdown,
)

# mtport.rtti is the public face of the analysis (rtti_analysis) and its outputs (rtti_doc):
# it re-exports their names and keeps the command line.
__all__ = [
    "Base",
    "Function",
    "Image",
    "JSON_OUT",
    "KNOWN_SLOTS",
    "MARKDOWN_OUT",
    "MAX_BASES",
    "MAX_OFFSET_TO_TOP",
    "MAX_THUNK_SIZE",
    "METHOD_KEY",
    "NAMES_OUT",
    "NOTES",
    "OPERATOR_DELETE",
    "PUBLIC_MASK",
    "PURE_VIRTUAL",
    "Rtti",
    "RttiError",
    "SCHEMA_VERSION",
    "Slot",
    "TYPEINFO_KIND_NAMES",
    "TYPEINFO_VTABLES",
    "TypeInfo",
    "VIRTUAL_MASK",
    "VTable",
    "analyze",
    "call_targets",
    "counts",
    "demangle_type",
    "find_typeinfos",
    "find_vtables",
    "layout_problems",
    "names_json",
    "pc_relative_values",
    "slot_exceptions",
    "to_json",
    "to_markdown",
]

# -- command line ---------------------------------------------------------------------------


def generate(repo: Path, slice_path: Path | None = None) -> dict:
    """Write the three outputs below ``repo``; return the counts."""
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    data = path.read_bytes()
    r = analyze(macho.MachO(data))
    source = gc.source_info(path, data, repo)
    outputs = {
        JSON_OUT: gc.dump_json(to_json(r, source)),
        NAMES_OUT: gc.dump_json(names_json(r)),
        MARKDOWN_OUT: to_markdown(r, source, NOTES),
    }
    gc.write_outputs(repo, outputs)
    return counts(r)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.rtti", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate", help=f"write {JSON_OUT}, {NAMES_OUT} and {MARKDOWN_OUT}")
    gen.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    gen.add_argument("--slice", type=Path, help="armv7 slice (default: from scripts/bootstrap)")
    sub.add_parser("json", help="print the vtables.json document").add_argument("slice", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "generate":
            for key, value in generate(args.repo, args.slice).items():
                print(f"  {key:<26} {value}")
        else:
            data = args.slice.read_bytes()
            r = analyze(macho.MachO(data))
            sys.stdout.write(gc.dump_json(to_json(r, gc.source_info(args.slice, data))))
    except (OSError, macho.MachOError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
