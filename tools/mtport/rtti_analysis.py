"""The analysis half of :mod:`mtport.rtti`: ``type_info`` and vtable parsing, ownership and
names of the slot functions, consistency checks.

The method is described in the module docstring of ``tools/mtport/rtti.py``, which re-exports
every name defined here.
"""

from __future__ import annotations

import bisect
import re
from collections import Counter
from dataclasses import dataclass, field
from functools import cached_property

from mtport import armscan, macho
from mtport import ghidra_common as gc
from mtport.armscan import pc_relative_values
from mtport.names import hexaddr, parse_address

TYPEINFO_VTABLES = {
    "__ZTVN10__cxxabiv117__class_type_infoE": "class",
    "__ZTVN10__cxxabiv120__si_class_type_infoE": "si",
    "__ZTVN10__cxxabiv121__vmi_class_type_infoE": "vmi",
}
TYPEINFO_KIND_NAMES = {
    "class": "__class_type_info",
    "si": "__si_class_type_info",
    "vmi": "__vmi_class_type_info",
}
PURE_VIRTUAL = "___cxa_pure_virtual"
OPERATOR_DELETE = "__ZdlPv"
VIRTUAL_MASK = 0x1
PUBLIC_MASK = 0x2
MAX_OFFSET_TO_TOP = 0x10000
MAX_BASES = 64
MAX_THUNK_SIZE = 32
# Localization keys that leak C++ method names: "CStateGame.Paint.7" (ScriptExtra.strings).
METHOD_KEY = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\.([A-Za-z_][A-Za-z0-9_]*)\.[0-9]+")


# Slot names of the IState machine that no localization key gives, from how the engine calls
# them (#12, docs/re/program-flow.md): (class that introduced the slot, index) -> (method,
# evidence). Unused slots are the ones with no call site through the vtable in the game code.
KNOWN_SLOTS: dict[tuple[str, int], tuple[str, str]] = {
    ("IState", 2): (
        "Enter",
        "called on the new current state by startClet 0x5C2D and ChangeState 0xADED",
    ),
    ("IState", 3): ("Exit", "called on the current state before ChangeState 0xADED replaces it"),
    ("IState", 4): ("Resume", "called on the parent state after a pop (0xAABD, 0xADED)"),
    ("IState", 5): (
        "Pause",
        "called on the current state by Clet_Pause 0x5B8D and Clet_HandleEvent 0x5A75 (event 7)",
    ),
    ("IState", 7): ("OnKeyReleased", "SkyCanvas::Tick 0x2590D passes the released key code"),
    ("IState", 10): (
        "Update",
        "SkyCanvas::Tick 0x2590D, every frame while CMTimer runs, before Paint",
    ),
    ("IState", 11): ("SetParam", "ChangeState 0xADED passes its third argument before Enter"),
    ("IState", 13): (
        "PushState",
        "Pause, SetParent(current), SetCurrentState, SetParam, Enter; no call through the vtable",
    ),
    ("IState", 14): (
        "PopState",
        "Exit, SetCurrentState(parent), parent Resume; no call through the vtable",
    ),
    ("IState", 15): (
        "PopAndRestart",
        "like PopState, then Exit/SetParam/Enter on the parent; no call through the vtable",
    ),
    ("IState", 16): (
        "PopAllStates",
        "pops until the current state has no parent; no call through the vtable",
    ),
    ("IState", 17): (
        "SetCurrentState",
        "deletes SkyCanvas+0xC (deleting destructor) and stores the new state there",
    ),
    ("IState", 18): ("SetParent", "stores this+8"),
    ("IState", 19): ("GetCurrentState", "returns SkyCanvas+0xC"),
    ("IState", 20): ("GetParent", "returns this+8"),
    ("IState", 21): (
        "GetRootState",
        "walks GetParent from the current state; no call through the vtable",
    ),
    ("IState", 22): (
        "OnPointerEvent",
        "(type, x, y): keeps the previous x, y; CStateTitle's "
        "posts key -5 on type 2; no call through the vtable",
    ),
    ("CStateMainMenu", 24): (
        "LoadCharacterSlots",
        "called by its Enter and OnKeyPressed; CDataMgr::LoadSlots, fills EAGLView characterInfos",
    ),
}


