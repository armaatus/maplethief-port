"""ADR 0014's prototype on the real armv7 slice (local only: needs the IPA).

* The decoder agrees with Capstone on every instruction the armscan sweep finds in __text.
* The prototype functions (recomp.prototype.FUNCTIONS: a leaf, IT blocks, VFP, an import
  call) lift cleanly into work/gen/proto/, compile with clang -std=c17 -Wall -Wextra -Werror
  -Wpedantic, and pass a syntax check for the MSVC target (clang --target=*-windows-msvc,
  with shim CRT headers because there is no Windows SDK here).
* Run with the image mapped, they return what independent references compute: the MSVC
  rand() LCG, Apple's Reachability sample logic, and IEEE float arithmetic.
"""

from __future__ import annotations

import math
import random
import shutil
import struct
import subprocess
import sys

import pytest

from recomp import prototype
from recomp.decode import decode_thumb
from recomp.loader import Image
from recomp_harness import INCLUDE, MSVC_SHIM, SUPPORT, TEST_RUNTIME, CCompiler, host_cc
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / "work" / "slices" / "MapleThief.armv7"
M = 0xFFFFFFFF


@pytest.fixture(scope="module")
def image() -> Image:
    return Image.load(SLICE)


@pytest.fixture(scope="module")
def built():
    return prototype.build(SLICE, prototype.OUT_DIR)


def test_decoder_matches_capstone_on_the_whole_binary(image: Image) -> None:
    from mtport import armscan, macho

    scanner = armscan.Scanner(macho.load(SLICE))
    base = image.text_base
    equal, differ, undecoded = 0, [], 0
    for ptr, start, end in scanner.functions():
        if not ptr & 1:
            continue
        insns, _, _ = scanner.decode(start, end, True)
        pending: tuple[int, ...] = ()
        prev = None
        for cs in insns:
            if prev is not None and cs.address != prev:
                pending = ()
            cond, pending = (pending[0], pending[1:]) if pending else (None, ())
            ours = decode_thumb(image.code, cs.address - base, cs.address, cond)
            prev = cs.address + cs.size
            if ours.mnemonic.startswith("it") and cond is None:
                pending = ours.it_conditions()
            if not ours.known:
                undecoded += 1  # #34 decodes every NEON instruction of the binary
                continue
            theirs = f"{cs.mnemonic} {cs.op_str}".strip()
            if ours.text() == theirs and ours.size == cs.size:
                equal += 1
            else:
                differ.append(f"{cs.address:#x}: {ours.text()} vs {theirs}")
    assert not differ, differ[:20]
    assert equal >= 330052 and undecoded == 0  # #34: the 381 NEON instructions included


def test_prototype_lifts_cleanly(built) -> None:
    lifted, sources = built
    assert [f.ptr for f in lifted] == [p for p, _, _ in prototype.FUNCTIONS]
    for f in lifted:
        assert f.clean, (f.name, f.problems)
    assert all(s.is_file() and s.is_relative_to(REPO / "work") for s in sources)
    assert "MT_CALL_IMPORT(" in sources[0].read_text(encoding="utf-8")  # Rand_Chance


def _clang() -> str:
    path = shutil.which("clang")
    if path is None:
        pytest.skip("clang not found")
    return path


def test_prototype_compiles_with_clang(built) -> None:
    _, sources = built
    gen = sources[0].parent
    clang = _clang()
    for src in sources:
        cmd = [clang, "-std=c17", "-Wall", "-Wextra", "-Werror", "-Wpedantic", "-O2"]
        cmd += ["-ffp-contract=off", f"-I{INCLUDE}", f"-I{gen}", "-fsyntax-only", str(src)]
        done = subprocess.run(cmd, capture_output=True, text=True)
        assert done.returncode == 0, done.stderr


