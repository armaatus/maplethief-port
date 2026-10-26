"""Lift a byte string as one function: the interface of the snippet tests (#39).

``lift_snippet(code, address)`` decodes ``code`` as Thumb code (A32 with ``arm=True``) at
guest ``address`` and returns a complete C translation unit that defines
``void <name>(mt_cpu *cpu)``. Running past the last instruction returns. The flags are
taken from and stored back to ``cpu->cpsr`` (bits 31..28, also at a trap), so a test can set
and check them. A literal-pool load from inside the snippet is folded like one from
``__TEXT``. A direct call or tail call to an address outside the snippet calls
``f_XXXXXXXX`` (function pointer, Thumb bit set for Thumb code); the harness defines those
(``Snippet.callees`` lists them). Everything else (memory, traps, ``mt_dispatch``, imports)
comes from the runtime the harness links.
"""

from __future__ import annotations

from dataclasses import dataclass

from recomp.discover import Function, discover_function
from recomp.lift.core import LiftOptions, c_name, lift_function

SNIPPET_OPTIONS = LiftOptions(flags_in=True, flags_out=True, fall_off_returns=True)


@dataclass(frozen=True)
class Snippet:
    source: str  # the C translation unit
    callees: frozenset[str]  # f_XXXXXXXX the snippet calls directly; the harness defines them
    unsupported: tuple[tuple[int, str], ...]
    problems: tuple[str, ...]


class _SnippetProgram:
    def __init__(self, code: bytes, address: int) -> None:
        self.code, self.address = code, address

    def const_word(self, addr: int, size: int) -> int | None:
        off = addr - self.address
        if off < 0 or off + size > len(self.code):
            return None
        return int.from_bytes(self.code[off : off + size], "little")

    def callee(self, ptr: int) -> str | None:
        return c_name(ptr)

    def import_at(self, addr: int) -> tuple[int, str] | None:
        return None


def snippet_function(code: bytes, address: int, arm: bool = False) -> Function:
    """``code`` discovered as one function at ``address`` (Thumb, or A32 if ``arm``)."""
    return discover_function(code, address, address | (0 if arm else 1), address + len(code))


def lift_snippet(code: bytes, address: int, name: str = "snippet", arm: bool = False) -> Snippet:
    fn = snippet_function(code, address, arm)
    program = _SnippetProgram(code, address)
    lifted = lift_function(fn, program, SNIPPET_OPTIONS, name=name)
    decls = [f"void {c}(mt_cpu *MT_RESTRICT cpu);" for c in sorted(lifted.callees)]
    head = ['#include "maplethief/recomp/recomp.h"', "", *decls, "" if decls else None]
    source = "\n".join(line for line in head if line is not None) + "\n" + lifted.source
    return Snippet(
        source,
        frozenset(lifted.callees),
        tuple(lifted.unsupported),
        tuple(lifted.problems),
    )