class RttiError(macho.MachOError):
    """The RTTI or vtable data is malformed."""


def _signed(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


# -- names ----------------------------------------------------------------------------------


def demangle_type(mangled: str) -> str | None:
    """The class name of an Itanium ``<name>`` (``6IState``, ``N2ns3FooE``), or None if it is
    anything else (templates, substitutions, ...)."""

    def source_name(pos: int) -> tuple[str, int] | None:
        end = pos
        while end < len(mangled) and mangled[end].isdigit():
            end += 1
        if end == pos or mangled[pos] == "0":
            return None
        length = int(mangled[pos:end])
        ident = mangled[end : end + length]
        if len(ident) != length or not ident.replace("_", "a").isalnum():
            return None
        return ident, end + length

    if mangled.startswith("N"):
        parts, pos = [], 1
        while pos < len(mangled) and mangled[pos] != "E":
            got = source_name(pos)
            if got is None:
                return None
            parts.append(got[0])
            pos = got[1]
        if pos != len(mangled) - 1 or len(parts) < 2:
            return None
        return "::".join(parts)
    got = source_name(0)
    if got is None or got[1] != len(mangled):
        return None
    return got[0]


# -- data model -----------------------------------------------------------------------------


@dataclass(frozen=True)
class Base:
    """One base of a class: ``offset`` into the derived object; ``public`` / ``virtual``."""

    typeinfo: int
    offset: int
    public: bool = True
    virtual: bool = False


@dataclass(frozen=True)
class TypeInfo:
    address: int
    kind: str  # "class" | "si" | "vmi"
    mangled: str
    name: str
    name_address: int
    size: int  # bytes of the type_info object
    flags: int = 0  # __vmi_class_type_info::__flags
    bases: tuple[Base, ...] = ()

    @property
    def end(self) -> int:
        return self.address + self.size


@dataclass(frozen=True)
class Slot:
    index: int
    address: int  # where the slot word is
    target: int  # function pointer (Thumb bit kept); 0 for a pure virtual slot
    pure: bool = False


@dataclass(frozen=True)
class VTable:
    cls: str
    address: int  # the offset_to_top word
    offset_to_top: int
    slots: tuple[Slot, ...]
    end: str  # what stopped the slot walk: "vtable", "typeinfo", "data", "section end", ...

    @property
    def address_point(self) -> int:
        return self.address + 8

    @property
    def primary(self) -> bool:
        return self.offset_to_top == 0


@dataclass
class Function:
    """One virtual function (or thunk) and every slot that holds it."""

    address: int  # function pointer
    owner: str
    kind: str  # "virtual" | "complete-dtor" | "deleting-dtor" | "thunk"
    index: int | None  # slot in the owner's primary vtable (None for a thunk)
    name: str = ""
    uses: list[tuple[str, int, int]] = field(default_factory=list)  # (class, vtable, index)
    thunk_of: int | None = None
    adjust: int = 0  # thunk: added to `this` before the call
    note: str = ""
    size: int = 0  # bytes up to the next LC_FUNCTION_STARTS entry


@dataclass
class Rtti:
    typeinfos: tuple[TypeInfo, ...]
    vtables: tuple[VTable, ...]
    functions: dict[int, Function]
    destructors: dict[str, int]  # class -> index of its complete-object destructor slot
    function_starts: frozenset[int]
    evidence: dict[str, list[str]] = field(default_factory=dict)
    # (class that introduced the slot, index) -> (method name, evidence)
    slot_names: dict[tuple[str, int], tuple[str, str]] = field(default_factory=dict)

    @cached_property
    def by_name(self) -> dict[str, TypeInfo]:
        return {t.name: t for t in self.typeinfos}

    @cached_property
    def by_address(self) -> dict[int, TypeInfo]:
        return {t.address: t for t in self.typeinfos}

    def vtables_of(self, cls: str) -> list[VTable]:
        return [v for v in self.vtables if v.cls == cls]

    def primary_vtable(self, cls: str) -> VTable | None:
        return next((v for v in self.vtables if v.cls == cls and v.primary), None)

    def bases(self, cls: str) -> list[tuple[str, Base]]:
        return [(self.by_address[b.typeinfo].name, b) for b in self.by_name[cls].bases]

    def primary_base(self, cls: str) -> str | None:
        """The base whose vtable layout the class's primary vtable extends (offset 0, not
        virtual), if any."""
        for name, b in self.bases(cls):
            if b.offset == 0 and not b.virtual:
                return name
        return None

    def derived(self, cls: str) -> list[str]:
        return [t.name for t in self.typeinfos if any(n == cls for n, _ in self.bases(t.name))]

    def ancestors(self, cls: str) -> set[str]:
        out: set[str] = set()
        todo = [cls]
        while todo:
            for name, _ in self.bases(todo.pop()):
                if name not in out:
                    out.add(name)
                    todo.append(name)
        return out

    def subobjects(self, cls: str) -> list[tuple[str, int]]:
        """``(class, offset)`` of the class and every non-virtual base subobject, outermost
        first."""
        out = [(cls, 0)]
        for name, b in self.bases(cls):
            if not b.virtual:
                out += [(n, b.offset + off) for n, off in self.subobjects(name)]
        return out

    def introducer(self, cls: str, index: int) -> str:
        """The most-base class along the primary-base chain whose primary vtable already has
        slot ``index``: the class that declared that virtual function."""
        while True:
            base = self.primary_base(cls)
            vt = self.primary_vtable(base) if base else None
            if vt is None or len(vt.slots) <= index:
                return cls
            cls = base

    def method(self, cls: str, index: int) -> str | None:
        """The recovered name of slot ``index`` of ``cls``, if there is evidence for one."""
        got = self.slot_names.get((self.introducer(cls, index), index))
        return got[0] if got else None

    def subobject_of(self, vtable: VTable) -> str:
        """The base class a secondary vtable is for (the outermost base at its offset)."""
        if vtable.primary:
            return vtable.cls
        for name, off in self.subobjects(vtable.cls):
            if off == -vtable.offset_to_top:
                return name
        return "?"

    def slot_name(self, target: int) -> str | None:
        f = self.functions.get(target)
        return f.name if f else None


# -- code helpers ---------------------------------------------------------------------------


def _sign_extend(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def call_targets(code: bytes, address: int, thumb: bool) -> list[int]:
    """The direct ``BL`` / ``BLX <imm>`` targets in ``code`` (at VM ``address``), as function
    pointers (Thumb bit set for Thumb targets), in order. Data in the code may decode as a
    call; callers only use this on whole function bodies."""
    out: list[int] = []
    if thumb:
        pos = 0
        while pos + 2 <= len(code):
            hw1 = code[pos] | code[pos + 1] << 8
            if hw1 >> 11 not in (0b11101, 0b11110, 0b11111):
                pos += 2
                continue
            if pos + 4 > len(code):
                break
            hw2 = code[pos + 2] | code[pos + 3] << 8
            if hw1 >> 11 == 0b11110 and hw2 & 0xC000 == 0xC000:
                s = (hw1 >> 10) & 1
                i1 = ~((hw2 >> 13) & 1 ^ s) & 1
                i2 = ~((hw2 >> 11) & 1 ^ s) & 1
                imm = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FF) << 12 | (hw2 & 0x7FF) << 1
                pc = address + pos + 4
                if hw2 & 0x1000:  # BL: stays in Thumb
                    out.append((pc + _sign_extend(imm, 25)) & 0xFFFFFFFF | 1)
                elif not hw2 & 1:  # BLX: to ARM, from Align(PC, 4)
                    out.append((pc & ~3) + _sign_extend(imm, 25) & 0xFFFFFFFC)
            pos += 4
    else:
        for pos in range(0, len(code) - 3, 4):
            word = int.from_bytes(code[pos : pos + 4], "little")
            pc = address + pos + 8
            offset = _sign_extend((word & 0xFFFFFF) << 2, 26)
            if word >> 28 == 0xF and word & 0x0E000000 == 0x0A000000:  # BLX <imm>: to Thumb
                out.append((pc + offset + ((word >> 23) & 2)) & 0xFFFFFFFF | 1)
            elif word >> 28 != 0xF and word & 0x0F000000 == 0x0B000000:  # BL
                out.append((pc + offset) & 0xFFFFFFFF)
    return out


class Image:
    """The bits of a :class:`macho.MachO` this module needs, with code helpers."""

    def __init__(self, image: macho.MachO) -> None:
        self.macho = image
        self.starts = tuple(image.function_starts)
        self.index = armscan.FunctionIndex(self.starts)
        self.text = image.section("__TEXT", "__text")
        self.stubs = image.section("__TEXT", "__symbolstub1")

    @cached_property
    def strings(self) -> dict[int, str]:
        """Address of every C string and CFString object → its text (UTF-8 only)."""
        return {
            parse_address(s["address"]): s["value"]
            for s in gc.macho_strings(self.macho)
            if s["encoding"] == "utf-8"
        }

    def string_refs(self, fp: int) -> list[str]:
        """The strings a Thumb function's body computes the address of, in order."""
        if not fp & 1:
            return []
        values = pc_relative_values(self.body(fp), fp & ~1)
        return [self.strings[v] for v in values if v in self.strings]

    def is_code(self, addr: int) -> bool:
        return self.text is not None and self.text.contains(addr & ~1)

    def function_end(self, fp: int) -> int:
        end = self.index.next_start(fp)
        if end is not None:
            return end
        return self.text.end if self.text is not None else fp & ~1

    def body(self, fp: int) -> bytes:
        addr = fp & ~1
        return self.macho.read(addr, max(0, self.function_end(fp) - addr))

    def calls(self, fp: int) -> list[int]:
        return call_targets(self.body(fp), fp & ~1, bool(fp & 1))

    def calls_import(self, fp: int, symbol: str) -> bool:
        """Whether ``fp`` calls the import ``symbol`` through its ``__symbolstub1`` stub. The
        stubs are ARM code, so only an even call target (Thumb ``BLX``) can be one; the stub's
        import comes from the indirect symbol table (:meth:`macho.MachO.stub_symbol`)."""
        return any(
            not t & 1
            and self.stubs is not None
            and self.stubs.contains(t)
            and self.macho.stub_symbol(t) == symbol
            for t in self.calls(fp)
        )


# -- parsing --------------------------------------------------------------------------------


def _typeinfo(image: macho.MachO, address: int, kind: str) -> TypeInfo:
    name_address = image.u32(address + 4)
    try:
        raw = image.cstring(name_address, limit=1024)
    except macho.MachOError as e:
        raise RttiError(f"type_info at {hexaddr(address)}: bad __name: {e}") from e
    mangled = raw.decode("ascii", "replace")
    name = demangle_type(mangled) or mangled
    if kind == "class":
        return TypeInfo(address, kind, mangled, name, name_address, 8)
    if kind == "si":
        base = image.u32(address + 8)
        return TypeInfo(address, kind, mangled, name, name_address, 12, bases=(Base(base, 0),))
    flags, count = image.u32(address + 8), image.u32(address + 12)
    if count > MAX_BASES:
        raise RttiError(f"type_info at {hexaddr(address)}: {count} bases")
    bases = []
    for i in range(count):
        base, offset_flags = image.u32(address + 16 + 8 * i), image.u32(address + 20 + 8 * i)
        bases.append(
            Base(
                base,
                _sign_extend(offset_flags >> 8, 24),
                bool(offset_flags & PUBLIC_MASK),
                bool(offset_flags & VIRTUAL_MASK),
            )
        )
    size = 16 + 8 * count
    return TypeInfo(address, kind, mangled, name, name_address, size, flags, tuple(bases))


def find_typeinfos(image: macho.MachO) -> list[TypeInfo]:
    """Every ``type_info`` object: the targets of the three ``__cxxabiv1`` vtable binds."""
    found: dict[int, TypeInfo] = {}
    for b in image.binds:
        kind = TYPEINFO_VTABLES.get(b.symbol)
        if kind is None or b.kind == "lazy":
            continue
        if b.addend != 8:
            raise RttiError(f"type_info vptr at {hexaddr(b.address)} has addend {b.addend}")
        found.setdefault(b.address, _typeinfo(image, b.address, kind))
    out = sorted(found.values(), key=lambda t: t.address)
    known = {t.address for t in out}
    seen: set[str] = set()
    for t in out:
        if t.name in seen:
            raise RttiError(f"two type_info objects for {t.name}")
        seen.add(t.name)
        for base in t.bases:
            if base.typeinfo not in known:
                raise RttiError(f"{t.name}: base {hexaddr(base.typeinfo)} is not a type_info")
    return out


def data_words(image: macho.MachO):
    """``(address, value)`` of every aligned word in a file-backed data section."""
    for s in image.sections:
        if s.flags & 0xFF in (0x1, 0xC):  # S_ZEROFILL, S_GB_ZEROFILL
            continue
        if s.segname == "__TEXT" and s.sectname != "__const":
            continue
        start = (s.addr + 3) & ~3
        if start >= s.end:
            continue
        raw = image.read(start, (s.end - start) & ~3)
        for i in range(0, len(raw), 4):
            yield start + i, int.from_bytes(raw[i : i + 4], "little")


def find_vtables(img: Image, typeinfos: list[TypeInfo]) -> list[VTable]:
    image = img.macho
    by_address = {t.address: t for t in typeinfos}
    inside = sorted((t.address, t.end) for t in typeinfos)

    def in_typeinfo(addr: int) -> bool:
        i = bisect.bisect_right(inside, (addr, 0xFFFFFFFF)) - 1
        return i >= 0 and inside[i][0] <= addr < inside[i][1]

    heads = []  # (vtable address, offset_to_top, class)
    for addr, value in data_words(image):
        t = by_address.get(value)
        if t is None or in_typeinfo(addr) or image.bound_symbol(addr) is not None:
            continue
        sec = image.section_at(addr - 4)
        if sec is None or not sec.contains(addr):
            continue
        offset = _signed(image.u32(addr - 4))
        if -MAX_OFFSET_TO_TOP < offset <= 0 and not offset % 4:
            heads.append((addr - 4, offset, t.name))
    starts = {a for a, _, _ in heads}
    ti_starts = set(by_address)
    out = []
    for start, offset, cls in heads:
        slots: list[Slot] = []
        sec = image.section_at(start)
        addr = start + 8
        while True:
            if addr in starts:
                end = "vtable"
                break
            if addr in ti_starts:
                end = "typeinfo"
                break
            if addr + 4 > sec.end:
                end = "section end"
                break
            symbol = image.bound_symbol(addr)
            if symbol == PURE_VIRTUAL:
                slots.append(Slot(len(slots), addr, 0, pure=True))
            elif symbol is not None:
                end = f"bind {symbol}"
                break
            else:
                value = image.u32(addr)
                if not value or not img.is_code(value):
                    end = "data"
                    break
                slots.append(Slot(len(slots), addr, value))
            addr += 4
        out.append(VTable(cls, start, offset, tuple(slots), end))
    return out


# -- ownership and names --------------------------------------------------------------------


def _order(r: Rtti) -> list[str]:
    """Class names, every base before its derived classes (then by type_info address)."""
    done: list[str] = []
    seen: set[str] = set()

    def visit(name: str) -> None:
        if name in seen:
            return
        seen.add(name)
        for base, _ in r.bases(name):
            visit(base)
        done.append(name)

    for t in r.typeinfos:
        visit(t.name)
    return done


def _find_destructors(r: Rtti, img: Image) -> dict[str, int]:
    out: dict[str, int] = {}
    for cls in _order(r):
        vt = r.primary_vtable(cls)
        if vt is None:
            continue

        def pair_ok(i: int, vt: VTable = vt) -> bool:
            if i + 1 >= len(vt.slots):
                return False
            a, b = vt.slots[i], vt.slots[i + 1]
            return (
                not a.pure
                and not b.pure
                and a.target != b.target
                and img.calls_import(a.target, OPERATOR_DELETE)
                and img.calls_import(b.target, OPERATOR_DELETE)
            )

        base = r.primary_base(cls)
        if base is not None and base in out:
            index = out[base]
            if pair_ok(index):
                out[cls] = index
            else:
                r.evidence.setdefault(cls, []).append(
                    f"slots {index}/{index + 1} (the destructor pair of {base}) don't both "
                    "call operator delete"
                )
            continue
        for i in range(len(vt.slots) - 1):
            if pair_ok(i):
                out[cls] = i
                break
    return out


def analyze(
    image: macho.MachO, known_slots: dict[tuple[str, int], tuple[str, str]] | None = None
) -> Rtti:
    """Recover the classes, vtables and slot functions; ``known_slots`` defaults to
    :data:`KNOWN_SLOTS` (entries for classes the image doesn't have are ignored)."""
    img = Image(image)
    typeinfos = find_typeinfos(image)
    vtables = find_vtables(img, typeinfos)
    vtables.sort(key=lambda v: v.address)
    r = Rtti(tuple(typeinfos), tuple(vtables), {}, {}, frozenset(img.starts))
    r.destructors = _find_destructors(r, img)

    uses: dict[int, list[tuple[str, int, int]]] = {}
    for vt in r.vtables:
        for s in vt.slots:
            if not s.pure:
                uses.setdefault(s.target, []).append((vt.cls, vt.address, s.index))
    primary = {v.address for v in r.vtables if v.primary}

    # Functions held by a primary vtable: owned by the most-base class that holds them.
    for target, where in sorted(uses.items()):
        in_primary = [(c, a, i) for c, a, i in where if a in primary]
        if not in_primary:
            continue
        holders = {c for c, _, _ in where}
        owner = next(
            (c for c in _order(r) if c in holders and holders - {c} <= _descendants(r, c)),
            None,
        )
        note = ""
        if owner is None:
            owner = in_primary[0][0]
            note = "held by unrelated classes: " + ", ".join(sorted(holders))
        index = next((i for c, a, i in in_primary if c == owner), in_primary[0][2])
        kind = "virtual"
        if owner in r.destructors and index in (r.destructors[owner], r.destructors[owner] + 1):
            kind = "complete-dtor" if index == r.destructors[owner] else "deleting-dtor"
        r.functions[target] = Function(target, owner, kind, index, uses=where, note=note)

    # Functions only in secondary vtables: this-adjusting thunks (or unexplained).
    for target, where in sorted(uses.items()):
        if target in r.functions:
            continue
        cls, vt_addr, index = where[0]
        vt = next(v for v in r.vtables if v.address == vt_addr)
        calls = img.calls(target) if target in r.function_starts else []
        size = img.function_end(target) - (target & ~1)
        f = Function(target, cls, "thunk", None, uses=where, adjust=vt.offset_to_top)
        if len(calls) == 1 and calls[0] in r.functions and size <= MAX_THUNK_SIZE:
            f.thunk_of = calls[0]
        else:
            f.kind = "virtual"
            f.note = "only in a secondary vtable, and not a recognizable thunk"
            f.index = index
        r.functions[target] = f

    _name_slots(r, img)
    _name_known_slots(r, KNOWN_SLOTS if known_slots is None else known_slots)
    for f in r.functions.values():
        f.name = _function_name(r, f)
        if f.address in r.function_starts:
            f.size = img.function_end(f.address) - (f.address & ~1)
    return r


def _name_slots(r: Rtti, img: Image) -> None:
    """Method names from leaked localization keys: a slot function of class C that looks up
    ``C.Method.N`` keys implements ``C::Method``, which names that slot for the class that
    introduced it and for every override."""
    found: dict[tuple[str, int], list[tuple[str, str]]] = {}
    for f in sorted(r.functions.values(), key=lambda f: f.address):
        if f.kind != "virtual" or f.index is None:
            continue
        keys = [METHOD_KEY.fullmatch(s) for s in img.string_refs(f.address)]
        methods = Counter(k.group(2) for k in keys if k and k.group(1) == f.owner)
        if not methods:
            continue
        method, n = sorted(methods.items(), key=lambda kv: (-kv[1], kv[0]))[0]
        why = f"{hexaddr(f.address)} ({f.owner}, slot {f.index}) looks up {n} {f.owner}.{method}.N"
        if len(methods) > 1:
            why += " (and " + ", ".join(f"{m} x{c}" for m, c in methods.items() if m != method)
            why += ")"
        found.setdefault((r.introducer(f.owner, f.index), f.index), []).append((method, why))
    for key, got in sorted(found.items()):
        methods = sorted({m for m, _ in got})
        if len(methods) == 1:
            r.slot_names[key] = (methods[0], "; ".join(w for _, w in got))
        else:
            r.evidence.setdefault(key[0], []).append(
                f"slot {key[1]}: conflicting method keys {methods}: " + "; ".join(w for _, w in got)
            )


def _name_known_slots(r: Rtti, known: dict[tuple[str, int], tuple[str, str]]) -> None:
    """Names from call-site evidence for slots that no localization key names."""
    for (cls, index), (method, why) in sorted(known.items()):
        vt = r.primary_vtable(cls) if any(t.name == cls for t in r.typeinfos) else None
        if vt is None or index >= len(vt.slots) or r.introducer(cls, index) != cls:
            continue
        have = r.slot_names.get((cls, index))
        if have is None:
            r.slot_names[(cls, index)] = (method, why)
        elif have[0] != method:
            r.evidence.setdefault(cls, []).append(
                f"slot {index}: localization keys say {have[0]}, call sites say {method} ({why})"
            )


def _descendants(r: Rtti, cls: str) -> set[str]:
    return {t.name for t in r.typeinfos if cls in r.ancestors(t.name)}


def _member(r: Rtti, f: Function) -> str:
    if f.kind == "complete-dtor":
        return f"~{f.owner.split('::')[-1]}"
    if f.kind == "deleting-dtor":
        return f"~{f.owner.split('::')[-1]}_deleting"
    return r.method(f.owner, f.index) or f"vf{f.index}"


def _function_name(r: Rtti, f: Function) -> str:
    if f.kind == "thunk" and f.thunk_of is not None:
        target = r.functions[f.thunk_of]
        return f"{target.owner}::{_member(r, target)}@thunk{f.adjust}"
    return f"{f.owner}::{_member(r, f)}"


# -- checks ---------------------------------------------------------------------------------


def slot_exceptions(r: Rtti) -> list[dict]:
    """Every vtable slot that is not an ``LC_FUNCTION_STARTS`` function, with the reason."""
    out = []
    for vt in r.vtables:
        for s in vt.slots:
            if s.pure:
                reason = f"pure virtual: bound to {PURE_VIRTUAL} (0 in the file)"
            elif s.target in r.function_starts:
                continue
            elif s.target ^ 1 in r.function_starts:
                reason = "points at a function start in the wrong mode"
            else:
                reason = "not a function start"
            out.append(
                {
                    "class": vt.cls,
                    "vtable": hexaddr(vt.address),
                    "index": s.index,
                    "slot": hexaddr(s.address),
                    "target": hexaddr(s.target),
                    "pure": s.pure,
                    "reason": reason,
                }
            )
    return out


def layout_problems(r: Rtti) -> list[str]:
    """Inconsistencies between the hierarchy and the vtables (empty when all is well)."""
    out = []
    for t in r.typeinfos:
        vts = r.vtables_of(t.name)
        prim = [v for v in vts if v.primary]
        if len(prim) > 1:
            out.append(f"{t.name}: {len(prim)} primary vtables")
        base = r.primary_base(t.name)
        if prim and base and r.primary_vtable(base):
            if len(prim[0].slots) < len(r.primary_vtable(base).slots):
                out.append(f"{t.name}: fewer slots than its primary base {base}")
        for v in vts:
            if v.primary:
                continue
            sub = r.subobject_of(v)
            sub_vt = r.primary_vtable(sub)
            if sub == "?":
                out.append(f"{t.name}: secondary vtable at {hexaddr(v.address)} has no base")
            elif sub_vt is not None and len(sub_vt.slots) != len(v.slots):
                out.append(
                    f"{t.name}: secondary vtable for {sub} has {len(v.slots)} slots, "
                    f"{sub} has {len(sub_vt.slots)}"
                )
        for name, b in r.bases(t.name):
            expected = b.offset == 0 or not r.primary_vtable(name)
            if not expected and not any(v.offset_to_top == -b.offset for v in vts):
                out.append(f"{t.name}: no secondary vtable for base {name} at {b.offset}")
    return out
