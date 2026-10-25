"""The guest image as the recompiler sees it (#31 extends this into the full loader).

Built on ``mtport.macho``, the one Mach-O parser of the repository (ADR 0002, ADR 0012):
this module adds the recompiler's view (immutable bytes, function bounds, stubs), it does
not parse Mach-O itself. #31 adds the rebase and bind tables, the stub/lazy/non-lazy
pointer -> import map for all 333 imports, init functions, ObjC section ranges and the
loader manifest ``work/recomp/loader.json`` for the runtime (#43).
"""

from __future__ import annotations

import bisect
from dataclasses import dataclass
from functools import cached_property
from pathlib import Path

from mtport import macho
from recomp import REPO

DEFAULT_SLICE = REPO / "work" / "slices" / "MapleThief.armv7"


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

    @property
    def code(self) -> bytes:
        """``__TEXT`` from its first byte (guest address ``text_base``)."""
        return self._text_bytes

    @property
    def text_base(self) -> int:
        return self.text_seg.vmaddr

    @cached_property
    def function_starts(self) -> tuple[int, ...]:
        """``LC_FUNCTION_STARTS`` function pointers (Thumb bit set), sorted."""
        return tuple(sorted(self.macho.function_starts))

    def function_end(self, ptr: int) -> int:
        """The next function start after ``ptr`` (or the end of ``__text``)."""
        starts = [p & ~1 for p in self.function_starts]
        i = bisect.bisect_right(starts, ptr & ~1)
        return starts[i] if i < len(starts) else self.text.end

    def const_word(self, addr: int, size: int) -> int | None:
        """The value at ``addr`` if it lies in ``__TEXT`` (immutable), else None."""
        off = addr - self.text_base
        if size not in (1, 2, 4, 8) or off < 0 or off + size > len(self._text_bytes):
            return None
        return int.from_bytes(self._text_bytes[off : off + size], "little")

    @cached_property
    def stubs(self) -> dict[int, Stub]:
        """``__symbolstub1`` stubs by address."""
        return {
            s.address: Stub(s.address, s.symbol)
            for s in self.macho.indirect_slots
            if s.kind == "stub" and s.symbol is not None
        }

    def segment_bytes(self, name: str) -> tuple[int, bytes]:
        """``(vmaddr, file contents)`` of a segment, e.g. to map it in a test runtime."""
        seg = self.macho.segment(name)
        return seg.vmaddr, self.macho.data[seg.fileoff : seg.fileoff + seg.filesize]
