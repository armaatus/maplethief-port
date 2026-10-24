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
from collections import Counter, defaultdict
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from mtport import armscan, macho, objc
from mtport import ghidra_common as gc
from mtport.names import hexaddr, parse_address

SCHEMA_VERSION = 1
JSON_OUT = "work/re/imports.json"
NAMES_OUT = "work/re/names/imports.json"
MARKDOWN_OUT = "docs/re/imports.md"
GHIDRA_DIR = "work/ghidra"

DISPATCHERS = (
    "_objc_msgSend",
    "_objc_msgSend_stret",
    "_objc_msgSend_fpret",
    "_objc_msgSendSuper2",
    "_objc_msgSendSuper2_stret",
)
# Sections whose bound words are pointer slots: code that loads one uses the import itself.
SLOT_SECTIONS = frozenset({"__nl_symbol_ptr", "__lazy_symbol", "__objc_classrefs"})
# In __data a bound word is a slot only when code loads it (e.g. a malloc/free table);
# taking its address builds an object (a vtable).

GAME, CHARTBOOST, BOTH, UNREFERENCED = "game", "chartboost", "both", "unreferenced"
CLASSES = (GAME, BOTH, CHARTBOOST, UNREFERENCED)


# -- 1. import table ----------------------------------------------------------------------


@dataclass
class Import:
    name: str
    library: str
    weak: bool
    stubs: list[int] = field(default_factory=list)
    lazy: list[int] = field(default_factory=list)
    non_lazy: list[int] = field(default_factory=list)
    binds: list[tuple[int, str]] = field(default_factory=list)  # (address, section), not stubs

    @property
    def framework(self) -> str:
        return framework_name(self.library)

    @property
    def kind(self) -> str:
        if self.name.startswith(objc.METACLASS_SYMBOL):
            return "metaclass"
        if self.name.startswith(objc.CLASS_SYMBOL):
            return "class"
        # "address": no stub; only its address is bound (a data symbol, or a function used
        # through a pointer such as __cxa_pure_virtual in vtables).
        return "function" if self.stubs else "address"


def framework_name(install_name: str) -> str:
    """``/System/Library/Frameworks/UIKit.framework/UIKit`` → ``UIKit``,
    ``/usr/lib/libSystem.B.dylib`` → ``libSystem``."""
    base = install_name.rsplit("/", 1)[-1]
    if base.startswith("lib") and ".dylib" in base:
        return base.split(".", 1)[0]
    return base


def import_table(image: macho.MachO) -> list[Import]:
    """Every undefined symbol in ``nlist`` order, with its stubs, pointers and binds."""
    imports: dict[str, Import] = {}
    for sym in image.undefined_symbols:
        weak_lib = False
        if 1 <= sym.library_ordinal <= len(image.dylibs):
            weak_lib = image.dylibs[sym.library_ordinal - 1][1]
        weak = weak_lib or bool(sym.desc & 0x40)  # N_WEAK_REF
        imports[sym.name] = Import(sym.name, image.library_name(sym.library_ordinal), weak)
    for slot in image.indirect_slots:
        imp = imports.get(slot.symbol) if slot.symbol else None
        if imp is None:
            continue
        {"stub": imp.stubs, "lazy": imp.lazy, "non_lazy": imp.non_lazy}[slot.kind].append(
            slot.address
        )
    pointer_slots = {s.address for s in image.indirect_slots if s.kind != "stub"}
    seen = set()
    for b in image.binds:
        imp = imports.get(b.symbol)
        if imp is None or b.address in pointer_slots or (b.address, b.symbol) in seen:
            continue
        seen.add((b.address, b.symbol))
        section = image.section_at(b.address)
        imp.binds.append((b.address, section.sectname if section else "?"))
    for imp in imports.values():
        imp.binds.sort()
    return list(imports.values())


def check_stubs(image: macho.MachO) -> list[str]:
    """Problems with the stub → lazy pointer → symbol mapping (empty when consistent)."""
    problems = []
    slots = {s.address: s for s in image.indirect_slots}
    lazy_binds = {b.address: b.symbol for b in image.binds if b.kind in ("lazy", "weak")}
    for s in image.indirect_slots:
        if s.kind == "stub":
            word = image.u32(s.address)
            if word & 0xFFFFF000 != 0xE59FF000:  # ldr pc, [pc, #imm12]
                problems.append(f"stub {hexaddr(s.address)}: not ldr pc, [pc, #n]")
                continue
            target = slots.get(s.address + 8 + (word & 0xFFF))
            if target is None or target.kind != "lazy" or target.symbol != s.symbol:
                problems.append(f"stub {hexaddr(s.address)} ({s.symbol}): bad lazy pointer")
        elif s.kind == "lazy" and lazy_binds.get(s.address) != s.symbol:
            problems.append(f"lazy pointer {hexaddr(s.address)} ({s.symbol}): bind disagrees")
        elif s.kind == "non_lazy" and s.symbol and image.bound_symbol(s.address) != s.symbol:
            problems.append(f"non-lazy pointer {hexaddr(s.address)} ({s.symbol}): bind disagrees")
    return problems


# -- 2. Ghidra inputs ---------------------------------------------------------------------


@dataclass
class Ghidra:
    names: dict[int, str]  # function pointer -> name
    name_sources: dict[int, str]
    xrefs: list[dict]


