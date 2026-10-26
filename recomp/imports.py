"""Import ids, their classification and the generated import table (#36, ADR 0019).

Every one of the slice's imports (the undefined ``nlist`` symbols, ``Image.imports``) gets an
id: its index in ``nlist`` order. Generated code calls a function import as
``MT_CALL_IMPORT(id)`` through ``mt_import_table[id]``, which the runtime fills at start-up
by name from its HLE registry, with the default *unimplemented import* handler for the rest
(``runtime/include/maplethief/hle/imports.h``).

**Classification.** An import is a *function* or *data*:

- *function*: it has a ``__symbolstub1`` stub (called with ``bl``/``blx``/``b``), or it is
  one of :data:`ADDRESS_FUNCTIONS`, functions the game only reaches through a pointer
  (``___cxa_pure_virtual`` in vtables, the personality routines, ``_realloc``). Its guest
  code address is its first stub, or ``MT_IMPORT_THUNK_BASE + 4 * id`` when it has none; the
  generated thunk table maps that address to a host function that calls the import, so a
  guest pointer to an import is callable through ``mt_dispatch``.
- *data*: an ObjC class or metaclass object (``_OBJC_CLASS_$_*``, ``_OBJC_METACLASS_$_*``),
  or one of :data:`DATA_OBJECTS` (``_kCFBooleanTrue``, ``__NSConcreteStackBlock``,
  ``___stderrp``, ``_CGRectZero``, ...). The runtime allocates a zeroed guest object of the
  listed size and alignment at load time, lets the owning HLE module fill it, and binds every
  pointer to it. An ``address`` import that is in neither table is a :attr:`problems` entry
  (and gets :data:`UNKNOWN_DATA` so that the program still links).

``STRET`` marks the functions that return a structure through a hidden pointer in r0: the
``*_stret`` message senders and the C functions of :data:`STRUCT_RETURNS` (iOS returns every
structure larger than 4 bytes in memory, ADR 0019).

**Binds.** ``mt_import_binds`` lists every word the bind and lazy-bind streams of
``LC_DYLD_INFO`` write, with the import id and the addend: the runtime writes
``address of import + addend`` there (``mt_imports_apply_binds``). Lazy pointers are bound up
front like the others. The weak-definition stream (``operator new``/``delete`` coalescing,
ordinal 0) is left out: the same words have an ordinary bind too.

``work/gen/imports.c`` (:meth:`ImportTable.c_source`, ADR 0014 outputs; never committed)::

    mt_import_names[]   symbol of each id
    mt_import_table[]   the slots the runtime fills (storage only)
    mt_import_count     MT_IMPORT_COUNT
    mt_import_descs[]   name, library, MT_IMPORT_* flags, guest code address, data size/align
    mt_import_binds[]   {slot, id, addend}, ascending by slot; mt_import_bind_count
    mt_import_thunks[]  {guest code address, thunk}, ascending; mt_import_thunk_count
"""

from __future__ import annotations

from collections.abc import Iterable, Sequence
from dataclasses import dataclass

from mtport import imports as re_imports
from mtport import macho
from recomp.loader import Image

Import = re_imports.Import

# Guest code addresses of stub-less function imports: MT_IMPORT_THUNK_BASE + 4 * id
# (maplethief/recomp/imports.h). Nothing is mapped there; the address only keys the dispatch.
THUNK_BASE = 0xFFFE0000

# MT_IMPORT_* flags (maplethief/recomp/imports.h).
FUNCTION = 0x1
DATA = 0x2
WEAK = 0x4
STRET = 0x8

# Functions the game reaches only through a pointer (no stub): a vtable slot, the
# personality field of an SjLj function context, a function pointer in __data.
ADDRESS_FUNCTIONS = {
    "___cxa_pure_virtual": "C++ ABI: the slot of a pure virtual function in vtables",
    "___gxx_personality_sj0": "C++ SjLj personality routine, stored in function contexts",
    "___objc_personality_v0": "ObjC exception personality routine, stored in function contexts",
    "_realloc": "libc realloc, called through a pointer by bson (Chartboost)",
    "dyld_stub_binder": "dyld's lazy binder; only __stub_helper uses it, never recompiled code",
}

# ObjC class_t on 32-bit: isa, superclass, cache, vtable, data (5 pointers).
OBJC_CLASS = (20, 4, "ObjC class object (class_t: isa, superclass, cache, vtable, data)")

_PTR = 4  # a pointer-sized variable: the import is the variable, its value is the object
_NSSTRING = (_PTR, 4, "NSString * const variable")
_CFSTRING = (_PTR, 4, "CFStringRef const variable")

