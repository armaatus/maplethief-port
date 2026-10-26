"""The output stage (#38, ADR 0021) on synthetic programs: the address-cell partition, write
if changed, gen.cmake, and a synthetic program built through cmake/GeneratedCode.cmake with
the host compiler (MSVC cl on the Windows runner), including how many translation units an
incremental build recompiles. No game code."""

from __future__ import annotations

import os
import random
import subprocess
from pathlib import Path

import pytest

from genbuild import gen_build
from recomp.discover import discover_function
from recomp.imports import ImportTable
from recomp.lift.core import Lifted, c_name, lift_function
from recomp.output import (
    DEFAULT_UNITS,
    gen_cmake,
    partition,
    unit_name,
    write_if_changed,
    write_program,
)
from thumbasm import Thumb

IMPORTS = ImportTable(("___udivsi3",), {0x1F00: 0})


def fake(ptr: int, size: int = 100, tag: str = "") -> Lifted:
    """A compilable stand-in for one lifted function, padded to about ``size`` bytes."""
    body = f"void {c_name(ptr)}(mt_cpu *MT_RESTRICT cpu)\n{{\n    (void)cpu;\n}}\n"
    pad = max(0, size - len(body) - 8)
    return Lifted(ptr, c_name(ptr), f"/* {tag}{'x' * pad} */\n{body}")


def tiled(count: int, seed: int = 1, base: int = 0x10000) -> tuple[list[Lifted], tuple[int, int]]:
    """``count`` functions tiling an address range, guest size ~ C size, as in __text."""
    rng = random.Random(seed)
    out, addr = [], base
    for _ in range(count):
        size = rng.choice([8, 16, 40, 100, 400, 2000])
        out.append(fake(addr | 1, size * 40))
        addr += size
    return out, (base, addr)


def backdate(directory: Path) -> dict[str, int]:
    """Set every file's mtime 100 s into the past; returns name -> mtime (ns)."""
    stamps = {}
    for f in directory.iterdir():
        st = f.stat()
        os.utime(f, ns=(st.st_atime_ns - 10**11, st.st_mtime_ns - 10**11))
        stamps[f.name] = f.stat().st_mtime_ns
    return stamps


def rewritten(directory: Path, stamps: dict[str, int]) -> set[str]:
    files = {f.name: f.stat().st_mtime_ns for f in directory.iterdir()}
    return {n for n, t in files.items() if stamps.get(n) != t} | (stamps.keys() - files.keys())


# -- partition -----------------------------------------------------------------------------


def test_partition_cuts_the_span_into_equal_address_cells() -> None:
    ptrs = [0x1000 + 0x10 * i + 1 for i in range(64)]  # Thumb pointers, 16 bytes apart
    cells = partition(ptrs, 8, (0x1000, 0x1400))
    assert sorted(cells) == list(range(8))
    assert all(len(c) == 8 for c in cells.values())
    assert cells[0][0] == 0x1001 and cells[7][-1] == 0x13F1
    assert [p for u in sorted(cells) for p in cells[u]] == ptrs  # address order, contiguous


def test_partition_is_deterministic_and_order_independent() -> None:
    funcs, span = tiled(500)
    ptrs = [f.ptr for f in funcs]
    shuffled = ptrs[:]
    random.Random(7).shuffle(shuffled)
    assert (
        partition(ptrs, 32, span) == partition(shuffled, 32, span) == partition(ptrs * 2, 32, span)
    )


def test_adding_or_removing_a_function_moves_no_other_function() -> None:
    funcs, span = tiled(500)
    ptrs = [f.ptr for f in funcs]
    before = {p: u for u, c in partition(ptrs, 32, span).items() for p in c}
    for change in (ptrs[:250] + ptrs[251:], ptrs + [ptrs[100] + 2]):
        after = {p: u for u, c in partition(change, 32, span).items() for p in c}
        assert all(after[p] == before[p] for p in after if p in before)


def test_partition_edges() -> None:
    assert partition([], 8) == {}
    assert partition([0x2001], 8) == {0: [0x2001]}
    # outside the span: the first or last cell; the default span is the functions' own range
    assert partition([0x10, 0x5001], 4, (0x1000, 0x2000)) == {0: [0x10], 3: [0x5001]}
    assert partition([0x1000, 0x1801, 0x1FFF], 2) == {0: [0x1000], 1: [0x1801, 0x1FFF]}
    for bad in (0, 1001):
        with pytest.raises(ValueError):
            partition([0x1001], bad)
    with pytest.raises(ValueError):
        partition([0x1001], 4, (0x2000, 0x2000))