def load_ghidra(directory: Path) -> Ghidra:
    """The parts of the Ghidra export in ``directory`` (``work/ghidra``) this module reads."""
    functions = gc.load_export(directory / "functions.json")
    xrefs = gc.load_export(directory / "xrefs.json")
    names, sources = {}, {}
    for f in functions["functions"]:
        ptr = parse_address(f["address"])
        names[ptr] = f["name"]
        sources[ptr] = f["name_source"]
    return Ghidra(names, sources, xrefs["xrefs"])


# Data that holds function pointers for the code that references it: block descriptors
# (copy/dispose helpers), global block literals (invoke), vtables.
POINTER_DATA = (("__DATA", "__const"), ("__DATA", "__data"), ("__TEXT", "__const"))


def call_graph(
    xrefs: Iterable[dict],
    starts: Iterable[int],
    image: macho.MachO,
    data_refs: Iterable[tuple[int, int]] = (),
) -> dict[int, set[int]]:
    """``caller -> callees`` over the LC_FUNCTION_STARTS functions in ``__text``.

    Any reference from inside one function to another function's entry is an edge: a call,
    a tail jump, or an address taken (a callback, a block's invoke function). A function
    pointer stored in static data (:data:`POINTER_DATA`) is an edge from every function that
    references the object holding it: the nearest code-referenced address at most
    :data:`mtport.armscan.DATA_OBJECT_MAX` bytes before the pointer, in the same section
    (:func:`mtport.armscan.containing_object`). ObjC method lists are
    not followed: the runtime calls those IMPs. ``data_refs`` adds ``(function, data
    address)`` references that Ghidra's xrefs miss (from :mod:`mtport.armscan`).
    """
    text = image.section("__TEXT", "__text")
    sections = [s for s in (image.section(*k) for k in POINTER_DATA) if s is not None]
    index = armscan.FunctionIndex(starts)
    entries = {s & ~1: s for s in index.starts}
    graph: dict[int, set[int]] = {s: set() for s in index.starts}
    data_users: dict[int, set[int]] = defaultdict(set)  # data address -> code referrers
    data_ptrs: list[tuple[int, int]] = []  # (data address, function)
    for x in xrefs:
        to = x["to"]
        if to.startswith("EXTERNAL"):
            continue
        site, target = parse_address(x["from"]), parse_address(to)
        callee = entries.get(target & ~1)
        if text.contains(site):
            caller = index.of(site)
            if callee is not None and caller is not None and caller != callee:
                graph[caller].add(callee)
            elif callee is None and caller is not None:
                if any(s.contains(target) for s in sections):
                    data_users[target].add(caller)
        elif callee is not None and any(s.contains(site) for s in sections):
            data_ptrs.append((site, callee))
    pointers = set(index.starts)
    for sect in sections:  # pointers Ghidra did not mark as such
        if sect.size < 4 or (sect.flags & 0xFF) == 1:  # S_ZEROFILL
            continue
        raw = image.read(sect.addr, sect.size & ~3)
        for off in range(0, len(raw), 4):
            value = int.from_bytes(raw[off : off + 4], "little")
            if value in pointers:
                data_ptrs.append((sect.addr + off, value))
    for caller, target in data_refs:
        if entries.get(target & ~1) is None and any(s.contains(target) for s in sections):
            data_users[target].add(caller)
    referenced = sorted(data_users)
    for site, callee in data_ptrs:
        start = armscan.containing_object(referenced, site, image, armscan.DATA_OBJECT_MAX)
        if start is None:
            continue
        for caller in data_users[start]:
            if caller != callee:
                graph[caller].add(callee)
    return graph


# -- 4. reachability ----------------------------------------------------------------------


def classify_functions(
    functions: Iterable[int],
    callees: dict[int, set[int]],
    chartboost: Iterable[int],
    dead: Iterable[int] = (),
) -> dict[int, str]:
    """First-pass library tagging (replaceable by #10): ``function -> class``.

    ``chartboost`` are the known Chartboost entry points and ``dead`` entry points nothing can
    reach (category methods whose selector is never sent). The game's roots are every other
    function that no function in the image references: ObjC IMPs, C++ virtuals, callbacks,
    ``start``. A function reachable from Chartboost entries only is ``chartboost``, from game
    roots only ``game``, from both ``both``, from dead entries only ``unreferenced``. One that
    is reachable from no root at all (a cycle with no way in) counts as ``game``.
    """
    functions = list(functions)
    known = set(functions)
    cb = set(chartboost) & known
    dead = (set(dead) & known) - cb
    called = {g for f in functions for g in callees.get(f, ())}
    game_roots = [f for f in functions if f not in called and f not in cb and f not in dead]

    def closure(roots: Iterable[int]) -> set[int]:
        seen = set(roots)
        stack = list(seen)
        while stack:
            for g in callees.get(stack.pop(), ()):
                if g not in seen:
                    seen.add(g)
                    stack.append(g)
        return seen

    from_game, from_cb, from_dead = closure(game_roots), closure(cb), closure(dead)
    out = {}
    for f in functions:
        if f in from_cb and f in from_game:
            out[f] = BOTH
        elif f in from_cb:
            out[f] = CHARTBOOST
        elif f in from_dead and f not in from_game:
            out[f] = UNREFERENCED
        else:
            out[f] = GAME
    return out


