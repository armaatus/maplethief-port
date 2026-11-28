"""The differential test's import models (#305): the Python side (recomp/difftest/models.py,
run by the emulator) against hand-worked results, and against the C side
(recomp/difftest/models.c, the driver's import table) on the same cases, built with the host
compiler. Both run in CI; neither needs Unicorn nor the game."""

from __future__ import annotations

import random
import re
import struct
import subprocess
import zlib

import pytest

from recomp.difftest import host, models
from recomp.difftest.inputs import HEAP, WINDOW
from recomp_harness import host_cc
from repo_paths import REPO

TEXT = (0x1000, 0x2000)
STUB = 0xF0E44  # any address: the pc of a model's fault
LOW = 0x120000  # what the C side compares: [LOW, WINDOW)


def memory() -> models.Memory:
    return models.Memory(bytearray(WINDOW), TEXT)


def put(mem: models.Memory, addr: int, data: bytes) -> None:
    mem.data[addr : addr + len(data)] = data


def run(name: str, regs: list[int], mem: models.Memory) -> list[int]:
    r = list(regs) + [0] * (4 - len(regs))
    models.call(name, r, mem)
    return r


def test_the_c_table_lists_the_models_in_order() -> None:
    text = (REPO / "recomp" / "difftest" / "models.c").read_text(encoding="utf-8")
    names = re.search(r"mt_import_names\[\] = \{(.*?)\};", text, re.S)[1]
    assert re.findall(r'"([^"]*)"', names) == [*models.MODELS, ""]
    assert models.UNMODELED == len(models.MODELS)
    assert set(models.IMPLEMENTATIONS) == set(models.MODELS)


def test_division() -> None:
    mem = memory()
    m32 = 0xFFFFFFFF
    assert run("___divsi3", [7, m32 - 1], mem)[0] == m32 - 2  # 7 / -2 = -3
    assert run("___modsi3", [m32 - 6, 2], mem)[0] == m32  # -7 % 2 = -1
    assert run("___divsi3", [0x80000000, m32], mem)[0] == 0x80000000  # INT_MIN / -1
    assert run("___divsi3", [5, 0], mem)[0] == 0 and run("___modsi3", [5, 0], mem)[0] == 5
    assert run("___udivsi3", [m32, 16], mem)[0] == 0x0FFFFFFF
    assert run("___umodsi3", [m32, 16], mem)[0] == 15
    r = run("___divdi3", [0, 0xFFFFFFFF, 2, 0], mem)  # -(2^32) / 2 = -(2^31)
    assert (r[0], r[1]) == (0x80000000, 0xFFFFFFFF)
    r = run("___udivdi3", [0, 1, 2, 0], mem)  # 2^32 / 2
    assert (r[0], r[1]) == (0x80000000, 0)


def test_strings_and_memory() -> None:
    mem = memory()
    put(mem, HEAP, b"abc\0")
    put(mem, HEAP + 0x10, b"abd\0")
    assert run("_strlen", [HEAP], mem)[0] == 3
    assert run("_strcmp", [HEAP, HEAP + 0x10], mem)[0] == (ord("c") - ord("d")) & 0xFFFFFFFF
    assert run("_memcmp", [HEAP, HEAP + 0x10, 2], mem)[0] == 0
    assert run("_strcpy", [HEAP + 0x20, HEAP], mem)[0] == HEAP + 0x20
    assert mem.data[HEAP + 0x20 : HEAP + 0x24] == b"abc\0"
    run("_strcat", [HEAP + 0x20, HEAP + 0x10], mem)
    assert mem.data[HEAP + 0x20 : HEAP + 0x27] == b"abcabd\0"
    run("_memset", [HEAP + 0x40, 0x1FF, 3], mem)
    assert mem.data[HEAP + 0x40 : HEAP + 0x44] == b"\xff\xff\xff\0"
    put(mem, HEAP + 0x50, b"12345")
    run("_memcpy", [HEAP + 0x51, HEAP + 0x50, 4], mem)  # overlapping: byte by byte, upward
    assert mem.data[HEAP + 0x50 : HEAP + 0x55] == b"11111"