def test_cells_are_balanced_by_guest_size() -> None:
    """With C size proportional to guest size, a cell holds at most its share plus the
    biggest function (which no partition can split)."""
    funcs, span = tiled(2655, seed=3)
    sizes = {f.ptr: len(f.source) for f in funcs}
    cells = partition(sizes, DEFAULT_UNITS, span)
    per_cell = [sum(sizes[p] for p in c) for c in cells.values()]
    mean = sum(sizes.values()) / DEFAULT_UNITS
    assert len(cells) == DEFAULT_UNITS
    assert max(per_cell) <= 1.15 * mean + max(sizes.values())
    assert min(per_cell) >= 0.85 * mean - max(sizes.values())


# -- write_program ---------------------------------------------------------------------------


def test_write_program_writes_every_file_once(tmp_path) -> None:
    funcs, span = tiled(300)
    sources = write_program(tmp_path, funcs, IMPORTS, 16, span)
    names = [s.name for s in sources]
    assert names[-2:] == ["imports.c", "dispatch.c"]
    assert names[:-2] == [unit_name(u) for u in sorted(partition([f.ptr for f in funcs], 16, span))]
    assert {f.name for f in tmp_path.iterdir()} == {*names, "mt_gen.h", "gen.cmake"}
    text = "".join((tmp_path / n).read_text(encoding="utf-8") for n in names[:-2])
    assert all(text.count(f"void {f.name}(") == 1 for f in funcs)
    assert all(
        (tmp_path / n).read_text(encoding="utf-8").startswith('#include "mt_gen.h"\n')
        for n in names[:-2]
    )
    assert (tmp_path / "gen.cmake").read_text(encoding="utf-8") == gen_cmake(names)
    stamps = backdate(tmp_path)
    assert write_program(tmp_path, funcs, IMPORTS, 16, span) == sources
    assert rewritten(tmp_path, stamps) == set()  # nothing changed: no file touched


@pytest.mark.parametrize("index", [0, 137, 299])
@pytest.mark.parametrize("factor", [0.0, 0.5, 3.0, 50.0])
def test_changing_one_function_rewrites_exactly_one_unit(tmp_path, index: int, factor: float):
    """Perturbing one function's C, whatever its new size, rewrites only its own unit."""
    funcs, span = tiled(300)
    write_program(tmp_path, funcs, IMPORTS, 16, span)
    stamps = backdate(tmp_path)
    old = funcs[index]
    funcs[index] = fake(old.ptr, int(len(old.source) * factor) + 1, tag="changed ")
    write_program(tmp_path, funcs, IMPORTS, 16, span)
    (unit,) = [u for u, c in partition([f.ptr for f in funcs], 16, span).items() if old.ptr in c]
    assert rewritten(tmp_path, stamps) == {unit_name(unit)}


def test_adding_a_function_rewrites_its_unit_and_the_lists(tmp_path) -> None:
    funcs, span = tiled(300)
    write_program(tmp_path, funcs, IMPORTS, 16, span)
    stamps = backdate(tmp_path)
    new = fake(funcs[150].ptr + 2)
    write_program(tmp_path, [*funcs, new], IMPORTS, 16, span)
    (unit,) = [u for u, c in partition([new.ptr], 16, span).items()]
    assert rewritten(tmp_path, stamps) == {unit_name(unit), "mt_gen.h", "dispatch.c"}


def test_fewer_units_remove_stale_files(tmp_path) -> None:
    funcs, span = tiled(300)
    write_program(tmp_path, funcs, IMPORTS, 16, span)
    (tmp_path / "fn_notes.c").write_text("/* not ours */\n", encoding="utf-8")
    sources = write_program(tmp_path, funcs, IMPORTS, 4, span)
    assert sorted(f.name for f in tmp_path.glob("fn_*.c")) == [
        "fn_000.c", "fn_001.c", "fn_002.c", "fn_003.c", "fn_notes.c",
    ]  # fmt: skip
    assert "fn_004.c" not in (tmp_path / "gen.cmake").read_text(encoding="utf-8")
    assert len(sources) == 6


def test_write_if_changed(tmp_path) -> None:
    path = tmp_path / "sub" / "a.c"
    assert write_if_changed(path, "int x;\n") is True
    assert write_if_changed(path, "int x;\n") is False
    assert write_if_changed(path, "int y;\n") is True
    assert path.read_bytes() == b"int y;\n"


# -- the synthetic program, built through cmake/GeneratedCode.cmake ----------------------------