def chartboost_entries(
    image: objc.ObjCImage,
    selector_users: dict[str, set[int]],
    functions: Iterable[int],
    callees: dict[int, set[int]],
) -> tuple[set[int], dict[int, str], dict[str, str]]:
    """Chartboost entry points, the resulting function classes and each category's class.

    Seeds: the IMPs of Chartboost classes. The framework never calls a category method on a
    framework class, so its callers are the functions that send its selector. Until nothing
    changes: a category method whose selector no live function sends is dead, and one whose
    live senders are all Chartboost's is a Chartboost entry.
    """
    seeds = {
        m.imp
        for k in image.classes
        if k.group == "chartboost"
        for m in k.instance_methods + k.class_methods
        if m.imp
    }
    methods = [
        m for cat in image.categories for m in cat.instance_methods + cat.class_methods if m.imp
    ]
    functions = list(functions)
    dead: set[int] = set()
    while True:
        classes = classify_functions(functions, callees, seeds, dead)
        new_seeds, new_dead = set(seeds), set()
        for m in methods:
            users = selector_users.get(m.selector, set()) - {m.imp}
            live = {f for f in users if classes.get(f) != UNREFERENCED}
            if not live:
                new_dead.add(m.imp)
            elif all(classes.get(f) == CHARTBOOST for f in live):
                new_seeds.add(m.imp)
        if new_seeds == seeds and new_dead == dead:
            break
        seeds, dead = new_seeds, new_dead
    categories = {}
    for cat in image.categories:
        tags = {classes.get(m.imp, GAME) for m in cat.instance_methods + cat.class_methods}
        tags.discard(UNREFERENCED)
        g = bool(tags & {GAME, BOTH})
        c = bool(tags & {CHARTBOOST, BOTH})
        categories[f"{cat.class_name}({cat.name})"] = import_class(g, c)
    return seeds, classes, categories


def import_class(game: bool, chartboost: bool) -> str:
    if game and chartboost:
        return BOTH
    if game:
        return GAME
    if chartboost:
        return CHARTBOOST
    return UNREFERENCED


def sides(tag: str) -> tuple[bool, bool]:
    """``(used by the game, used by Chartboost)`` for a function or owner class."""
    return tag in (GAME, BOTH), tag in (CHARTBOOST, BOTH)


# -- 3 + 5. uses and classes --------------------------------------------------------------


@dataclass
class Analysis:
    """Everything :func:`to_json` and :func:`to_markdown` render."""

    imports: list[Import]
    stub_problems: list[str]
    function_classes: dict[int, str]
    chartboost_seeds: set[int]
    category_classes: dict[str, str]
    names: dict[int, str]
    sites: dict[str, list[tuple[int, int, str]]]  # import -> (site, function, kind)
    owners: dict[str, list[dict]]  # import -> data uses
    classes: dict[str, str]  # import -> game/both/chartboost/unreferenced
    ghidra_check: dict[str, dict]
    sends: list[dict]
    scan_stats: dict


def _function_name(names: dict[int, str], ptr: int) -> str:
    return names.get(ptr) or f"FUN_{ptr & ~1:08x}"


def analyse(
    image: macho.MachO, objc_image: objc.ObjCImage, scan: armscan.Scan, ghidra: Ghidra
) -> Analysis:
    table = import_table(image)
    text = image.section("__TEXT", "__text")
    starts = sorted(s for s in image.function_starts if text.contains(s & ~1))

    stub_of = {a: i.name for i in table for a in i.stubs}
    pointer_of = {a: i.name for i in table for a in i.lazy + i.non_lazy}
    bind_of = {a: (i.name, sect) for i in table for a, sect in i.binds}
    loaded = {r.target for r in scan.refs if r.kind == "load"}

    def is_slot(addr: int) -> bool:
        if addr in pointer_of:
            return True
        name_sect = bind_of.get(addr)
        if name_sect is None:
            return False
        return name_sect[1] in SLOT_SECTIONS or (name_sect[1] == "__data" and addr in loaded)

    sites: dict[str, list[tuple[int, int, str]]] = defaultdict(list)
    object_users: dict[int, set[int]] = defaultdict(set)
    for r in scan.refs:
        if r.kind in ("call", "tail"):
            if r.target in stub_of:
                sites[stub_of[r.target]].append((r.site, r.function, r.kind))
        elif is_slot(r.target):
            name = pointer_of.get(r.target) or bind_of[r.target][0]
            sites[name].append((r.site, r.function, r.kind))
        elif r.target in bind_of:
            object_users[r.target].add(r.function)

    # Function classes: Chartboost entries + reachability over Ghidra's call graph.
    graph = call_graph(
        ghidra.xrefs, starts, image, ((r.function, r.target) for r in scan.refs if r.kind != "call")
    )
    selrefs = dict(objc_image.selrefs)
    selector_users: dict[str, set[int]] = defaultdict(set)
    for r in scan.refs:
        if r.target in selrefs and r.kind in ("load", "address"):
            selector_users[selrefs[r.target]].add(r.function)
    seeds, fclass, catclass = chartboost_entries(objc_image, selector_users, starts, graph)

    # Data uses: who owns the structure a bind sits in.
    struct_owner: list[tuple[int, int, str, str]] = []  # (lo, hi, description, class)
    for k in objc_image.classes:
        tag = CHARTBOOST if k.group == "chartboost" else GAME
        struct_owner.append((k.address, k.address + 20, f"class {k.name}", tag))
        struct_owner.append((k.metaclass, k.metaclass + 20, f"metaclass {k.name}", tag))
    for cat in objc_image.categories:
        label = f"{cat.class_name}({cat.name})"
        struct_owner.append((cat.address, cat.address + 24, f"category {label}", catclass[label]))

    owners: dict[str, list[dict]] = defaultdict(list)
    for imp in table:
        for addr, sect in imp.binds:
            if is_slot(addr):
                continue
            entry = {"address": hexaddr(addr), "section": sect}
            found = next((o for o in struct_owner if o[0] <= addr < o[1]), None)
            if found is not None:
                entry.update(owner=found[2], **{"class": found[3]})
            elif addr in object_users:
                users = sorted(object_users[addr])
                g = any(sides(fclass.get(f, GAME))[0] for f in users)
                c = any(sides(fclass.get(f, GAME))[1] for f in users)
                what = {"__cfstring": "CFString", "__const": "object"}.get(sect, "object")
                entry.update(
                    owner=f"{what} at {hexaddr(addr)}, used by {len(users)} function(s)",
                    functions=[hexaddr(f) for f in users],
                    **{"class": import_class(g, c)},
                )
            elif sect == "__cfstring":
                entry.update(owner="CFString no code references", **{"class": UNREFERENCED})
            else:
                entry.update(owner=f"static data in {sect} (C++ vtable / RTTI)", **{"class": GAME})
            owners[imp.name].append(entry)

    classes: dict[str, str] = {}
    for imp in table:
        g = c = False
        for _, func, _ in sites.get(imp.name, ()):
            sg, sc = sides(fclass.get(func, GAME))
            g, c = g or sg, c or sc
        for entry in owners.get(imp.name, ()):
            sg, sc = sides(entry["class"])
            g, c = g or sg, c or sc
        classes[imp.name] = import_class(g, c)

    check = ghidra_cross_check(table, sites, ghidra, text, is_slot)
    sends = resolve_sends(image, objc_image, scan, stub_of, fclass)
    stats = {
        "functions": scan.functions,
        "instructions": scan.instructions,
        "skipped_bytes": scan.skipped_bytes,
        "refs": len(scan.refs),
        "sends": len(scan.sends),
    }
    for imp in table:
        sites[imp.name].sort()
    return Analysis(
        table,
        check_stubs(image),
        fclass,
        seeds,
        catclass,
        ghidra.names,
        dict(sites),
        dict(owners),
        classes,
        check,
        sends,
        stats,
    )


