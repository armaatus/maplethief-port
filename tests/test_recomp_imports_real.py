"""Real-asset checks of the import table (#36, ADR 0019) against the armv7 slice.

Skipped when work/ipa/ is absent (CI). Acceptance of #36: every one of the 333 imports is
classified, and after the runtime links the generated table each slot holds either an HLE
implementation or the default unimplemented-import handler, which logs the import's name and
its caller. The generated ``imports.c`` is compiled with the runtime's import code and run:
it links the table, places the data objects, applies all 1,268 binds and calls every function
import that is left to the default handler (policy ``continue``).
"""

from __future__ import annotations

import subprocess
import sys

import pytest

import recomp_harness as rh
from mtport import bootstrap as bs
from recomp import imports as rimports
from recomp import loader
from recomp.imports import ImportTable
from recomp.output import header
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")
# What runtime/src/hle/registry.cpp implements in its core part, the one the C harness links
# (MT_HLE_CORE_ONLY); tests/test_hle_libc_real.py checks the libc entries, and
# tests/cpp/objc_real_test.cpp and boot_real_test.cpp the Objective-C runtime's (#47).
IMPLEMENTED = {
    "___divdi3", "___divsi3", "___floatdisf", "___modsi3", "___udivdi3", "___udivsi3",
    "___umodsi3",
    # the SjLj unwinder and catch helpers (#37)
    "__Unwind_SjLj_Register", "__Unwind_SjLj_Resume", "__Unwind_SjLj_Unregister",
    "__ZSt9terminatev", "_objc_begin_catch", "_objc_end_catch", "_objc_exception_rethrow",
    # process start-up and exit (#43)
    "___cxa_atexit", "_UIApplicationMain", "_exit",
}  # fmt: skip


@pytest.fixture(scope="module")
def image() -> loader.Image:
    return loader.Image.load(SLICE)


@pytest.fixture(scope="module")
def table(image) -> ImportTable:
    return ImportTable.from_image(image)


def test_every_import_is_classified(image, table):
    assert table.problems == ()
    assert table.counts() == {
        "imports": 333,
        "functions": 222,
        "functions with a stub": 217,
        "address-taken functions": 5,
        "data": 111,
        "objc classes": 79,
        "stret": 6,
        "weak": sum(i.weak for i in image.imports),
        "binds": 1055 + 213,  # the bind and lazy-bind streams; not the 4 weak definitions
        "thunks": 222,
        "problems": 0,
    }
    assert table.names == tuple(i.name for i in image.imports)  # id = nlist order
    for e, imp in zip(table.entries, image.imports, strict=True):
        assert e.function == bool(imp.stubs or imp.name in rimports.ADDRESS_FUNCTIONS), e
        assert e.data != e.function
        if e.function:
            assert e.addr == (imp.stubs[0] if imp.stubs else rimports.THUNK_BASE + 4 * e.id)
        else:
            assert e.size >= 4 and e.align in (4, 16) and e.addr == 0, e
    stret = {e.name for e in table.entries if e.flags & rimports.STRET}
    assert stret == {"_objc_msgSend_stret", *rimports.STRUCT_RETURNS}
    # The tables name nothing the binary does not import.
    for name in (*rimports.ADDRESS_FUNCTIONS, *rimports.DATA_OBJECTS, *rimports.STRUCT_RETURNS):
        assert table.named(name) is not None, name
    # The examples of the issue are data objects.
    for name in ("_kCFBooleanTrue", "__NSConcreteStackBlock", "___stderrp", "_CGRectZero"):
        assert table.named(name).data, name


def test_binds_cover_every_pointer(image, table):
    by_slot = {b.slot: b for b in table.binds}
    for b in image.binds:
        if b.kind == "weak":
            assert by_slot[b.address].id == table.named(b.symbol).id  # bound plainly too
            continue
        mine = by_slot[b.address]
        assert (table.names[mine.id], mine.addend) == (b.symbol, b.addend)
    for imp in image.imports:
        for slot in (*imp.lazy, *imp.non_lazy, *(a for a, _ in imp.binds)):
            assert table.names[by_slot[slot].id] == imp.name


