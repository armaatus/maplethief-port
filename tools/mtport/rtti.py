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
that declared the slot and in every override. Destructors take a pair of slots, complete
object (D1) then deleting (D0) as the Itanium ABI orders them. GCC 4.2 at -O0 emits both from
one body, with ``operator delete`` (``__ZdlPv``) behind a constant flag, so a slot pair whose two
functions both call ``operator delete`` is a destructor pair; derived classes keep the pair
at their primary base's position. A secondary-vtable function that is in no primary vtable and
makes exactly one call is a non-virtual thunk to the function it calls.

Command line::

    python3 -m mtport.rtti generate [--repo DIR] [--slice PATH]
        # writes work/re/vtables.json, work/re/names/rtti.json and docs/re/cpp-classes.md
    python3 -m mtport.rtti json SLICE      # the vtables.json document on stdout
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from functools import cached_property
from pathlib import Path

from mtport import macho, names

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

# Facts about MapleThief 1.5 that the generic output can't state; generate() adds them.
NOTES = (
    "**63 classes, not 64.** docs/ANALYSIS.md used to list 64 RTTI names. The 64th, `CPW`, "
    "is not a type name: the bytes `43 50 57` (`CPW`) occur only six times, all inside one "
    "Thumb function in `__text` (`0xBCED8`–`0xBCFE6`), where they are `str r3, [r0, r1]` "
    "(`43 50`) followed by the first byte of an `ldr.w r0, [r7, #-0x1c]` (`57 f8 1c 0c`). No "
    "`type_info` points at them. Every `type_info` object in the image is reached from one of "
    "the 63 `__cxxabiv1` vtable binds, and every class name they hold is listed below."
)
SCHEMA_VERSION = 1
JSON_OUT = "work/re/vtables.json"
NAMES_OUT = "work/re/names/rtti.json"
MARKDOWN_OUT = "docs/re/cpp-classes.md"


class RttiError(macho.MachOError):
    """The RTTI or vtable data is malformed."""


def hexaddr(value: int) -> str:
    return f"0x{value:X}"


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


def pc_relative_values(code: bytes, address: int) -> list[int]:
    """The addresses Thumb code in ``code`` (at VM ``address``) computes PC-relatively, in
    order: ``movw rX, #lo`` / ``movt rX, #hi`` or ``ldr rX, [pc, #imm]`` (a literal), then
    ``add rX, pc``. That is how GCC at -O0 materializes a string or vtable address. Only these
    instructions are tracked, so this is a cheap scan, not a data-flow analysis."""
    regs: dict[int, int] = {}
    out: list[int] = []
    pos = 0
    base = address

    def literal(at: int) -> int | None:
        off = at - base
        if 0 <= off and off + 4 <= len(code):
            return int.from_bytes(code[off : off + 4], "little")
        return None

    while pos + 2 <= len(code):
        hw1 = code[pos] | code[pos + 1] << 8
        pc = base + pos + 4
        if hw1 >> 11 in (0b11101, 0b11110, 0b11111):
            if pos + 4 > len(code):
                break
            hw2 = code[pos + 2] | code[pos + 3] << 8
            imm16 = (
                (hw1 & 0xF) << 12 | ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | hw2 & 0xFF
            )
            rd = (hw2 >> 8) & 0xF
            if hw1 & 0xFBF0 == 0xF240 and not hw2 & 0x8000:  # MOVW
                regs[rd] = imm16
            elif hw1 & 0xFBF0 == 0xF2C0 and not hw2 & 0x8000:  # MOVT
                if rd in regs:
                    regs[rd] = (regs[rd] & 0xFFFF) | imm16 << 16
            elif hw1 & 0xFF7F == 0xF85F:  # LDR.W Rt, [PC, #+/-imm12]
                imm = hw2 & 0xFFF
                value = literal((pc & ~3) + (imm if hw1 & 0x80 else -imm))
                if value is not None:
                    regs[hw2 >> 12] = value
            pos += 4
            continue
        if hw1 & 0xF800 == 0x4800:  # LDR Rt, [PC, #imm8 * 4]
            value = literal((pc & ~3) + (hw1 & 0xFF) * 4)
            if value is not None:
                regs[(hw1 >> 8) & 7] = value
        elif hw1 & 0xFF78 == 0x4478:  # ADD Rdn, PC
            rdn = (hw1 & 7) | (hw1 >> 4) & 8
            if rdn in regs:
                value = (regs.pop(rdn) + pc) & 0xFFFFFFFF
                out.append(value)
        pos += 2
    return out


