"""The import table (recomp/imports.py, #36, ADR 0019) and the runtime's handling of it.

Synthetic imports only. The classification, ids, binds and the generated ``imports.c`` are
checked in Python; then a lifted Thumb snippet that calls two import stubs (one the runtime
implements, one it does not) runs against the generated table and the runtime's import code
(runtime/src/hle/), once per value of ``MAPLETHIEF_UNIMPLEMENTED_IMPORT``. The real slice's
333 imports are checked in tests/test_recomp_imports_real.py.
"""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

import pytest

import importfixtures as fx
import recomp_harness as rh
from mtport import imports as re_imports
from mtport import macho
from recomp import imports as rimports
from recomp import loader
from recomp.imports import ImportTable
from recomp.output import header
from recomp.snippet import lift_snippet
from thumbasm import Thumb

Import = re_imports.Import
SYS = "/usr/lib/libSystem.B.dylib"
GCC = "/usr/lib/libgcc_s.1.dylib"
UIKIT = "/System/Library/Frameworks/UIKit.framework/UIKit"
CF = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"
STUB_DIV, STUB_FROB, STUB_STRET = 0x3000, 0x3004, 0x3008
NL_TRUE, DATA_PURE, CLASSREF = 0x5000, 0x5010, 0x5014


def synthetic() -> tuple[list[Import], list[macho.Bind]]:
    imports = [
        Import("___divsi3", GCC, False, stubs=[STUB_DIV], lazy=[0x4000]),
        Import("_frobnicate", UIKIT, False, stubs=[STUB_FROB], lazy=[0x4004]),
        Import("_kCFBooleanTrue", CF, False, non_lazy=[NL_TRUE]),
        Import(
            "___cxa_pure_virtual",
            "/usr/lib/libstdc++.6.dylib",
            False,
            binds=[(DATA_PURE, "__data")],
        ),  # fmt: skip
        Import("_OBJC_CLASS_$_UIView", UIKIT, True, binds=[(CLASSREF, "__objc_classrefs")]),
        Import("_objc_msgSend_stret", "/usr/lib/libobjc.A.dylib", False, stubs=[STUB_STRET]),
    ]
    binds = [
        macho.Bind("bind", NL_TRUE, "_kCFBooleanTrue", 3, 1, 0, 0),
        macho.Bind("bind", DATA_PURE, "___cxa_pure_virtual", 4, 1, 0, 0),
        macho.Bind("bind", CLASSREF, "_OBJC_CLASS_$_UIView", 2, 1, 8, 1),
        macho.Bind("weak", 0x4000, "___divsi3", 0, 1, 0, 0),  # coalescing: not carried
        macho.Bind("lazy", 0x4004, "_frobnicate", 2, 0, 0, 0),
        macho.Bind("lazy", 0x4000, "___divsi3", 1, 0, 0, 0),
    ]
    return imports, binds


@pytest.fixture(scope="module")
def table() -> ImportTable:
    return ImportTable.from_imports(*synthetic())


def test_classification(table):
    e = table.entries
    assert [x.id for x in e] == list(range(6))
    assert table.names == tuple(i.name for i in synthetic()[0])
    assert (e[0].flags, e[0].addr, e[0].stubs) == (rimports.FUNCTION, STUB_DIV, (STUB_DIV,))
    assert e[2].flags == rimports.DATA and (e[2].size, e[2].align, e[2].addr) == (4, 4, 0)
    # An address-taken function without a stub gets a thunk address.
    assert e[3].flags == rimports.FUNCTION and e[3].addr == rimports.THUNK_BASE + 4 * 3
    assert e[4].flags == rimports.DATA | rimports.WEAK and (e[4].size, e[4].align) == (20, 4)
    assert e[5].flags == rimports.FUNCTION | rimports.STRET
    assert table.problems == ()
    assert table.at(STUB_FROB) == (1, "_frobnicate")
    assert table.at(STUB_FROB | 1) == (1, "_frobnicate")
    assert table.at(NL_TRUE) is None  # only stubs are call targets
    assert table.named("_kCFBooleanTrue") is e[2] and table.named("_nope") is None
    assert table.thunks == ((STUB_DIV, 0), (STUB_FROB, 1), (STUB_STRET, 5), (0xFFFE000C, 3))
    assert table.counts() == {
        "imports": 6,
        "functions": 4,
        "functions with a stub": 3,
        "address-taken functions": 1,
        "data": 2,
        "objc classes": 1,
        "stret": 1,
        "weak": 1,
        "binds": 5,
        "thunks": 4,
        "problems": 0,
    }


def test_struct_returning_c_functions_are_stret():
    entry, problem = rimports.classify(Import("_CGRectInset", CF, False, stubs=[0x10]), 0)
    assert entry.flags & rimports.STRET and "CGRect" in entry.what and problem is None


def test_binds_carry_the_addend_and_skip_the_weak_stream(table):
    assert [(b.slot, b.id, b.addend) for b in table.binds] == [
        (0x4000, 0, 0),
        (0x4004, 1, 0),
        (NL_TRUE, 2, 0),
        (DATA_PURE, 3, 0),
        (CLASSREF, 4, 8),
    ]