DRIVER = r"""
#include "mt_gen.h"
#include "test_runtime.h"
#include "maplethief/hle/imports.h"

#include <stdio.h>

const mt_fn_entry mt_fn_table[1] = {{0u, 0}};
const uint32_t mt_fn_count = 0u;
const mt_fn_index mt_fn_idx = {0u, 4u, 0u, 0u, 0}; /* no functions (#35) */

static uint32_t heap = 0x180000u;
static uint32_t bump(void *user, uint32_t size, uint32_t align) {
    uint32_t at = (heap + align - 1u) & ~(align - 1u);
    (void)user;
    heap = at + size;
    return at;
}
static void to_stdout(void *user, const char *line) {
    (void)user;
    printf("LOG %s\n", line);
}

int main(void) {
    static mt_cpu cpu;
    static uint32_t addrs[MT_IMPORT_COUNT];
    mt_import_config cfg = mt_import_config_default();
    uint32_t n_hle, i;
    const mt_hle_entry *hle = mt_hle_registry(&n_hle);
    mt_test_init(&cpu, 0x200000u, 0u);
    cfg.unimplemented = MT_UNIMPLEMENTED_CONTINUE;
    cfg.log = to_stdout;
    printf("UNIMPLEMENTED %u\n",
           mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle, &cfg));
    for (i = 0; i < mt_import_count; i++) {
        const char *kind = mt_import_table[i] == NULL                      ? "null"
                           : mt_import_table[i] == mt_import_unimplemented ? "default"
                                                                            : "hle";
        printf("SLOT %u %s %s\n", i, kind, mt_import_names[i]);
    }
    if (mt_imports_place(&cpu, mt_import_descs, mt_import_count, hle, n_hle, bump, NULL,
                         addrs) != 0 ||
        mt_imports_apply_binds(&cpu, mt_import_binds, mt_import_bind_count, addrs,
                               mt_import_count) != 0) {
        return 3;
    }
    for (i = 0; i < mt_import_count; i++) {
        printf("ADDR %u 0x%08X\n", i, addrs[i]);
    }
    for (i = 0; i < mt_import_bind_count; i++) {
        printf("BIND 0x%08X 0x%08X\n", mt_import_binds[i].slot,
               mt_ld32(&cpu, mt_import_binds[i].slot, 0u));
    }
    for (i = 0; i < mt_import_count; i++) {
        if ((mt_import_descs[i].flags & MT_IMPORT_FUNCTION) &&
            mt_import_table[i] == mt_import_unimplemented) {
            cpu.r[14] = 0x10001u + 4u * i;
            mt_import_table[i](&cpu, i); /* what MT_CALL_IMPORT(i) does */
        }
    }
    printf("CALLS %u\n", mt_import_unimplemented_calls(0u));
    return 0;
}
"""


@pytest.fixture(scope="module")
def linked(table, tmp_path_factory) -> list[str]:
    out = tmp_path_factory.mktemp("imports_real")
    (out / "mt_gen.h").write_text(header([], table), encoding="utf-8")
    (out / "imports.c").write_text(table.c_source(), encoding="utf-8")
    (out / "main.c").write_text(DRIVER, encoding="utf-8")
    exe = out / ("imports.exe" if sys.platform == "win32" else "imports")
    sources = [out / "main.c", out / "imports.c", rh.TEST_RUNTIME, *rh.IMPORT_RUNTIME]
    rh.host_cc().build(sources, exe, [rh.INCLUDE, rh.SUPPORT, rh.RUNTIME_SRC, out])
    done = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert done.returncode == 0, done.stdout[-2000:] + done.stderr
    return done.stdout.splitlines()


def test_every_slot_has_an_implementation_or_the_default_handler(table, linked):
    """Acceptance (#36): all 333 slots are filled, by name."""
    slots = [line.split() for line in linked if line.startswith("SLOT ")]
    assert len(slots) == 333
    assert [s[3] for s in slots] == list(table.names)
    hle = {s[3] for s in slots if s[2] == "hle"}
    assert hle == IMPLEMENTED
    assert all(s[2] in ("hle", "default") for s in slots)
    unimplemented = sum(e.function for e in table.entries) - len(IMPLEMENTED)
    assert f"UNIMPLEMENTED {unimplemented}" in linked


def test_data_objects_are_placed_and_every_bind_is_written(table, linked):
    addrs = {int(i): int(a, 16) for _, i, a in (x.split() for x in linked if x[:5] == "ADDR ")}
    for e in table.entries:
        if e.function:
            assert addrs[e.id] == e.addr
        else:
            assert addrs[e.id] >= 0x180000 and addrs[e.id] % e.align == 0
    data = sorted((addrs[e.id], e.size) for e in table.entries if e.data)
    assert all(a + n <= b for (a, n), (b, _) in zip(data, data[1:], strict=False))  # no overlap
    written = {
        int(s, 16): int(v, 16) for _, s, v in (x.split() for x in linked if x[:5] == "BIND ")
    }
    assert len(written) == len(table.binds) == 1268
    for b in table.binds:
        assert written[b.slot] == (addrs[b.id] + b.addend) & 0xFFFFFFFF


def test_unimplemented_imports_log_name_and_caller(table, linked):
    """Acceptance (#36): each call logs the import's name and the guest caller (continue)."""
    logs = [line[4:] for line in linked if line.startswith("LOG ")]
    expected = []
    for e in table.entries:
        if e.function and e.name not in IMPLEMENTED:
            lib = e.library.rsplit("/", 1)[-1]
            stret = " (struct result left unwritten)" if e.flags & rimports.STRET else ""
            expected.append(
                f"unimplemented import {e.name} (id {e.id}, {lib}) called from "
                f"0x{0x10001 + 4 * e.id:08X}: returning 0{stret}"
            )
    assert logs == expected
