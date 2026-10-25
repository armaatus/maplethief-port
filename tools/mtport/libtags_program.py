"""The inputs of :mod:`mtport.libtags`: the tags and file names, the :class:`Program` built
from the slice, the Ghidra exports and the ObjC metadata (the reference graph, data objects),
and the vtables of #9 (:func:`rtti_vtables`). ``mtport.libtags`` re-exports every name
defined here.
"""

from __future__ import annotations

import struct
from collections import defaultdict
from dataclasses import dataclass, field

from . import armscan, macho
from . import ghidra_common as gc
from .names import parse_address

SCHEMA_VERSION = 1
JSON_OUT = "work/re/function-tags.json"
MARKDOWN_OUT = "docs/re/libraries.md"
FUNCTIONS_JSON = "work/ghidra/functions.json"
XREFS_JSON = "work/ghidra/xrefs.json"
OBJC_JSON = "work/re/objc.json"
OBJC_STEP = "python3 -m mtport.objc generate"
GENERATE_STEP = "python3 -m mtport.libtags generate"
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

# A data reference P -> G is attributed to the closest code-referenced address in
# [P - DATA_OBJECT_MAX, P] (armscan.containing_object; the same window as mtport.imports).
DATA_OBJECT_MAX = armscan.DATA_OBJECT_MAX

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
        a = armscan.containing_object(self._anchors, p, self.image, DATA_OBJECT_MAX)
        return set() if a is None else set(self.code_to_data[a])

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


def build_program(
    image: macho.MachO, functions_doc: dict, xrefs_doc: dict, objc_doc: dict
) -> Program:
    """Combine the slice with the Ghidra exports and the ObjC metadata."""
    starts = sorted(image.function_starts)
    text = image.section("__TEXT", "__text")
    ghidra = {parse_address(f["address"]) & ~1: f for f in functions_doc["functions"]}
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
        src_addr = parse_address(x["from"])
        src = (parse_address(x["function"]) & ~1) if x["function"] else prog.owner(src_addr)
        if src is not None and src not in funcs:
            src = prog.owner(src_addr)
        dst = parse_address(to)
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
