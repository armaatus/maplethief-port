"""SjLj exceptions (#37) on the real armv7 slice (local only: needs the IPA).

* Every function that registers an SjLj context (205) has exactly one, found without a
  problem: its frame offset, personality, LSDA, resume address and the jbuf[1] store.
* Every LSDA in ``__gcc_except_tab`` parses; the 205 tables tile the section exactly (only
  alignment padding between them), and what they contain is what ANALYSIS.md says.
* Every landing-pad index of every LSDA, run through the function's own resume dispatch,
  reaches a landing pad; an index past the end reaches the dispatch's trap.
* All 205 functions are lifted with their resume hook (after that store) and resume label,
  and compile with clang -std=c17 -Wall -Wextra -Werror -Wpedantic and for the MSVC target.
* Nothing raises in normal play: no import that starts an exception exists, and the only
  raising calls (``_Unwind_SjLj_Resume``, ``objc_exception_rethrow``) and every
  ``std::terminate`` call are in landing-pad code.
* Two real functions run with the image mapped: an exception thrown by an import they call
  runs their real landing pad (the cleanup) and reaches a handler outside; without one
  they return normally and leave the context chain as they found it.
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys

import pytest

from recomp import exceptions as ex
from recomp import prototype
from recomp.__main__ import main
from recomp.discover import discover
from recomp.imports import ImportTable
from recomp.loader import Image
from recomp.output import write_program
from recomp_harness import (
    IMPORT_RUNTIME,
    INCLUDE,
    MSVC_SHIM,
    RUNTIME_SRC,
    SUPPORT,
    TEST_RUNTIME,
    CCompiler,
    host_cc,
)
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
SJLJ_C = REPO / "runtime" / "src" / "sjlj.c"
CHARTBOOST = 0xC0000  # every Chartboost SjLj function is above this, every game one below


@pytest.fixture(scope="module")
def image() -> Image:
    return Image.load(SLICE)


@pytest.fixture(scope="module")
def found(image: Image):
    return discover(image)


@pytest.fixture(scope="module")
def rep(image: Image, found) -> ex.Report:
    return ex.report(image, found)


@pytest.fixture(scope="module")
def lifted(image: Image, found, rep: ex.Report):
    return prototype.lift(image, [c.ptr for c in rep.contexts], discovery=found)


def test_every_sjlj_function_has_one_context(rep: ex.Report, found, image: Image) -> None:
    registering = {
        p for p, fn in found.functions.items() if ex.REGISTER in ex._import_calls(fn, image)
    }
    with_entries = {p for p, fn in found.functions.items() if fn.entries}
    assert len(rep.contexts) == len(registering) == len(with_entries) == 205
    assert {c.ptr for c in rep.contexts} == registering == with_entries
    for c in rep.contexts:
        fn = found.functions[c.ptr]
        assert not c.problems, (hex(c.ptr), c.problems)
        assert (c.resume,) == tuple(fn.entries)
        assert len(c.unregister) >= 1 and len(c.resume_calls) >= 1
        assert c.frame is not None and 0 <= c.frame < 0x1000 and c.frame % 4 == 0
    c = rep.counts()
    assert c["personality:___gxx_personality_sj0"] == 187
    assert c["personality:___objc_personality_v0"] == 18
    assert sum(c.ptr > CHARTBOOST for c in rep.contexts) == 16


def test_every_shipped_lsda_parses_and_they_tile_the_section(rep: ex.Report, image) -> None:
    sec = image.macho.section("__TEXT", "__gcc_except_tab")
    lsdas = sorted(rep.lsdas.values(), key=lambda x: x.address)
    assert len(lsdas) == 205 and lsdas[0].address == sec.addr
    pos = sec.addr
    for x in lsdas:
        gap = x.address - pos
        assert 0 <= gap <= 3 and image.macho.read(pos, gap) == bytes(gap), hex(pos)
        assert x.address % 4 == 0
        pos = x.end
    assert pos == sec.addr + sec.size  # every byte of the section is accounted for
    for x in lsdas:
        assert x.lpstart is None and x.call_site_encoding == ex.DW_EH_PE_UDATA4
        assert x.ttype_encoding in (ex.DW_EH_PE_OMIT, 0x9B)
        assert set(x.specs.values()) <= {()}  # only empty throw() specifications
        assert all(t == ex.TypeRef(0) for t in x.types.values())  # only catch-all
        assert [s.landing_pad for s in x.call_sites] == list(range(len(x.call_sites)))
    c = rep.counts()
    assert (c["call_sites"], c["cleanup"], c["spec"], c["catch-all"]) == (1238, 928, 258, 52)
    assert c.get("catch", 0) == 0 and c["lsda_bytes"] == 4055


def test_every_landing_pad_index_reaches_its_landing_pad(rep: ex.Report, found) -> None:
    for ctx in rep.contexts:
        lsda = rep.lsdas[ctx.lsda]
        fn = found.functions[ctx.ptr]
        n = len(lsda.call_sites)
        outcome = rep.dispatches[ctx.ptr]
        for i in range(n):
            assert outcome[i].split()[0] in ("call", "branch"), (hex(ctx.ptr), i, outcome[i])
        if ctx.ptr < CHARTBOOST:  # LLVM-GCC: a chain of compares that ends in the trap
            assert outcome[n].startswith("trap"), (hex(ctx.ptr), outcome)
        else:  # clang's cmp #n; bhi admits n too (one past its n-entry table), so n + 1
            (table,) = [t for t in fn.tables.values() if t.kind == "branch"]
            assert table.count == n + 1
            assert ex.dispatch(fn, ctx, n + 1).startswith("trap")


def test_all_sjlj_functions_are_lifted_with_their_resume_hook(rep: ex.Report, lifted) -> None:
    by_ptr = {c.ptr: c for c in rep.contexts}
    for f in lifted:
        ctx = by_ptr[f.ptr]
        assert f.source.count("MT_SJLJ_SETJMP(") == 1, hex(f.ptr)
        assert f"goto L_{ctx.resume:08X}; }}" in f.source
        assert f"\nL_{ctx.resume:08X}:;\n" in f.source
        store = f.source.index(f"/* {ctx.store:08X}: ")
        hook = f.source.index("MT_SJLJ_SETJMP(")
        nxt = f.source.index("/* ", store + 3)
        assert store < hook < nxt, hex(f.ptr)  # inside the store's own C
        assert f.clean, (hex(f.ptr), f.problems)  # with #34's NEON, nothing is left


def _clang() -> str:
    path = shutil.which("clang")
    if path is None:
        pytest.skip("clang not found")
    return path


def test_the_lifted_sjlj_functions_compile(lifted, image: Image, tmp_path) -> None:
    sources = write_program(tmp_path / "gen", lifted, ImportTable.from_image(image))
    cmd = [_clang(), "-std=c17", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O1"]
    cmd += ["-ffp-contract=off", f"-I{INCLUDE}", f"-I{sources[0].parent}", "-c"]
    for src in sources:
        done = subprocess.run([*cmd, str(src), "-o", str(src.with_suffix(".o"))],
                              capture_output=True, text=True)  # fmt: skip
        assert done.returncode == 0, done.stderr[-3000:]
    for msc in (False, True):
        cmd = [_clang(), "--target=x86_64-pc-windows-msvc", "-fms-compatibility"]
        cmd += ["-fms-extensions", "-std=c17", "-Wall", "-Wextra", "-Werror"]
        cmd += [f"-isystem{MSVC_SHIM}", f"-I{INCLUDE}", f"-I{sources[0].parent}", "-fsyntax-only"]
        if msc:
            cmd += ["-U__clang__", "-Wno-unknown-pragmas"]
        done = subprocess.run([*cmd, str(sources[0]), str(SJLJ_C)], capture_output=True, text=True)
        assert done.returncode == 0, done.stderr[-3000:]


def test_nothing_raises_in_normal_play(rep: ex.Report, found, image: Image) -> None:
    imported = {i.name for i in image.imports}
    starters = {"___cxa_throw", "___cxa_allocate_exception", "_objc_exception_throw",
                "__Unwind_SjLj_RaiseException", "__Unwind_RaiseException",
                "__Unwind_SjLj_ForcedUnwind", "___cxa_rethrow", "___cxa_begin_catch"}  # fmt: skip
    assert not starters & imported
    assert {k: len(v) for k, v in rep.raisers.items()} == {
        ex.RESUME: 205, "_objc_exception_rethrow": 2,
    }  # fmt: skip
    pads = ("__ZSt9terminatev", ex.RESUME, "_objc_exception_rethrow", "_objc_begin_catch",
            "_objc_end_catch")  # fmt: skip
    sites = 0
    for fn in found.functions.values():
        calls = ex._import_calls(fn, image)
        if not any(n in calls for n in pads):
            continue
        reach, work = set(), list(fn.entries)
        while work:
            a = work.pop()
            if a not in reach:
                reach.add(a)
                work.extend(fn.succ.get(a, ()))
        for n in pads:
            for site in calls.get(n, ()):
                assert site in reach, (hex(fn.ptr), n, hex(site))  # only in landing pads
                sites += 1
    assert sites == 258 + 205 + 2 + 2 + 2


def test_command_writes_the_report(tmp_path) -> None:
    out = tmp_path / "exceptions.json"
    assert main(["exceptions", "--slice", str(SLICE), "--out", str(out)]) == 0
    doc = json.loads(out.read_text(encoding="utf-8"))
    assert doc["schema"] == 1 and doc["counts"]["contexts"] == 205
    assert len(doc["contexts"]) == 205 and not any(c["problems"] for c in doc["contexts"])


# --- running real landing pads ------------------------------------------------------------

OUTER_FC, OUTER_LSDA, UE, SELF = 0x1F0100, 0x1F0200, 0x1F0000, 0x1F8000


@pytest.mark.parametrize(
    "ptr, cleanup",
    [(0x5839, "_objc_sync_exit"), (0xD33F9, "__Block_object_dispose")],
    ids=["EAGLView_Instance-Instance", "chartboost-d33f9"],
)
def test_a_real_landing_pad_runs_and_the_exception_reaches_the_handler(
    image: Image, found, ptr: int, cleanup: str, tmp_path
) -> None:
    """The driver is the outer frame: it registers a catch-all context and takes the host
    setjmp of its jump buffer itself. objc_msgSend throws (mode 1) or returns (mode 0)."""
    lifted = prototype.lift(image, [ptr], discovery=found)
    imports = ImportTable.from_image(image)
    sources = write_program(tmp_path / "gen", lifted, imports)
    data_vm, data = image.segment_bytes("__DATA")
    text_vm, text = image.segment_bytes("__TEXT")
    (tmp_path / "text.bin").write_bytes(text)
    (tmp_path / "data.bin").write_bytes(data)
    lsda = bytes.fromhex("ff000a0102000101 00 00000000")  # call site 1: catch-all
    driver = "\n".join(
        [
            '#include "test_runtime.h"',
            '#include "mt_gen.h"',
            '#include "maplethief/sjlj.h"',
            '#include "maplethief/hle/imports.h"',
            "#include <stdio.h>",
            "#include <stdlib.h>",
            "#include <string.h>",
            "static mt_cpu cpu;",
            "static int mode, sends;",
            "static void logged(mt_cpu *c, uint32_t id) {",
            '    printf("call %s r0=0x%08X\\n", mt_import_names[id], c->r[0]);',
            "}",
            "static void imp_other(mt_cpu *c, uint32_t id) { logged(c, id); c->r[0] = 0u; }",
            "static void imp_send(mt_cpu *c, uint32_t id) {",
            "    logged(c, id);",
            f"    if (mode && sends++ == 0) mt_sjlj_throw(c, 0x{UE:X}u, 0x5000u, 0x7777u);",
            "    c->r[0] = 0u;",
            "}",
            "int main(int argc, char **argv) {",
            "    uint32_t i, n_hle;",
            "    const mt_hle_entry *hle;",
            f"    const uint32_t fc = 0x{OUTER_FC:X}u;",
            "    mt_sjlj_state *s;",
            "    if (argc < 4) return 2;",
            "    mode = atoi(argv[3]);",
            "    mt_test_init(&cpu, 0x200000u, MT_IMPORT_COUNT);",
            f"    if (mt_test_load(&cpu, argv[1], 0x{text_vm:X}u) ||"
            f" mt_test_load(&cpu, argv[2], 0x{data_vm:X}u)) return 2;",
            "    /* The runtime's start-up (ADR 0019): the HLE registry by name, then fakes for",
            "     * what is unimplemented and for the ObjC runtime (#47), which needs its own",
            "     * start-up; the SjLj unwinder and the @catch helpers stay real. */",
            "    hle = mt_hle_registry(&n_hle);",
            "    mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle,",
            "                    0);",
            "    for (i = 0; i < MT_IMPORT_COUNT; i++) {",
            "        const char *n = mt_import_names[i];",
            "        int objc = (strncmp(n, \"_objc_\", 6) == 0 &&",
            "                    strstr(n, \"catch\") == NULL && strstr(n, \"rethrow\") == NULL) ||",
            "                   strncmp(n, \"__Block_\", 8) == 0;",
            "        if (mt_import_table[i] == mt_import_unimplemented || objc)",
            "            mt_import_table[i] = imp_other;",
            '        if (strcmp(mt_import_names[i], "_objc_msgSend") == 0)'
            " mt_import_table[i] = imp_send;",
            "    }",
            "    for (i = 0; i < mt_import_bind_count; i++) { /* the personality pointers */",
            "        const mt_import_bind *b = &mt_import_binds[i];",
            '        if (strstr(mt_import_descs[b->id].name, "_personality_") != NULL)',
            "            mt_st32(&cpu, b->slot,",
            "                    mt_import_descs[b->id].addr + (uint32_t)b->addend, 0u);",
            "    }",
            *(f"    cpu.mem[0x{OUTER_LSDA + i:X}u] = 0x{b:02X}u;" for i, b in enumerate(lsda)),
            "    s = mt_sjlj_state_of(&cpu);",
            "    mt_sjlj_init(s);",
            "    mt_sjlj_bind_personalities(s, mt_import_descs, mt_import_count);",
            '    printf("personalities 0x%X 0x%X\\n", s->gxx_personality, s->objc_personality);',
            "    mt_st32(&cpu, fc + MT_SJLJ_FC_CALL_SITE, 1u, 0u);",
            "    mt_st32(&cpu, fc + MT_SJLJ_FC_PERSONALITY, s->objc_personality, 0u);",
            f"    mt_st32(&cpu, fc + MT_SJLJ_FC_LSDA, 0x{OUTER_LSDA:X}u, 0u);",
            "    mt_st32(&cpu, fc + MT_SJLJ_FC_JBUF, 0x7777u, 0u);",
            "    mt_st32(&cpu, fc + MT_SJLJ_FC_JBUF + 8u, cpu.r[MT_SP], 0u);",
            "    mt_sjlj_register(&cpu, fc);",
            "    if (MT_HOST_SETJMP(*mt_sjlj_jmpbuf(&cpu, fc + MT_SJLJ_FC_JBUF))) {",
            '        printf("caught ue=0x%X selector=%d pad=%u r7=0x%X sp=0x%X top=0x%X\\n",',
            "               mt_ld32(&cpu, fc + MT_SJLJ_FC_DATA, 0u),",
            "               (int)mt_ld32(&cpu, fc + MT_SJLJ_FC_DATA + 4u, 0u),",
            "               mt_ld32(&cpu, fc + MT_SJLJ_FC_CALL_SITE, 0u), cpu.r[7],",
            "               cpu.r[MT_SP], s->top);",
            "        return 0;",
            "    }",
            f"    cpu.r[0] = 0x{SELF:X}u;",
            "    cpu.r[1] = 0u;",
            "    cpu.r[14] = MT_TEST_RETURN;",
            f"    f_{ptr:08X}(&cpu);",
            '    printf("returned sp=0x%X top=0x%X\\n", cpu.r[MT_SP], s->top);',
            "    return 0;",
            "}",
            "",
        ]
    )
    (tmp_path / "driver.c").write_text(driver, encoding="utf-8")
    exe = tmp_path / ("landing.exe" if sys.platform == "win32" else "landing")
    cc = CCompiler(_clang(), False, True) if shutil.which("clang") else host_cc()
    cc.build([tmp_path / "driver.c", TEST_RUNTIME, *IMPORT_RUNTIME, *sources], exe,
             [INCLUDE, RUNTIME_SRC, SUPPORT, sources[0].parent])  # fmt: skip
    runs = {}
    for mode in (0, 1):
        done = subprocess.run(
            [str(exe), str(tmp_path / "text.bin"), str(tmp_path / "data.bin"), str(mode)],
            capture_output=True, text=True, timeout=60,
        )  # fmt: skip
        assert done.returncode == 0, done.stdout + done.stderr
        runs[mode] = done.stdout.splitlines()
    sp0 = 0x200000 - 64
    personalities = runs[0][0].split()
    base, gxx, objc = (
        0xFFFE0000,
        imports.named("___gxx_personality_sj0").id,
        imports.named("___objc_personality_v0").id,
    )
    assert personalities == ["personalities", f"0x{base + 4 * gxx:X}", f"0x{base + 4 * objc:X}"]
    normal, thrown = runs[0][1:], runs[1][1:]
    assert normal[-1] == f"returned sp=0x{sp0:X} top=0x{OUTER_FC:X}", normal
    assert any(line.startswith(f"call {cleanup} ") for line in normal)
    first_send = next(i for i, line in enumerate(thrown) if "_objc_msgSend" in line)
    after = [line.split()[1] for line in thrown[first_send + 1 : -1]]
    assert after == [cleanup], thrown  # the landing pad ran its cleanup, then resumed
    assert thrown[-1] == (
        f"caught ue=0x{UE:X} selector=1 pad=0 r7=0x7777 sp=0x{sp0:X} top=0x{OUTER_FC:X}"
    ), thrown