@pytest.mark.parametrize("msc_path", [False, True], ids=["clang-macros", "msc-macros"])
def test_prototype_compiles_for_the_msvc_target(built, msc_path: bool) -> None:
    """clang in MSVC mode (LLP64, Microsoft extensions); with -U__clang__ the support
    headers take their _MSC_VER branch (no musttail, __forceinline, __declspec)."""
    _, sources = built
    gen = sources[0].parent
    cmd = [_clang(), "--target=x86_64-pc-windows-msvc", "-fms-compatibility", "-fms-extensions"]
    cmd += ["-std=c17", "-Wall", "-Wextra", "-Werror", f"-isystem{MSVC_SHIM}"]
    cmd += [f"-I{INCLUDE}", f"-I{gen}", "-fsyntax-only"]
    if msc_path:  # clang does not know MSVC's own spelling, #pragma fp_contract(off)
        cmd += ["-U__clang__", "-Wno-unknown-pragmas"]
    done = subprocess.run([*cmd, *map(str, sources)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr


# --- running the lifted functions ----------------------------------------------------------


def msvc_rand(seed: int) -> tuple[int, int]:
    seed = (seed * 214013 + 2531011) & M
    return seed, (seed >> 16) & 0x7FFF


def reachability(flags: int) -> int:
    """Apple's Reachability sample, -networkStatusForFlags: (0 none, 1 WiFi, 2 WWAN)."""
    if flags & 0x2 == 0:  # kSCNetworkReachabilityFlagsReachable
        return 0
    status = 0
    if flags & 0x4 == 0:  # ConnectionRequired
        status = 1
    if flags & 0x28 and flags & 0x10 == 0:  # OnDemand | OnTraffic, not InterventionRequired
        status = 1
    if flags & 0x40000:  # IsWWAN
        status = 2
    return status


def f32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def r32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def seed_address(image: Image) -> int:
    """Rand_Next reads its seed through ``ldr.w ip, [pc, #n]; add ip, pc``."""
    ldr = decode_thumb(image.code, 0x22604 - image.text_base, 0x22604)
    assert ldr.text() == "ldr.w ip, [pc, #0x34]"
    literal = image.const_word(((0x22604 + 4) & ~3) + 0x34, 4)
    return (literal + 0x22608 + 4) & M


def test_prototype_runs_like_the_references(built, image: Image, tmp_path) -> None:
    rng = random.Random(30)
    seed_addr = seed_address(image)
    data_vm, data = image.segment_bytes("__DATA")
    text_vm, text = image.segment_bytes("__TEXT")
    assert data_vm <= seed_addr < data_vm + 0x2A000
    (tmp_path / "text.bin").write_bytes(text)
    (tmp_path / "data.bin").write_bytes(data)
    seed0 = 0x1234ABCD
    percents = [rng.randrange(-5, 106) for _ in range(300)]
    chances = [(rng.randrange(0, 50), rng.randrange(1, 60)) for _ in range(300)]
    points = [tuple(r32(rng.uniform(-500, 500)) for _ in range(4)) for _ in range(300)]
    points += [(1.5, 2.5, 1.5, 2.5), (0.0, 0.0, 3.0, 4.0)]
    flags = [rng.getrandbits(32) & rng.choice((0x4003E, 0xFFFFFFFF)) for _ in range(500)]
    flags += [0, 2, 6, 0x12, 0x22, 0x3A, 0x40002, 0x40006]

    def arr(name: str, values) -> str:
        return f"static const uint32_t {name}[] = {{{', '.join(f'0x{v:X}u' for v in values)}}};"

    driver = "\n".join(
        [
            '#include "test_runtime.h"',
            '#include "mt_gen.h"',
            "#include <stdio.h>",
            arr("percents", [p & M for p in percents]),
            arr("chance_a", [a for a, _ in chances]),
            arr("chance_b", [b for _, b in chances]),
            arr("pts", [f32(v) for p in points for v in p]),
            arr("flags", flags),
            "static uint32_t call(mt_cpu *cpu, mt_guest_fn fn, uint32_t a, uint32_t b, uint32_t c,",
            "                     uint32_t d) {",
            "    cpu->r[0] = a; cpu->r[1] = b; cpu->r[2] = c; cpu->r[3] = d;",
            "    cpu->r[14] = MT_TEST_RETURN;",
            "    fn(cpu);",
            "    return cpu->r[0];",
            "}",
            "int main(int argc, char **argv) {",
            "    mt_cpu cpu;",
            "    unsigned i;",
            "    (void)argc;",
            "    mt_test_init(&cpu, 0x200000u, MT_IMPORT_COUNT);",
            f"    if (mt_test_load(&cpu, argv[1], 0x{text_vm:X}u) ||"
            f" mt_test_load(&cpu, argv[2], 0x{data_vm:X}u)) return 2;",
            f"    mt_st32(&cpu, 0x{seed_addr:X}u, 0x{seed0:X}u, 0u);",
            "    for (i = 0; i < 200; i++)",
            '        printf("next %u\\n", call(&cpu, f_000225E5, 0, 0, 0, 0));',
            "    for (i = 0; i < sizeof percents / 4; i++)",
            '        printf("percent %u\\n", call(&cpu, f_000226B9, percents[i], 0, 0, 0));',
            "    for (i = 0; i < sizeof chance_a / 4; i++)",
            '        printf("chance %u\\n",',
            "               call(&cpu, f_00022685, chance_a[i], chance_b[i], 0, 0));",
            "    for (i = 0; i < sizeof pts / 16; i++)",
            '        printf("dist 0x%08X\\n", call(&cpu, f_000B45FD, pts[4 * i], pts[4 * i + 1],',
            "                                        pts[4 * i + 2], pts[4 * i + 3]));",
            "    for (i = 0; i < sizeof flags / 4; i++)",
            '        printf("reach %u\\n", call(&cpu, f_000CA049, 0, 0, flags[i], 0));',
            "    return 0;",
            "}",
            "",
        ]
    )
    (tmp_path / "driver.c").write_text(driver, encoding="utf-8")
    _, sources = built
    exe = tmp_path / ("proto.exe" if sys.platform == "win32" else "proto")
    cc = CCompiler(_clang(), False, True) if shutil.which("clang") else host_cc()
    cc.build(
        [tmp_path / "driver.c", TEST_RUNTIME, *sources], exe, [INCLUDE, SUPPORT, sources[0].parent]
    )
    done = subprocess.run(
        [str(exe), str(tmp_path / "text.bin"), str(tmp_path / "data.bin")],
        capture_output=True, text=True, timeout=60,
    )  # fmt: skip
    assert done.returncode == 0, done.stdout + done.stderr
    lines = done.stdout.split("\n")

    seed = seed0
    expected = []
    for _ in range(200):
        seed, v = msvc_rand(seed)
        expected.append(f"next {v}")
    for n in percents:
        seed, v = msvc_rand(seed)
        expected.append(f"percent {int(v % 100 + 1 <= n)}")
    for a, b in chances:
        seed, v = msvc_rand(seed)
        expected.append(f"chance {int(v % b + 1 <= a)}")
    for x1, y1, x2, y2 in points:
        dx, dy = r32(x2 - x1), r32(y2 - y1)
        expected.append(f"dist 0x{f32(r32(math.sqrt(r32(r32(dx * dx) + r32(dy * dy))))):08X}")
    expected += [f"reach {reachability(f)}" for f in flags]
    assert lines[: len(expected)] == expected