def test_faults_and_text_writes() -> None:
    mem = memory()
    with pytest.raises(models.Fault) as f:
        run("_memset", [WINDOW - 2, 0, 8], mem)
    assert f.value.addr == WINDOW and mem.data[WINDOW - 2 :] == b"\0\0"
    with pytest.raises(models.Fault) as f:
        run("_memcpy", [HEAP, 0xFFFFFFF0, 1], mem)  # the read faults first
    assert f.value.addr == 0xFFFFFFF0
    put(mem, WINDOW - 3, b"xyz")
    with pytest.raises(models.Fault) as f:
        run("_strlen", [WINDOW - 3], mem)
    assert f.value.addr == WINDOW
    with pytest.raises(models.TextWrite):
        run("_memset", [TEXT[0] - 1, 0, 4], mem)


def test_a_lazy_window_loads_only_the_pages_a_model_touches() -> None:
    full = memory()
    put(full, HEAP + 0xFFE, b"ab")
    put(full, HEAP + 0x1000, b"cd\0")
    loads: list[int] = []

    def load(addr: int, n: int) -> bytes:
        loads.append(addr)
        return bytes(full.data[addr : addr + n])

    lazy = models.Memory(bytearray(b"\xee" * WINDOW), TEXT, load)
    assert run("_strlen", [HEAP + 0xFFE], lazy)[0] == 4  # across a page boundary
    run("_memset", [HEAP + 0x2002, 0x41, 2], lazy)
    assert loads == [HEAP, HEAP + 0x1000, HEAP + 0x2000]
    assert lazy.data[HEAP + 0x2000 : HEAP + 0x2005] == b"\0\0AA\0"  # the rest of the page kept
    assert lazy.dirty == [(HEAP + 0x2002, HEAP + 0x2004)]


# -- against the C side ------------------------------------------------------------------

HARNESS = r"""
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "maplethief/recomp/recomp.h"

const uint32_t mt_dt_import_stubs[%(n)d] = {%(stubs)s};
static jmp_buf stop;
static uint32_t fault_addr, fault_pc;

void mt_mem_fault(mt_cpu *cpu, uint32_t addr, uint32_t size, uint32_t pc) {
    (void)cpu; (void)size;
    fault_addr = addr;
    fault_pc = pc;
    longjmp(stop, 1);
}

void mt_trap(mt_cpu *cpu, uint32_t kind, uint32_t pc, uint32_t detail) {
    (void)cpu; (void)kind; (void)pc; (void)detail;
    exit(3);
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static mt_cpu cpu;

static void call(uint32_t id) {
    if (setjmp(stop) == 0) {
        mt_import_table[id](&cpu, id);
    }
}

int main(int argc, char **argv) {
    FILE *in = fopen(argv[1], "rb");
    uint32_t head[2], c;
    (void)argc;
    cpu.mem = (uint8_t *)calloc(%(window)du, 1);
    cpu.mem_size = %(window)du;
    if (in == NULL || cpu.mem == NULL || fread(head, 4, 2, in) != 2) {
        return 2;
    }
    for (c = 0; c < head[0]; c++) {
        uint32_t rec[7];
        if (fread(rec, 4, 7, in) != 7 || rec[6] > head[1] ||
            fread(cpu.mem + rec[5], 1, rec[6], in) != rec[6]) {
            return 2;
        }
        memcpy(cpu.r, rec + 1, 16);
        fault_addr = fault_pc = 0;
        call(rec[0]);
        printf("%%u %%u %%u %%u %%u %%u\n", fault_pc != 0u, fault_addr, fault_pc, cpu.r[0],
               cpu.r[1], crc32(cpu.mem + %(low)du, %(window)du - %(low)du));
        memset(cpu.mem + %(low)du, 0, %(window)du - %(low)du);
    }
    return 0;
}
"""