class Image:
    """The bits of a :class:`macho.MachO` this module needs, with code helpers."""

    def __init__(self, image: macho.MachO) -> None:
        self.macho = image
        self.starts = tuple(image.function_starts)
        self._sorted = sorted(a & ~1 for a in self.starts)
        self.text = image.section("__TEXT", "__text")
        self.stubs = image.section("__TEXT", "__symbolstub1")
        self._lazy = {b.address: b.symbol for b in image.binds if b.kind == "lazy"}

    @cached_property
    def strings(self) -> dict[int, str]:
        """Address of every C string and CFString object → its text (UTF-8 only)."""
        from mtport import ghidra_common

        return {
            int(s["address"], 16): s["value"]
            for s in ghidra_common.macho_strings(self.macho)
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
        addr = fp & ~1
        i = bisect.bisect_right(self._sorted, addr)
        if i < len(self._sorted):
            return self._sorted[i]
        return self.text.end if self.text is not None else addr

    def body(self, fp: int) -> bytes:
        addr = fp & ~1
        return self.macho.read(addr, max(0, self.function_end(fp) - addr))

    def stub_symbol(self, target: int) -> str | None:
        """The import a ``__symbolstub1`` stub jumps to (``ldr pc, [pc, #imm]`` → pointer)."""
        if self.stubs is None or target & 1 or not self.stubs.contains(target):
            return None
        word = self.macho.u32(target)
        if word & 0xFF7FF000 != 0xE51FF000:
            return None
        imm = word & 0xFFF
        ptr = target + 8 + (imm if word & 0x00800000 else -imm)
        return self.macho.bound_symbol(ptr) or self._lazy.get(ptr)

    def calls(self, fp: int) -> list[int]:
        return call_targets(self.body(fp), fp & ~1, bool(fp & 1))

    def calls_import(self, fp: int, symbol: str) -> bool:
        return any(self.stub_symbol(t) == symbol for t in self.calls(fp))


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


def _data_words(image: macho.MachO):
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
    for addr, value in _data_words(image):
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


def analyze(image: macho.MachO) -> Rtti:
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


# -- JSON -----------------------------------------------------------------------------------


def counts(r: Rtti) -> dict[str, int]:
    slots = [s for v in r.vtables for s in v.slots]
    kinds = [f.kind for f in r.functions.values()]
    return {
        "classes": len(r.typeinfos),
        "class_type_info": sum(t.kind == "class" for t in r.typeinfos),
        "si_class_type_info": sum(t.kind == "si" for t in r.typeinfos),
        "vmi_class_type_info": sum(t.kind == "vmi" for t in r.typeinfos),
        "classes_with_vtable": len({v.cls for v in r.vtables}),
        "vtables": len(r.vtables),
        "secondary_vtables": sum(not v.primary for v in r.vtables),
        "slots": len(slots),
        "pure_slots": sum(s.pure for s in slots),
        "functions": len(r.functions),
        "virtual_functions": kinds.count("virtual"),
        "destructors": kinds.count("complete-dtor") + kinds.count("deleting-dtor"),
        "thunks": kinds.count("thunk"),
        "slot_exceptions": len(slot_exceptions(r)),
        "non_pure_slot_exceptions": sum(not e["pure"] for e in slot_exceptions(r)),
    }


def to_json(r: Rtti, source: dict | None = None) -> dict:
    classes = []
    for t in r.typeinfos:
        vts = []
        for v in r.vtables_of(t.name):
            vts.append(
                {
                    "address": hexaddr(v.address),
                    "address_point": hexaddr(v.address_point),
                    "offset_to_top": v.offset_to_top,
                    "subobject": r.subobject_of(v),
                    "end": v.end,
                    "slots": [
                        {
                            "index": s.index,
                            "slot": hexaddr(s.address),
                            "target": None if s.pure else hexaddr(s.target),
                            "pure": s.pure,
                            "name": None if s.pure else r.slot_name(s.target),
                            "in_function_starts": (not s.pure) and s.target in r.function_starts,
                        }
                        for s in v.slots
                    ],
                }
            )
        dtor = r.destructors.get(t.name)
        classes.append(
            {
                "name": t.name,
                "mangled": t.mangled,
                "typeinfo": hexaddr(t.address),
                "typeinfo_kind": TYPEINFO_KIND_NAMES[t.kind],
                "typeinfo_name": hexaddr(t.name_address),
                "vmi_flags": t.flags,
                "bases": [
                    {"name": n, "offset": b.offset, "public": b.public, "virtual": b.virtual}
                    for n, b in r.bases(t.name)
                ],
                "derived": r.derived(t.name),
                "destructor_slots": None if dtor is None else [dtor, dtor + 1],
                "vtables": vts,
                "evidence": r.evidence.get(t.name, []),
            }
        )
    functions = [
        {
            "address": hexaddr(f.address),
            "name": f.name,
            "owner": f.owner,
            "kind": f.kind,
            "index": f.index,
            "thunk_of": None if f.thunk_of is None else hexaddr(f.thunk_of),
            "adjust": f.adjust,
            "in_function_starts": f.address in r.function_starts,
            "size": f.size,
            "uses": [{"class": c, "vtable": hexaddr(a), "index": i} for c, a, i in f.uses],
            "note": f.note,
        }
        for f in sorted(r.functions.values(), key=lambda f: f.address)
    ]
    return {
        "schema": SCHEMA_VERSION,
        "source": source or {},
        "counts": counts(r),
        "classes": classes,
        "functions": functions,
        "slot_exceptions": slot_exceptions(r),
        "layout_problems": layout_problems(r),
    }


def names_json(r: Rtti) -> dict:
    """``work/re/names/rtti.json``: every slot function plus data labels for the type_info
    objects, their name strings and the vtables."""
    entries = []
    for f in sorted(r.functions.values(), key=lambda f: f.address):
        comment = {
            "virtual": f"virtual, slot {f.index} of {f.owner}'s vtable",
            "complete-dtor": "complete-object destructor (D1)",
            "deleting-dtor": "deleting destructor (D0)",
            "thunk": f"non-virtual thunk: this += {f.adjust}, then the call",
        }[f.kind]
        others = sorted({c for c, _, _ in f.uses} - {f.owner})
        if others:
            comment += "; also in the vtables of " + ", ".join(others)
        if f.note:
            comment += "; " + f.note
        entries.append(
            {"address": hexaddr(f.address), "name": f.name, "kind": "function", "comment": comment}
        )
    for t in r.typeinfos:
        bases = ", ".join(n for n, _ in r.bases(t.name)) or "none"
        entries.append(
            {
                "address": hexaddr(t.address),
                "name": f"{t.name}::typeinfo",
                "kind": "data",
                "comment": f"{TYPEINFO_KIND_NAMES[t.kind]} for {t.name}; bases: {bases}",
            }
        )
        entries.append(
            {
                "address": hexaddr(t.name_address),
                "name": f"{t.name}::typeinfo_name",
                "kind": "data",
                "comment": f"type_info name {t.mangled!r}",
            }
        )
    for v in r.vtables:
        sub = r.subobject_of(v)
        label = f"{v.cls}::vtable" if v.primary else f"{v.cls}::vtable_for_{sub.replace('::', '_')}"
        entries.append(
            {
                "address": hexaddr(v.address),
                "name": label,
                "kind": "data",
                "comment": f"offset_to_top {v.offset_to_top}, {len(v.slots)} slots from "
                f"{hexaddr(v.address_point)}",
            }
        )
    entries.sort(key=lambda e: int(e["address"], 16))
    doc = {"source": "rtti", "names": entries}
    names.parse(doc, NAMES_OUT)
    return doc


# -- Markdown -------------------------------------------------------------------------------


def _bases_text(r: Rtti, cls: str) -> str:
    parts = []
    for name, b in r.bases(cls):
        extra = [] if b.public else ["non-public"]
        if b.virtual:
            extra.append("virtual")
        if b.offset:
            extra.append(f"at +{b.offset}")
        parts.append(f"`{name}`" + (f" ({', '.join(extra)})" if extra else ""))
    return ", ".join(parts) or "–"


def _anchor(name: str) -> str:
    return name.lower().replace("::", "")


def to_markdown(r: Rtti, source: dict | None = None, notes: str = "") -> str:
    c = counts(r)
    src = source or {}
    lines = [
        "# C++ classes, RTTI and vtables",
        "",
        "<!-- Generated by `python3 -m mtport.rtti generate` (tools/mtport/rtti.py). "
        "Do not edit by hand: rerun the generator. -->",
        "",
        f"Recovered from the `{src.get('arch', 'armv7')}` slice"
        + (f" (`{src['path']}`, sha256 `{src['sha256']}`)" if "sha256" in src else "")
        + " by `python3 -m mtport.rtti generate`, which also writes `work/re/vtables.json` and "
        "the Ghidra names `work/re/names/rtti.json`. Addresses are armv7 VM addresses; slot "
        "targets are function pointers (Thumb bit set). How the data is found and the naming "
        "rules are in the docstring of `tools/mtport/rtti.py`.",
        "",
        "| | Count |",
        "|---|---|",
        f"| Classes (`type_info` objects) | {c['classes']} ({c['class_type_info']} "
        f"`__class_type_info`, {c['si_class_type_info']} `__si_class_type_info`, "
        f"{c['vmi_class_type_info']} `__vmi_class_type_info`) |",
        f"| Classes with a vtable | {c['classes_with_vtable']} |",
        f"| Vtables | {c['vtables']} ({c['secondary_vtables']} secondary) |",
        f"| Slots | {c['slots']} ({c['pure_slots']} pure virtual) |",
        f"| Distinct slot functions | {c['functions']} ({c['virtual_functions']} virtual, "
        f"{c['destructors']} destructors, {c['thunks']} thunks) |",
        f"| Slots that are not an `LC_FUNCTION_STARTS` function | {c['slot_exceptions']} "
        f"(all pure virtual: {c['non_pure_slot_exceptions'] == 0}) |",
        "",
        "Function names: `Class::vfN` is slot N of the owning class's primary vtable (an "
        "override keeps its base's number), `Class::~Class` / `Class::~Class_deleting` are the "
        "complete-object (D1) and deleting (D0) destructors, and `Name@thunk-N` is a "
        "non-virtual thunk that adds -N to `this` and calls `Name`. Data labels: "
        "`Class::typeinfo`, `Class::typeinfo_name`, `Class::vtable` and "
        "`Class::vtable_for_Base` (a secondary vtable).",
        "",
    ]
    if notes:
        lines += [notes.rstrip(), ""]

    lines += ["## Hierarchy", "", "```mermaid", "classDiagram", "    direction LR"]
    for t in r.typeinfos:
        if not r.bases(t.name) and not r.derived(t.name):
            continue
        for name, b in r.bases(t.name):
            label = f" : +{b.offset}" if b.offset else ""
            lines.append(f"    {_mermaid(name)} <|-- {_mermaid(t.name)}{label}")
    lone = [t.name for t in r.typeinfos if not r.bases(t.name) and not r.derived(t.name)]
    lines += ["```", ""]
    if lone:
        lines += [
            f"Classes with no base and no derived class ({len(lone)}): "
            + ", ".join(f"`{n}`" for n in lone)
            + ".",
            "",
        ]

    lines += [
        "## Classes",
        "",
        "| Class | Bases | Derived | `type_info` | Vtable | Slots | Destructor slots |",
        "|---|---|---|---|---|---|---|",
    ]
    for t in r.typeinfos:
        vt = r.primary_vtable(t.name)
        derived = ", ".join(f"`{n}`" for n in r.derived(t.name)) or "–"
        dtor = r.destructors.get(t.name)
        lines.append(
            f"| [`{t.name}`](#{_anchor(t.name)}) | {_bases_text(r, t.name)} | {derived} "
            f"| `{hexaddr(t.address)}` ({TYPEINFO_KIND_NAMES[t.kind]}) "
            f"| {'–' if vt is None else f'`{hexaddr(vt.address)}`'} "
            f"| {'–' if vt is None else len(vt.slots)} "
            f"| {'–' if dtor is None else f'{dtor}, {dtor + 1}'} |"
        )
    lines.append("")

    _interfaces(lines, r)
    lines += ["## Vtables", ""]
    for t in r.typeinfos:
        lines += [f"### {t.name}", ""]
        facts = [
            f"`{TYPEINFO_KIND_NAMES[t.kind]}` at `{hexaddr(t.address)}`, name "
            f"`{t.mangled}` at `{hexaddr(t.name_address)}`",
            f"bases: {_bases_text(r, t.name)}",
        ]
        if t.kind == "vmi":
            facts.append(f"`__flags` {t.flags}")
        facts += r.evidence.get(t.name, [])
        lines += [f"- {f}" for f in facts] + [""]
        vts = r.vtables_of(t.name)
        if not vts:
            lines += ["No vtable in the image.", ""]
        for v in vts:
            title = "Primary vtable" if v.primary else f"Vtable for the `{r.subobject_of(v)}` base"
            lines += [
                f"{title} at `{hexaddr(v.address)}` (offset_to_top {v.offset_to_top}, address "
                f"point `{hexaddr(v.address_point)}`, {len(v.slots)} slots, followed by "
                f"{v.end}):",
                "",
                "| Slot | Target | Function | Bytes | Defined in |",
                "|---|---|---|---|---|",
            ]
            for s in v.slots:
                if s.pure:
                    method = r.method(t.name, s.index) if v.primary else None
                    what = f"pure virtual `{method}`" if method else "pure virtual"
                    lines.append(f"| {s.index} | – | {what} (`{PURE_VIRTUAL}`) | – | – |")
                    continue
                f = r.functions[s.target]
                own = "this class" if f.owner == t.name else f"`{f.owner}`"
                lines.append(
                    f"| {s.index} | `{hexaddr(s.target)}` | `{f.name}` | {f.size} | {own} |"
                )
            lines.append("")

    exc = slot_exceptions(r)
    lines += [
        "## Slot check",
        "",
        "Every slot of every vtable above is checked against `LC_FUNCTION_STARTS` "
        "(`tests/test_rtti_real.py`). The exceptions are the pure virtual slots: the compiler "
        f"leaves them 0 and dyld binds them to `{PURE_VIRTUAL}` from libstdc++, which "
        "aborts if a pure virtual function is ever called. There is no function to name.",
        "",
        f"{len(exc)} exceptions:",
        "",
        "| Class | Vtable | Slot | Slot address | Reason |",
        "|---|---|---|---|---|",
    ]
    for e in exc:
        lines.append(
            f"| `{e['class']}` | `{e['vtable']}` | {e['index']} | `{e['slot']}` | {e['reason']} |"
        )
    lines.append("")
    problems = layout_problems(r)
    lines += ["Layout cross-checks (base slot counts, secondary vtables): "]
    lines[-1] += "all consistent." if not problems else f"{len(problems)} problems:"
    lines.append("")
    lines += [f"- {p}" for p in problems]
    if problems:
        lines.append("")
    return "\n".join(lines)


def _interfaces(lines: list[str], r: Rtti) -> None:
    """One table per class that declares virtual functions besides its destructor: each
    slot it declares, against itself and every class that extends its vtable."""
    lines += [
        "## Virtual interfaces",
        "",
        "Each class below declares virtual functions besides its destructor. The table shows "
        "every slot it declares and what each derived class puts there (*inherited* = the "
        "function of the class in brackets; sizes in bytes). Method names come from "
        "localization keys the functions look up: a function of class `C` that loads "
        "`C.Method.N` keys (`ScriptExtra.strings`, see docs/ANALYSIS.md) is `C::Method`. "
        "Other slots keep their number.",
        "",
    ]
    for t in r.typeinfos:
        vt = r.primary_vtable(t.name)
        if vt is None:
            continue
        dtor = r.destructors.get(t.name)
        skip = set() if dtor is None else {dtor, dtor + 1}
        own = [
            s.index
            for s in vt.slots
            if s.index not in skip and r.introducer(t.name, s.index) == t.name
        ]
        if not own:
            continue
        family = [t.name] + [
            c
            for c in _order(r)
            if c != t.name and _primary_chain_has(r, c, t.name) and r.primary_vtable(c)
        ]
        lines += [
            f"### {t.name} interface",
            "",
            "| Slot | Method | " + " | ".join(f"`{c}`" for c in family) + " |",
            "|---|---|" + "---|" * len(family),
        ]
        for i in own:
            cells = []
            for c in family:
                s = r.primary_vtable(c).slots[i]
                if s.pure:
                    cells.append("pure")
                    continue
                f = r.functions[s.target]
                text = f"`{hexaddr(s.target)}` {f.size} B"
                cells.append(text if f.owner == c else f"*inherited* ({f.owner})")
            lines.append(f"| {i} | {r.method(t.name, i) or '–'} | " + " | ".join(cells) + " |")
        lines.append("")
        named = [(i, r.slot_names[(t.name, i)]) for i in own if (t.name, i) in r.slot_names]
        for i, (method, why) in named:
            lines.append(f"- slot {i} = `{method}`: {why}")
        if named:
            lines.append("")


def _primary_chain_has(r: Rtti, cls: str | None, ancestor: str) -> bool:
    while cls is not None:
        if cls == ancestor:
            return True
        cls = r.primary_base(cls)
    return False


def _mermaid(name: str) -> str:
    return name.replace("::", "_")


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
    r = analyze(macho.MachO(data))
    source = source_info(path, data, repo)
    outputs = {
        JSON_OUT: _dump(to_json(r, source)),
        NAMES_OUT: _dump(names_json(r)),
        MARKDOWN_OUT: to_markdown(r, source, NOTES),
    }
    for rel, text in outputs.items():
        out = repo / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text, encoding="utf-8", newline="\n")
        print(f"wrote {rel}")
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
            sys.stdout.write(_dump(to_json(r, source_info(args.slice, data))))
    except (OSError, macho.MachOError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
