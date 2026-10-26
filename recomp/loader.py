"""The guest image as the recompiler and the runtime see it (#31): the loader.

Built on ``mtport.macho``, the one Mach-O parser of the repository (ADR 0002, ADR 0012): this
module adds the recompiler's view, it does not parse Mach-O itself. The import table is
``mtport.imports.import_table`` (#11), the one implementation of "which addresses stand for
which import". :class:`Image` maps the thin armv7 slice at its link-time addresses (slide 0)
and exposes:

- the layout: ``segments`` and ``sections`` with their file <-> VM mapping (``vm_to_offset``,
  ``offset_to_vm``), the immutable ``__TEXT`` bytes (``code``, ``const_word``);
- code: ``function_starts`` (``LC_FUNCTION_STARTS``, Thumb bit kept), ``function_end``,
  ``entry_point`` (``LC_UNIXTHREAD`` pc), ``init_functions`` (``__mod_init_func``);
- dyld fixups: ``rebases`` and ``binds`` (bind, weak and lazy streams of ``LC_DYLD_INFO``);
- imports: ``imports`` (all 333 undefined symbols, each with its ``__symbolstub1`` stubs and
  ``__lazy_symbol`` / ``__nl_symbol_ptr`` pointers and its other bound words), ``stubs``,
  ``import_at`` (stub, pointer or bound word -> import);
- ``objc_sections`` (every ``__objc_*`` section and ``__cfstring``), ``problems`` (consistency
  checks) and ``manifest`` (below).

The input is untrusted data: it is parsed, never executed.

**Loader manifest.** ``python -m recomp load`` writes :meth:`Image.manifest` to
``work/recomp/loader.json`` (ADR 0014 outputs; git-ignored). The runtime (#43) reads it to map
the slice and apply its fixups. Addresses are strings ``"0x"`` + upper-case hex (``hexaddr``),
sizes and counts are integers, every list is sorted as stated, and the layout is
``ghidra_common.dump_json`` (indent 1, final newline). Schema version 1::

    {"schema": 1,
     "source": {"path": "work/slices/MapleThief.armv7", "arch": "armv7", "sha256": "..."},
     "header": {"cputype": 12, "cpusubtype": 9, "filetype": 2, "flags": "0x10085"},
     "entry_point": "0x27E0",              # LC_UNIXTHREAD pc: Thumb bit = mode (0x27E0 is ARM)
     "segments": [                          # load-command order
       {"name": "__TEXT", "vmaddr": "0x1000", "vmsize": 983040, "fileoff": 0,
        "filesize": 983040, "maxprot": 5, "initprot": 5}],   # VM_PROT_READ 1|WRITE 2|EXECUTE 4
     "sections": [                          # load-command order
       {"segment": "__DATA", "name": "__bss", "addr": "0x11AA80", "size": 1016,
        "offset": 0,                        # file offset; 0 for zero-fill sections
        "type": 1, "attributes": "0x0",     # flags & 0xFF (S_ZEROFILL 1, ...), flags & ~0xFF
        "zerofill": true}],
     "dylibs": [{"ordinal": 1, "path": "/System/Library/...", "weak": false}],
     "imports": [                           # nlist order: one per undefined symbol
       {"name": "_objc_msgSend", "library": "/usr/lib/libobjc.A.dylib", "weak": false,
        "kind": "function",                 # function | class | metaclass | address
        "stubs": ["0xF0FA8"],               # __symbolstub1 stubs (call targets)
        "lazy_pointers": ["0xF1318"],       # __lazy_symbol slots
        "non_lazy_pointers": [],            # __nl_symbol_ptr slots
        "binds": []},                       # other bound words, ascending
       {"name": "_OBJC_CLASS_$_NSNumber", "kind": "class", "stubs": [], "lazy_pointers": [],
        "non_lazy_pointers": [], "binds": ["0xFB2C4"], ...}],   # here an __objc_classrefs slot
     "binds": [                             # stream order: bind, then weak, then lazy
       {"kind": "bind", "address": "0xF1418", "symbol": "_NSLocalizedDescriptionKey",
        "ordinal": 2,                       # 1-based dylib; 0 self, -1 main executable, -2 flat
        "type": 1, "addend": 0, "flags": 0}],  # BIND_TYPE_*; BIND_SYMBOL_FLAGS_* (1 = weak import)
     "rebases": [{"address": "0x...", "type": 1}],   # stream order; REBASE_TYPE_*
     "function_starts": ["0x27E0", "0x2851", ...],   # ascending, Thumb bit kept
     "init_functions": ["0x...", ...],      # __mod_init_func order: run them in this order
     "objc_sections": [{"segment": "__DATA", "name": "__objc_classlist", "addr": "0xF14D4",
                        "size": 300}]}      # address order

A ``weak`` bind (``ordinal`` 0) is a C++ weak definition (``operator new``/``delete``) that
dyld may coalesce; the same word also has a plain bind to the library. Lazy pointers start out
pointing into ``__stub_helper``; the runtime binds them up front like non-lazy ones.
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import imports as re_imports
from mtport import macho
from mtport.names import hexaddr
from recomp import REPO

DEFAULT_SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
MANIFEST_OUT = REPO / "work" / "recomp" / "loader.json"
SCHEMA_VERSION = 1

S_ZEROFILL = 0x1
S_MOD_INIT_FUNC_POINTERS = 0x9
OBJC_PREFIX = "__objc_"
# CFString literals: ObjC objects whose isa is bound to ___CFConstantStringClassReference.
OBJC_EXTRA_SECTIONS = ("__cfstring",)

Import = re_imports.Import  # name, library, weak, kind, stubs, lazy, non_lazy, binds


@dataclass(frozen=True)
class Stub:
    """An import stub of ``__symbolstub1``: its address and the imported symbol."""

    address: int
    symbol: str


class Image:
    """A thin armv7 Mach-O mapped at its link-time addresses (slide 0)."""

    def __init__(self, data: bytes) -> None:
        self.macho = macho.MachO(data)
        text = self.macho.section("__TEXT", "__text")
        if text is None:
            raise macho.MachOError("no __TEXT,__text section")
        self.text = text
        self.text_seg = self.macho.segment("__TEXT")
        self._text_bytes = self.macho.read(self.text_seg.vmaddr, self.text_seg.filesize)

    @classmethod
    def load(cls, path: str | Path = DEFAULT_SLICE) -> Image:
        return cls(Path(path).read_bytes())

    # -- layout --------------------------------------------------------------------------

    @property
    def data(self) -> bytes:
        """The whole file."""
        return self.macho.data

    @property
    def segments(self) -> tuple[macho.Segment, ...]:
        return self.macho.segments

    @property
    def sections(self) -> tuple[macho.Section, ...]:
        return self.macho.sections

    def vm_to_offset(self, addr: int) -> int:
        """File offset of VM address ``addr``; raises for zero-fill or unmapped addresses."""
        return self.macho.vm_to_offset(addr)

    def offset_to_vm(self, offset: int) -> int:
        """VM address of file offset ``offset``; raises for bytes no segment maps."""
        return self.macho.offset_to_vm(offset)

    def segment_bytes(self, name: str) -> tuple[int, bytes]:
        """``(vmaddr, file contents)`` of a segment, e.g. to map it in a test runtime."""
        seg = self.macho.segment(name)
        return seg.vmaddr, self.macho.data[seg.fileoff : seg.fileoff + seg.filesize]

    @property
    def code(self) -> bytes:
        """``__TEXT`` from its first byte (guest address ``text_base``)."""
        return self._text_bytes

    @property
    def text_base(self) -> int:
        return self.text_seg.vmaddr

    def const_word(self, addr: int, size: int) -> int | None:
        """The value at ``addr`` if it lies in ``__TEXT`` (immutable), else None."""
        off = addr - self.text_base
        if size not in (1, 2, 4, 8) or off < 0 or off + size > len(self._text_bytes):
            return None
        return int.from_bytes(self._text_bytes[off : off + size], "little")

    # -- code ----------------------------------------------------------------------------

    @cached_property
    def function_starts(self) -> tuple[int, ...]:
        """``LC_FUNCTION_STARTS`` function pointers (Thumb bit set), sorted."""
        return tuple(sorted(self.macho.function_starts))

    @cached_property
    def _start_addrs(self) -> list[int]:
        return [p & ~1 for p in self.function_starts]

    def function_end(self, ptr: int) -> int:
        """The next function start after ``ptr`` (or the end of ``__text``)."""
        starts = self._start_addrs
        i = bisect.bisect_right(starts, ptr & ~1)
        return starts[i] if i < len(starts) else self.text.end

    @property
    def entry_point(self) -> int | None:
        """The ``LC_UNIXTHREAD`` pc (crt1 ``start``; its Thumb bit picks the mode)."""
        return self.macho.entry_point

    @cached_property
    def init_functions(self) -> tuple[int, ...]:
        """The static constructors: every word of the ``S_MOD_INIT_FUNC_POINTERS`` sections
        (``__mod_init_func``), in order. They are function pointers (Thumb bit set)."""
        out: list[int] = []
        for s in self._sections_of_type(S_MOD_INIT_FUNC_POINTERS):
            if s.size % 4:
                raise macho.MachOError(f"{s.sectname}: size 0x{s.size:X} is not whole pointers")
            out += [self.macho.u32(s.addr + 4 * i) for i in range(s.size // 4)]
        return tuple(out)

    def _sections_of_type(self, kind: int) -> list[macho.Section]:
        return [s for s in self.sections if s.flags & macho.SECTION_TYPE == kind]

    # -- dyld fixups ---------------------------------------------------------------------

    @property
    def rebases(self) -> tuple[macho.Rebase, ...]:
        """Every rebase of ``LC_DYLD_INFO`` (none at slide 0 matter, but the runtime may slide)."""
        return self.macho.rebases

    @property
    def binds(self) -> tuple[macho.Bind, ...]:
        """Every binding of the bind, weak and lazy streams, in stream order."""
        return self.macho.binds

    # -- imports -------------------------------------------------------------------------

    @cached_property
    def imports(self) -> tuple[Import, ...]:
        """Every undefined symbol in ``nlist`` order, with its stubs, pointers and binds.
        The entries are shared: treat them as read-only."""
        return tuple(re_imports.import_table(self.macho))

    @cached_property
    def _import_by_name(self) -> dict[str, Import]:
        return {imp.name: imp for imp in self.imports}

    def import_named(self, name: str) -> Import | None:
        return self._import_by_name.get(name)

    @cached_property
    def _import_by_address(self) -> dict[int, Import]:
        out: dict[int, Import] = {}
        for imp in self.imports:
            for addr in (*imp.stubs, *imp.lazy, *imp.non_lazy, *(a for a, _ in imp.binds)):
                out.setdefault(addr, imp)
        return out

    def import_at(self, addr: int) -> Import | None:
        """The import whose stub (``addr`` may carry the Thumb bit), lazy or non-lazy pointer,
        or other bound word is at ``addr``; None for any other address."""
        return self._import_by_address.get(addr & ~1)

    @cached_property
    def stubs(self) -> dict[int, Stub]:
        """``__symbolstub1`` stubs by address."""
        return {
            s.address: Stub(s.address, s.symbol)
            for s in self.macho.indirect_slots
            if s.kind == "stub" and s.symbol is not None
        }

    # -- ObjC ----------------------------------------------------------------------------

    @cached_property
    def objc_sections(self) -> tuple[macho.Section, ...]:
        """Every ``__objc_*`` section (both segments) and ``__cfstring``, in address order."""
        return tuple(
            sorted(
                (
                    s
                    for s in self.sections
                    if s.sectname.startswith(OBJC_PREFIX) or s.sectname in OBJC_EXTRA_SECTIONS
                ),
                key=lambda s: s.addr,
            )
        )

    def objc_section(self, sectname: str) -> macho.Section | None:
        for s in self.objc_sections:
            if s.sectname == sectname:
                return s
        return None

    # -- checks and the manifest ---------------------------------------------------------

    def problems(self) -> list[str]:
        """Inconsistencies the runtime would trip over (empty for a sound image)."""
        out = re_imports.check_stubs(self.macho)
        starts = set(self.function_starts)
        entry = self.entry_point
        if entry is None:
            out.append("no LC_UNIXTHREAD entry point")
        elif entry not in starts:
            out.append(f"entry point {hexaddr(entry)} is not a function start")
        for ptr in self.init_functions:
            if ptr not in starts:
                out.append(f"init function {hexaddr(ptr)} is not a function start")
        for b in self.binds:
            if b.symbol not in self._import_by_name:
                out.append(f"{b.kind} bind at {hexaddr(b.address)}: {b.symbol} is not an import")
        writable = [s for s in self.segments if s.initprot & 2]
        for r in self.rebases:
            if not any(s.contains(r.address) for s in writable):
                out.append(f"rebase at {hexaddr(r.address)} is not in a writable segment")
        return out

    def manifest(self, source: dict | None = None) -> dict:
        """The loader manifest (schema in the module docstring). ``source`` is the
        ``ghidra_common.source_info`` block of the slice (default: no path, the SHA-256)."""
        m = self.macho
        if source is None:
            source = {**gc.source_info(Path(), m.data), "path": None}
        return {
            "schema": SCHEMA_VERSION,
            "source": source,
            "header": {
                "cputype": m.cputype,
                "cpusubtype": m.cpusubtype,
                "filetype": m.filetype,
                "flags": hexaddr(m.flags),
            },
            "entry_point": None if self.entry_point is None else hexaddr(self.entry_point),
            "segments": [
                {
                    "name": s.name,
                    "vmaddr": hexaddr(s.vmaddr),
                    "vmsize": s.vmsize,
                    "fileoff": s.fileoff,
                    "filesize": s.filesize,
                    "maxprot": s.maxprot,
                    "initprot": s.initprot,
                }
                for s in self.segments
            ],
            "sections": [_section_json(s) for s in self.sections],
            "dylibs": [
                {"ordinal": i, "path": path, "weak": weak}
                for i, (path, weak) in enumerate(m.dylibs, 1)
            ],
            "imports": [
                {
                    "name": imp.name,
                    "library": imp.library,
                    "weak": imp.weak,
                    "kind": imp.kind,
                    "stubs": [hexaddr(a) for a in sorted(imp.stubs)],
                    "lazy_pointers": [hexaddr(a) for a in sorted(imp.lazy)],
                    "non_lazy_pointers": [hexaddr(a) for a in sorted(imp.non_lazy)],
                    "binds": [hexaddr(a) for a, _ in imp.binds],
                }
                for imp in self.imports
            ],
            "binds": [
                {
                    "kind": b.kind,
                    "address": hexaddr(b.address),
                    "symbol": b.symbol,
                    "ordinal": b.ordinal,
                    "type": b.type,
                    "addend": b.addend,
                    "flags": b.flags,
                }
                for b in self.binds
            ],
            "rebases": [{"address": hexaddr(r.address), "type": r.type} for r in self.rebases],
            "function_starts": [hexaddr(p) for p in self.function_starts],
            "init_functions": [hexaddr(p) for p in self.init_functions],
            "objc_sections": [
                {"segment": s.segname, "name": s.sectname, "addr": hexaddr(s.addr), "size": s.size}
                for s in self.objc_sections
            ],
        }

    def summary(self) -> dict[str, int]:
        """Counts for the command line and the tests."""
        imps = self.imports
        return {
            "segments": len(self.segments),
            "sections": len(self.sections),
            "dylibs": len(self.macho.dylibs),
            "imports": len(imps),
            "function imports": sum(bool(i.stubs) for i in imps),
            "data imports": sum(not i.stubs for i in imps),
            "stubs": len(self.stubs),
            "binds": sum(b.kind == "bind" for b in self.binds),
            "weak binds": sum(b.kind == "weak" for b in self.binds),
            "lazy binds": sum(b.kind == "lazy" for b in self.binds),
            "rebases": len(self.rebases),
            "function starts": len(self.function_starts),
            "init functions": len(self.init_functions),
            "objc sections": len(self.objc_sections),
        }


def _section_json(s: macho.Section) -> dict:
    kind = s.flags & macho.SECTION_TYPE
    return {
        "segment": s.segname,
        "name": s.sectname,
        "addr": hexaddr(s.addr),
        "size": s.size,
        "offset": s.offset,
        "type": kind,
        "attributes": hexaddr(s.flags & ~macho.SECTION_TYPE & 0xFFFFFFFF),
        "zerofill": kind == S_ZEROFILL,
    }


def write_manifest(slice_path: Path = DEFAULT_SLICE, out: Path = MANIFEST_OUT) -> Image:
    """Load ``slice_path`` and write its manifest to ``out``; return the image."""
    data = Path(slice_path).read_bytes()
    image = Image(data)
    gc.write_text(out, gc.dump_json(image.manifest(gc.source_info(Path(slice_path), data, REPO))))
    return image