def test_unknown_imports_are_problems():
    imports = [Import("_kMystery", CF, False, non_lazy=[0x10])]
    binds = [
        macho.Bind("bind", 0x10, "_kMystery", 1, 1, 0, 0),
        macho.Bind("bind", 0x10, "_kMystery", 1, 1, 4, 0),
        macho.Bind("bind", 0x14, "_kGhost", 1, 1, 0, 0),
    ]
    t = ImportTable.from_imports(imports, binds)
    assert (t.entries[0].flags, t.entries[0].size) == (rimports.DATA, rimports.UNKNOWN_DATA[0])
    assert len(t.problems) == 3
    assert "no known size" in t.problems[0]
    assert "two different values" in t.problems[1]
    assert "_kGhost is not an import" in t.problems[2]


def test_from_image():
    image = loader.Image(fx.build(b"\x70\x47" * 8, [fx.TEXT | 1]))
    t = ImportTable.from_image(image)
    assert t.names == tuple(n for n, _ in fx.UNDEFINED)
    assert t.at(fx.STUB_MSGSEND) == (1, "_objc_msgSend")
    assert [e.name for e in t.entries if e.function] == ["_foo", "_objc_msgSend", "_bar"]
    # _kConst, _free (bound in __data, no stub) and _unused are not in the tables.
    assert len(t.problems) == 3
    assert {b.slot for b in t.binds} == {fx.NL, fx.CLASSREF, fx.DATA_WORD, fx.LAZY,
                                         fx.LAZY + 4, fx.LAZY + 8}  # fmt: skip


def test_c_source(table):
    src = table.c_source()
    assert '#include "mt_gen.h"' in src
    assert 'const char *const mt_import_names[6] = {\n    "___divsi3", /* 0 */' in src
    assert "const uint32_t mt_import_count = 6u;" in src
    assert (
        '{"___cxa_pure_virtual", "/usr/lib/libstdc++.6.dylib", MT_IMPORT_FUNCTION, '
        "0xFFFE000Cu, 0u, 0u}, /* 3: address-taken function:" in src
    )
    assert '"_OBJC_CLASS_$_UIView", ' in src and "MT_IMPORT_DATA | MT_IMPORT_WEAK, " in src
    assert "{0x00005014u, 4u, 8}," in src and "mt_import_bind_count = 5u;" in src
    assert "static void mt_import_thunk_3(mt_cpu *MT_RESTRICT cpu) {" in src
    assert "{0xFFFE000Cu, mt_import_thunk_3}," in src and "mt_import_thunk_count = 4u;" in src


def test_c_strings_are_escaped():
    assert rimports.c_string('a"b\\c?d') == '"a\\"b\\\\c\\?d"'
    assert rimports.c_string("x\nyé") == '"x\\012y\\303\\251"'
    t = ImportTable.from_imports([Import('_ev*/il"', SYS, False, stubs=[0x10])])
    assert '"_ev*/il\\""' in t.c_source() and '/* _ev*_/il" */' in t.c_source()


def test_empty_table_still_compiles_as_c():
    src = ImportTable.from_imports([]).c_source()
    assert "mt_import_names[1] = {\n    0,\n};" in src and "mt_import_count = 0u;" in src


# -- the runtime, under recompiled code -----------------------------------------------------

DRIVER = r"""
#include "mt_gen.h"
#include "test_runtime.h"
#include "maplethief/hle/imports.h"

#include <stdio.h>

void snippet(mt_cpu *MT_RESTRICT cpu);
const mt_fn_entry mt_fn_table[1] = {{0u, 0}};
const uint32_t mt_fn_count = 0u;
const mt_fn_index mt_fn_idx = {0u, 4u, 0u, 0u, 0}; /* no functions (#35) */

static mt_cpu cpu; /* static: unchanged by the longjmp of a trap */
static jmp_buf trap_jmp;
static uint32_t heap = 0x180000u;

static uint32_t bump(void *user, uint32_t size, uint32_t align) {
    uint32_t at = (heap + align - 1u) & ~(align - 1u);
    (void)user;
    heap = at + size;
    return at;
}

int main(void) {
    mt_import_config cfg = mt_import_config_default();
    uint32_t n_hle, i, addrs[MT_IMPORT_COUNT];
    const mt_hle_entry *hle = mt_hle_registry(&n_hle);
    mt_test_init(&cpu, 0x200000u, 0u);
    if (mt_import_config_from_env(&cfg) != 0) {
        return 2;
    }
    printf("unimplemented=0x%08X\n",
           mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle, &cfg));
    if (mt_imports_place(&cpu, mt_import_descs, mt_import_count, hle, n_hle, bump, NULL,
                         addrs) != 0 ||
        mt_imports_apply_binds(&cpu, mt_import_binds, mt_import_bind_count, addrs,
                               mt_import_count) != 0) {
        return 3;
    }
    for (i = 0; i < mt_import_count; i++) {
        printf("addr%u=0x%08X\n", i, addrs[i]);
    }
    for (i = 0; i < mt_import_bind_count; i++) {
        printf("bind%u=0x%08X\n", i, mt_ld32(&cpu, mt_import_binds[i].slot, 0u));
    }
    mt_test_trap_jmp = &trap_jmp;
    if (setjmp(trap_jmp) == 0) {
        snippet(&cpu);
        printf("returned=0x1\n");
    }
    mt_test_print_state(&cpu);
    return 0;
}
"""