# Data symbols: (size in bytes, alignment, what the guest object is). The import's address is
# the object's address; for a variable, the HLE module stores the value it holds.
DATA_OBJECTS: dict[str, tuple[int, int, str]] = {
    # Constant object pointers (CF/Foundation/UIKit/QuartzCore/OpenGLES/AVFoundation).
    "_AVAudioSessionCategoryAmbient": _NSSTRING,
    "_NSLocaleCountryCode": _NSSTRING,
    "_NSLocaleLanguageCode": _NSSTRING,
    "_NSLocalizedDescriptionKey": _NSSTRING,
    "_NSURLAuthenticationMethodServerTrust": _NSSTRING,
    "_kCAFillModeForwards": _NSSTRING,
    "_kEAGLColorFormatRGBA8": _NSSTRING,
    "_kEAGLDrawablePropertyColorFormat": _NSSTRING,
    "_kEAGLDrawablePropertyRetainedBacking": _NSSTRING,
    "_kCFBundleIdentifierKey": _CFSTRING,
    "_kCFBundleVersionKey": _CFSTRING,
    "_kCFRunLoopCommonModes": _CFSTRING,
    "_kCFRunLoopDefaultMode": _CFSTRING,
    "_kCFAllocatorDefault": (_PTR, 4, "CFAllocatorRef const variable"),
    "_kCFBooleanFalse": (_PTR, 4, "CFBooleanRef const variable"),
    "_kCFBooleanTrue": (_PTR, 4, "CFBooleanRef const variable"),
    "_kCFNumberNaN": (_PTR, 4, "CFNumberRef const variable"),
    "_kCFNumberNegativeInfinity": (_PTR, 4, "CFNumberRef const variable"),
    "_kCFNumberPositiveInfinity": (_PTR, 4, "CFNumberRef const variable"),
    "___stderrp": (_PTR, 4, "FILE * variable (stderr)"),
    # CoreGraphics constants (CGFloat is float on armv7).
    "_CGRectZero": (16, 4, "CGRect {0, 0, 0, 0}"),
    "_CGRectNull": (16, 4, "CGRect {inf, inf, 0, 0}"),
    "_CGAffineTransformIdentity": (24, 4, "CGAffineTransform {1, 0, 0, 1, 0, 0}"),
    # Blocks runtime: the isa of stack and global blocks (libclosure: void *[32]).
    "__NSConcreteStackBlock": (128, 4, "block isa (libclosure void *_NSConcreteStackBlock[32])"),
    "__NSConcreteGlobalBlock": (128, 4, "block isa (libclosure void *_NSConcreteGlobalBlock[32])"),
    # CFString literals point their isa here: CF declares int[12]; it is a class object.
    "___CFConstantStringClassReference": (48, 4, "isa of CFString literals (CF: int[12])"),
    # libdispatch's main queue object; the game only passes its address to dispatch_*.
    "__dispatch_main_q": (64, 4, "dispatch_queue_s of the main queue (opaque; address only)"),
    # ObjC runtime: class structures store these addresses in their cache and vtable fields.
    "__objc_empty_cache": (12, 4, "struct objc_cache {mask, occupied, buckets[1]}"),
    "__objc_empty_vtable": (16, 4, "IMP table (opaque; only its address is stored)"),
    # C++ ABI type_info vtables: offset-to-top, typeinfo, virtual functions. type_info
    # objects point at +8 (bind addend 8).
    "__ZTVN10__cxxabiv117__class_type_infoE": (64, 4, "vtable of __class_type_info"),
    "__ZTVN10__cxxabiv120__si_class_type_infoE": (64, 4, "vtable of __si_class_type_info"),
    "__ZTVN10__cxxabiv121__vmi_class_type_infoE": (64, 4, "vtable of __vmi_class_type_info"),
}

# Size and alignment for an address import that no table lists (always a problem).
UNKNOWN_DATA = (64, 16, "unknown data symbol")

# C functions that return a structure larger than 4 bytes, so through a hidden pointer in r0.
STRUCT_RETURNS = {
    "_CGRectInset": "CGRect",
    "_CGAffineTransformMakeRotation": "CGAffineTransform",
    "_CATransform3DRotate": "CATransform3D",
    "_CATransform3DScale": "CATransform3D",
    "_CATransform3DTranslate": "CATransform3D",
}

# Bind kinds of LC_DYLD_INFO that mt_import_binds carries (not "weak", see the docstring).
BIND_KINDS = ("bind", "lazy")


