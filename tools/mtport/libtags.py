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
import bisect
import hashlib
import json
import re
import struct
import sys
from collections import defaultdict
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from pathlib import Path

from . import armscan, macho
from . import ghidra_common as gc
from .objc import is_chartboost

SCHEMA_VERSION = 1
JSON_OUT = "work/re/function-tags.json"
MARKDOWN_OUT = "docs/re/libraries.md"
FUNCTIONS_JSON = "work/ghidra/functions.json"
XREFS_JSON = "work/ghidra/xrefs.json"
OBJC_JSON = "work/re/objc.json"
# Written by tools/ghidra/scripts/apply_tags.py: the scheme's tags read back from Ghidra.
READBACK_JSON = "work/re/ghidra-tags.json"
READBACK_SCHEMA = 1

APP_OBJC = "app-objc"
ENGINE = "engine"
UNKNOWN = "unknown"
LIB_PREFIX = "lib:"
CHARTBOOST = "lib:chartboost"
CRT1 = "lib:crt1"
STL = "lib:stl"

# A data object (block literal, pointer table, vtable) is at most this big: a data
# reference P -> G is attributed to the closest code-referenced address in [P - this, P].
DATA_OBJECT_MAX = 0x400

# Rule names in priority order (the order the rules run in; first assignment wins).
RULE_DOCS = {
    "objc-imp": "IMP of an ObjC method (work/re/objc.json): app class -> app-objc, "
    "Chartboost class -> lib:chartboost, category -> its library",
    "entry-point": "LC_UNIXTHREAD entry = crt1 start -> lib:crt1; the function start calls "
    "is main -> app-objc",
    "export": "exported C symbol with a known library prefix (EXPORT_LIBRARIES)",
    "signature": "contains or references a strong library marker (SIGNATURES)",
    "vtable": "virtual function of a class with an RTTI name: std:: -> lib:stl, else engine",
    "static-init": "C++ static constructor listed in __mod_init_func -> engine",
    "objc-file": "referenced only from one ObjC class's code and linked next to its IMPs "
    "(blocks, static helpers, inlined CGGeometry functions of that .m file)",
    "closure": "every referrer (call, jump, address taken, via a data object) has this lib:* tag",
    "reach": "reachable from app-objc/engine code without passing through library code",
    "layout": "linked between two functions with the same tag (object files are contiguous)",
    "unknown": "no rule applies",
}

# Categories on system classes and the code they come from. A category not listed here is
# lib:chartboost when linked inside the Chartboost range, else app-objc.
CATEGORY_LIBRARIES = {
    "IdentifierAddition": (
        "lib:uidevice-identifier",
        "UIDevice+IdentifierAddition from the open-source UIDevice-with-UniqueIdentifier-"
        "for-iOS-5 (Georg Kitz): -macaddress, -uniqueDeviceIdentifier, "
        "-uniqueGlobalDeviceIdentifier",
    ),
    "MD5Addition": (
        "lib:uidevice-identifier",
        "NSString+MD5Addition from the same project: -stringFromMD5 (CC_MD5)",
    ),
}


# -- signatures ----------------------------------------------------------------------------


@dataclass(frozen=True)
class Signature:
    """One library marker. ``kind`` is ``bytes`` (anywhere in the file outside
    ``__LINKEDIT``) or ``const32`` (a 32-bit value built by ``movw``/``movt``, or a literal
    pool word in ``__text``). A *strong* marker identifies the library on its own."""

    library: str
    kind: str
    value: bytes | int
    what: str
    strong: bool = True


SIGNATURES: tuple[Signature, ...] = (
    # zlib 1.x: inflate/deflate messages, the crc32 table, the adler32 modulus.
    Signature("lib:zlib", "bytes", b"incorrect header check", "zlib inflate message"),
    Signature("lib:zlib", "bytes", b"invalid distance too far back", "zlib inflate message"),
    Signature("lib:zlib", "bytes", b"invalid block type", "zlib inflate message"),
    Signature("lib:zlib", "bytes", b" inflate ", "zlib inflate copyright banner"),
    Signature("lib:zlib", "bytes", b" deflate ", "zlib deflate copyright banner"),
    Signature("lib:zlib", "bytes", struct.pack("<3I", 0, 0x77073096, 0xEE0E612C), "crc32 table"),
    Signature("lib:zlib", "const32", 0xEDB88320, "crc32 polynomial (reflected)", strong=False),
    Signature("lib:zlib", "const32", 65521, "adler32 modulus 65521", strong=False),
    # libpng: the signature, chunk tags as strings, version banner and messages.
    Signature("lib:png", "bytes", b"\x89PNG\r\n\x1a\n", "PNG file signature"),
    Signature("lib:png", "bytes", b"libpng", "libpng version/banner string"),
    Signature("lib:png", "bytes", b"IHDR", "PNG chunk tag IHDR"),
    Signature("lib:png", "bytes", b"IDAT", "PNG chunk tag IDAT"),
    Signature("lib:png", "bytes", b"Invalid IHDR data", "libpng error message"),
    # Lua 4.0-5.1: bytecode signature, opcode names, runtime error strings.
    Signature("lib:lua", "bytes", b"\x1bLua", 'LUA_SIGNATURE "\\033Lua"'),
    Signature("lib:lua", "const32", 0x61754C1B, 'LUA_SIGNATURE "\\033Lua" as a word'),
    Signature("lib:lua", "bytes", b"GETUPVAL\x00", "Lua opcode name table"),
    Signature("lib:lua", "bytes", b"attempt to call", "Lua runtime error"),
    Signature("lib:lua", "bytes", b"attempt to index", "Lua runtime error"),
    Signature("lib:lua", "bytes", b"stack traceback", "Lua debug library"),
    Signature("lib:lua", "bytes", b"__index\x00", "Lua metamethod name"),
    # libstdc++ templates instantiated in the binary leave error strings and mangled names.
    Signature("lib:stl", "bytes", b"vector::_M_", "std::vector length/insert error string"),
    Signature("lib:stl", "bytes", b"basic_string::", "std::basic_string error string"),
    Signature("lib:stl", "bytes", b"_ZNSt", "mangled std:: symbol"),
    # Apple's oalTouch sample, MyOpenALSupport.h: MyGetOpenALAudioData, alBufferDataStaticProc.
    Signature(
        "lib:apple-openal-support",
        "bytes",
        b"MyGetOpenALAudioData",
        "MyGetOpenALAudioData() messages (MyOpenALSupport.h, Apple oalTouch sample)",
    ),
    Signature(
        "lib:apple-openal-support",
        "bytes",
        b"alBufferDataStatic\x00",
        'alcGetProcAddress("alBufferDataStatic") (alBufferDataStaticProc, MyOpenALSupport.h)',
    ),
    # UIDevice+IdentifierAddition's -macaddress (sysctl NET_RT_IFLIST on en0).
    Signature(
        "lib:uidevice-identifier",
        "bytes",
        b"Error: if_nametoindex error",
        "-macaddress error message of UIDevice+IdentifierAddition",
    ),
    # Chartboost's bundled SBJson and BSON code.
    Signature(CHARTBOOST, "bytes", b"org.brautaset.", "SBJson error domain (CBJson*)"),
    Signature(CHARTBOOST, "bytes", b"BSON error.", "CBBSON error message"),
    Signature(CHARTBOOST, "bytes", b"/Chartboost/", "Chartboost build path"),
)

