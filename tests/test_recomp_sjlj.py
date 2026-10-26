"""SjLj exceptions in recompiled code (#37, ADR 0020): synthetic programs, every OS.

Each ``tests/recomp/sjlj/<name>.sjlj`` file is a small hand-written guest program in the
snippet format (``tests/recomp/README.md``): several functions, each starting at an
``fn_<name>:`` label (the first one is called), with SjLj contexts built the way the game's
LLVM-GCC code or Chartboost's clang code builds them, and LSDAs as ``[in]`` memory. Every
function is discovered and lifted on its own (resume hooks and labels included), linked
with ``runtime/src/sjlj.c`` (the unwinder) and the test runtime, run, and compared with
``[out]``: the named registers and memory words, plus r4-r11 and sp unchanged (the
callee-saved registers must survive the longjmps), or ``trap = <kind> <pc>``.

The guest calls the unwinder through these external functions (defined in :data:`PRELUDE`):

    ext_9000  _Unwind_SjLj_Register(fc)        ext_9040  objc_begin_catch(ue) -> object
    ext_9010  _Unwind_SjLj_Unregister(fc)      ext_9050  objc_end_catch()
    ext_9020  _Unwind_SjLj_Resume(ue)          ext_9060  objc_exception_rethrow()
    ext_9030  throw(type = r0, object = r1)    ext_9070  log(r0): appended at DATA+4, count at DATA
    ext_9080  std::terminate()                 ext_9090  log(live exceptions * 0x100 + caught)

and the personality is 0x9101 (``__gxx_personality_sj0``) or 0x9111 (``__objc_personality_v0``).
The bytes are checked by Capstone (everywhere) and clang (where it has the ARM target), as
for the snippet suite. No game code.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

import recomp_snippets as rs
from recomp import exceptions as ex
from recomp.discover import Function, discover_function
from recomp.dispatch import DispatchTable
from recomp.lift.core import LiftOptions, c_name, lift_function
from recomp_harness import INCLUDE, SUPPORT, TEST_RUNTIME, WINDOW, host_cc, parse_state, run_exe
from repo_paths import REPO

SJLJ_DIR = REPO / "tests" / "recomp" / "sjlj"
SJLJ_C = REPO / "runtime" / "src" / "sjlj.c"
FILES = sorted(SJLJ_DIR.glob("*.sjlj"))
IDS = [p.stem for p in FILES]
NORETURN = (0x9020, 0x9030, 0x9060, 0x9080)  # Resume, throw, rethrow, terminate
GXX, OBJC = 0x9101, 0x9111

PRELUDE = r"""
#define MT_SJLJ_TEST_UE 0x1F0000u
#define MT_SJLJ_TEST_LOG 0x100000u
static uint32_t mt_test_throws;
static void mt_test_log(mt_cpu *cpu, uint32_t value) {
    uint32_t n = mt_ld32(cpu, MT_SJLJ_TEST_LOG, 0u);
    mt_st32(cpu, MT_SJLJ_TEST_LOG + 4u + 4u * n, value, 0u);
    mt_st32(cpu, MT_SJLJ_TEST_LOG, n + 1u, 0u);
}
void f_00009001(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_register(cpu, 0u); }
void f_00009011(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_unregister(cpu, 0u); }
void f_00009021(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_resume(cpu, 0u); }
void f_00009031(mt_cpu *MT_RESTRICT cpu) {
    uint32_t ue = MT_SJLJ_TEST_UE + 0x20u * (mt_test_throws++ & 7u);
    mt_sjlj_throw(cpu, ue, cpu->r[0], cpu->r[1]);
}
void f_00009041(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_objc_begin_catch(cpu, 0u); }
void f_00009051(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_objc_end_catch(cpu, 0u); }
void f_00009061(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_objc_exception_rethrow(cpu, 0u); }
void f_00009071(mt_cpu *MT_RESTRICT cpu) { mt_test_log(cpu, cpu->r[0]); }
void f_00009081(mt_cpu *MT_RESTRICT cpu) { mt_sjlj_hle_terminate(cpu, 0u); }
void f_00009091(mt_cpu *MT_RESTRICT cpu) {
    const mt_sjlj_state *s = mt_sjlj_state_of(cpu);
    uint32_t i, live = 0u;
    for (i = 0; i < MT_SJLJ_MAX_EXCEPTIONS; i++) {
        if (s->exceptions[i].ue != 0u) {
            live++;
        }
    }
    mt_test_log(cpu, live * 0x100u + s->caught_count);
}
"""


@dataclass(frozen=True)
class Program:
    snip: rs.Snippet
    functions: list[Function]  # discovered, in address order; the first one is called

    @property
    def code(self) -> bytes:
        return self.snip.code


class _Lift:
    """:class:`recomp.lift.core.Program` over the listing: everything is a C function."""

    def __init__(self, code: bytes) -> None:
        self.code = code

    def const_word(self, addr: int, size: int) -> int | None:
        off = addr - rs.SNIPPET_BASE
        if off < 0 or off + size > len(self.code):
            return None
        return int.from_bytes(self.code[off : off + size], "little")

    def callee(self, ptr: int) -> str | None:
        return c_name(ptr)

    def import_at(self, addr: int) -> tuple[int, str] | None:
        return None


def load(path: Path) -> Program:
    snip = rs.load(path)
    starts = sorted(a for n, a in snip.labels.items() if n.startswith("fn_"))
    assert starts and starts[0] == rs.SNIPPET_BASE, "the code starts with an fn_ label"
    ends = [*starts[1:], rs.SNIPPET_BASE + len(snip.code)]
    fns = [
        discover_function(snip.code, rs.SNIPPET_BASE, s | 1, e, noreturn=NORETURN)
        for s, e in zip(starts, ends, strict=True)
    ]
    return Program(snip, fns)


def lift(prog: Program):
    lifted = [lift_function(fn, _Lift(prog.code), LiftOptions()) for fn in prog.functions]
    for f in lifted:
        assert not f.unsupported and not f.problems, (f.name, f.unsupported, f.problems)
    return lifted


def run(prog: Program, tmp_path: Path) -> dict[str, int]:
    snip = prog.snip
    lifted = lift(prog)
    names = sorted({c for f in lifted for c in f.callees} | {f.name for f in lifted})
    decls = [f"void {n}(mt_cpu *MT_RESTRICT cpu);" for n in names]
    unit = ['#include "maplethief/recomp/recomp.h"', "", *decls, ""] + [f.source for f in lifted]
    (tmp_path / "program.c").write_text("\n".join(unit), encoding="utf-8")
    read = {f"m{i}": (addr, size) for i, (addr, size, _) in enumerate(snip.mem_out)}
    memory = {rs.SNIPPET_BASE: snip.code, **snip.mem_in}
    lines = [
        '#include "test_runtime.h"',
        '#include "maplethief/sjlj.h"',
        '#include "maplethief/recomp/dispatch.h"',
        "#include <stdio.h>",
        "",
        *decls,
        PRELUDE,
        "mt_import_fn mt_import_table[1];",
        'const char *const mt_import_names[1] = {""};',
        "const mt_fn_entry mt_import_thunks[1] = {{0u, 0}};",
        "const uint32_t mt_import_thunk_count = 0u;",
        DispatchTable(()).data_source(),
        "static mt_cpu cpu;",
        "static jmp_buf trap_jmp;",
        "",
        "int main(void) {",
        f"    mt_test_init(&cpu, 0x{WINDOW:X}u, 0u);",
        "    mt_sjlj_init(mt_sjlj_state_of(&cpu));",
        f"    mt_sjlj_set_personalities(mt_sjlj_state_of(&cpu), 0x{GXX:X}u, 0x{OBJC:X}u);",
    ]
    for addr, data in sorted(memory.items()):
        for i, b in enumerate(data):
            lines.append(f"    cpu.mem[0x{addr + i:X}u] = 0x{b:02X}u;")
    for key, value in sorted(snip.regs_in.items()):
        lines.append(f"    cpu.r[{int(key[1:])}] = 0x{value:08X}u;")
    lines += [
        "    mt_test_trap_jmp = &trap_jmp;",
        "    if (setjmp(trap_jmp) == 0) {",
        f"        {lifted[0].name}(&cpu);",
        '        printf("returned=0x1\\n");',
        "    }",
        "    mt_test_print_state(&cpu);",
    ]
    for name, (addr, size) in sorted(read.items()):
        fn = {1: "mt_ld8", 2: "mt_ld16", 4: "mt_ld32"}[size]
        lines.append(f'    printf("{name}=0x%08X\\n", {fn}(&cpu, 0x{addr:X}u, 0u));')
    lines += ["    return 0;", "}", ""]
    (tmp_path / "main.c").write_text("\n".join(lines), encoding="utf-8")
    exe = tmp_path / ("program.exe" if sys.platform == "win32" else "program")
    sources = [tmp_path / "main.c", tmp_path / "program.c", TEST_RUNTIME, SJLJ_C]
    host_cc().build(sources, exe, [INCLUDE, SUPPORT])
    status, out = run_exe(exe)
    assert status == 0, f"exited with {status}:\n{out}\n--- C ---\n" + "\n".join(unit)
    return parse_state(out)


def differences(prog: Program, got: dict[str, int]) -> list[str]:
    snip = prog.snip
    want: dict[str, int | None] = {f"r{n}": snip.regs_in.get(f"r{n}", 0) for n in range(4, 12)}
    want["r13"] = rs.SP0
    if snip.trap is not None:
        want = {"trap_kind": snip.trap[0], "trap_pc": snip.trap[1]}
    else:
        want["returned"] = 1
    want |= {k: v for k, v in snip.regs_out.items() if k[0] == "r"}
    want |= {f"m{i}": value for i, (_, _, value) in enumerate(snip.mem_out)}
    diffs = []
    for key, value in want.items():
        if value is not None and got.get(key) != value:
            shown = "missing" if key not in got else f"{got[key]:#x}"
            diffs.append(f"{key}: got {shown}, expected {value:#x}")
    return diffs


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_program(tmp_path: Path, path: Path) -> None:
    prog = load(path)
    got = run(prog, tmp_path)
    diffs = differences(prog, got)
    listing = "\n".join(f"  {line.address:#06x}  {line.asm}" for line in prog.snip.lines)
    assert not diffs, f"{prog.snip.title}\n" + "\n".join(diffs) + f"\n{listing}"


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_program_bytes_match_capstone(path: Path) -> None:
    assert not rs.capstone_mismatches(load(path).snip)


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_program_bytes_match_clang(path: Path) -> None:
    if rs.clang() is None:
        pytest.skip("no clang with the ARM target (thumbv7-none-eabi) on PATH")
    snip = load(path).snip
    encoded = rs.clang_encode(snip)
    diffs = []
    for line in snip.lines:
        clang_bytes = encoded[line.lineno]
        if clang_bytes is not None and clang_bytes != line.data:  # None: a relocation
            ours = (line.data or b"").hex()
            diffs.append(f"line {line.lineno} {line.asm!r}: {ours}, clang {clang_bytes.hex()}")
    assert not diffs, "\n".join(diffs)


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_every_context_gets_one_resume_hook(path: Path) -> None:
    """Every function that registers a context is lifted with exactly one setjmp hook,
    after its jbuf[1] store, and a label at its resume address."""
    prog = load(path)
    for fn, f in zip(prog.functions, lift(prog), strict=True):
        if not any(t == 0x9001 for t in fn.calls.values()):
            assert "MT_SJLJ_SETJMP" not in f.source
            continue
        (resume,) = fn.entries
        assert f.source.count("MT_SJLJ_SETJMP") == 1, f.source
        assert f"goto L_{resume:08X}; }}" in f.source
        assert f"\nL_{resume:08X}:;\n" in f.source


class _FakeImage:
    """What :func:`recomp.exceptions.find_context` asks an image, for a listing."""

    NAMES = {0x9000: ex.REGISTER, 0x9010: ex.UNREGISTER, 0x9020: ex.RESUME,
             GXX & ~1: "___gxx_personality_sj0", OBJC & ~1: "___objc_personality_v0"}  # fmt: skip

    def __init__(self, prog: Program) -> None:
        self.prog = prog
        self.const = _Lift(prog.code)

    def import_at(self, addr: int):
        name = self.NAMES.get(addr & ~1)
        return None if name is None else type("Import", (), {"name": name})()

    def const_word(self, addr: int, size: int) -> int | None:
        return self.const.const_word(addr, size)


def _read(prog: Program):
    mem = dict(prog.snip.mem_in)

    def read(addr: int, n: int) -> bytes:
        out = b""
        for a in range(addr, addr + n):
            for base, data in mem.items():
                if base <= a < base + len(data):
                    out += data[a - base : a - base + 1]
                    break
            else:
                return out
        return out

    return read


@pytest.mark.parametrize("path", FILES, ids=IDS)
def test_census_finds_the_contexts_and_their_dispatch(path: Path) -> None:
    """recomp.exceptions on the listings: each context's layout, personality, LSDA, the
    resume store the lifter hooks, and the dispatch per landing pad: every index of the
    LSDA reaches a landing pad, the index two past the last one the dispatch's trap."""
    prog = load(path)
    image = _FakeImage(prog)
    found = 0
    for fn in prog.functions:
        ctx = ex.find_context(fn, image)
        if ctx is None:
            continue
        found += 1
        assert not ctx.problems, ctx
        assert ctx.personality in ex.PERSONALITIES
        assert ctx.resume == min(fn.entries)
        lsda = ex.parse_lsda(_read(prog), ctx.lsda)
        pads = sorted({s.landing_pad for s in lsda.call_sites})
        for i in pads:
            assert not ex.dispatch(fn, ctx, i).startswith(("trap", "lost")), (hex(fn.ptr), i)
        assert ex.dispatch(fn, ctx, max(pads) + 2).startswith("trap")
    assert found


def test_lsda_parser_reads_the_try_catch_tables() -> None:
    prog = load(SJLJ_DIR / "try_catch.sjlj")
    lsda = ex.parse_lsda(_read(prog), 0x100100)
    (site,) = lsda.call_sites
    assert (site.index, site.landing_pad, site.action) == (1, 0, 1)
    assert [a.filter for a in lsda.chain(site)] == [1, 2]
    assert lsda.types == {1: ex.TypeRef(0x5000), 2: ex.TypeRef(0x6000)}
    assert lsda.kind(site) == "catch"
    assert lsda.end == 0x100100 + 19