def ghidra_cross_check(table, sites, ghidra: Ghidra, text, is_slot) -> dict[str, dict]:
    """Per import: Capstone's call + load sites against Ghidra's.

    Ghidra side: ``*_CALL`` / ``*_JUMP`` references from ``__text`` to the import's stub, and
    ``READ`` references to one of its pointer slots. Ghidra's ``COMPUTED_CALL`` to a stub
    through a loaded data pointer is the same access as the ``READ`` of that pointer, and is
    left out.
    """
    stub_of = {a: i.name for i in table for a in i.stubs}
    slot_of = {}
    for i in table:
        for a in i.lazy + i.non_lazy + [a for a, _ in i.binds]:
            if is_slot(a):
                slot_of[a] = i.name
    theirs: dict[str, set[int]] = defaultdict(set)
    for x in ghidra.xrefs:
        if x["to"].startswith("EXTERNAL"):
            continue
        site, to, kind = parse_address(x["from"]), parse_address(x["to"]), x["type"]
        if not text.contains(site):
            continue
        if to in stub_of and kind != "COMPUTED_CALL" and kind.endswith(("CALL", "JUMP")):
            theirs[stub_of[to]].add(site)
        elif to in slot_of and kind == "READ":
            theirs[slot_of[to]].add(site)
    out = {}
    for imp in table:
        ours = {s for s, _, k in sites.get(imp.name, ()) if k in ("call", "tail", "load")}
        addr_only = {s for s, _, k in sites.get(imp.name, ()) if k == "address"}
        gh = theirs.get(imp.name, set())
        out[imp.name] = {
            "capstone": len(ours),
            "ghidra": len(gh),
            "only_capstone": sorted(ours - gh),
            "only_ghidra": sorted(gh - ours),
            "capstone_address_only": sorted(addr_only),
        }
    return out


# -- 6. ObjC sends ------------------------------------------------------------------------


_ALLOC = ("alloc", "allocWithZone:", "new")
_SELF = ("retain", "autorelease", "self")