# Symbols crt1.o defines; the export trie still lists them.
CRT1_EXPORTS = ("start", "_NXArgc", "_NXArgv", "_environ", "___progname")

# Exported C functions -> library: (tag, name regex, what).
EXPORT_LIBRARIES: tuple[tuple[str, str, str], ...] = (
    (
        CHARTBOOST,
        r"_bson_",
        "BSON encoder from the MongoDB C driver (bson.c), bundled by Chartboost for CBBSON",
    ),
    (CHARTBOOST, r"_cb_bson_|_CBNativeMoreApps", "Chartboost C helper"),
)

# Imports that would be present if libstdc++ containers/strings were instantiated.
STL_IMPORT_MARKERS = re.compile(r"^__ZNS[st]|^__ZSt\d+__throw|^__ZNKS[st]|_Rb_tree|_List_node")


# -- inputs --------------------------------------------------------------------------------


@dataclass
class Function:
    address: int  # even
    thumb: bool
    span: int  # bytes to the next start (or the end of __text)
    body: int  # Ghidra body size
    name: str

    @property
    def pointer(self) -> str:
        return gc.pointer(self.address, self.thumb)


@dataclass
class Program:
    """Everything the rules look at, independent of Ghidra and of the files' layout."""

    image: macho.MachO
    functions: dict[int, Function]
    objc: dict
    # function -> {referenced function: how}, from code and through data objects
    refs: dict[int, dict[int, str]] = field(default_factory=dict)
    # function -> imported symbols it calls through __symbolstub1
    imports: dict[int, set[str]] = field(default_factory=dict)
    # data address P (outside __text) -> functions it points at, from xrefs
    data_refs: dict[int, set[int]] = field(default_factory=dict)
    # data address A -> functions whose code references it
    code_to_data: dict[int, set[int]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.starts = sorted(self.functions)
        self.index = armscan.FunctionIndex(self.starts)
        text = self.image.section("__TEXT", "__text")
        self.text = (text.addr, text.end) if text else (0, 0)

    def owner(self, addr: int) -> int | None:
        """The function whose span holds ``addr`` (a code address in ``__text``)."""
        if not self.text[0] <= addr < self.text[1]:
            return None
        return self.index.of(addr)

    def object_users(self, p: int) -> set[int]:
        """Functions whose code references the data object holding address ``p``: the
        closest code-referenced address at or below ``p`` in the same section, at most
        :data:`DATA_OBJECT_MAX` bytes before it."""
        if not hasattr(self, "_anchors"):
            self._anchors = sorted(self.code_to_data)
        i = bisect.bisect_right(self._anchors, p) - 1
        sec = self.image.section_at(p)
        if i < 0 or sec is None:
            return set()
        a = self._anchors[i]
        if p - a > DATA_OBJECT_MAX or not sec.contains(a):
            return set()
        return set(self.code_to_data[a])

    def pointers_to(self, addr: int) -> list[int]:
        """Aligned data words (outside ``__text``) whose value is ``addr``."""
        if not hasattr(self, "_pointers"):
            index: dict[int, list[int]] = defaultdict(list)
            for sec in self.image.sections:
                if sec.offset == 0 or sec.sectname == "__text" or sec.size < 4:
                    continue
                base = sec.addr + (-sec.addr % 4)
                count = (sec.end - base) // 4
                words = struct.unpack(f"<{count}I", self.image.read(base, count * 4))
                for i, w in enumerate(words):
                    index[w].append(base + 4 * i)
            self._pointers = dict(index)
        return self._pointers.get(addr, [])

    def data_users(self, addr: int) -> set[int]:
        """Functions that reference the data at ``addr`` directly, or through one pointer
        (e.g. the CFString object that points at a C string)."""
        users = set(self.code_to_data.get(addr, ()))
        for p in self.pointers_to(addr):
            users |= self.object_users(p)
        return users

    def referrers(self) -> dict[int, dict[int, str]]:
        out: dict[int, dict[int, str]] = defaultdict(dict)
        for src, dsts in self.refs.items():
            for dst, how in dsts.items():
                out[dst][src] = how
        return out


def _int(text: str) -> int:
    return int(text, 16)


def build_program(
    image: macho.MachO, functions_doc: dict, xrefs_doc: dict, objc_doc: dict
) -> Program:
    """Combine the slice with the Ghidra exports and the ObjC metadata."""
    starts = sorted(image.function_starts)
    text = image.section("__TEXT", "__text")
    ghidra = {_int(f["address"]) & ~1: f for f in functions_doc["functions"]}
    funcs: dict[int, Function] = {}
    even = [s & ~1 for s in starts]
    for i, raw in enumerate(starts):
        addr = raw & ~1
        end = even[i + 1] if i + 1 < len(even) else (text.end if text else addr)
        g = ghidra.get(addr, {})
        funcs[addr] = Function(addr, bool(raw & 1), end - addr, g.get("size", 0), g.get("name", ""))
    prog = Program(image, funcs, objc_doc)

    stub = image.section("__TEXT", "__symbolstub1")
    stubs: dict[int, str] = {}
    for addr, g in ghidra.items():
        if stub and stub.contains(addr):
            thunk = g.get("thunk_of") or ""
            stubs[addr] = thunk.split("::")[-1] if thunk.startswith("EXTERNAL:") else g["name"]

    refs: dict[int, dict[int, str]] = defaultdict(dict)
    imports: dict[int, set[str]] = defaultdict(set)
    data_refs: dict[int, set[int]] = defaultdict(set)
    code_to_data: dict[int, set[int]] = defaultdict(set)
    for x in xrefs_doc["xrefs"]:
        to = x["to"]
        if to.startswith("EXTERNAL:"):
            continue
        src_addr = _int(x["from"])
        src = (_int(x["function"]) & ~1) if x["function"] else prog.owner(src_addr)
        if src is not None and src not in funcs:
            src = prog.owner(src_addr)
        dst = _int(to)
        target = dst & ~1 if (dst & ~1) in funcs else None
        if src is None:
            # From data: remember what it points at (not from __LINKEDIT, which Ghidra
            # loads but which holds no pointers).
            if target is not None and image.section_at(src_addr) is not None:
                data_refs[src_addr].add(target)
            continue
        if target is not None:
            if target != src:
                how = (
                    "call"
                    if x["type"].endswith("CALL")
                    else ("jump" if x["type"].endswith("JUMP") else "pointer")
                )
                prev = refs[src].get(target)
                if prev is None or (prev != "call" and how == "call"):
                    refs[src][target] = how
        elif (dst & ~1) in stubs:
            imports[src].add(stubs[dst & ~1])
        elif not (prog.text[0] <= dst < prog.text[1]):
            code_to_data[dst].add(src)
    # Ghidra only types some data words as pointers (not, e.g., block descriptors in
    # __data), so also take every aligned word outside code and ObjC metadata that equals a
    # function pointer (Thumb bit included) as a reference.
    entries = {f.address | int(f.thumb): f.address for f in funcs.values()}
    for sec in image.sections:
        if sec.offset == 0 or sec.sectname in ("__text", "__symbolstub1", "__stub_helper"):
            continue
        if sec.sectname.startswith("__objc_") or sec.size < 4:
            continue
        base = sec.addr + (-sec.addr % 4)
        count = (sec.end - base) // 4
        for i, w in enumerate(struct.unpack(f"<{count}I", image.read(base, count * 4))):
            if w in entries:
                data_refs[base + 4 * i].add(entries[w])
    prog.refs = dict(refs)
    prog.imports = dict(imports)
    prog.data_refs = dict(data_refs)
    prog.code_to_data = dict(code_to_data)
    _add_data_object_edges(prog)
    return prog


def _add_data_object_edges(prog: Program) -> None:
    """F -> G when F references the data object holding a pointer to G."""
    for p, targets in sorted(prog.data_refs.items()):
        for f in sorted(prog.object_users(p)):
            for g in sorted(targets):
                if g != f:
                    prog.refs.setdefault(f, {}).setdefault(g, f"via data 0x{p:X}")


# -- vtables -------------------------------------------------------------------------------


@dataclass(frozen=True)
class VTable:
    address: int  # the offset-to-top word
    class_name: str  # demangled RTTI name
    typeinfo: int
    functions: tuple[int, ...]  # even addresses of the virtual functions (no pure virtuals)


def rtti_vtables(prog: Program) -> list[VTable]:
    """The vtables #9 finds (:mod:`mtport.rtti`): RTTI typeinfo objects via their binds, then
    every vtable that points at one. Pure-virtual slots are left out."""
    from . import rtti

    typeinfos = rtti.find_typeinfos(prog.image)
    out = []
    for v in rtti.find_vtables(rtti.Image(prog.image), typeinfos):
        funcs = tuple(s.target & ~1 for s in v.slots if s.target and not s.pure)
        out.append(VTable(v.address, v.cls, prog.image.u32(v.address + 4), funcs))
    return out


# -- tagging -------------------------------------------------------------------------------


@dataclass
class Assignment:
    tag: str
    rule: str
    reason: str


@dataclass
class Tagger:
    prog: Program
    assigned: dict[int, Assignment] = field(default_factory=dict)
    # library -> evidence records {"address", "reason"} that identify it
    evidence: dict[str, list[dict]] = field(default_factory=lambda: defaultdict(list))
    signatures: list[dict] = field(default_factory=list)
    vtables: list[VTable] = field(default_factory=list)

    def assign(self, addr: int, tag: str, rule: str, reason: str) -> bool:
        if addr not in self.prog.functions or addr in self.assigned:
            return False
        self.assigned[addr] = Assignment(tag, rule, reason)
        return True

    def tag(self, addr: int) -> str | None:
        a = self.assigned.get(addr)
        return a.tag if a else None

    def note(self, library: str, address: int | str, reason: str) -> None:
        addr = address if isinstance(address, str) else f"0x{address:X}"
        self.evidence[library].append({"address": addr, "reason": reason})


def _ptr(prog: Program, addr: int) -> str:
    f = prog.functions.get(addr)
    return f.pointer if f else f"0x{addr:X}"


def rule_objc(t: Tagger) -> None:
    prog, objc = t.prog, t.prog.objc
    cb_imps: list[int] = []
    for cls in objc["classes"]:
        is_cb = is_chartboost(cls["name"])
        tag = CHARTBOOST if is_cb else APP_OBJC
        for kind, methods in (("-", cls["instance_methods"]), ("+", cls["class_methods"])):
            for m in methods:
                if not m["imp"]:
                    continue
                addr = _int(m["imp"]) & ~1
                if is_cb:
                    cb_imps.append(addr)
                t.assign(addr, tag, "objc-imp", f"{kind}[{cls['name']} {m['selector']}]")
    cb_classes = [c["name"] for c in objc["classes"] if is_chartboost(c["name"])]
    if cb_imps:
        lo, hi = min(cb_imps), max(cb_imps)
        t.note(
            CHARTBOOST,
            f"0x{lo:X}-0x{hi:X}",
            f"IMPs of the {len(cb_classes)} Chartboost classes ({', '.join(cb_classes[:3])}, "
            "...) are linked as one contiguous block",
        )
    lo = min(cb_imps) if cb_imps else None
    hi = max(cb_imps) if cb_imps else None
    for cat in objc["categories"]:
        owner = f"{cat['class']['name']}({cat['name']})"
        imps = [
            (_int(m["imp"]) & ~1, kind, m["selector"])
            for kind, ms in (("-", cat["instance_methods"]), ("+", cat["class_methods"]))
            for m in ms
            if m["imp"]
        ]
        if cat["name"] in CATEGORY_LIBRARIES:
            tag, why = CATEGORY_LIBRARIES[cat["name"]]
        elif lo is not None and imps and all(lo <= a <= hi for a, _, _ in imps):
            tag, why = CHARTBOOST, "category linked inside the Chartboost block"
        else:
            tag, why = APP_OBJC, "category linked with the app"
        for addr, kind, sel in imps:
            t.assign(addr, tag, "objc-imp", f"{kind}[{owner} {sel}]")
        if tag.startswith(LIB_PREFIX) and imps:
            first = min(a for a, _, _ in imps)
            own = {a for a, _, _ in imps}
            senders = sorted({f for _, _, sel in imps for f in _selector_senders(prog, sel)} - own)
            sent = ", ".join(_ptr(prog, f) for f in senders[:6]) or "nobody outside the category"
            plural = "s" if len(imps) != 1 else ""
            t.note(
                tag,
                _ptr(prog, first),
                f"category {owner} ({len(imps)} IMP{plural}): {why}; "
                f"its selectors are sent from {sent}",
            )


def _selector_senders(prog: Program, selector: str) -> set[int]:
    """Functions that load the ``__objc_selrefs`` slot of ``selector``."""
    out: set[int] = set()
    for ref in prog.objc.get("selrefs", []):
        if ref["selector"] == selector:
            out |= prog.code_to_data.get(_int(ref["address"]), set())
    return out


def rule_entry(t: Tagger) -> None:
    prog = t.prog
    pc = prog.image.entry_point
    if pc is None or (pc & ~1) not in prog.functions:
        return
    start = pc & ~1
    t.assign(start, CRT1, "entry-point", "LC_UNIXTHREAD pc: crt1.o start")
    t.note(CRT1, _ptr(prog, start), "LC_UNIXTHREAD entry point (crt1.o start, ARM mode)")
    crt_data = [e for e in prog.image.exports if e.name in CRT1_EXPORTS]
    if crt_data:
        names = ", ".join(f"{e.name} 0x{e.address:X}" for e in crt_data)
        t.note(CRT1, f"0x{crt_data[0].address:X}", f"crt1.o's exported symbols: {names}")
    callees = [g for g, how in sorted(prog.refs.get(start, {}).items()) if how == "call"]
    for g in callees:
        if "_UIApplicationMain" in prog.imports.get(g, set()):
            t.assign(g, APP_OBJC, "entry-point", "main: called by start, calls UIApplicationMain")


def rule_exports(t: Tagger) -> None:
    """Exported C symbols (the export trie is not stripped) with a known library prefix."""
    prog = t.prog
    for lib_tag, pattern, what in EXPORT_LIBRARIES:
        hits = [
            e
            for e in prog.image.exports
            if e.address is not None
            and re.match(pattern, e.name)
            and (e.address & ~1) in prog.functions
        ]
        for e in hits:
            t.assign(e.address & ~1, lib_tag, "export", f"exported symbol {e.name}: {what}")
        if hits:
            names = ", ".join(e.name for e in hits[:4]) + (" ..." if len(hits) > 4 else "")
            t.note(
                lib_tag,
                _ptr(prog, min(e.address & ~1 for e in hits)),
                f"{len(hits)} exported functions {names}: {what}",
            )


def _const32_hits(prog: Program) -> dict[int, list[int]]:
    """32-bit constants built by Thumb-2 ``movw`` (+ ``movt`` to the same register within
    the next 7 instructions) anywhere in ``__text``, mapped to the addresses of the ``movw``
    (:func:`mtport.armscan.movw_movt_constants`)."""
    text = prog.image.section("__TEXT", "__text")
    return armscan.movw_movt_constants(prog.image.read(text.addr, text.size // 2 * 2), text.addr)


def _find_bytes(image: macho.MachO, needle: bytes) -> list[int]:
    """VM addresses of ``needle`` in every section (not ``__LINKEDIT``)."""
    hits = []
    for seg in image.segments:
        for sec in seg.sections:
            if sec.offset == 0:  # zerofill
                continue
            blob = image.read(sec.addr, sec.size)
            pos = blob.find(needle)
            while pos >= 0:
                hits.append(sec.addr + pos)
                pos = blob.find(needle, pos + 1)
    return hits


def _string_start(image: macho.MachO, addr: int) -> int:
    """Start of the C string holding ``addr`` if it is in a C-string section, else ``addr``."""
    sec = image.section_at(addr)
    if sec is None or sec.flags & gc.SECTION_TYPE != gc.S_CSTRING_LITERALS:
        return addr
    blob = image.read(sec.addr, addr - sec.addr)
    return sec.addr + blob.rfind(b"\0") + 1


def rule_signatures(t: Tagger) -> None:
    prog = t.prog
    consts = _const32_hits(prog)
    text = prog.image.section("__TEXT", "__text")
    for sig in SIGNATURES:
        if sig.kind == "bytes":
            hits = _find_bytes(prog.image, sig.value)
        else:
            hits = list(consts.get(sig.value, []))
            hits += [
                a for a in _find_bytes(prog.image, struct.pack("<I", sig.value)) if text.contains(a)
            ]
        users: set[int] = set()
        for h in hits:
            if text.contains(h):
                users.add(prog.owner(h))
            else:
                users |= prog.data_users(_string_start(prog.image, h))
        record = {
            "library": sig.library,
            "marker": sig.what,
            "strong": sig.strong,
            "hits": [f"0x{h:X}" for h in sorted(hits)],
            "functions": [_ptr(prog, u) for u in sorted(users)],
        }
        t.signatures.append(record)
        if not hits:
            continue
        where = ", ".join(record["hits"][:3]) + (" ..." if len(hits) > 3 else "")
        if sig.strong:
            funcs = ", ".join(record["functions"][:4]) + (" ..." if len(users) > 4 else "")
            t.note(
                sig.library,
                record["hits"][0],
                f"{sig.what} at {where}" + (f", used by {funcs}" if funcs else ""),
            )
            for u in sorted(users):
                t.assign(u, sig.library, "signature", f"{sig.what} ({where})")


def rule_stl_imports(t: Tagger) -> None:
    """libstdc++ is a dylib here; a template instantiation of its containers or strings would
    import out-of-line members (``__throw_*``, ``_Rb_tree_*``, ...). Record what is imported."""
    syms = sorted({b.symbol for b in t.prog.image.binds})
    cxx = [s for s in syms if s.startswith("__Z")]
    stl = [s for s in cxx if STL_IMPORT_MARKERS.search(s)]
    t.signatures.append(
        {
            "library": STL,
            "marker": "libstdc++ container/string imports",
            "strong": True,
            "hits": stl,
            "functions": [],
            "note": "C++ imports present: " + ", ".join(cxx),
        }
    )


EngineEvidence = Callable[[Program], list[VTable]]


def make_vtable_rule(provider: EngineEvidence = rtti_vtables) -> Callable[[Tagger], None]:
    def rule_vtables(t: Tagger) -> None:
        t.vtables = sorted(provider(t.prog), key=lambda v: v.address)
        for vt in t.vtables:
            std = vt.class_name.startswith(("std::", "__gnu_cxx::"))
            tag = STL if std else ENGINE
            for i, f in enumerate(vt.functions):
                t.assign(f, tag, "vtable", f"{vt.class_name} vtable 0x{vt.address:X} slot {i}")
            if std:
                t.note(STL, f"0x{vt.address:X}", f"vtable of {vt.class_name}")

    return rule_vtables


def rule_static_init(t: Tagger) -> None:
    sec = t.prog.image.section("__DATA", "__mod_init_func")
    if sec is None:
        return
    for i in range(sec.size // 4):
        ptr = t.prog.image.u32(sec.addr + 4 * i)
        t.assign(ptr & ~1, ENGINE, "static-init", f"__mod_init_func[{i}] at 0x{sec.addr + 4 * i:X}")


def _describe(prog: Program, t: Tagger, src: int, how: str) -> str:
    a = t.assigned.get(src)
    who = a.reason if a and a.rule == "objc-imp" else _ptr(prog, src)
    return f"{how} from {who}"


def _objc_owners(objc: dict) -> dict[int, str]:
    """IMP (even address) -> owning class, ``Class(Category)`` for category methods."""
    out: dict[int, str] = {}
    for cls in objc["classes"]:
        for m in cls["instance_methods"] + cls["class_methods"]:
            if m["imp"]:
                out.setdefault(_int(m["imp"]) & ~1, cls["name"])
    for cat in objc["categories"]:
        for m in cat["instance_methods"] + cat["class_methods"]:
            if m["imp"]:
                out.setdefault(_int(m["imp"]) & ~1, f"{cat['class']['name']}({cat['name']})")
    return out


def rule_objc_file(t: Tagger) -> None:
    """A function referenced only from the code of one ObjC class, and linked right before,
    among or right after that class's IMPs, belongs to the class's ``.m`` file."""
    prog = t.prog
    file_of = {a: k for a, k in _objc_owners(prog.objc).items() if a in t.assigned}
    imps = sorted(file_of)
    referrers = prog.referrers()

    def neighbours(addr: int) -> set[str]:
        i = bisect.bisect_left(imps, addr)
        return {file_of[imps[j]] for j in (i - 1, i) if 0 <= j < len(imps)}

    changed = True
    while changed:
        changed = False
        for addr in prog.starts:
            if addr in t.assigned:
                continue
            refs = referrers.get(addr)
            if not refs:
                continue
            files = {file_of.get(r) for r in refs}
            if len(files) != 1 or None in files:
                continue
            (key,) = files
            if key not in neighbours(addr):
                continue
            tags = {t.tag(r) for r in refs}
            if len(tags) != 1:
                continue
            (tag,) = tags
            src = min(refs)
            more = f" (+{len(refs) - 1} more)" if len(refs) > 1 else ""
            t.assign(addr, tag, "objc-file", f"{key}: {_describe(prog, t, src, refs[src])}{more}")
            file_of[addr] = key
            changed = True


def rule_closure(t: Tagger) -> None:
    prog = t.prog
    referrers = prog.referrers()
    changed = True
    while changed:
        changed = False
        for addr in prog.starts:
            if addr in t.assigned:
                continue
            refs = referrers.get(addr)
            if not refs:
                continue
            tags = {t.tag(r) for r in refs}
            if len(tags) != 1:
                continue
            (tag,) = tags
            if tag is None or not tag.startswith(LIB_PREFIX):
                continue
            src = min(refs)
            more = f" (+{len(refs) - 1} more)" if len(refs) > 1 else ""
            t.assign(addr, tag, "closure", _describe(prog, t, src, refs[src]) + more)
            changed = True


def rule_reach(t: Tagger) -> None:
    prog = t.prog
    roots = [a for a in prog.starts if t.tag(a) in (APP_OBJC, ENGINE)]
    queue = list(roots)
    while queue:
        src = queue.pop(0)
        for dst, how in sorted(prog.refs.get(src, {}).items()):
            if dst in t.assigned:
                continue
            t.assign(dst, ENGINE, "reach", _describe(prog, t, src, how))
            queue.append(dst)


def rule_layout(t: Tagger) -> None:
    prog = t.prog
    starts = prog.starts
    changed = True
    while changed:
        changed = False
        for i, addr in enumerate(starts):
            if addr in t.assigned:
                continue
            lo = next((starts[j] for j in range(i - 1, -1, -1) if starts[j] in t.assigned), None)
            hi = next(
                (starts[j] for j in range(i + 1, len(starts)) if starts[j] in t.assigned), None
            )
            if lo is None or hi is None:
                continue
            if t.tag(lo) == t.tag(hi):
                t.assign(
                    addr, t.tag(lo), "layout", f"between {_ptr(prog, lo)} and {_ptr(prog, hi)}"
                )
                changed = True


def rule_unknown(t: Tagger) -> None:
    for addr in t.prog.starts:
        t.assign(addr, UNKNOWN, "unknown", "no rule applies")


def default_rules(engine_evidence: EngineEvidence = rtti_vtables) -> list:
    return [
        ("objc-imp", rule_objc),
        ("entry-point", rule_entry),
        ("export", rule_exports),
        ("signature", rule_signatures),
        ("signature", rule_stl_imports),
        ("vtable", make_vtable_rule(engine_evidence)),
        ("static-init", rule_static_init),
        ("objc-file", rule_objc_file),
        ("closure", rule_closure),
        ("reach", rule_reach),
        ("layout", rule_layout),
        ("closure", rule_closure),
        ("reach", rule_reach),
        ("unknown", rule_unknown),
    ]


def tag_program(prog: Program, rules: Iterable | None = None) -> Tagger:
    t = Tagger(prog)
    for _, rule in rules if rules is not None else default_rules():
        rule(t)
    check_coverage([prog.functions[a].pointer for a in t.assigned], prog.image.function_starts)
    return t


# -- coverage ------------------------------------------------------------------------------


class CoverageError(ValueError):
    pass


def check_coverage(pointers: Iterable[str], starts: Iterable[int]) -> None:
    """Raise unless ``pointers`` are exactly the ``LC_FUNCTION_STARTS`` entries (as function
    pointers, Thumb bit included), each once."""
    want = sorted(gc.hexaddr(s) for s in starts)
    got = sorted(pointers)
    if len(set(got)) != len(got):
        dup = sorted({p for p in got if got.count(p) > 1})
        raise CoverageError(f"duplicate functions: {dup[:10]}")
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))
    if missing or extra:
        raise CoverageError(
            f"missing {len(missing)} {missing[:10]}, extra {len(extra)} {extra[:10]}"
        )


def check_doc(doc: dict, image: macho.MachO) -> dict:
    """Validate a function-tags document against the slice; return the counts."""
    if doc.get("schema") != SCHEMA_VERSION:
        raise CoverageError(f"schema {doc.get('schema')!r}, expected {SCHEMA_VERSION}")
    records = doc["functions"]
    check_coverage([r["address"] for r in records], image.function_starts)
    counts: dict[str, int] = defaultdict(int)
    for r in records:
        tag = r["tag"]
        if tag not in (APP_OBJC, ENGINE, UNKNOWN) and not (
            tag.startswith(LIB_PREFIX) and len(tag) > len(LIB_PREFIX)
        ):
            raise CoverageError(f"{r['address']}: bad tag {tag!r}")
        if r["rule"] not in RULE_DOCS or not r["reason"]:
            raise CoverageError(f"{r['address']}: rule {r['rule']!r} without a reason")
        counts[tag] += 1
    for tag in counts:
        if tag.startswith(LIB_PREFIX) and not doc["libraries"].get(tag, {}).get("evidence"):
            raise CoverageError(f"{tag} has no evidence")
    return dict(counts)


# -- output --------------------------------------------------------------------------------


def _sort_tags(tags: Iterable[str]) -> list[str]:
    order = {APP_OBJC: 0, ENGINE: 1}
    return sorted(set(tags), key=lambda s: (order.get(s, 3 if s == UNKNOWN else 2), s))


def to_json(t: Tagger, source: dict) -> str:
    prog = t.prog
    records = []
    counts: dict[str, dict[str, int]] = {
        tag: {"functions": 0, "bytes": 0, "body_bytes": 0} for tag in (APP_OBJC, ENGINE, UNKNOWN)
    }
    rules: dict[str, int] = defaultdict(int)
    for addr in prog.starts:
        f = prog.functions[addr]
        a = t.assigned[addr]
        records.append(
            {
                "address": f.pointer,
                "size": f.span,
                "body": f.body,
                "tag": a.tag,
                "rule": a.rule,
                "reason": a.reason,
                "name": f.name,
            }
        )
        c = counts.setdefault(a.tag, {"functions": 0, "bytes": 0, "body_bytes": 0})
        c["functions"] += 1
        c["bytes"] += f.span
        c["body_bytes"] += f.body
        rules[a.rule] += 1
    libraries = {
        tag: {"evidence": t.evidence.get(tag, []), **counts[tag]}
        for tag in _sort_tags(counts)
        if tag.startswith(LIB_PREFIX)
    }
    header = {
        "schema": SCHEMA_VERSION,
        "tool": "mtport.libtags",
        "program": source,
        "counts": {k: counts[k] for k in _sort_tags(counts)},
        "rules": [
            {"rule": r, "description": d, "functions": rules.get(r, 0)}
            for r, d in RULE_DOCS.items()
        ],
        "libraries": libraries,
        "signatures": t.signatures,
        "vtables": [
            {
                "address": f"0x{v.address:X}",
                "class": v.class_name,
                "typeinfo": f"0x{v.typeinfo:X}",
                "functions": len(v.functions),
            }
            for v in t.vtables
        ],
        "coverage": {"function_starts": len(prog.image.function_starts), "tagged": len(records)},
    }
    return gc.dumps(header, "functions", records)


# -- API -----------------------------------------------------------------------------------


class FunctionTags:
    """Read-only view of ``work/re/function-tags.json``."""

    def __init__(self, doc: dict) -> None:
        self.doc = doc
        self.records = {_int(r["address"]) & ~1: r for r in doc["functions"]}
        self._index = armscan.FunctionIndex(self.records)

    @staticmethod
    def _key(addr: int | str) -> int:
        return (_int(addr) if isinstance(addr, str) else addr) & ~1

    def record(self, addr: int | str) -> dict | None:
        return self.records.get(self._key(addr))

    def tag_of(self, addr: int | str) -> str | None:
        """Tag of the function starting at ``addr`` (Thumb bit optional), or None."""
        r = self.record(addr)
        return r["tag"] if r else None

    def tag_containing(self, addr: int | str) -> str | None:
        """Tag of the function whose span holds the code address ``addr``."""
        key = self._key(addr)
        start = self._index.of(key)
        if start is None:
            return None
        r = self.records[start]
        return r["tag"] if key < start + r["size"] else None

    def functions_with(self, tag: str) -> list[str]:
        """Function pointers with ``tag`` (a ``lib:`` prefix alone matches every library)."""
        match = (lambda t: t.startswith(LIB_PREFIX)) if tag == LIB_PREFIX else tag.__eq__
        return [r["address"] for _, r in sorted(self.records.items()) if match(r["tag"])]

    def is_library(self, addr: int | str) -> bool:
        tag = self.tag_of(addr)
        return bool(tag and tag.startswith(LIB_PREFIX))

    def tags(self) -> list[str]:
        return list(self.doc["counts"])

    def counts(self) -> dict[str, dict[str, int]]:
        return self.doc["counts"]


TAG_DOCS = {
    APP_OBJC: "MapleThief ObjC shell: app class IMPs, main, code of their .m files",
    ENGINE: "MapleThief C/C++ game code",
    UNKNOWN: "no libtags rule applied",
}


def is_scheme_tag(name: str) -> bool:
    """Whether ``name`` is a tag of this scheme (and so managed by ``apply_tags.py``)."""
    return name in TAG_DOCS or (name.startswith(LIB_PREFIX) and len(name) > len(LIB_PREFIX))


def ghidra_plan(doc: dict) -> dict[int, str]:
    """Function entry (even VM address) -> the one Ghidra function tag it should carry."""
    return {_int(r["address"]) & ~1: r["tag"] for r in doc["functions"]}


def readback_json(doc: dict, held: dict[int, Iterable[str]]) -> str:
    """``READBACK_JSON``: the scheme tags Ghidra holds (``held``: even function entry -> its
    tag names), one record per function that has any, keyed by ``doc``'s function pointers
    (``gc.hexaddr`` of the entry for a function ``doc`` doesn't list). Deterministic."""
    pointers = {_int(r["address"]) & ~1: r["address"] for r in doc["functions"]}
    records = []
    for entry in sorted(held):
        tags = sorted(t for t in set(held[entry]) if is_scheme_tag(t))
        if tags:
            records.append({"address": pointers.get(entry, gc.hexaddr(entry)), "tags": tags})
    return gc.dumps({"schema": READBACK_SCHEMA, "count": len(records)}, "functions", records)


def readback_problems(doc: dict, readback: dict) -> list[str]:
    """Where the tags read back from Ghidra differ from ``doc`` (empty when they agree)."""
    if readback.get("schema") != READBACK_SCHEMA:
        return [f"readback schema {readback.get('schema')!r}, expected {READBACK_SCHEMA}"]
    plan = ghidra_plan(doc)
    held = {_int(r["address"]) & ~1: r["tags"] for r in readback["functions"]}
    problems = [
        f"{gc.hexaddr(a)}: Ghidra has {held.get(a, [])}, expected [{want!r}]"
        for a, want in sorted(plan.items())
        if held.get(a) != [want]
    ]
    problems += [
        f"{gc.hexaddr(a)}: Ghidra has {held[a]}, untagged"
        for a in sorted(held.keys() - plan.keys())
    ]
    return problems


def tag_comments(doc: dict) -> dict[str, str]:
    """Comment for each tag used in ``doc`` (Ghidra's function tag comment)."""
    out = {}
    for tag, c in doc["counts"].items():
        if not c["functions"]:
            continue
        if tag in TAG_DOCS:
            out[tag] = TAG_DOCS[tag]
        else:
            ev = doc["libraries"].get(tag, {}).get("evidence", [])
            first = ev[0]["reason"] if ev else ""
            out[tag] = f"statically linked library code (libtags #10): {first}"[:1000]
    return out


def load(path: str | Path | None = None) -> FunctionTags:
    p = Path(path) if path else Path(__file__).resolve().parents[2] / JSON_OUT
    return FunctionTags(json.loads(p.read_text(encoding="utf-8")))


# -- Markdown ------------------------------------------------------------------------------


FINDINGS = """## Findings

These hold for MapleThief 1.5 (armv7); the tables below are the evidence.

- **Chartboost 3.0.4** (`lib:chartboost`) is linked as one block at the end of `__text`,
  from `0xC2D60` (after the last app class, `HelpScrollKeyPadView`) to the end at `0xD991C`.
  Besides the 57 `CB*` classes it holds the `NSArray`/`NSDictionary`
  `(ATFunctionalCollectionExtensions)` categories (linked inside the block and sent only from
  Chartboost code), an SBJson copy renamed to `CBJson*` (`org.brautaset.*` error domains), and
  `CBBSON` on top of the MongoDB C driver's `bson.c`, whose `_bson_*` functions are still in
  the export trie. The rest are blocks, their copy/dispose helpers and static C helpers.
  Chartboost is compiled with optimization (e.g. `-[CBAPIConnection sendRequest:]` at
  `0xC5375` keeps values in registers); the game's own code is not (below).
- **`UIDevice(IdentifierAddition)` + `NSString(MD5Addition)`** (`lib:uidevice-identifier`)
  are not Chartboost's: they are linked with the app, between `GameCenterManager` and
  `TermsView`, and the only sender of `-uniqueGlobalDeviceIdentifier` is the game function
  `0x98865` (engine). Chartboost has its own MAC-address code, `+[CBIdentity cbma]`
  (`0xCA23D`), which shares the `"en0"` string with `-macaddress`.
- **crt1** (`lib:crt1`) contributes only `start` (`0x27E0`, ARM); `main` (`0x280D`) is the
  app's.
- **Apple sample code**: `MyGetOpenALAudioData` and `alBufferDataStaticProc` from
  `MyOpenALSupport.h` (oalTouch) are `lib:apple-openal-support`. `GameCenterManager` is
  adapted from Apple's GKTapper sample (its assert names
  `-[GameCenterManager callDelegate:withArg:error:]`, the build path is the game's
  `Classes/GameCenterManager.mm`); it stays `app-objc` because it is part of the ObjC shell
  that gets reimplemented anyway.
- **No zlib, libpng, stock Lua or libstdc++ code.** None of the strong markers below occur. The one
  `0xEDB88320` (crc32 polynomial) is inside a C++ constructor (`0x24EA1`: it stores a vtable
  pointer, then fills a 256-entry table), so the engine has its own crc32, but there is no
  inflate/deflate, no crc table in data and no adler32 modulus. PNGs are decoded by
  UIKit/CoreGraphics. The script VM classes `Lua`, `TFunc`, `Table`, `Symbol`, `Undump` and
  `Build` are game C++ classes with RTTI and vtables, with no Lua 4/5 strings, opcode names or
  `"\\033Lua"` signature, so they are `engine`. They are a C++ rewrite of the **Lua 2.4**
  interpreter (its builtin names such as `setfallback` remain at `0x117950`; see
  [script-vm.md](script-vm.md)), not the Lua C library. libstdc++ is only imported for the C++ ABI
  (`operator new/delete`, `__cxa_*`, typeinfo vtables, `std::terminate`): no container or
  string members, no `std::` RTTI and no STL error strings, so no STL instantiations can be
  identified. Header-only templates such as `std::min` would be indistinguishable from game
  code without symbols.
- **The game is compiled without optimization** (`-O0` style: every value is stored to and
  reloaded from the stack, constants are built as `movs` + `movt #0`, e.g. `0x24EA1`). Each
  app `.m` file therefore carries out-of-line copies of SDK struct-maker inlines (44-byte
  two-float and 76/108-byte four-float constructors, `CGPointMake`/`CGRectMake`-style); the
  `objc-file` rule keeps them `app-objc`.

"""


def _kib(n: int) -> str:
    return f"{n:,} ({n / 1024:.1f} KiB)"


def to_markdown(doc: dict) -> str:
    counts = doc["counts"]
    total_f = sum(c["functions"] for c in counts.values())
    total_b = sum(c["bytes"] for c in counts.values())
    src = doc["program"]
    lines = [
        "# Statically linked libraries and function tags",
        "",
        "<!-- Generated by `python3 -m mtport.libtags generate` (tools/mtport/libtags.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Every one of the {total_f:,} `LC_FUNCTION_STARTS` functions of the armv7 slice "
        f"(sha256 `{src.get('sha256', '?')}`) has exactly one tag. The machine-readable result "
        "is `work/re/function-tags.json` (git-ignored, regenerate with "
        "`python3 -m mtport.libtags generate` after `tools/ghidra/import`); "
        "`tools/ghidra/run-script apply_tags.py` puts the tags on the Ghidra functions. "
        "Rules and their order are documented in `tools/mtport/libtags.py`.",
        "",
        "*Bytes* is each function's span from its start to the next start (so the column sums "
        "to the `__text` bytes from the first start on, literal pools and padding included); "
        "*body* is the size of Ghidra's function body.",
        "",
        "## Counts per tag",
        "",
        "| Tag | Functions | Bytes | Body bytes | Share of bytes |",
        "|---|---|---|---|---|",
    ]
    for tag, c in counts.items():
        lines.append(
            f"| `{tag}` | {c['functions']:,} | {_kib(c['bytes'])} | {c['body_bytes']:,} "
            f"| {100 * c['bytes'] / total_b:.1f}% |"
        )
    lines += [
        f"| **total** | **{total_f:,}** | **{_kib(total_b)}** "
        f"| **{sum(c['body_bytes'] for c in counts.values()):,}** | 100% |",
        "",
        FINDINGS,
        "## Rules",
        "",
        "Rules run in this order; the first one that claims a function wins.",
        "",
        "| Rule | Functions | Meaning |",
        "|---|---|---|",
    ]
    for r in doc["rules"]:
        lines.append(f"| `{r['rule']}` | {r['functions']:,} | {r['description']} |")
    by_rule: dict[tuple[str, str], int] = defaultdict(int)
    for rec in doc["functions"]:
        by_rule[(rec["tag"], rec["rule"])] += 1
    lines += [
        "",
        "Functions per tag and rule:",
        "",
        "| Tag | " + " | ".join(f"`{r['rule']}`" for r in doc["rules"]) + " |",
        "|---|" + "---|" * len(doc["rules"]),
    ]
    for tag in counts:
        cells = [str(by_rule.get((tag, r["rule"]), "")) or "–" for r in doc["rules"]]
        lines.append(f"| `{tag}` | " + " | ".join(cells) + " |")
    lines += ["", "## Libraries", ""]
    for tag, lib in doc["libraries"].items():
        lines += [
            f"### `{tag}`",
            "",
            f"{lib['functions']:,} function{'s' if lib['functions'] != 1 else ''}, "
            f"{_kib(lib['bytes'])} bytes.",
            "",
            "| Address | Evidence |",
            "|---|---|",
        ]
        for ev in lib["evidence"]:
            lines.append(f"| `{ev['address']}` | {ev['reason']} |")
        lines.append("")
    lines += [
        "## Signature searches",
        "",
        "Markers searched for in every section of the slice (*bytes*) and as 32-bit constants "
        "built with `movw`/`movt` or held in `__text` literal pools (*const*). A strong marker "
        "tags the functions that contain or reference it; a weak one is only reported.",
        "",
        "| Library | Marker | Strong | Hits | Functions |",
        "|---|---|---|---|---|",
    ]
    for s in doc["signatures"]:
        hits = ", ".join(f"`{h}`" for h in s["hits"][:4]) + (" …" if len(s["hits"]) > 4 else "")
        funcs = ", ".join(f"`{h}`" for h in s["functions"][:4])
        if len(s["functions"]) > 4:
            funcs += " …"
        lines.append(
            f"| `{s['library']}` | {s['marker']} | {'yes' if s['strong'] else 'no'} "
            f"| {hits or 'none'} | {funcs or '–'} |"
        )
    lines.append("")
    return "\n".join(lines)


# -- command line --------------------------------------------------------------------------


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def generate(repo: Path, slice_path: Path | None = None) -> dict:
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    data = path.read_bytes()
    image = macho.MachO(data)
    prog = build_program(
        image,
        _read_json(repo / FUNCTIONS_JSON),
        _read_json(repo / XREFS_JSON),
        _read_json(repo / OBJC_JSON),
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
            counts = check_doc(_read_json(args.file), macho.load(sl))
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
