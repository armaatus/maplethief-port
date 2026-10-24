"""Objective-C 2 runtime metadata of a thin 32-bit Mach-O image (the armv7 slice).

Walks the metadata the compiler emits into ``__DATA`` and turns it into plain data: classes
(``class_t`` → ``class_ro_t``) with their metaclasses (class methods), ivars, properties and
adopted protocols, categories, protocols, and the ``__objc_selrefs`` / ``__objc_classrefs`` /
``__objc_superrefs`` tables. Classes that live in a system framework (an imported superclass,
a category's target, a classref) are stored as 0 and bound by dyld, so their names come from
the image's bind info (:attr:`mtport.macho.MachO.binds`).

Layouts (32-bit, every field a u32 unless noted; see objc4 ``objc-runtime-new.h``)::

    class_t          { isa, superclass, cache, vtable, data }    data & ~3 → class_ro_t
    class_ro_t       { flags, instanceStart, instanceSize, ivarLayout, name, baseMethods,
                       baseProtocols, ivars, weakIvarLayout, baseProperties }
    method_list_t    { entsizeAndFlags, count } then count x method_t { name, types, imp }
    ivar_list_t      { entsize, count } then count x ivar_t { offset*, name, type,
                       alignment (log2), size }
    property_list_t  { entsize, count } then count x property_t { name, attributes }
    protocol_list_t  { count } then count x protocol_t*
    protocol_t       { isa, name, protocols, instanceMethods, classMethods,
                       optionalInstanceMethods, optionalClassMethods, instanceProperties,
                       size, flags }
    category_t       { name, cls, instanceMethods, classMethods, protocols,
                       instanceProperties }

The low two bits of ``entsizeAndFlags`` are flags and are masked off. A method's ``imp`` keeps
the Thumb bit. The input is untrusted data: every read goes through the bounds-checked
:mod:`mtport.macho` accessors, and list counts are checked against the bytes they need.

Command line::

    python3 -m mtport.objc generate [--repo DIR] [--slice PATH]
        # writes work/re/objc.json, work/re/names/objc.json and docs/re/objc.md
    python3 -m mtport.objc json SLICE      # the objc.json document on stdout
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from mtport import macho

RO_META = 0x1
RO_ROOT = 0x2
RO_HAS_CXX_STRUCTORS = 0x4
_RO_FLAG_NAMES = {RO_META: "META", RO_ROOT: "ROOT", RO_HAS_CXX_STRUCTORS: "HAS_CXX_STRUCTORS"}

CLASS_SYMBOL = "_OBJC_CLASS_$_"
METACLASS_SYMBOL = "_OBJC_METACLASS_$_"

SCHEMA_VERSION = 1
JSON_OUT = "work/re/objc.json"
NAMES_OUT = "work/re/names/objc.json"
MARKDOWN_OUT = "docs/re/objc.md"

# A list header claiming more entries than this is treated as corrupt.
_MAX_LIST = 0x10000


class ObjCError(macho.MachOError):
    """The ObjC metadata is malformed."""


@dataclass(frozen=True)
class Method:
    selector: str
    types: str
    imp: int  # Thumb bit kept; 0 for protocol methods


@dataclass(frozen=True)
class Ivar:
    name: str
    type: str
    offset: int | None  # value of the ivar offset variable; None if it has none
    offset_address: int  # address of that variable (in __objc_ivar), 0 if none
    alignment: int  # log2 of the alignment in bytes
    size: int


@dataclass(frozen=True)
class Property:
    name: str
    attributes: str


@dataclass(frozen=True)
class ClassRef:
    """A class pointer: an image class (``address`` set) or an imported one (``address`` None)."""

    name: str
    address: int | None
    meta: bool = False


@dataclass
class Protocol:
    address: int
    name: str
    protocols: list[str] = field(default_factory=list)
    instance_methods: list[Method] = field(default_factory=list)
    class_methods: list[Method] = field(default_factory=list)
    optional_instance_methods: list[Method] = field(default_factory=list)
    optional_class_methods: list[Method] = field(default_factory=list)
    properties: list[Property] = field(default_factory=list)


@dataclass
class ObjCClass:
    address: int
    name: str
    metaclass: int
    superclass: ClassRef | None
    flags: int
    instance_start: int
    instance_size: int
    meta_flags: int = 0
    protocols: list[str] = field(default_factory=list)
    ivars: list[Ivar] = field(default_factory=list)
    instance_methods: list[Method] = field(default_factory=list)
    class_methods: list[Method] = field(default_factory=list)
    properties: list[Property] = field(default_factory=list)
    class_properties: list[Property] = field(default_factory=list)

    @property
    def group(self) -> str:
        return "chartboost" if is_chartboost(self.name) else "app"


@dataclass
class Category:
    address: int
    name: str
    cls: ClassRef | None
    instance_methods: list[Method] = field(default_factory=list)
    class_methods: list[Method] = field(default_factory=list)
    protocols: list[str] = field(default_factory=list)
    properties: list[Property] = field(default_factory=list)

    @property
    def class_name(self) -> str:
        return self.cls.name if self.cls else "?"


@dataclass
class ObjCImage:
    classes: list[ObjCClass]
    categories: list[Category]
    protocols: list[Protocol]
    selrefs: list[tuple[int, str]]
    classrefs: list[tuple[int, ClassRef | None]]
    superrefs: list[tuple[int, ClassRef | None]]
    protorefs: list[tuple[int, str]]


def is_chartboost(name: str) -> bool:
    """The Chartboost SDK's classes: ``Chartboost`` and the ``CB`` prefix."""
    return name == "Chartboost" or (name.startswith("CB") and name[2:3].isupper())