def resolve_sends(
    image, objc_image: objc.ObjCImage, scan: armscan.Scan, stub_of, fclass
) -> list[dict]:
    """Each send with its selector and, where determinable, the class that implements it."""
    selrefs = dict(objc_image.selrefs)
    classrefs = {slot: ref for slot, ref in objc_image.classrefs if ref is not None}
    superrefs = {slot: ref for slot, ref in objc_image.superrefs if ref is not None}
    own = {k.name: k for k in objc_image.classes}
    cat_methods: dict[str, set[tuple[str, bool]]] = defaultdict(set)
    for cat in objc_image.categories:
        for m in cat.instance_methods:
            cat_methods[cat.class_name].add((m.selector, False))
        for m in cat.class_methods:
            cat_methods[cat.class_name].add((m.selector, True))
    implemented = {
        m.selector for k in objc_image.classes for m in k.instance_methods + k.class_methods
    } | {s for v in cat_methods.values() for s, _ in v}

    def implementer(cls: str, meta: bool, selector: str) -> tuple[str, bool]:
        """Walk an image class's superclass chain: ``(class that implements, in image)``."""
        name = cls
        while name in own:
            k = own[name]
            methods = k.class_methods if meta else k.instance_methods
            if (
                any(m.selector == selector for m in methods)
                or (selector, meta) in cat_methods[name]
            ):
                return name, True
            if k.superclass is None:
                return name, True
            name = k.superclass.name
        if (selector, meta) in cat_methods[name]:
            return f"{name}(category)", True
        return name, False

    # Superref loads per function, in site order (for objc_msgSendSuper2).
    super_loads: dict[int, list[tuple[int, objc.ClassRef]]] = defaultdict(list)
    for r in scan.refs:
        if r.target in superrefs and r.kind in ("load", "address"):
            super_loads[r.function].append((r.site, superrefs[r.target]))

    results: dict[int, tuple[str | None, tuple[str, bool] | None]] = {}
    out = []
    for s in scan.sends:
        stub = stub_of.get(s.stub, "?")
        stret = stub.endswith("_stret")
        recv_v, sel_v = (s.regs[1], s.regs[2]) if stret else (s.regs[0], s.regs[1])
        selector = selrefs.get(sel_v[1]) if sel_v and sel_v[0] == "m" else None
        receiver: tuple[str, bool] | None = None  # (class name, class method?)
        how = None
        if "Super2" in stub:
            loads = [ref for site, ref in super_loads.get(s.function, ()) if site < s.site]
            if loads and loads[-1].name in own and own[loads[-1].name].superclass:
                receiver = (own[loads[-1].name].superclass.name, loads[-1].meta)
                how = "super"
        elif recv_v and recv_v[0] == "m" and recv_v[1] in classrefs:
            ref = classrefs[recv_v[1]]
            receiver, how = (ref.name, not ref.meta), "classref"
        elif recv_v and recv_v[0] == "ret" and recv_v[1] in results:
            prev_sel, prev_recv = results[recv_v[1]]
            if prev_recv and prev_sel:
                if prev_recv[1] and prev_sel in _ALLOC:
                    receiver, how = (prev_recv[0], False), "alloc"
                elif not prev_recv[1] and (prev_sel.startswith("init") or prev_sel in _SELF):
                    receiver, how = (prev_recv[0], False), "init"
        results[s.site] = (selector, receiver)
        entry = {
            "site": s.site,
            "function": s.function,
            "stub": stub,
            "selector": selector,
            "receiver": None,
            "class_method": None,
            "how": how,
            "implemented_by": None,
            "in_image": None,
            "class": fclass.get(s.function, GAME),
        }
        if receiver is not None:
            entry["receiver"], entry["class_method"] = receiver
            if selector is not None:
                impl, in_image = implementer(receiver[0], receiver[1], selector)
                entry["implemented_by"], entry["in_image"] = impl, in_image
        elif selector is not None:
            entry["in_image"] = selector in implemented
        out.append(entry)
    return out


# -- outputs ------------------------------------------------------------------------------


def import_rows(a: Analysis) -> list[dict]:
    """One record per import, in ``nlist`` order (the body of ``imports.json``)."""
    rows = []
    for imp in a.imports:
        sites = a.sites.get(imp.name, [])
        per_function = Counter(f for _, f, _ in sites)
        functions = [
            {
                "function": hexaddr(f),
                "name": _function_name(a.names, f),
                "class": a.function_classes.get(f, GAME),
                "sites": n,
            }
            for f, n in sorted(per_function.items(), key=lambda kv: (-kv[1], kv[0]))
        ]
        check = a.ghidra_check[imp.name]
        rows.append(
            {
                "name": imp.name,
                "library": imp.library,
                "framework": imp.framework,
                "kind": imp.kind,
                "weak": imp.weak,
                "class": a.classes[imp.name],
                "code_sites": len(sites),
                "data_uses": len(a.owners.get(imp.name, [])),
                "stubs": [hexaddr(x) for x in imp.stubs],
                "lazy_pointers": [hexaddr(x) for x in imp.lazy],
                "non_lazy_pointers": [hexaddr(x) for x in imp.non_lazy],
                "binds": [{"address": hexaddr(x), "section": s} for x, s in imp.binds],
                "functions": functions,
                "sites": [[hexaddr(s), hexaddr(f), k] for s, f, k in sites],
                "data": a.owners.get(imp.name, []),
                "ghidra": {
                    "sites": check["ghidra"],
                    "capstone_sites": check["capstone"],
                    "only_capstone": [hexaddr(x) for x in check["only_capstone"]],
                    "only_ghidra": [hexaddr(x) for x in check["only_ghidra"]],
                },
            }
        )
    return rows


def message_rows(a: Analysis) -> list[dict]:
    """Sends grouped by (implementing class, class method?, selector), sorted."""
    groups: dict[tuple, dict] = {}
    for e in a.sends:
        if e["selector"] is None:
            continue
        key = (e["implemented_by"], e["class_method"], e["selector"], e["in_image"])
        g = groups.setdefault(
            key,
            {
                "class": e["implemented_by"],
                "class_method": e["class_method"],
                "selector": e["selector"],
                "in_image": e["in_image"],
                "game": 0,
                "chartboost": 0,
                "unreferenced": 0,
            },
        )
        side = e["class"] if e["class"] in (CHARTBOOST, UNREFERENCED) else GAME
        g[side] += 1
    return sorted(
        groups.values(),
        key=lambda g: (
            g["class"] is None,
            g["class"] or "",
            not g["class_method"],
            g["selector"],
            bool(g["in_image"]),
        ),
    )


