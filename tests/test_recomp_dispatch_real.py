"""The dispatch table of the real armv7 slice (#35; local only: needs the IPA).

* Every ``LC_FUNCTION_STARTS`` function (2,654 Thumb, the ARM entry stub) is in the table,
  and so is every address the program can reach indirectly that discovery knows: ObjC IMPs,
  vtable slots, init functions, the entry point, call targets and the code pointers computed
  in code (``Function.code_refs``). The only seeds outside it are weak "data" words that look
  like Thumb pointers into the middle of functions. Every function import's guest code
  address (its stubs, or its thunk address, #36) is an import thunk, so a bound pointer to
  an import (``___cxa_pure_virtual`` in a vtable, ``_realloc``) is callable.
* The generated ``dispatch.c`` and ``imports.c`` (with ``mt_gen.h``, as ``recomp lift``
  writes them), compiled with the host compiler and the runtime's lookup, find every
  function and every import thunk by its address and reject the mode-flipped and
  mid-function neighbours.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from recomp.discover import STRONG_SOURCES, Discovery, discover
from recomp.dispatch import DispatchTable, c_source
from recomp.imports import ImportTable
from recomp.loader import Image
from recomp.output import header
from recomp_harness import INCLUDE, SUPPORT, TEST_RUNTIME, host_cc, run_exe
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
MISS = 0xFFFFFFFF


@pytest.fixture(scope="module")
def image() -> Image:
    if not SLICE.is_file():
        pytest.skip("needs work/slices/MapleThief.armv7 (scripts/bootstrap)")
    return Image.load(SLICE)


@pytest.fixture(scope="module")
def found(image: Image) -> Discovery:
    return discover(image)


@pytest.fixture(scope="module")
def imports(image: Image) -> ImportTable:
    return ImportTable.from_image(image)


@pytest.fixture(scope="module")
def table(image: Image) -> DispatchTable:
    return DispatchTable.build(image.function_starts)


def test_every_function_start_is_in_the_table(image: Image, table: DispatchTable) -> None:
    starts = list(image.function_starts)
    assert len(starts) == 2655
    for p in starts:
        e = table.lookup(p)
        assert e is not None and e.fn == f"f_{p:08X}", hex(p)
    assert sum(p & 1 for p in starts) == 2654
    assert [p for p in starts if not p & 1] == [0x27E0]  # the ARM entry stub, even
    assert len(table.entries) == len(starts)
    print(
        f"dispatch: {len(table.entries)} functions, {table.buckets} buckets of "
        f"{1 << table.shift} B ({2 * (table.buckets + 1)} B index), "
        f"at most {table.max_per_bucket()} per bucket"
    )


def test_every_function_import_is_callable_through_its_thunk(
    image: Image, imports: ImportTable, table: DispatchTable
) -> None:
    thunks = dict(imports.thunks)
    for addr in image.stubs:
        assert addr in thunks and table.lookup(addr) is None, hex(addr)
    functions = [e for e in imports.entries if e.function]
    for e in functions:
        assert thunks.get(e.addr) == e.id, e.name
    by_id = {e.id: e for e in imports.entries}
    bound = {b.id for b in imports.binds if by_id[b.id].function}
    assert {"___cxa_pure_virtual", "_realloc"} <= {by_id[i].name for i in bound}
    print(
        f"dispatch: {len(thunks)} import thunks ({len(image.stubs)} stubs), "
        f"{len(functions)} function imports, {len(bound)} of them bound to data pointers"
    )


def test_every_indirect_target_discovery_knows_is_in_the_table(
    image: Image, found: Discovery, table: DispatchTable
) -> None:
    strong = {p: s for p, s in found.seeds.items() if s & STRONG_SOURCES}
    missing = sorted(p for p in strong if table.lookup(p) is None)
    assert not missing, [f"{p:#x} {sorted(found.seeds[p])}" for p in missing]
    for source in ("objc", "vtable", "init", "entry", "call"):
        assert any(source in s for s in strong.values()), source
    text = image.text
    refs = {v for f in found.functions.values() for v in f.code_refs.values()}
    code_refs = sorted(v for v in refs if text.contains(v & ~1))
    assert code_refs, "no code pointers computed in code"
    assert not [hex(v) for v in code_refs if table.lookup(v) is None]
    # A seed that is not a function is an odd data word that only looks like a pointer.
    others = {p: s for p, s in found.seeds.items() if table.lookup(p) is None}
    assert all(s == {"data"} for s in others.values()), others
    for p in others:
        fn = found.containing(p & ~1)
        assert fn is not None and p & ~1 != fn.start, hex(p)
    print(
        f"dispatch: {len(strong)} strong seeds and {len(code_refs)} code pointers in the "
        f"table; {len(others)} weak data seeds are not functions"
    )


def test_generated_dispatch_c_finds_every_function_and_import(
    image: Image, imports: ImportTable, table: DispatchTable, tmp_path: Path
) -> None:
    ptrs = list(image.function_starts)
    (tmp_path / "mt_gen.h").write_text(header(ptrs, imports), encoding="utf-8")
    (tmp_path / "dispatch.c").write_text(c_source(ptrs), encoding="utf-8")
    (tmp_path / "imports.c").write_text(imports.c_source(), encoding="utf-8")
    # Expected r0 after calling what the lookup returns: the function pointer for a
    # function, 0x80000000 + id for an import thunk, MISS for no function.
    thunks = dict(imports.thunks)
    probes: dict[int, int] = {}
    for addr in [e.addr for e in table.entries] + list(thunks):
        for p in (addr, addr ^ 1, addr + 2, (addr & ~1) + 4):
            p &= 0xFFFFFFFF
            hit = table.lookup(p)
            if hit is not None:
                probes[p] = hit.addr
            elif p in thunks:
                probes[p] = 0x80000000 + thunks[p]
            else:
                probes[p] = MISS
    for p in (0, 1, 0x27DF, 0xFFFFFFF1, image.text.end + 1):
        probes[p] = MISS
    rows = ",\n".join(f"    {{0x{p:08X}u, 0x{v:08X}u}}" for p, v in sorted(probes.items()))
    lines = ['#include "mt_gen.h"', '#include "maplethief/recomp/dispatch.h"', "#include <stdio.h>"]
    lines += [
        f"void f_{p:08X}(mt_cpu *MT_RESTRICT cpu) {{ cpu->r[0] = 0x{p:08X}u; }}" for p in ptrs
    ]
    lines += [
        "static void imp(mt_cpu *cpu, uint32_t id) { cpu->r[0] = 0x80000000u + id; }",
        f"static const uint32_t probes[{len(probes)}][2] = {{",
        rows,
        "};",
        "int main(void) {",
        "    static mt_cpu cpu;",
        "    uint32_t i, bad = 0, hits = 0;",
        "    for (i = 0; i < MT_IMPORT_COUNT; i++) {",
        "        mt_import_table[i] = imp;",
        "    }",
        f"    for (i = 0; i < {len(probes)}u; i++) {{",
        "        mt_guest_fn fn = mt_fn_lookup(probes[i][0]);",
        "        cpu.r[0] = 0xFFFFFFFFu;",
        "        if (fn != NULL) {",
        "            fn(&cpu);",
        "            hits++;",
        "        }",
        "        if (cpu.r[0] != probes[i][1]) {",
        '            printf("bad 0x%08X: 0x%08X\\n", probes[i][0], cpu.r[0]);',
        "            bad++;",
        "        }",
        "    }",
        '    printf("probes=%u hits=%u bad=%u\\n", i, hits, bad);',
        "    return bad != 0;",
        "}",
        "",
    ]
    (tmp_path / "main.c").write_text("\n".join(lines), encoding="utf-8")
    exe = tmp_path / ("dispatch.exe" if sys.platform == "win32" else "dispatch")
    sources = [tmp_path / n for n in ("main.c", "dispatch.c", "imports.c")] + [TEST_RUNTIME]
    host_cc().build(sources, exe, [INCLUDE, SUPPORT, tmp_path])
    status, out = run_exe(exe)
    assert status == 0, out[-3000:]
    hits = sum(v != MISS for v in probes.values())
    assert out.strip().endswith(f"probes={len(probes)} hits={hits} bad=0"), out[-3000:]
    assert hits == len(table.entries) + len(thunks)
    print(f"dispatch: C lookup over {len(probes)} probes, {hits} hits, no mismatch")
