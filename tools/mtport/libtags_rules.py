"""The tagging rules of :mod:`mtport.libtags`, strongest first (see its module docstring),
the library markers they search for and the coverage check. ``mtport.libtags`` re-exports
every name defined here.
"""

from __future__ import annotations

import bisect
import re
import struct
from collections import defaultdict
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field

from mtport.libtags_program import (
    APP_OBJC,
    CATEGORY_LIBRARIES,
    CHARTBOOST,
    CRT1,
    ENGINE,
    LIB_PREFIX,
    RULE_DOCS,
    SCHEMA_VERSION,
    STL,
    UNKNOWN,
    Program,
    VTable,
    rtti_vtables,
)

from . import armscan, macho
from . import ghidra_common as gc
from .names import parse_address
from .objc import is_chartboost

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
                addr = parse_address(m["imp"]) & ~1
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
            (parse_address(m["imp"]) & ~1, kind, m["selector"])
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
            out |= prog.code_to_data.get(parse_address(ref["address"]), set())
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
                out.setdefault(parse_address(m["imp"]) & ~1, cls["name"])
    for cat in objc["categories"]:
        for m in cat["instance_methods"] + cat["class_methods"]:
            if m["imp"]:
                out.setdefault(
                    parse_address(m["imp"]) & ~1, f"{cat['class']['name']}({cat['name']})"
                )
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