def hexaddr(value: int) -> str:
    return f"0x{value:X}"


class Parser:
    """Parses the ObjC metadata of one :class:`mtport.macho.MachO`."""

    def __init__(self, image: macho.MachO) -> None:
        self.m = image
        self._names: dict[int, str] = {}
        self._protocols: dict[int, Protocol] = {}
        self._protocol_order: list[int] = []

    # -- primitives ----------------------------------------------------------------------

    def words(self, addr: int, count: int) -> tuple[int, ...]:
        return struct.unpack(f"<{count}I", self.m.read(addr, 4 * count))

    def string(self, addr: int) -> str:
        if addr == 0:
            return ""
        return self.m.cstring(addr).decode("utf-8", "replace")

    def pointer_list(self, section: tuple[str, str]) -> list[tuple[int, int]]:
        """``(slot address, pointer)`` for each u32 of a ``__DATA`` pointer section."""
        s = self.m.section(*section)
        if s is None:
            return []
        if s.size % 4:
            raise ObjCError(f"{section[1]} size 0x{s.size:X} is not a multiple of 4")
        values = self.words(s.addr, s.size // 4) if s.size else ()
        return [(s.addr + 4 * i, v) for i, v in enumerate(values)]

    def _list_header(self, addr: int, entsize_min: int, what: str) -> tuple[int, int]:
        entsize_flags, count = self.words(addr, 2)
        entsize = entsize_flags & 0xFFFC
        if entsize < entsize_min or count > _MAX_LIST:
            raise ObjCError(f"{what} at 0x{addr:X}: entsize {entsize}, count {count}")
        self.m.read(addr + 8, entsize * count)  # the whole list must be file-backed
        return entsize, count

    # -- lists ---------------------------------------------------------------------------

    def methods(self, addr: int) -> list[Method]:
        if addr == 0:
            return []
        entsize, count = self._list_header(addr, 12, "method_list_t")
        out = []
        for i in range(count):
            name, types, imp = self.words(addr + 8 + i * entsize, 3)
            out.append(Method(self.string(name), self.string(types), imp))
        return out

    def ivars(self, addr: int) -> list[Ivar]:
        if addr == 0:
            return []
        entsize, count = self._list_header(addr, 20, "ivar_list_t")
        out = []
        for i in range(count):
            offset_ptr, name, type_, alignment, size = self.words(addr + 8 + i * entsize, 5)
            offset = self.m.u32(offset_ptr) if offset_ptr else None
            out.append(
                Ivar(self.string(name), self.string(type_), offset, offset_ptr, alignment, size)
            )
        return out

    def properties(self, addr: int) -> list[Property]:
        if addr == 0:
            return []
        entsize, count = self._list_header(addr, 8, "property_list_t")
        out = []
        for i in range(count):
            name, attributes = self.words(addr + 8 + i * entsize, 2)
            out.append(Property(self.string(name), self.string(attributes)))
        return out

    def protocol_list(self, addr: int) -> list[str]:
        if addr == 0:
            return []
        (count,) = self.words(addr, 1)
        if count > _MAX_LIST:
            raise ObjCError(f"protocol_list_t at 0x{addr:X}: count {count}")
        return [self.protocol(p).name for p in self.words(addr + 4, count)] if count else []

    # -- objects -------------------------------------------------------------------------

    def protocol(self, addr: int) -> Protocol:
        if addr in self._protocols:
            return self._protocols[addr]
        if addr == 0:
            raise ObjCError("null protocol pointer")
        _, name, protocols, inst, cls, opt_inst, opt_cls, props = self.words(addr, 8)
        proto = Protocol(addr, self.string(name))
        self._protocols[addr] = proto  # before recursing: protocol graphs may share nodes
        self._protocol_order.append(addr)
        proto.protocols = self.protocol_list(protocols)
        proto.instance_methods = self.methods(inst)
        proto.class_methods = self.methods(cls)
        proto.optional_instance_methods = self.methods(opt_inst)
        proto.optional_class_methods = self.methods(opt_cls)
        proto.properties = self.properties(props)
        return proto

    def class_ro(self, cls_addr: int) -> tuple[int, ...]:
        data = self.words(cls_addr, 5)[4]
        return self.words(data & ~3, 10)

    def class_name(self, cls_addr: int) -> str:
        if cls_addr not in self._names:
            self._names[cls_addr] = self.string(self.class_ro(cls_addr)[4])
        return self._names[cls_addr]

    def class_ref(self, slot: int, value: int | None = None) -> ClassRef | None:
        """The class a pointer slot refers to: the image class it holds, or what dyld binds."""
        if value is None:
            value = self.m.u32(slot)
        if value:
            meta = bool(self.class_ro(value)[0] & RO_META)
            return ClassRef(self.class_name(value), value, meta)
        symbol = self.m.bound_symbol(slot)
        if symbol is None:
            return None
        for prefix, meta in ((CLASS_SYMBOL, False), (METACLASS_SYMBOL, True)):
            if symbol.startswith(prefix):
                return ClassRef(symbol[len(prefix) :], None, meta)
        return ClassRef(symbol, None)

    def objc_class(self, addr: int) -> ObjCClass:
        isa = self.words(addr, 1)[0]
        flags, start, size, _, name, methods, protocols, ivars, _, props = self.class_ro(addr)
        if flags & RO_META:
            raise ObjCError(f"class 0x{addr:X} in __objc_classlist is a metaclass")
        cls = ObjCClass(
            address=addr,
            name=self.string(name),
            metaclass=isa,
            superclass=self.class_ref(addr + 4),
            flags=flags,
            instance_start=start,
            instance_size=size,
        )
        self._names[addr] = cls.name
        cls.protocols = self.protocol_list(protocols)
        cls.ivars = self.ivars(ivars)
        cls.instance_methods = self.methods(methods)
        cls.properties = self.properties(props)
        if isa:
            meta = self.class_ro(isa)
            if not meta[0] & RO_META:
                raise ObjCError(f"isa 0x{isa:X} of {cls.name} is not a metaclass")
            cls.meta_flags = meta[0]
            cls.class_methods = self.methods(meta[5])
            cls.class_properties = self.properties(meta[9])
        return cls

    def category(self, addr: int) -> Category:
        name, _, inst, cls_methods, protocols, props = self.words(addr, 6)
        return Category(
            address=addr,
            name=self.string(name),
            cls=self.class_ref(addr + 4),
            instance_methods=self.methods(inst),
            class_methods=self.methods(cls_methods),
            protocols=self.protocol_list(protocols),
            properties=self.properties(props),
        )

    def parse(self) -> ObjCImage:
        classes = [self.objc_class(p) for _, p in self.pointer_list(("__DATA", "__objc_classlist"))]
        categories = [self.category(p) for _, p in self.pointer_list(("__DATA", "__objc_catlist"))]
        listed = [self.protocol(p) for _, p in self.pointer_list(("__DATA", "__objc_protolist"))]
        protorefs = [
            (slot, self.protocol(p).name)
            for slot, p in self.pointer_list(("__DATA", "__objc_protorefs"))
        ]
        # __objc_protolist order first, then protocols only reached through references.
        order = [p.address for p in listed] + self._protocol_order
        protocols = [self._protocols[a] for a in dict.fromkeys(order)]
        selrefs = [
            (slot, self.string(p)) for slot, p in self.pointer_list(("__DATA", "__objc_selrefs"))
        ]
        classrefs = [
            (slot, self.class_ref(slot, p))
            for slot, p in self.pointer_list(("__DATA", "__objc_classrefs"))
        ]
        superrefs = [
            (slot, self.class_ref(slot, p))
            for slot, p in self.pointer_list(("__DATA", "__objc_superrefs"))
        ]
        return ObjCImage(classes, categories, protocols, selrefs, classrefs, superrefs, protorefs)


def parse(image: macho.MachO) -> ObjCImage:
    return Parser(image).parse()


# -- JSON ----------------------------------------------------------------------------------


def _methods_json(methods: list[Method], with_imp: bool = True) -> list[dict]:
    out = []
    for m in methods:
        entry = {"selector": m.selector, "types": m.types}
        if with_imp:
            entry["imp"] = hexaddr(m.imp)
        out.append(entry)
    return out


def _props_json(props: list[Property]) -> list[dict]:
    return [{"name": p.name, "attributes": p.attributes} for p in props]


def _ref_json(ref: ClassRef | None) -> dict | None:
    if ref is None:
        return None
    return {
        "name": ref.name,
        "address": None if ref.address is None else hexaddr(ref.address),
        "imported": ref.address is None,
        "meta": ref.meta,
    }


def flag_names(flags: int) -> list[str]:
    names = [n for bit, n in _RO_FLAG_NAMES.items() if flags & bit]
    rest = flags & ~sum(_RO_FLAG_NAMES)
    return names + ([hexaddr(rest)] if rest else [])


def to_json(objc: ObjCImage, source: dict | None = None) -> dict:
    """The ``work/re/objc.json`` document (deterministic for a given image)."""
    classes = []
    for c in objc.classes:
        classes.append(
            {
                "name": c.name,
                "group": c.group,
                "address": hexaddr(c.address),
                "metaclass": hexaddr(c.metaclass),
                "superclass": _ref_json(c.superclass),
                "flags": flag_names(c.flags),
                "meta_flags": flag_names(c.meta_flags),
                "instance_start": c.instance_start,
                "instance_size": c.instance_size,
                "protocols": c.protocols,
                "ivars": [
                    {
                        "name": i.name,
                        "type": i.type,
                        "offset": i.offset,
                        "offset_address": hexaddr(i.offset_address),
                        "alignment_log2": i.alignment,
                        "size": i.size,
                    }
                    for i in c.ivars
                ],
                "properties": _props_json(c.properties),
                "class_properties": _props_json(c.class_properties),
                "instance_methods": _methods_json(c.instance_methods),
                "class_methods": _methods_json(c.class_methods),
            }
        )
    categories = [
        {
            "name": cat.name,
            "class": _ref_json(cat.cls),
            "address": hexaddr(cat.address),
            "protocols": cat.protocols,
            "properties": _props_json(cat.properties),
            "instance_methods": _methods_json(cat.instance_methods),
            "class_methods": _methods_json(cat.class_methods),
        }
        for cat in objc.categories
    ]
    protocols = [
        {
            "name": p.name,
            "address": hexaddr(p.address),
            "protocols": p.protocols,
            "instance_methods": _methods_json(p.instance_methods, False),
            "class_methods": _methods_json(p.class_methods, False),
            "optional_instance_methods": _methods_json(p.optional_instance_methods, False),
            "optional_class_methods": _methods_json(p.optional_class_methods, False),
            "properties": _props_json(p.properties),
        }
        for p in objc.protocols
    ]

    def refs(table):
        return [{"address": hexaddr(slot), "class": _ref_json(ref)} for slot, ref in table]

    doc = {
        "tool": "mtport.objc",
        "schema_version": SCHEMA_VERSION,
        "source": source or {},
        "counts": counts(objc),
        "classes": classes,
        "categories": categories,
        "protocols": protocols,
        "selrefs": [{"address": hexaddr(a), "selector": s} for a, s in objc.selrefs],
        "classrefs": refs(objc.classrefs),
        "superrefs": refs(objc.superrefs),
        "protorefs": [{"address": hexaddr(a), "protocol": s} for a, s in objc.protorefs],
    }
    return doc


def counts(objc: ObjCImage) -> dict:
    def methods(classes):
        return sum(len(c.instance_methods) + len(c.class_methods) for c in classes)

    app = [c for c in objc.classes if c.group == "app"]
    cb = [c for c in objc.classes if c.group == "chartboost"]
    return {
        "classes": len(objc.classes),
        "app_classes": len(app),
        "chartboost_classes": len(cb),
        "categories": len(objc.categories),
        "protocols": len(objc.protocols),
        "app_methods": methods(app),
        "chartboost_methods": methods(cb),
        "category_methods": methods(objc.categories),
        "imps": len(imp_names(objc)),
        "selrefs": len(objc.selrefs),
        "classrefs": len(objc.classrefs),
        "superrefs": len(objc.superrefs),
    }


# -- names (the Ghidra naming contract, see #7) ---------------------------------------------


def method_name(owner: str, method: Method, class_method: bool) -> str:
    return f"{'+' if class_method else '-'}[{owner} {method.selector}]"


def _all_imps(objc: ObjCImage):
    for c in objc.classes:
        for m in c.instance_methods:
            yield method_name(c.name, m, False), m
        for m in c.class_methods:
            yield method_name(c.name, m, True), m
    for cat in objc.categories:
        owner = f"{cat.class_name}({cat.name})"
        for m in cat.instance_methods:
            yield method_name(owner, m, False), m
        for m in cat.class_methods:
            yield method_name(owner, m, True), m


def imp_names(objc: ObjCImage) -> dict[int, list[tuple[str, Method]]]:
    """IMP address → every ``(name, method)`` implemented there, in metadata order."""
    out: dict[int, list[tuple[str, Method]]] = {}
    for name, m in _all_imps(objc):
        if m.imp:
            out.setdefault(m.imp, []).append((name, m))
    return out


def names_json(objc: ObjCImage) -> dict:
    """``work/re/names/objc.json``: one ``kind: function`` entry per IMP, sorted by address."""
    names = []
    for imp, owners in sorted(imp_names(objc).items()):
        name, method = owners[0]
        comment = f"types {method.types}"
        if len(owners) > 1:
            comment += "; also " + ", ".join(n for n, _ in owners[1:])
        names.append(
            {"address": hexaddr(imp), "name": name, "kind": "function", "comment": comment}
        )
    doc = {"source": "objc", "names": names}
    check_names(doc)
    return doc


_NAME_KEYS = {"address", "name", "kind"}
_NAME_OPTIONAL = {"comment"}


def check_names(doc: object) -> None:
    """Raise ``ValueError`` unless ``doc`` follows the names contract (see #7).

    ``{"source": str, "names": [{"address": "0x2A3C1", "name": str, "kind": "function",
    "comment": str (optional)}]}``: upper-case hex VM addresses (Thumb bit kept for code), one
    entry per address, non-empty names without line breaks.
    """
    if not isinstance(doc, dict) or set(doc) != {"source", "names"}:
        raise ValueError("names document must have exactly the keys 'source' and 'names'")
    if not isinstance(doc["source"], str) or not doc["source"]:
        raise ValueError("'source' must be a non-empty string")
    if not isinstance(doc["names"], list):
        raise ValueError("'names' must be a list")
    seen: set[int] = set()
    for i, entry in enumerate(doc["names"]):
        where = f"names[{i}]"
        if not isinstance(entry, dict):
            raise ValueError(f"{where} is not an object")
        keys = set(entry)
        if not _NAME_KEYS <= keys <= _NAME_KEYS | _NAME_OPTIONAL:
            raise ValueError(f"{where} has keys {sorted(keys)}")
        addr = entry["address"]
        if not (
            isinstance(addr, str)
            and addr.startswith("0x")
            and len(addr) > 2
            and all(ch in "0123456789ABCDEF" for ch in addr[2:])
            and (len(addr) == 3 or addr[2] != "0")
        ):
            raise ValueError(f"{where}: address {addr!r} is not upper-case hex like 0x2A3C1")
        value = int(addr, 16)
        if value in seen:
            raise ValueError(f"{where}: duplicate address {addr}")
        seen.add(value)
        name = entry["name"]
        if not isinstance(name, str) or not name.strip() or any(c in name for c in "\r\n\0"):
            raise ValueError(f"{where}: bad name {name!r}")
        if entry["kind"] != "function":
            raise ValueError(f"{where}: kind {entry['kind']!r} is not 'function'")
        if "comment" in entry and not isinstance(entry["comment"], str):
            raise ValueError(f"{where}: comment must be a string")


# -- Markdown -------------------------------------------------------------------------------


def _code(text: str) -> str:
    text = text.replace("|", "\\|")
    return f"`` {text} ``" if "`" in text else f"`{text}`"


def _super_text(ref: ClassRef | None) -> str:
    return "–" if ref is None else _code(ref.name)


def _imp_range(methods: list[Method]) -> str:
    imps = [m.imp for m in methods if m.imp]
    return f"`{hexaddr(min(imps))}`–`{hexaddr(max(imps))}`" if imps else "–"


def _method_table(lines: list[str], title: str, methods: list[Method]) -> None:
    if not methods:
        return
    lines += [f"{title} ({len(methods)}):", "", "| IMP | Selector | Types |", "|---|---|---|"]
    for m in methods:
        lines.append(f"| `{hexaddr(m.imp)}` | {_code(m.selector)} | {_code(m.types)} |")
    lines.append("")


def _props_table(lines: list[str], title: str, props: list[Property]) -> None:
    if not props:
        return
    lines += [f"{title} ({len(props)}):", "", "| Name | Attributes |", "|---|---|"]
    for p in props:
        lines.append(f"| {_code(p.name)} | {_code(p.attributes)} |")
    lines.append("")


def to_markdown(objc: ObjCImage, source: dict | None = None) -> str:
    c = counts(objc)
    app = [k for k in objc.classes if k.group == "app"]
    cb = [k for k in objc.classes if k.group == "chartboost"]
    src = source or {}
    lines = [
        "# Objective-C metadata",
        "",
        "<!-- Generated by `python3 -m mtport.objc generate` (tools/mtport/objc.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Recovered from the `{src.get('arch', 'armv7')}` slice"
        + (f" (`{src['path']}`, sha256 `{src['sha256']}`)" if "sha256" in src else "")
        + " by `python3 -m mtport.objc generate`, which also writes the machine-readable "
        "`work/re/objc.json` and the Ghidra names `work/re/names/objc.json`. IMP addresses are "
        "armv7 VM addresses with the Thumb bit set. Ivar offsets are the values of the "
        "`__objc_ivar` offset variables. App vs Chartboost: a class is Chartboost's when its "
        "name is `Chartboost` or starts with `CB` + an upper-case letter.",
        "",
        "| | Count |",
        "|---|---|",
        f"| Classes (`__objc_classlist`) | {c['classes']} "
        f"({c['app_classes']} app + {c['chartboost_classes']} Chartboost) |",
        f"| Categories (`__objc_catlist`) | {c['categories']} |",
        f"| Protocols (`__objc_protolist` + referenced) | {c['protocols']} |",
        f"| App methods (instance + class) | {c['app_methods']} |",
        f"| Chartboost methods (instance + class) | {c['chartboost_methods']} |",
        f"| Category methods | {c['category_methods']} |",
        f"| Distinct IMPs (one Ghidra name each) | {c['imps']} |",
        f"| Selector refs (`__objc_selrefs`) | {c['selrefs']} |",
        f"| Class refs (`__objc_classrefs`) | {c['classrefs']} |",
        f"| Super refs (`__objc_superrefs`) | {c['superrefs']} |",
        "",
        "## App classes",
        "",
        "| Class | Superclass | Protocols | Ivars | Instance methods | Class methods | IMPs |",
        "|---|---|---|---|---|---|---|",
    ]
    for k in app:
        protos = ", ".join(_code(p) for p in k.protocols) or "–"
        lines.append(
            f"| [{_code(k.name)}](#{k.name.lower()}) | {_super_text(k.superclass)} | {protos} "
            f"| {len(k.ivars)} | {len(k.instance_methods)} | {len(k.class_methods)} "
            f"| {_imp_range(k.instance_methods + k.class_methods)} |"
        )
    lines.append("")
    for k in app:
        lines += [f"### {k.name}", ""]
        sup = "root class" if k.superclass is None else f"superclass {_super_text(k.superclass)}"
        facts = [
            sup + (" (imported)" if k.superclass and k.superclass.address is None else ""),
            f"`class_t` at `{hexaddr(k.address)}`, metaclass `{hexaddr(k.metaclass)}`",
            f"instance size {k.instance_size} (own ivars from {k.instance_start})",
        ]
        if k.flags:
            facts.append("flags " + ", ".join(flag_names(k.flags)))
        if k.protocols:
            facts.append("adopts " + ", ".join(_code(p) for p in k.protocols))
        lines += [f"- {f}" for f in facts] + [""]
        if k.ivars:
            lines += [f"Ivars ({len(k.ivars)}):", "", "| Offset | Name | Type | Size |"]
            lines.append("|---|---|---|---|")
            for i in k.ivars:
                lines.append(f"| {i.offset} | {_code(i.name)} | {_code(i.type)} | {i.size} |")
            lines.append("")
        _props_table(lines, "Properties", k.properties)
        _props_table(lines, "Class properties", k.class_properties)
        _method_table(lines, "Instance methods", k.instance_methods)
        _method_table(lines, "Class methods", k.class_methods)

    lines += [
        "## Categories",
        "",
        "Categories on system classes, from snippets bundled with the app or Chartboost.",
        "",
    ]
    for cat in objc.categories:
        lines += [f"### {cat.class_name} ({cat.name})", ""]
        lines += [f"- `category_t` at `{hexaddr(cat.address)}`"]
        if cat.protocols:
            lines.append("- adopts " + ", ".join(_code(p) for p in cat.protocols))
        lines.append("")
        _props_table(lines, "Properties", cat.properties)
        _method_table(lines, "Instance methods", cat.instance_methods)
        _method_table(lines, "Class methods", cat.class_methods)

    lines += [
        "## Protocols",
        "",
        "Method counts per kind (req = required, opt = optional; inst = instance, cls = class).",
        "",
        "| Protocol | Adopts | req inst | req cls | opt inst | opt cls | Properties |",
        "|---|---|---|---|---|---|---|",
    ]
    for p in objc.protocols:
        adopts = ", ".join(_code(x) for x in p.protocols) or "–"
        lines.append(
            f"| {_code(p.name)} | {adopts} | {len(p.instance_methods)} | {len(p.class_methods)} "
            f"| {len(p.optional_instance_methods)} | {len(p.optional_class_methods)} "
            f"| {len(p.properties)} |"
        )
    lines.append("")

    cb_methods = [m for k in cb for m in k.instance_methods + k.class_methods]
    lines += [
        "## Chartboost",
        "",
        f"The bundled Chartboost SDK contributes {len(cb)} classes with "
        f"{len(cb_methods)} methods (IMPs {_imp_range(cb_methods)}). Every method is in "
        "`work/re/objc.json` and `work/re/names/objc.json` (Ghidra names); "
        "this section only summarizes them.",
        "",
        "| Class | Superclass | Ivars | Instance methods | Class methods | IMPs |",
        "|---|---|---|---|---|---|",
    ]
    for k in cb:
        lines.append(
            f"| {_code(k.name)} | {_super_text(k.superclass)} | {len(k.ivars)} "
            f"| {len(k.instance_methods)} | {len(k.class_methods)} "
            f"| {_imp_range(k.instance_methods + k.class_methods)} |"
        )
    lines.append("")
    return "\n".join(lines)


# -- command line ---------------------------------------------------------------------------


def _dump(doc: dict) -> str:
    return json.dumps(doc, indent=1, ensure_ascii=False) + "\n"


def source_info(path: Path, data: bytes, repo: Path | None = None) -> dict:
    try:
        shown = path.resolve().relative_to(repo.resolve()).as_posix() if repo else str(path)
    except ValueError:
        shown = path.name
    return {"path": shown, "arch": "armv7", "sha256": hashlib.sha256(data).hexdigest()}


def generate(repo: Path, slice_path: Path | None = None) -> dict:
    """Write the three outputs below ``repo``; return the counts."""
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    data = path.read_bytes()
    image = macho.MachO(data)
    objc = parse(image)
    source = source_info(path, data, repo)
    outputs = {
        JSON_OUT: _dump(to_json(objc, source)),
        NAMES_OUT: _dump(names_json(objc)),
        MARKDOWN_OUT: to_markdown(objc, source),
    }
    for rel, text in outputs.items():
        out = repo / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {rel}")
    return counts(objc)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.objc", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    gen = sub.add_parser("generate", help=f"write {JSON_OUT}, {NAMES_OUT} and {MARKDOWN_OUT}")
    gen.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    gen.add_argument("--slice", type=Path, help="armv7 slice (default: from scripts/bootstrap)")
    sub.add_parser("json", help="print the objc.json document").add_argument("slice", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "generate":
            for key, value in generate(args.repo, args.slice).items():
                print(f"  {key:<20} {value}")
        else:
            data = args.slice.read_bytes()
            objc = parse(macho.MachO(data))
            sys.stdout.write(_dump(to_json(objc, source_info(args.slice, data))))
    except (OSError, macho.MachOError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