def cases() -> list[tuple[str, list[int], int, bytes]]:
    """(model, r0-r3, patch address, patch bytes): random strings and blocks in the heap,
    sizes across the window's end, and random divisions."""
    rng = random.Random("305:models")
    out = []
    ends = (WINDOW - 1, WINDOW - 7, WINDOW + 5, 0xFFFFFFFF)
    for i in range(400):
        name = models.MODELS[i % len(models.MODELS)]
        blob = bytes(rng.choice((0, 0, 65, 66, rng.randrange(256))) for _ in range(64))
        at = HEAP + rng.randrange(0, 0x3000)
        ptrs = [at + rng.randrange(64) for _ in range(2)]
        if rng.random() < 0.2:
            ptrs[rng.randrange(2)] = rng.choice(ends)
        if rng.random() < 0.15:
            blob, at = b"q" * 40, WINDOW - 40  # a string the window cuts
            ptrs[1] = at
        size = rng.choice((0, 1, 5, 64, 0x30000, rng.randrange(1 << 32)))
        if name.startswith("___"):
            regs = [rng.choice((0, 1, 0x80000000, 0xFFFFFFFF, rng.randrange(1 << 32)))
                    for _ in range(4)]  # fmt: skip
        else:
            regs = [ptrs[0], ptrs[1] if name != "_memset" else rng.randrange(1 << 32), size, 0]
        out.append((name, regs, at, blob))
    return out


def python_side(case) -> tuple[int, int, int, int, int, int]:
    name, regs, at, blob = case
    mem = memory()
    put(mem, at, blob)
    r = list(regs)
    try:
        models.call(name, r, mem)
    except models.Fault as f:
        return (1, f.addr, STUB, regs[0], regs[1], zlib.crc32(mem.data[LOW:]))
    return (0, 0, 0, r[0], r[1], zlib.crc32(mem.data[LOW:]))


def test_python_and_c_models_agree(tmp_path) -> None:
    cc = host_cc()
    src = tmp_path / "harness.c"
    stubs = ", ".join(f"0x{STUB:X}u" for _ in models.MODELS)
    src.write_text(
        HARNESS % {"n": len(models.MODELS), "stubs": stubs, "window": WINDOW, "low": LOW},
        encoding="utf-8",
    )
    exe = tmp_path / "harness.exe"
    objs = []
    for s in (src, host.MODELS_C):
        obj = tmp_path / (s.stem + ".o")
        done = subprocess.run(
            host.Compiler(cc.path, cc.msvc).compile(s, obj), capture_output=True, text=True,
            cwd=tmp_path,
        )  # fmt: skip
        assert done.returncode == 0, done.stdout + done.stderr
        objs.append(obj)
    done = subprocess.run(
        host.Compiler(cc.path, cc.msvc).link(objs, exe), capture_output=True, text=True,
        cwd=tmp_path,
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    all_cases = cases()
    data = [struct.pack("<2I", len(all_cases), 64)]
    for name, regs, at, blob in all_cases:
        data.append(struct.pack("<7I", models.MODEL_IDS[name], *regs, at, len(blob)) + blob)
    (tmp_path / "cases.bin").write_bytes(b"".join(data))
    out = subprocess.run([str(exe), str(tmp_path / "cases.bin")], capture_output=True,
                         text=True, timeout=120)  # fmt: skip
    assert out.returncode == 0, out.stderr
    c_side = [tuple(map(int, line.split())) for line in out.stdout.splitlines()]
    assert len(c_side) == len(all_cases)
    faults = 0
    for case, c in zip(all_cases, c_side, strict=True):
        py = python_side(case)
        # A fault leaves r0 and r1 as they were on the Python side; the C side's are moot.
        got = c if not c[0] else (*c[:3], case[1][0], case[1][1], c[5])
        assert got == py, (case[0], [hex(x) for x in case[1]], hex(case[2]))
        faults += py[0]
    assert 40 < faults < len(all_cases) - 100