def counts(a: Analysis) -> dict:
    by_class = Counter(a.classes.values())
    sends = a.sends
    msgs = message_rows(a)
    framework = [m for m in msgs if m["class"] is not None and m["in_image"] is False]
    unknown_fw = [m for m in msgs if m["class"] is None and m["in_image"] is False]
    return {
        "imports": len(a.imports),
        "game": by_class[GAME],
        "both": by_class[BOTH],
        "chartboost": by_class[CHARTBOOST],
        "unreferenced": by_class[UNREFERENCED],
        "hle": by_class[GAME] + by_class[BOTH],
        "stubs": sum(len(i.stubs) for i in a.imports),
        "lazy_pointers": sum(len(i.lazy) for i in a.imports),
        "non_lazy_pointers": sum(len(i.non_lazy) for i in a.imports),
        "code_sites": sum(len(v) for v in a.sites.values()),
        "functions": len(a.function_classes),
        "game_functions": sum(1 for c in a.function_classes.values() if c == GAME),
        "chartboost_functions": sum(1 for c in a.function_classes.values() if c == CHARTBOOST),
        "both_functions": sum(1 for c in a.function_classes.values() if c == BOTH),
        "unreferenced_functions": sum(1 for c in a.function_classes.values() if c == UNREFERENCED),
        "sends": len(sends),
        "sends_with_selector": sum(1 for e in sends if e["selector"] is not None),
        "sends_with_receiver": sum(1 for e in sends if e["receiver"] is not None),
        "framework_messages": len(framework),
        "framework_messages_game": sum(1 for m in framework if m["game"]),
        "unknown_receiver_framework_selectors": len(unknown_fw),
        "ghidra_agree": sum(
            1 for v in a.ghidra_check.values() if not v["only_capstone"] and not v["only_ghidra"]
        ),
    }


def to_json(a: Analysis, source: dict | None = None) -> dict:
    sends = [
        {
            "site": hexaddr(e["site"]),
            "function": hexaddr(e["function"]),
            "stub": e["stub"],
            "selector": e["selector"],
            "receiver": e["receiver"],
            "class_method": e["class_method"],
            "how": e["how"],
            "implemented_by": e["implemented_by"],
            "in_image": e["in_image"],
            "class": e["class"],
        }
        for e in a.sends
    ]
    return {
        "tool": "mtport.imports",
        "schema_version": SCHEMA_VERSION,
        "source": source or {},
        "counts": counts(a),
        "scan": a.scan_stats,
        "stub_problems": a.stub_problems,
        "category_classes": a.category_classes,
        "imports": import_rows(a),
        "messages": message_rows(a),
        "sends": sends,
        "functions": [
            {"function": hexaddr(f), "name": _function_name(a.names, f), "class": c}
            for f, c in sorted(a.function_classes.items())
        ],
    }


def names_json(a: Analysis, ghidra: Ghidra) -> dict:
    """``work/re/names/imports.json``: a name for each stub Ghidra left unnamed.

    Ghidra's Mach-O loader names the stubs itself (``_strcmp``; the C++ ones demangled, such
    as ``operator.new``), so this is normally empty. An entry is kept for a stub whose Ghidra
    name is missing, a default ``FUN_…`` name, or one this file set (``name_source`` names),
    so applying the file and regenerating it gives the same file.
    """
    out = []
    for imp in a.imports:
        for stub in imp.stubs:
            name, source = ghidra.names.get(stub), ghidra.name_sources.get(stub)
            unnamed = name is None or name.startswith(("FUN_", "thunk_FUN_"))
            if unnamed or (source == "names" and name == imp.name):
                out.append(
                    {
                        "address": hexaddr(stub),
                        "name": imp.name,
                        "kind": "function",
                        "comment": f"stub for {imp.name} ({imp.framework})",
                    }
                )
    return {"source": "imports", "names": sorted(out, key=lambda e: parse_address(e["address"]))}


# -- Markdown -----------------------------------------------------------------------------


_CLASS_TEXT = {
    GAME: "game",
    BOTH: "both",
    CHARTBOOST: "Chartboost only",
    UNREFERENCED: "unreferenced",
}


def _code(text: str) -> str:
    text = text.replace("|", "\\|")
    return f"`` {text} ``" if "`" in text else f"`{text}`"


def _anchor(text: str) -> str:
    return "".join(ch for ch in text.lower().replace(" ", "-") if ch.isalnum() or ch in "-_")


def _framework_order(rows: list[dict]) -> list[str]:
    sites = Counter()
    for r in rows:
        sites[r["framework"]] += r["code_sites"]
    return sorted(sites, key=lambda f: (-sites[f], f))


def _callers(r: dict, limit: int = 3) -> str:
    fs = r["functions"]
    if not fs:
        return "–"
    shown = ", ".join(f"{_code(f['name'])} ({f['sites']})" for f in fs[:limit])
    return shown + (f" +{len(fs) - limit} more" if len(fs) > limit else "")


def _data_text(r: dict) -> str:
    if not r["data"]:
        return "–"
    owners = Counter(d["owner"].split(" at ", 1)[0].split(" ", 1)[0] for d in r["data"])
    return ", ".join(f"{n} {kind}" for kind, n in sorted(owners.items()))