BASE, END = 0x1000, 0x1500
F0, F1, F2, F3, F4 = 0x1001, 0x1101, 0x1201, 0x1301, 0x1401
STUB = 0x1F00  # ___udivsi3, an import stub (not lifted)


def synthetic_code(bump: int) -> bytes:
    """Five Thumb functions, one per 0x100 bytes: a leaf, a direct call into another unit,
    an indirect call through dispatch, an import call and a tail call."""
    t = Thumb(BASE).movw(0, 0x1234).bx_lr()  # F0: return 0x1234
    t.raw(b"\0" * (F1 - 1 - t.here))
    t.push_r7_lr().bl(F0).adds(0, 0, bump).pop_r7_pc()  # F1: F0() + bump
    t.raw(b"\0" * (F2 - 1 - t.here))
    t.push_r7_lr().movw(3, F1).h(0x4798).adds(0, 0, 2).pop_r7_pc()  # F2: (*F1)() + 2
    t.raw(b"\0" * (F3 - 1 - t.here))
    t.push_r7_lr().movs(0, 200).movs(1, 7).bl(STUB).pop_r7_pc()  # F3: 200 / 7
    t.raw(b"\0" * (F4 - 1 - t.here))
    t.b_w(F2)  # F4: tail call to F2
    return bytes(t.code) + b"\0" * (END - t.here)


class SyntheticProgram:
    def __init__(self, code: bytes) -> None:
        self.code = code

    def const_word(self, addr: int, size: int) -> int | None:
        off = addr - BASE
        if 0 <= off and off + size <= len(self.code):
            return int.from_bytes(self.code[off : off + size], "little")
        return None

    def callee(self, ptr: int) -> str | None:
        return c_name(ptr) if ptr in (F0, F1, F2, F3, F4) else None

    def import_at(self, addr: int) -> tuple[int, str] | None:
        return IMPORTS.at(addr)


def synthetic_program(bump: int) -> list[Lifted]:
    code = synthetic_code(bump)
    program = SyntheticProgram(code)
    out = []
    for ptr in (F0, F1, F2, F3, F4):
        lifted = lift_function(discover_function(code, BASE, ptr, (ptr & ~1) + 0x100), program)
        assert lifted.clean, lifted.problems
        out.append(lifted)
    return out


MAIN = """#include <stdio.h>
#include "test_runtime.h"
#include "mt_gen.h"

static mt_cpu cpu;

static uint32_t run(mt_guest_fn fn)
{
    cpu.r[13] = 0x10000u - 64u;
    cpu.r[14] = MT_TEST_RETURN;
    fn(&cpu);
    return cpu.r[0];
}

int main(void)
{
    mt_test_init(&cpu, 0x10000u, MT_IMPORT_COUNT);
    printf("f0=%u f1=%u f2=%u f3=%u f4=%u count=%u\\n", run(f_00001001), run(f_00001101),
           run(f_00001201), run(f_00001301), run(f_00001401), mt_fn_count);
    return 0;
}
"""


def run_program(exe: Path) -> str:
    done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stdout
    return done.stdout.strip()


def test_synthetic_program_builds_and_rebuilds_one_unit(tmp_path) -> None:
    """Build the synthetic program through gen.cmake, change one function's output, and
    check that the incremental build compiles exactly that function's unit."""
    gen, main = tmp_path / "gen", tmp_path / "main.c"
    main.write_text(MAIN, encoding="utf-8")
    sources = write_program(gen, synthetic_program(1), IMPORTS, 5, (BASE, END))
    assert [s.name for s in sources] == [
        *(unit_name(u) for u in range(5)),
        "imports.c",
        "dispatch.c",
    ]
    build = gen_build(tmp_path / "build", gen, main)
    assert sorted(build.pending()) == sorted(
        ["main.c", "test_runtime.c", *(s.name for s in sources)]
    )
    build.build()
    expect = "f0={0} f1={1} f2={2} f3=28 f4={2} count=5"
    assert run_program(build.exe) == expect.format(0x1234, 0x1235, 0x1237)

    write_program(gen, synthetic_program(1), IMPORTS, 5, (BASE, END))
    assert build.pending() == []  # the same output: nothing to compile

    write_program(gen, synthetic_program(3), IMPORTS, 5, (BASE, END))  # F1 changes
    assert build.pending() == [unit_name(1)]
    log = build.build()
    assert log.count("Building C object") == 1, log
    assert run_program(build.exe) == expect.format(0x1234, 0x1237, 0x1239)