def snippet_code() -> tuple[bytes, int]:
    """r0 = 100 / 7 through ___divsi3 (implemented), keep it in r4, call _frobnicate
    (unimplemented), then r0 += 5. Returns the code and the caller address of _frobnicate."""
    t = Thumb(rh.SNIPPET_BASE).push_r7_lr().movs(0, 100).movs(1, 7).bl(STUB_DIV).mov(4, 0)
    t.bl(STUB_FROB)
    ret = t.here | 1
    t.adds(0, 0, 5).pop_r7_pc()
    return bytes(t.code), ret


@pytest.fixture(scope="module")
def program(tmp_path_factory) -> tuple[Path, int]:
    """The snippet linked with the generated table and the runtime's import code."""
    out = tmp_path_factory.mktemp("imports")
    table = ImportTable.from_imports(*synthetic())
    code, ret = snippet_code()
    stubs = {s: (e.id, e.name) for e in table.entries for s in e.stubs}
    snip = lift_snippet(code, rh.SNIPPET_BASE, imports=stubs)
    assert not snip.unsupported and not snip.problems, snip
    assert "MT_CALL_IMPORT(1u); /* _frobnicate */" in snip.source
    (out / "mt_gen.h").write_text(header([], table), encoding="utf-8")
    (out / "imports.c").write_text(table.c_source(), encoding="utf-8")
    (out / "snippet.c").write_text(snip.source, encoding="utf-8")
    (out / "main.c").write_text(DRIVER, encoding="utf-8")
    exe = out / ("imports.exe" if sys.platform == "win32" else "imports")
    sources = [out / n for n in ("main.c", "snippet.c", "imports.c")]
    sources += [rh.TEST_RUNTIME, *rh.IMPORT_RUNTIME]
    rh.host_cc().build(sources, exe, [rh.INCLUDE, rh.SUPPORT, rh.RUNTIME_SRC, out])
    return exe, ret


def run(exe: Path, policy: str | None) -> tuple[int, dict[str, int], str]:
    env = dict(os.environ)
    env.pop("MAPLETHIEF_UNIMPLEMENTED_IMPORT", None)
    if policy is not None:
        env["MAPLETHIEF_UNIMPLEMENTED_IMPORT"] = policy
    done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60, env=env)
    return done.returncode, rh.parse_state(done.stdout), done.stderr


def test_runtime_links_places_and_binds(program):
    status, state, _ = run(program[0], "continue")
    assert status == 0
    assert state["unimplemented"] == 3  # _frobnicate, ___cxa_pure_virtual, _objc_msgSend_stret
    assert state["addr0"] == STUB_DIV and state["addr3"] == 0xFFFE000C
    true_obj, view_class = state["addr2"], state["addr4"]
    assert true_obj == 0x180000 and view_class == 0x180004
    # binds are ascending by slot: 0x4000 0x4004 0x5000 0x5010 0x5014
    assert [state[f"bind{i}"] for i in range(5)] == [
        STUB_DIV, STUB_FROB, true_obj, 0xFFFE000C, view_class + 8]  # fmt: skip


@pytest.mark.parametrize("policy", [None, "stop"])
def test_unimplemented_import_logs_and_stops_by_default(program, policy):
    exe, caller = program
    status, state, err = run(exe, policy)
    assert status == 0 and "returned" not in state
    assert (state["trap_kind"], state["trap_pc"], state["trap_detail"]) == (7, caller, 1)
    assert state["r4"] == 14  # ___divsi3 ran in the runtime before
    expected = f"unimplemented import _frobnicate (id 1, UIKit) called from 0x{caller:08X}"
    assert f"{expected}: stopping" in err


@pytest.mark.parametrize("policy", ["continue", "trace"])
def test_unimplemented_import_logs_and_continues(program, policy):
    exe, caller = program
    status, state, err = run(exe, policy)
    assert status == 0 and state["returned"] == 1
    assert (state["r4"], state["r0"], state["r1"]) == (14, 5, 0)  # 0 + 5 after the call
    expected = f"unimplemented import _frobnicate (id 1, UIKit) called from 0x{caller:08X}"
    assert err.count(f"{expected}: returning 0") == 1


def test_a_bad_policy_is_reported(program):
    status, _, err = run(program[0], "sometimes")
    assert status == 2 and "MAPLETHIEF_UNIMPLEMENTED_IMPORT=sometimes is not a policy" in err