def to_markdown(a: Analysis, source: dict | None = None) -> str:
    c = counts(a)
    rows = import_rows(a)
    src = source or {}
    lines = [
        "# Imports",
        "",
        "<!-- Generated by `python3 -m mtport.imports generate` (tools/mtport/imports.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Every symbol the `{src.get('arch', 'armv7')}` slice imports"
        + (f" (`{src['path']}`, sha256 `{src['sha256']}`)" if "sha256" in src else "")
        + ", with its library, its code sites (calls of its stub, loads of its pointer slot), "
        "the functions that use it and whether the game, Chartboost or both need it. This is "
        "the HLE work list. Machine-readable: `work/re/imports.json` (every site, every "
        "calling function, every send). Method, cross-checks and caveats are at the "
        "[end](#method).",
        "",
        f"**The HLE layer must implement {c['hle']} of the {c['imports']} imports**: "
        f"{c['game']} used only by game code and {c['both']} used by both the game and "
        f"Chartboost. {c['chartboost']} are used only by Chartboost and can be stubbed once ads "
        f"are, and {c['unreferenced']} {'is' if c['unreferenced'] == 1 else 'are'} never used by "
        f"code ({', '.join(_code(r['name']) for r in rows if r['class'] == UNREFERENCED) or '–'}"
        + (
            "; `dyld_stub_binder` is called only by the lazy-binding `__stub_helper`, which a "
            "static recompiler does not need)."
            if any(r["name"] == "dyld_stub_binder" and r["class"] == UNREFERENCED for r in rows)
            else ")."
        ),
        "",
        "| Class | Imports | Meaning |",
        "|---|---|---|",
        f"| game | {c['game']} | only game functions use it |",
        f"| both | {c['both']} | game and Chartboost functions use it |",
        f"| Chartboost only | {c['chartboost']} | only Chartboost functions use it |",
        f"| unreferenced | {c['unreferenced']} | no function uses it |",
        f"| **HLE (game + both)** | **{c['hle']}** | |",
        "",
        "## By framework",
        "",
        "| Framework | Imports | game | both | Chartboost only | unreferenced | HLE | Code sites |",
        "|---|---|---|---|---|---|---|---|",
    ]
    order = _framework_order(rows)
    for fw in order:
        fr = [r for r in rows if r["framework"] == fw]
        k = Counter(r["class"] for r in fr)
        lines.append(
            f"| [{fw}](#{_anchor(fw)}) | {len(fr)} | {k[GAME]} | {k[BOTH]} | {k[CHARTBOOST]} "
            f"| {k[UNREFERENCED]} | {k[GAME] + k[BOTH]} | {sum(r['code_sites'] for r in fr)} |"
        )
    lines += [
        f"| **Total** | **{c['imports']}** | {c['game']} | {c['both']} | {c['chartboost']} "
        f"| {c['unreferenced']} | **{c['hle']}** | {c['code_sites']} |",
        "",
        "## Imports",
        "",
        "Per framework, sorted by code sites. *Code sites*: `bl`/`blx`/`b` to the stub, and "
        "loads of the import's pointer slot (`__nl_symbol_ptr`, `__objc_classrefs`, a bound "
        "`__data` word). *Data uses*: binds inside a structure (a class's superclass or isa, "
        "a CFString's isa, a vtable entry, a block's isa); the structure's owner decides the "
        "class. *Kind*: `function` (called through its stub), `class`/`metaclass` (ObjC), "
        "`address` (no stub, only its address is bound: data, or a function such as "
        "`___cxa_pure_virtual` reached through a pointer). *Functions*: distinct functions "
        "with a code site, the busiest named with their site count.",
        "",
    ]
    for fw in order:
        fr = sorted(
            (r for r in rows if r["framework"] == fw),
            key=lambda r: (-r["code_sites"], -r["data_uses"], r["name"]),
        )
        lib = fr[0]["library"]
        lines += [
            f"### {fw}",
            "",
            f"`{lib}`{' (weak)' if all(r['weak'] for r in fr) else ''}: {len(fr)} imports, "
            f"{sum(1 for r in fr if r['class'] in (GAME, BOTH))} for the HLE layer.",
            "",
            "| Import | Kind | Class | Code sites | Data uses | Functions | Busiest callers |",
            "|---|---|---|---|---|---|---|",
        ]
        for r in fr:
            lines.append(
                f"| {_code(r['name'])} | {r['kind']} | {_CLASS_TEXT[r['class']]} "
                f"| {r['code_sites']} | {_data_text(r)} | {len(r['functions'])} "
                f"| {_callers(r)} |"
            )
        lines.append("")

    msgs = message_rows(a)
    framework = [m for m in msgs if m["class"] is not None and m["in_image"] is False]
    unknown = [m for m in msgs if m["class"] is None and m["in_image"] is False]
    ambiguous = [m for m in msgs if m["class"] is None and m["in_image"]]
    lines += [
        "## Objective-C messages",
        "",
        f"{c['sends']} calls of the `objc_msgSend` family; {c['sends_with_selector']} with a "
        f"known selector (the `__objc_selrefs` slot loaded into r1, or r2 for `_stret`) and "
        f"{c['sends_with_receiver']} with a known receiving class: a class loaded from "
        "`__objc_classrefs` (a class method), the result of `+alloc`/`+new` or `-init…` on one "
        "(an instance method), or the superclass for `objc_msgSendSuper2`. A message to one of "
        "the image's own classes is listed under the framework class whose implementation it "
        "reaches (the first imported superclass without an override in the image).",
        "",
        "### By receiving framework class",
        "",
        f"{len(framework)} (class, selector) pairs, {c['framework_messages_game']} of them sent "
        "by game code. Counts are sends from game / Chartboost functions.",
        "",
        "| Class | Selector | Game | Chartboost |",
        "|---|---|---|---|",
    ]
    for m in framework:
        sel = ("+" if m["class_method"] else "-") + m["selector"]
        lines.append(f"| {_code(m['class'])} | {_code(sel)} | {m['game']} | {m['chartboost']} |")
    lines += [
        "",
        "### Receiver not determined",
        "",
        f"{len(unknown)} selectors that no class or category in the image implements, so "
        "every send of them reaches a framework method; the receiver is not known statically "
        f"(an ivar, an argument, a return value). Another {len(ambiguous)} selectors with an "
        "unknown receiver are also implemented in the image (`init`, `dealloc`, delegate "
        "methods), so they may reach either; they are in `work/re/imports.json` → `messages`.",
        "",
        "| Selector | Game | Chartboost |",
        "|---|---|---|",
    ]
    for m in sorted(unknown, key=lambda m: (-(m["game"] + m["chartboost"]), m["selector"])):
        lines.append(f"| {_code(m['selector'])} | {m['game']} | {m['chartboost']} |")
    lines += ["", *_method_section(a, c)]
    return "\n".join(lines) + "\n"