@dataclass(frozen=True)
class ImportEntry:
    """One import with its id, classification and guest placement."""

    id: int
    name: str
    library: str
    flags: int
    stubs: tuple[int, ...]  # __symbolstub1 stubs, ascending
    addr: int  # function: guest code address (first stub or thunk address); data: 0
    size: int  # data: object size in bytes; function: 0
    align: int  # data: alignment (a power of two); function: 0
    what: str  # why it is classified this way (a C comment in imports.c)

    @property
    def function(self) -> bool:
        return bool(self.flags & FUNCTION)

    @property
    def data(self) -> bool:
        return bool(self.flags & DATA)


@dataclass(frozen=True)
class ImportBind:
    """A word the runtime writes at load time: ``address of import id + addend``."""

    slot: int
    id: int
    addend: int


def classify(imp: Import, index: int) -> tuple[ImportEntry, str | None]:
    """The entry for ``imp`` with id ``index``, and a problem if it is not classified."""
    weak = WEAK if imp.weak else 0
    stubs = tuple(sorted(imp.stubs))
    if stubs or imp.name in ADDRESS_FUNCTIONS:
        flags = FUNCTION | weak
        if imp.name.endswith("_stret") or imp.name in STRUCT_RETURNS:
            flags |= STRET
        addr = stubs[0] if stubs else THUNK_BASE + 4 * index
        what = "stub" if stubs else f"address-taken function: {ADDRESS_FUNCTIONS[imp.name]}"
        if imp.name in STRUCT_RETURNS:
            what += f"; returns {STRUCT_RETURNS[imp.name]} through r0"
        return ImportEntry(index, imp.name, imp.library, flags, stubs, addr, 0, 0, what), None
    problem = None
    if imp.kind in ("class", "metaclass"):
        size, align, what = OBJC_CLASS
    elif imp.name in DATA_OBJECTS:
        size, align, what = DATA_OBJECTS[imp.name]
    else:
        size, align, what = UNKNOWN_DATA
        problem = f"{imp.name}: address import with no known size (add it to DATA_OBJECTS)"
    entry = ImportEntry(index, imp.name, imp.library, DATA | weak, (), 0, size, align, what)
    return entry, problem


