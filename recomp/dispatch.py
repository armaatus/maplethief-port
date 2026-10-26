"""The guest address -> host function table, ``work/gen/dispatch.c`` (ADR 0014, #35).

Every indirect call or jump (``blx rN``, ``bx rN``, ``ldr pc``, ``mov pc``, a vtable slot, an
ObjC IMP, a callback) goes through ``mt_dispatch`` (calls.h), which looks the guest target
up in this table (``runtime/src/recomp/dispatch.c``; declarations in
``runtime/include/maplethief/recomp/dispatch.h``).

* **Keys** are guest function pointers: Thumb code with bit 0 set, ARM code (the entry stub,
  the ``__symbolstub1`` stubs) with it clear. A Thumb function reached in ARM state or an
  ARM one in Thumb state is not found, and the runtime says so (``mt_dispatch_describe``).
* **Entries**: one per lifted function (``f_XXXXXXXX``), and one per import stub, a static
  thunk that calls the import (``MT_CALL_IMPORT``), so a pointer to a stub is callable.
* **Index**: ``mt_fn_idx`` splits the address range into buckets of ``2**BUCKET_SHIFT``
  bytes; ``first[b]`` is the first table entry at or after bucket ``b``, so a lookup reads
  one index word and compares at most a few entries. On the armv7 slice that is 61,058
  buckets (122 KB) for 2,872 entries and at most 4 entries per bucket; the measurement that
  chose it is in the ADR 0014 addendum.

:func:`c_source` writes the whole file (``recomp/output.py`` calls it); the snippet harness
builds its small tables with :meth:`DispatchTable.data_source`, so both go through the same
index and lookup.
"""

from __future__ import annotations

import bisect
from collections import Counter
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field

from recomp.lift.core import c_name

BUCKET_SHIFT = 4  # 16-byte buckets
MAX_ENTRIES = 0xFFFF  # the index is uint16_t


@dataclass(frozen=True)
class Entry:
    """``addr``: the guest function pointer; ``fn``: the C function it dispatches to;
    ``import_id``: set for an import stub (``fn`` is then a thunk dispatch.c defines)."""

    addr: int
    fn: str
    import_id: int | None = None
    comment: str = ""


@dataclass(frozen=True)
class DispatchTable:
    entries: tuple[Entry, ...]
    shift: int = BUCKET_SHIFT
    end: int | None = None  # one past the last code address; default: last entry + 4
    base: int = field(init=False)
    first: tuple[int, ...] = field(init=False)

    def __post_init__(self) -> None:
        addrs = [e.addr for e in self.entries]
        if addrs != sorted(addrs):
            raise ValueError("dispatch entries must be sorted by address")
        plain = [a & ~1 for a in addrs]
        dup = sorted(a for a, n in Counter(plain).items() if n > 1)
        if dup:
            raise ValueError(f"two dispatch entries at {', '.join(f'{a:#x}' for a in dup)}")
        if len(addrs) > MAX_ENTRIES:
            raise ValueError(f"{len(addrs)} dispatch entries, the index holds {MAX_ENTRIES}")
        for e in self.entries:
            if not e.addr & 1 and e.addr & 2:
                raise ValueError(f"ARM function pointer {e.addr:#x} is not 4-byte aligned")
        base = plain[0] if plain else 0
        buckets = ((plain[-1] - base) >> self.shift) + 1 if plain else 0
        offsets = [(a - base) >> self.shift for a in plain]
        first = tuple(bisect.bisect_left(offsets, b) for b in range(buckets + 1))
        end = self.end if self.end is not None else (plain[-1] + 4 if plain else 0)
        if plain and end <= plain[-1]:
            raise ValueError(f"end {end:#x} is not past the last entry {plain[-1]:#x}")
        object.__setattr__(self, "base", base)
        object.__setattr__(self, "first", first)
        object.__setattr__(self, "end", end)

    @classmethod
    def build(
        cls,
        functions: Iterable[int],
        stubs: Mapping[int, int] | None = None,
        names: Iterable[str] = (),
        end: int | None = None,
    ) -> DispatchTable:
        """``functions``: lifted function pointers; ``stubs``: import stub address -> import
        id (``ImportTable.by_stub``), with ``names[id]`` for the comments; ``end``: one past
        the last code address (default: 4 bytes after the last entry, right for the stubs,
        which come last in the image)."""
        names = tuple(names)
        entries = {p: Entry(p, c_name(p)) for p in set(functions)}
        for addr, iid in (stubs or {}).items():
            name = names[iid] if iid < len(names) else ""
            entries[addr] = Entry(addr, f"mt_stub_{addr:08X}", iid, name)
        return cls(tuple(entries[a] for a in sorted(entries)), end=end)

    @property
    def buckets(self) -> int:
        return len(self.first) - 1 if self.first else 0

    def lookup(self, target: int) -> Entry | None:
        """What ``mt_fn_lookup`` returns for ``target`` (the same index walk, in Python)."""
        bucket = (((target & ~1) - self.base) & 0xFFFFFFFF) >> self.shift
        if bucket >= self.buckets:
            return None
        for e in self.entries[self.first[bucket] : self.first[bucket + 1]]:
            if e.addr == target:
                return e
        return None

    def max_per_bucket(self) -> int:
        return max((self.first[b + 1] - self.first[b] for b in range(self.buckets)), default=0)

    def data_source(self) -> str:
        """The table, the index and the stub thunks as C definitions; expects
        ``maplethief/recomp/dispatch.h`` and the function prototypes to be in scope."""
        lines: list[str] = []
        for e in self.entries:
            if e.import_id is not None:
                note = f" /* {e.comment} */" if e.comment else ""
                lines.append(
                    f"static void {e.fn}(mt_cpu *MT_RESTRICT cpu) "
                    f"{{ MT_CALL_IMPORT({e.import_id}u);{note} }}"
                )
        if lines:
            lines.append("")
        rows = [f"    {{0x{e.addr:08X}u, {e.fn}}}," for e in self.entries] or ["    {0u, 0},"]
        lines += ["const mt_fn_entry mt_fn_table[] = {", *rows, "};"]
        lines.append(f"const uint32_t mt_fn_count = {len(self.entries)}u;")
        lines.append("")
        first = self.first or (0,)
        lines.append(f"static const uint16_t mt_fn_first[{len(first)}] = {{")
        for i in range(0, len(first), 16):
            lines.append("    " + " ".join(f"{v}u," for v in first[i : i + 16]))
        lines.append("};")
        lines.append(
            f"const mt_fn_index mt_fn_idx = {{0x{self.base:08X}u, {self.shift}u, "
            f"{self.buckets}u, 0x{self.end:08X}u, mt_fn_first}};"
        )
        lines.append("")
        return "\n".join(lines)


def c_source(
    ptrs: Iterable[int], stubs: Mapping[int, int] | None = None, names: Iterable[str] = ()
) -> str:
    """``work/gen/dispatch.c`` for the lifted functions ``ptrs`` and the import stubs
    ``stubs`` (address -> import id, with ``names[id]``)."""
    table = DispatchTable.build(ptrs, stubs, names)
    head = [
        "/* Generated by recomp (ADR 0014, #35): guest function pointer -> host function.",
        f" * {len(table.entries)} entries, {table.buckets} buckets of {1 << table.shift} bytes,"
        f" at most {table.max_per_bucket()} entries per bucket. Never edit; never commit. */",
        '#include "mt_gen.h"',
        '#include "maplethief/recomp/dispatch.h"',
        "",
    ]
    return "\n".join(head) + table.data_source()
