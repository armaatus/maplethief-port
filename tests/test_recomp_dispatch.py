"""The dispatch table (#35): ``recomp/dispatch.py`` generates it, ``runtime/src/recomp/
dispatch.c`` looks targets up in it. Synthetic tables only (the real one is in
``test_recomp_dispatch_real.py``):

* the generator's checks (sorted, one entry per address, ARM pointers word aligned) and the
  bucket index, against a plain dictionary lookup for every address around every entry;
* the C lookup, compiled with the host compiler (MSVC on the Windows runner), for Thumb and
  ARM function pointers, their mode-flipped and mid-function neighbours, and addresses
  outside the table; an import stub's thunk;
* the miss diagnostic: guest PC of the call, the function it is in (the caller), lr, why the
  target is not a function, and the r7 backtrace.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from recomp.dispatch import DispatchTable, Entry, c_source
from recomp.imports import ImportTable
from recomp.lift.core import Lifted
from recomp.output import write_program
from recomp_harness import INCLUDE, SUPPORT, TEST_RUNTIME, host_cc, run_exe, run_snippet

M = 0xFFFFFFFF
RET = 0xFFFFFFF1  # MT_TEST_RETURN
SP0 = 0x200000 - 64


def synthetic_ptrs() -> list[int]:
    """Thumb functions with gaps from 4 bytes up (several per 16-byte bucket), ARM functions
    (word aligned, bit 0 clear) in between, and a stretch of 4-byte ARM import stubs."""
    ptrs, addr = [], 0x2000
    for gap in (4, 4, 6, 8, 2 + 2, 12, 16, 18, 32, 64, 100, 4, 4, 4, 256, 4096, 6):
        ptrs.append(addr | 1)
        addr += gap
    for arm in (0x8000, 0x8004, 0x8010, 0x9000):
        ptrs.append(arm)
    ptrs += [0x8008 | 1, 0x800E | 1]  # Thumb right next to ARM
    return sorted(ptrs)


STUBS = {0xA000 + 4 * i: i for i in range(5)}


def table() -> DispatchTable:
    return DispatchTable.build(synthetic_ptrs(), STUBS, [f"_imp{i}" for i in range(5)])


def probes(t: DispatchTable) -> list[int]:
    out = {0, 1, M, M - 1, t.base - 2, t.base - 1}
    for e in t.entries:
        for d in range(-6, 8):
            out.add((e.addr + d) & M)
        out.add(e.addr ^ 1)
    last = t.entries[-1].addr & ~1
    out |= {last + 16, last + 17, last + (1 << t.shift) * 3}
    return sorted(out)


def test_index_agrees_with_a_dictionary_for_every_nearby_address() -> None:
    t = table()
    by_addr = {e.addr: e for e in t.entries}
    for p in probes(t):
        assert t.lookup(p) == by_addr.get(p), hex(p)
    assert t.max_per_bucket() >= 3  # the scan inside a bucket is exercised
    assert len(t.entries) == len(synthetic_ptrs()) + len(STUBS)


def test_thumb_and_arm_pointers_are_distinct_keys() -> None:
    t = DispatchTable.build([0x2001, 0x3000])
    assert t.lookup(0x2001).fn == "f_00002001"
    assert t.lookup(0x2000) is None  # Thumb function in ARM state
    assert t.lookup(0x3000).fn == "f_00003000"
    assert t.lookup(0x3001) is None  # ARM function in Thumb state


@pytest.mark.parametrize(
    ("entries", "message"),
    [
        ((Entry(0x3001, "a"), Entry(0x2001, "b")), "sorted"),
        ((Entry(0x2000, "a"), Entry(0x2001, "b")), "two dispatch entries at 0x2000"),
        ((Entry(0x2002, "a"),), "not 4-byte aligned"),
    ],
)
def test_generator_rejects_bad_tables(entries: tuple[Entry, ...], message: str) -> None:
    with pytest.raises(ValueError, match=message):
        DispatchTable(entries)


def test_empty_table_finds_nothing() -> None:
    t = DispatchTable(())
    assert t.buckets == 0 and t.lookup(0x2001) is None
    assert "mt_fn_count = 0u" in t.data_source()


def test_dispatch_c_has_every_function_and_a_thunk_per_stub(tmp_path: Path) -> None:
    ptrs = [0x27E0, 0x2801, 0x2A3D]
    lifted = [
        Lifted(p, f"f_{p:08X}", f"void f_{p:08X}(mt_cpu *MT_RESTRICT cpu) {{}}\n") for p in ptrs
    ]
    imports = ImportTable(("_malloc", "_free"), {0xF0C90: 0, 0xF0C94: 1})
    write_program(tmp_path, lifted, imports)
    text = (tmp_path / "dispatch.c").read_text(encoding="utf-8")
    assert text == c_source(ptrs, imports.by_stub, imports.names)
    for p in ptrs:
        assert f"{{0x{p:08X}u, f_{p:08X}}}," in text
    assert "static void mt_stub_000F0C94(mt_cpu *MT_RESTRICT cpu) { MT_CALL_IMPORT(1u);" in text
    assert "{0x000F0C90u, mt_stub_000F0C90}," in text


def _build(tmp_path: Path, body: str, name: str) -> str:
    (tmp_path / "main.c").write_text(body, encoding="utf-8")
    exe = tmp_path / (f"{name}.exe" if sys.platform == "win32" else name)
    host_cc().build([tmp_path / "main.c", TEST_RUNTIME], exe, [INCLUDE, SUPPORT])
    status, out = run_exe(exe)
    assert status == 0, out
    return out


def _driver(t: DispatchTable, fns: list[str], extra: str = "") -> list[str]:
    lines = [
        '#include "test_runtime.h"',
        '#include "maplethief/recomp/dispatch.h"',
        "#include <stdio.h>",
        "",
        "static mt_cpu cpu;",
        "static uint32_t import_called;",
        "static void imp(mt_cpu *cpu_, uint32_t id) { (void)cpu_; import_called = id + 100u; }",
        "mt_import_fn mt_import_table[5] = {imp, imp, imp, imp, imp};",
        'const char *const mt_import_names[5] = {"_imp0", "_imp1", "_imp2", "_imp3", "_imp4"};',
    ]
    # Each function stores its own number, so the test never compares function pointers
    # (MSVC's identical-code folding may merge empty functions).
    lines += [f"void {f}(mt_cpu *MT_RESTRICT c) {{ c->r[0] = {i}u; }}" for i, f in enumerate(fns)]
    return lines + [extra, t.data_source()]


def test_c_lookup_matches_the_table_for_arm_and_thumb_pointers(tmp_path: Path) -> None:
    t = table()
    fns = [e.fn for e in t.entries if e.import_id is None]
    number = {f: i for i, f in enumerate(fns)}
    ps = probes(t)
    lines = _driver(t, fns)
    lines += [
        f"static const uint32_t probes[{len(ps)}] = {{{', '.join(f'0x{p:08X}u' for p in ps)}}};",
        "int main(void) {",
        "    uint32_t i;",
        f"    for (i = 0; i < {len(ps)}u; i++) {{",
        "        mt_guest_fn fn = mt_fn_lookup(probes[i]);",
        "        const mt_fn_entry *c = mt_fn_containing(probes[i]);",
        "        cpu.r[0] = 0xFFFFFFFFu;",
        "        import_called = 0u;",
        "        if (fn != NULL) {",
        "            fn(&cpu);",
        "        }",
        '        printf("%u %d %u 0x%08X 0x%08X\\n", i, fn != NULL, import_called, cpu.r[0],',
        "               c != NULL ? c->addr : 0u);",
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    out = _build(tmp_path, "\n".join(lines), "lookup")
    plain = sorted(t.entries, key=lambda e: e.addr & ~1)
    rows = out.splitlines()
    assert len(rows) == len(ps)
    thumb = arm = 0
    for p, row in zip(ps, rows, strict=True):
        _, found, called, r0, containing = row.split()
        want = t.lookup(p)
        assert int(found) == (want is not None), hex(p)
        if want is not None and want.import_id is not None:
            assert int(called) == want.import_id + 100, hex(p)
        elif want is not None:
            assert int(r0, 16) == number[want.fn], hex(p)
            thumb += p & 1
            arm += not p & 1
        before = [e for e in plain if e.addr & ~1 <= p & ~1]
        inside = before and p & ~1 < t.end
        assert int(containing, 16) == (before[-1].addr if inside else 0), hex(p)
    assert thumb >= 15 and arm >= 4


def test_miss_description_names_site_caller_reason_and_backtrace(tmp_path: Path) -> None:
    t = DispatchTable.build([0x1001, 0x2001, 0x3000, 0x4001], end=0x5000)
    fns = [e.fn for e in t.entries]
    # A frame chain on the guest stack: r7 -> {saved r7, lr in f_00004001} -> {0, RET}.
    cases = {
        "unknown": (0x5001, 0x1002),
        "thumb_as_arm": (0x2000, 0x1002),
        "arm_as_thumb": (0x3001, 0x1002),
        "misaligned": (0x3002, 0x1002),
        "inside": (0x2009, 0x1002),
        "null": (0, 0x1002),
        "below": (0x0801, 0x1002),
    }
    lines = _driver(t, fns)
    lines += [
        "int main(void) {",
        "    char text[1024];",
        "    mt_test_init(&cpu, 0x200000u, 0u);",
        f"    cpu.r[7] = 0x{SP0 - 16:X}u;",
        f"    mt_st32(&cpu, 0x{SP0 - 16:X}u, 0x{SP0 - 8:X}u, 0u);",
        f"    mt_st32(&cpu, 0x{SP0 - 12:X}u, 0x00004011u, 0u);",
        f"    mt_st32(&cpu, 0x{SP0 - 8:X}u, 0u, 0u);",
        f"    mt_st32(&cpu, 0x{SP0 - 4:X}u, 0x{RET:08X}u, 0u);",
        "    cpu.r[14] = 0x00001005u;",
    ]
    for name, (target, site) in cases.items():
        lines.append(
            f"    mt_dispatch_describe(&cpu, 0x{target:08X}u, 0x{site:08X}u, text, sizeof text);"
        )
        lines.append(f'    printf("{name}: %s\\n", text);')
    lines += [
        "    {",  # truncation: still NUL-terminated, length = what fit
        "        char small[16];",
        "        size_t n = mt_dispatch_describe(&cpu, 0x5001u, 0x1002u, small, sizeof small);",
        '        printf("small: %u [%s]\\n", (unsigned)n, small);',
        "    }",
        "    return 0;",
        "}",
        "",
    ]
    out = _build(tmp_path, "\n".join(lines), "describe")
    text = dict(line.split(": ", 1) for line in out.splitlines())
    common = "indirect call or jump at 0x00001002 (f_00001001+0x2), lr 0x00001005 (f_00001001+0x4)"
    backtrace = "backtrace (r7 chain): 0x00004011 (f_00004001+0x10) 0xFFFFFFF1"
    for name, (target, _) in cases.items():
        assert text[name].startswith(f"no guest function at 0x{target:08X}: {common}"), text[name]
        assert text[name].endswith(backtrace), text[name]
    assert "; past the last function;" in text["unknown"]
    assert (
        "; f_00002001 is Thumb code, but bit 0 of the target is clear (ARM)" in text["thumb_as_arm"]
    )
    assert "; f_00003000 is ARM code, but bit 0 of the target selects Thumb" in text["arm_as_thumb"]
    assert "; an ARM target must be 4-byte aligned" in text["misaligned"]
    assert "; 0x8 bytes after the start of f_00002001;" in text["inside"]
    assert "; a null function pointer;" in text["null"]
    assert "; below the first function;" in text["below"]
    assert text["small"] == "15 [no guest functi]"


def test_indirect_call_miss_reports_the_caller_and_backtrace(tmp_path: Path) -> None:
    """A guest function with a frame (push {r7, lr}; mov r7, sp) calls blx r3 to an
    unknown address. The trap names the call (pc), the target (detail), the function the
    call is in (the snippet itself, in the table at 0x1001) and the frame chain (RET)."""
    code = bytes.fromhex("80b56f46984780bd")  # push {r7, lr}; mov r7, sp; blx r3; pop
    out = run_snippet(
        tmp_path,
        code,
        {"r3": 0x5001, "r7": 0},
        functions={0x1001: "snippet"},
        expect_trap=True,
    )
    assert out["trap_kind"] == 7  # MT_TRAP_NO_FUNCTION
    assert out["trap_pc"] == 0x1004 and out["trap_detail"] == 0x5001
    assert out["r15"] == 0x1004  # the runtime records the site as the guest PC
    assert out["r14"] == 0x1007
    assert out["miss_caller"] == 0x1001
    assert out["miss_frames"] == 1 and out["miss_frame0"] == RET