@dataclass(frozen=True)
class ImportTable:
    """Every import by id, the stub -> id map the lifter uses, and the load-time binds."""

    entries: tuple[ImportEntry, ...]
    by_stub: dict[int, int]
    binds: tuple[ImportBind, ...]
    problems: tuple[str, ...]

    @classmethod
    def from_image(cls, image: Image) -> ImportTable:
        return cls.from_imports(image.imports, image.binds)

    @classmethod
    def from_imports(
        cls, imports: Sequence[Import], binds: Iterable[macho.Bind] = ()
    ) -> ImportTable:
        """From ``imports`` in id order and the dyld binds (``Image.binds``)."""
        entries, problems = [], []
        for i, imp in enumerate(imports):
            entry, problem = classify(imp, i)
            entries.append(entry)
            if problem:
                problems.append(problem)
        by_name = {e.name: e.id for e in entries}
        by_stub = {s: e.id for e in entries for s in e.stubs}
        out: dict[int, ImportBind] = {}
        for b in binds:
            if b.kind not in BIND_KINDS:
                continue
            if b.symbol not in by_name:
                problems.append(f"bind at 0x{b.address:X}: {b.symbol} is not an import")
                continue
            bind = ImportBind(b.address, by_name[b.symbol], b.addend)
            if out.setdefault(b.address, bind) != bind:
                problems.append(f"bind at 0x{b.address:X}: bound to two different values")
        table_binds = tuple(out[a] for a in sorted(out))
        return cls(tuple(entries), by_stub, table_binds, tuple(problems))

    @property
    def names(self) -> tuple[str, ...]:
        """``names[id]`` is the import's symbol."""
        return tuple(e.name for e in self.entries)

    def at(self, addr: int) -> tuple[int, str] | None:
        """``(id, symbol)`` of the import whose stub is at ``addr`` (Thumb bit ignored)."""
        i = self.by_stub.get(addr & ~1)
        return None if i is None else (i, self.entries[i].name)

    def named(self, name: str) -> ImportEntry | None:
        for e in self.entries:
            if e.name == name:
                return e
        return None

    @property
    def thunks(self) -> tuple[tuple[int, int], ...]:
        """``(guest code address, import id)`` of every function import, ascending: each stub,
        and the thunk address of a stub-less one. The dispatch table (#35) maps these."""
        pairs = [(s, e.id) for e in self.entries for s in e.stubs]
        pairs += [(e.addr, e.id) for e in self.entries if e.function and not e.stubs]
        return tuple(sorted(pairs))

    def counts(self) -> dict[str, int]:
        es = self.entries
        return {
            "imports": len(es),
            "functions": sum(e.function for e in es),
            "functions with a stub": sum(bool(e.stubs) for e in es),
            "address-taken functions": sum(e.function and not e.stubs for e in es),
            "data": sum(e.data for e in es),
            "objc classes": sum(e.data and e.what == OBJC_CLASS[2] for e in es),
            "stret": sum(bool(e.flags & STRET) for e in es),
            "weak": sum(bool(e.flags & WEAK) for e in es),
            "binds": len(self.binds),
            "thunks": len(self.thunks),
            "problems": len(self.problems),
        }

    def c_source(self) -> str:
        """``work/gen/imports.c``: names, descriptors, binds, thunks and the table storage."""
        lines = [
            "/* Generated by recomp (ADR 0014, ADR 0019): the import table. Never edit; never",
            " * commit. The runtime fills mt_import_table at start-up (maplethief/hle/imports.h).",
            " */",
            '#include "mt_gen.h"',
            "",
        ]
        n = max(1, len(self.entries))  # C has no empty arrays
        lines += _array(
            f"const char *const mt_import_names[{n}]",
            [f"{c_string(e.name)}, /* {e.id} */" for e in self.entries],
            "0",
        )
        lines += [
            f"mt_import_fn mt_import_table[{n}];",
            f"const uint32_t mt_import_count = {len(self.entries)}u;",
            "",
        ]
        lines += _array(
            f"const mt_import_desc mt_import_descs[{n}]",
            [
                f"{{{c_string(e.name)}, {c_string(e.library)}, {_flags(e.flags)}, "
                f"0x{e.addr:08X}u, {e.size}u, {e.align}u}}, /* {e.id}: {_comment(e.what)} */"
                for e in self.entries
            ],
            "{0, 0, 0u, 0u, 0u, 0u}",
        )
        lines += _array(
            f"const mt_import_bind mt_import_binds[{max(1, len(self.binds))}]",
            [f"{{0x{b.slot:08X}u, {b.id}u, {b.addend}}}," for b in self.binds],
            "{0u, 0u, 0}",
        )
        lines += [f"const uint32_t mt_import_bind_count = {len(self.binds)}u;", ""]
        thunks = self.thunks
        for i in sorted({i for _, i in thunks}):
            lines.append(f"static void {thunk_name(i)}(mt_cpu *MT_RESTRICT cpu) {{")
            lines.append(f"    MT_CALL_IMPORT({i}u); /* {_comment(self.entries[i].name)} */")
            lines.append("}")
        lines.append("")
        lines += _array(
            f"const mt_fn_entry mt_import_thunks[{max(1, len(thunks))}]",
            [f"{{0x{a:08X}u, {thunk_name(i)}}}," for a, i in thunks],
            "{0u, 0}",
        )
        lines += [f"const uint32_t mt_import_thunk_count = {len(thunks)}u;", ""]
        return "\n".join(lines)


def thunk_name(import_id: int) -> str:
    return f"mt_import_thunk_{import_id}"


def c_string(text: str) -> str:
    """``text`` as a C string literal; symbol names come from the binary (untrusted)."""
    out = []
    for ch in text:
        o = ord(ch)
        if ch in '"\\?':  # '?' as well: no trigraphs
            out.append("\\" + ch)
        elif 0x20 <= o < 0x7F:
            out.append(ch)
        else:
            out += [f"\\{b:03o}" for b in ch.encode("utf-8")]
    return '"' + "".join(out) + '"'


def _comment(text: str) -> str:
    return "".join(c if 0x20 <= ord(c) < 0x7F else "?" for c in text).replace("*/", "*_/")


_FLAG_NAMES = ((FUNCTION, "FUNCTION"), (DATA, "DATA"), (WEAK, "WEAK"), (STRET, "STRET"))


def _flags(flags: int) -> str:
    names = [f"MT_IMPORT_{n}" for bit, n in _FLAG_NAMES if flags & bit]
    return " | ".join(names) or "0u"


def _array(decl: str, rows: list[str], empty: str) -> list[str]:
    """A C array definition; C has no empty arrays, so an empty one gets one ``empty`` row
    (its count says 0)."""
    body = [f"    {r}" for r in rows] or [f"    {empty},"]
    return [f"{decl} = {{", *body, "};", ""]