def _method_section(a: Analysis, c: dict) -> list[str]:
    st = a.scan_stats
    mism = {n: v for n, v in a.ghidra_check.items() if v["only_capstone"] or v["only_ghidra"]}
    lines = [
        "## Method",
        "",
        "Generated by `python3 -m mtport.imports generate` from the slice and the Ghidra "
        "exports of `tools/ghidra/import`.",
        "",
        f"1. **Import table.** {c['imports']} undefined `nlist` symbols (LC_SYMTAB), library "
        f"from the two-level-namespace ordinal in `n_desc`. The LC_DYSYMTAB indirect table maps "
        f"{c['stubs']} `__symbolstub1` stubs, {c['lazy_pointers']} `__lazy_symbol` pointers and "
        f"{c['non_lazy_pointers']} `__nl_symbol_ptr` import pointers (the other "
        "`__nl_symbol_ptr` slots point into the image). Every stub is `ldr pc, [pc, #n]` and "
        "loads the lazy pointer of the same symbol; every pointer agrees with the dyld "
        "bind/lazy-bind/weak-bind streams"
        + (" (no exceptions)." if not a.stub_problems else f" except: {a.stub_problems}."),
        f"2. **Code sites** (`tools/mtport/armscan.py`, Capstone): {st['functions']} "
        f"`LC_FUNCTION_STARTS` functions, {st['instructions']} instructions decoded "
        f"({st['skipped_bytes']} bytes of literal pools and jump tables skipped). A forward "
        "data-flow pass over each function's basic blocks follows registers and stack slots "
        "through `ldr rX, [pc, #n]`, `movw`/`movt` and `add rX, pc` to the absolute slot "
        f"address. {c['code_sites']} code sites in all.",
        f"3. **Cross-check with Ghidra** (`work/ghidra/xrefs.json`): for {c['ghidra_agree']} of "
        f"{c['imports']} imports the call and load sites are identical. Ghidra's "
        "`COMPUTED_CALL`s through a bound `__data` pointer (`free`, `malloc` via a function "
        "table) are the same accesses as the `READ`s of that pointer and are not counted twice.",
    ]
    for name, v in sorted(mism.items()):
        lines.append(
            f"   - {_code(name)}: Capstone {v['capstone']}, Ghidra {v['ghidra']}; only "
            f"Ghidra {', '.join(_code(hexaddr(x)) for x in v['only_ghidra']) or '–'}, only "
            f"Capstone {', '.join(_code(hexaddr(x)) for x in v['only_capstone']) or '–'}. "
            + (
                "Capstone has the same slot as an address-only site at "
                + ", ".join(_code(hexaddr(x)) for x in v["capstone_address_only"])
                + ": the load sits after a join where only one path holds the address, which "
                "Ghidra follows and the data-flow intersection does not."
                if v["capstone_address_only"]
                else ""
            )
        )
    lines += [
        f"4. **Function classes** (first pass, `classify_functions`; #10 replaces it): "
        f"{c['functions']} functions: {c['game_functions']} game, {c['chartboost_functions']} "
        f"Chartboost, {c['both_functions']} both, {c['unreferenced_functions']} unreferenced. "
        f"Chartboost entries are the IMPs of the 57 Chartboost classes ({len(a.chartboost_seeds)} "
        "entries with the category methods below); a function reachable only from them in the "
        "call graph (Ghidra's references between functions, plus function pointers in block "
        "descriptors, block literals and vtables, attributed to the code that references the "
        "structure) is Chartboost's. Every other function without a caller is a game root "
        "(ObjC IMPs, C++ virtuals, callbacks, `start`).",
        "   Category methods on framework classes are called only by code that sends their "
        "selector, so they are classified by those senders: "
        + "; ".join(f"{_code(k)} {_CLASS_TEXT[v]}" for k, v in sorted(a.category_classes.items()))
        + ". Methods whose selector nothing sends are unreferenced.",
        "5. **Import classes.** An import is used by the game when a game function has a code "
        "site for it or a game structure binds it, and by Chartboost likewise. A class's "
        "superclass/isa binds belong to that class; a CFString's or block's isa to the "
        "functions that reference it; other binds in `__const`/`__data` (C++ vtables and RTTI) "
        "to the game.",
        "",
        "Caveats: selectors reached through `performSelector:`/`NSSelectorFromString` and "
        "classes through `NSClassFromString` are invisible here; both functions are imported "
        "(`_NSSelectorFromString` only by Chartboost). A value whose address escapes to a "
        "callee is not tracked through that callee.",
    ]
    return lines


# -- command line -------------------------------------------------------------------------


def run(image: macho.MachO, ghidra: Ghidra) -> Analysis:
    """Scan ``image`` and analyse it against the Ghidra exports."""
    table = import_table(image)
    dispatchers = [a for i in table if i.name in DISPATCHERS for a in i.stubs]
    scan = armscan.scan(image, dispatchers)
    return analyse(image, objc.parse(image), scan, ghidra)


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
