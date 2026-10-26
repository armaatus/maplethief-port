"""The ADR 0014 lifter end to end on synthetic Thumb-2: lift -> compile with the host C
compiler (MSVC cl on the Windows runner) -> run -> compare the final CPU state with a
Python model of the ARM ARM semantics. The encodings were assembled with clang
(thumbv7, cortex-a8, vfpv3); the assembly is next to each one. No game code."""

from __future__ import annotations

import math
import struct

import pytest

from recomp.snippet import lift_snippet
from recomp_harness import DATA_BASE, SNIPPET_BASE, run_snippet
from thumbasm import thumb_bl

M = 0xFFFFFFFF
N_, Z_, C_, V_ = 1 << 31, 1 << 30, 1 << 29, 1 << 28


def code(*parts: str) -> bytes:
    return b"".join(bytes.fromhex(p) for p in parts)


def nzcv(n: int, z: int, c: int, v: int) -> int:
    return n << 31 | z << 30 | c << 29 | v << 28


def add_with_carry(a: int, b: int, c: int) -> tuple[int, int, int]:
    """ARM ARM AddWithCarry: (result, carry, overflow)."""
    u = a + b + c
    r = u & M
    s = (a ^ 0x80000000) - 0x80000000 + ((b ^ 0x80000000) - 0x80000000) + c
    return r, u >> 32, int(s != (r ^ 0x80000000) - 0x80000000)


def f32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f32v(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def f64(x: float) -> int:
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def round32(x: float) -> float:
    return f32v(f32(x))


FLAGS = code("4218", "431a", "50eb0104", "70eb0105", "8842", "7047")
# adds r2, r0, r1 / subs r3, r0, r1 / adcs.w r4, r0, r1 / sbcs.w r5, r0, r1 / cmp r0, r1 / bx lr


@pytest.mark.parametrize(
    ("a", "b"),
    [(0xFFFFFFFF, 1), (0x7FFFFFFF, 1), (0, 1), (0x80000000, 0x80000000), (5, 3), (3, 5)],
)
def test_arithmetic_flags(tmp_path, a: int, b: int) -> None:
    out = run_snippet(tmp_path, FLAGS, {"r0": a, "r1": b})
    r2, _, _ = add_with_carry(a, b, 0)
    r3, c1, _ = add_with_carry(a, ~b & M, 1)
    r4, c2, _ = add_with_carry(a, b, c1)
    r5, _, _ = add_with_carry(a, ~b & M, c2)
    r, c, v = add_with_carry(a, ~b & M, 1)
    assert (out["r2"], out["r3"], out["r4"], out["r5"]) == (r2, r3, r4, r5)
    assert out["cpsr"] >> 28 == nzcv(r >> 31, int(r == 0), c, v) >> 28


IT = code("8842", "ccbf", "0122", "0222", "04bf", "431d", "0924", "7047")
# cmp r0, r1 / ite gt / movgt r2, #1 / movle r2, #2 / itt eq / addeq r3, r0, #5 /
# moveq r4, #9 / bx lr


@pytest.mark.parametrize(("a", "b"), [(5, 3), (3, 3), (0xFFFFFFFF, 1)])
def test_it_blocks(tmp_path, a: int, b: int) -> None:
    out = run_snippet(tmp_path, IT, {"r0": a, "r1": b, "r3": 0xAAAA, "r4": 0xBBBB})
    signed = lambda x: x - (1 << 32) if x >> 31 else x  # noqa: E731
    assert out["r2"] == (1 if signed(a) > signed(b) else 2)
    eq = a == b
    assert out["r3"] == ((a + 5) & M if eq else 0xAAAA)
    assert out["r4"] == (9 if eq else 0xBBBB)


def test_loop_with_conditional_branch(tmp_path) -> None:
    # movs r1, #0 / 1: adds r1, r1, r0 / subs r0, #1 / bne 1b / bx lr
    out = run_snippet(tmp_path, code("0021", "0918", "0138", "fcd1", "7047"), {"r0": 10})
    assert (out["r0"], out["r1"]) == (0, 55)
    assert out["cpsr"] & Z_


def test_call_push_pop_and_return(tmp_path) -> None:
    # push {r4, r7, lr} / add r7, sp, #4 / mov r4, r0 / bl 0x2000 / adds r0, r0, r4 /
    # pop {r4, r7, pc}
    body = code("90b5", "01af", "0446") + thumb_bl(SNIPPET_BASE + 6, 0x2000) + code("0019", "90bd")
    callee = (
        "void f_00002001(mt_cpu *MT_RESTRICT cpu) {\n"
        "    cpu->r[0] *= 3u;\n"
        "    cpu->r[2] = cpu->r[14];\n"
        "    cpu->r[4] = 0xDEADu;\n"  # a callee may clobber a register the caller saved
        "}"
    )
    assert lift_snippet(body, SNIPPET_BASE).callees == {"f_00002001"}
    out = run_snippet(tmp_path, body, {"r0": 7, "r4": 0x4444, "r7": 0x7777}, extra_c=callee)
    assert out["r0"] == 7 * 3 + 0xDEAD  # adds r0, r0, r4 sees the callee's r4
    assert (out["r4"], out["r7"]) == (0x4444, 0x7777)  # restored by pop
    assert out["r2"] == SNIPPET_BASE + 10 + 1  # lr: return address with the Thumb bit
    assert out["r13"] == 0x200000 - 64  # sp balanced


def b_w(at: int, target: int) -> bytes:
    """``b.w target`` (T4) at ``at``: BL's encoding with hw2 bit 14 clear."""
    hw1, hw2 = struct.unpack("<HH", thumb_bl(at, target))
    return struct.pack("<HH", hw1, hw2 & ~0x4000)


def test_tail_call_and_return_through_a_register(tmp_path) -> None:
    tail = b_w(SNIPPET_BASE, 0x2000)  # b.w 0x2000: a tail call to another function
    callee = "void f_00002001(mt_cpu *MT_RESTRICT cpu) { cpu->r[0] = 0x77u; }"
    out = run_snippet(tmp_path, tail, {"r0": 1}, extra_c=callee)
    assert out["r0"] == 0x77
    # mov r3, lr / bx r3: an indirect jump to the caller's address is a return
    out = run_snippet(tmp_path, code("7346", "1847"), {"r0": 5})
    assert out["r0"] == 5


def test_indirect_call_through_the_dispatch_table(tmp_path) -> None:
    # push {r7, lr} / blx r3 / pop {r7, pc}
    callee = "void target_fn(mt_cpu *MT_RESTRICT cpu) { cpu->r[0] = cpu->r[1] + 1u; }"
    out = run_snippet(
        tmp_path,
        code("80b5", "9847", "80bd"),
        {"r1": 41, "r3": 0x3001},
        extra_c=callee,
        functions={0x3001: "target_fn"},
    )
    assert out["r0"] == 42


LOADSTORE = code(
    "0278", "90f90130", "4488", "b0f90250", "50f8086b", "40f8046d", "80f80820", "50f82180", "7047"
)
# ldrb r2, [r0] / ldrsb.w r3, [r0, #1] / ldrh r4, [r0, #2] / ldrsh.w r5, [r0, #2] /
# ldr r6, [r0], #8 / str r6, [r0, #-4]! / strb.w r2, [r0, #8] / ldr.w r8, [r0, r1, lsl #2]


def test_loads_and_stores(tmp_path) -> None:
    mem = bytes([0x81, 0xF0, 0x34, 0x92]) + bytes(12) + struct.pack("<I", 0x0BADF00D)
    out = run_snippet(
        tmp_path,
        LOADSTORE,
        {"r0": DATA_BASE, "r1": 3},
        {DATA_BASE: mem},
        read={"w4": (DATA_BASE + 4, 4), "b12": (DATA_BASE + 12, 1)},
    )
    assert out["r2"] == 0x81
    assert out["r3"] == 0xFFFFFFF0
    assert out["r4"] == 0x9234
    assert out["r5"] == 0xFFFF9234
    assert out["r6"] == 0x9234F081
    assert out["r0"] == DATA_BASE + 4  # post-index +8, then pre-index -4 with writeback
    assert out["w4"] == 0x9234F081 and out["b12"] == 0x81
    assert out["r8"] == 0x0BADF00D  # [r0 + r1 << 2] = DATA_BASE + 16


def test_load_store_multiple(tmp_path) -> None:
    # ldm r0!, {r2, r3, r4} / stmdb r1!, {r2, r3} / push.w {r8, r9, lr} / pop.w {...} / bx lr
    mem = struct.pack("<3I", 0x11111111, 0x22222222, 0x33333333)
    out = run_snippet(
        tmp_path,
        code("1cc8", "21e90c00", "2de90043", "bde80043", "7047"),
        {"r0": DATA_BASE, "r1": DATA_BASE + 0x100, "r8": 8, "r9": 9},
        {DATA_BASE: mem},
        read={"lo": (DATA_BASE + 0xF8, 4), "hi": (DATA_BASE + 0xFC, 4)},
    )
    assert (out["r2"], out["r3"], out["r4"]) == (0x11111111, 0x22222222, 0x33333333)
    assert out["r0"] == DATA_BASE + 12 and out["r1"] == DATA_BASE + 0xF8
    assert (out["lo"], out["hi"]) == (0x11111111, 0x22222222)
    assert (out["r8"], out["r9"]) == (8, 9)


def test_literal_pool_is_folded(tmp_path) -> None:
    # ldr r0, [pc, #8] / ldr.w r1, [pc, #0xc] / adr r2, #4 / bx lr / nop / .long x2
    body = code("0248", "dff80c10", "01a2", "7047", "00bf", "78563412", "0df0feca")
    snippet = lift_snippet(body, SNIPPET_BASE)
    assert "0x12345678u" in snippet.source and "MT_LD32" not in snippet.source
    out = run_snippet(tmp_path, body)
    assert (out["r0"], out["r1"], out["r2"]) == (0x12345678, 0xCAFEF00D, SNIPPET_BASE + 0xC)


VFP = code(
    "00ee100a", "00ee901a", "30ee201a", "60ee201a", "b7eec12a", "b1eec23b", "b7eec34b",
    "b4eee00a", "f1ee10fa", "ccbf", "0122", "0022", "14ee103a", "55ec134b", "f1ee404a",
    "14ee906a", "7047",
)  # fmt: skip
# vmov s0, r0 / vmov s1, r1 / vadd.f32 s2, s0, s1 / vmul.f32 s3, s0, s1 /
# vcvt.f64.f32 d2, s2 / vsqrt.f64 d3, d2 / vcvt.f32.f64 s8, d3 / vcmpe.f32 s0, s1 /
# vmrs APSR_nzcv, fpscr / ite gt / movgt r2, #1 / movle r2, #0 / vmov r3, s8 /
# vmov r4, r5, d3 / vneg.f32 s9, s0 / vmov r6, s9 / bx lr


@pytest.mark.parametrize(("a", "b"), [(3.0, 4.0), (2.5, -1.25), (1e-3, 1e-3), (7.0, math.nan)])
def test_vfp(tmp_path, a: float, b: float) -> None:
    a, b = round32(a), round32(b)  # the guest sees the inputs as f32
    out = run_snippet(tmp_path, VFP, {"r0": f32(a), "r1": f32(b)})
    s2 = round32(a + b)
    s3 = round32(a * b)
    d3 = math.sqrt(s2) if s2 >= 0 else math.nan
    assert out["d1"] == f32(s3) << 32 | f32(s2)  # s2, s3 are the halves of d1
    if math.isnan(b):
        assert out["d1"] & M == 0x7FC00000 + (f32(b) & 0x3FFFFF)  # the NaN operand, quiet
        assert out["cpsr"] >> 28 == 0b0011 and out["r2"] == 0  # unordered: not gt
        return
    assert out["r4"] | out["r5"] << 32 == f64(d3)
    assert out["r3"] == f32(d3)
    assert out["r2"] == (1 if a > b else 0)
    expected = 0b0110 if a == b else 0b1000 if a < b else 0b0010
    assert out["cpsr"] >> 28 == expected
    assert out["r6"] == f32(a) ^ 0x80000000


def test_vfp_nan_results_follow_arm(tmp_path) -> None:
    # vmov s0, r0 / vmov s1, r1 / vadd.f32 s2, s0, s1 / vmov r2, s2 / vmov d1, r2, r3 /
    # vsqrt.f64 d2, d1 / vmov r4, r5, d2 / bx lr
    body = code(
        "00ee100a", "00ee901a", "30ee201a", "11ee102a", "43ec112b", "b1eec12b", "55ec124b", "7047"
    )
    out = run_snippet(tmp_path, body, {"r0": 0x7F800000, "r1": 0xFF800000, "r3": 0xBFF00000})
    assert out["r2"] == 0x7FC00000  # inf + -inf: ARM's default NaN (x86 would set the sign)
    assert (out["r5"], out["r4"]) == (0x7FF80000, 0)  # sqrt of a negative: default NaN


def test_multiplies(tmp_path) -> None:
    # smmul r2, r0, r1 / mls r3, r0, r1, r2 / umull r4, r5, r0, r1 / smull r6, r8, r0, r1 /
    # muls r0, r1, r0 / bx lr
    a, b = 0x9234F081, 0x51EB851F
    out = run_snippet(
        tmp_path, code("50fb01f2", "00fb1123", "a0fb0145", "80fb0168", "4843", "7047"),
        {"r0": a, "r1": b},
    )  # fmt: skip
    sa, sb = a - (1 << 32), b
    smul = (sa * sb) & 0xFFFFFFFFFFFFFFFF
    assert out["r2"] == smul >> 32
    assert out["r3"] == (out["r2"] - a * b) & M
    assert out["r4"] | out["r5"] << 32 == a * b
    assert out["r6"] | out["r8"] << 32 == smul
    assert out["r0"] == (a * b) & M
    assert bool(out["cpsr"] & N_) == bool((a * b) & 0x80000000)


def test_bit_operations(tmp_path) -> None:
    # ubfx r2, r0, #4, #8 / sbfx r3, r0, #4, #8 / bfi r4, r0, #8, #4 / uxtb r5, r0 /
    # sxth r6, r0 / clz r8, r0 / rev.w r9, r0 / bx lr
    x = 0x00F8A5C3
    out = run_snippet(
        tmp_path,
        code("c0f30712", "40f30713", "60f30b24", "c5b2", "06b2", "b0fa80f8", "90fa80f9", "7047"),
        {"r0": x, "r4": 0xFFFFFFFF},
    )
    field = (x >> 4) & 0xFF
    assert out["r2"] == field
    assert out["r3"] == (field - 0x100 if field & 0x80 else field) & M
    assert out["r4"] == (0xFFFFFFFF & ~0xF00) | (x & 0xF) << 8
    assert out["r5"] == x & 0xFF
    assert out["r6"] == (x & 0xFFFF) - (0x10000 if x & 0x8000 else 0) & M
    assert out["r8"] == 8
    assert out["r9"] == int.from_bytes(x.to_bytes(4, "little"), "big")


def test_shifts_and_carry(tmp_path) -> None:
    # lsls r2, r0, #4 / lsrs r3, r0, #1 / asrs r4, r0, #31 / lsls r5, r1 /
    # ror.w r6, r0, #8 / rrx r8, r0 / bx lr
    x = 0x90000001
    body = code("0201", "4308", "c417", "8d40", "4fea3026", "4fea3008", "7047")
    for r1, r5 in ((4, 0x18000000), (0, 0x12345678), (32, 0xFFFFFFFF), (0x128, 0xFFFFFFFF)):
        out = run_snippet(tmp_path, body, {"r0": x, "r1": r1, "r5": r5})
        assert out["r2"] == (x << 4) & M
        assert out["r3"] == x >> 1
        assert out["r4"] == 0xFFFFFFFF
        amount = r1 & 0xFF  # a register shift uses the bottom byte
        if amount == 0:  # result unchanged, C kept from asrs r4, r0, #31 (bit 30 of x)
            result, carry = r5, (x >> 30) & 1
        elif amount <= 32:
            result, carry = (r5 << amount) & M, (r5 >> (32 - amount)) & 1
        else:
            result, carry = 0, 0
        assert out["r5"] == result
        assert out["r6"] == ((x >> 8) | (x << 24)) & M
        assert out["r8"] == (x >> 1) | carry << 31  # rrx shifts in C of lsls r5, r1
        assert bool(out["cpsr"] & C_) == bool(carry)


def test_logic_and_shifter_carry(tmp_path) -> None:
    # ands r2, r0, #0xff00 / orrs.w r3, r0, r1, lsl #4 / eors.w r4, r0, r1 /
    # bics.w r5, r0, r1 / mvns r6, r0 / tst.w r0, #0x80000000 / bx lr
    a, b = 0x8000FF0F, 0x0F0000F1
    out = run_snippet(
        tmp_path,
        code("10f47f42", "50ea0113", "90ea0104", "30ea0105", "c643", "10f0004f", "7047"),
        {"r0": a, "r1": b, "cpsr": V_},
    )
    assert out["r2"] == a & 0xFF00
    assert out["r3"] == (a | (b << 4)) & M
    assert (out["r4"], out["r5"], out["r6"]) == (a ^ b, a & ~b & M, ~a & M)
    # tst.w with a rotated immediate: C = bit 31 of the immediate; V untouched
    assert out["cpsr"] >> 28 == nzcv(1, 0, 1, 1) >> 28


@pytest.mark.parametrize(("r0", "r1"), [(0, 2), (5, 1)])
def test_cbz(tmp_path, r0: int, r1: int) -> None:
    # cbz r0, 1f / movs r1, #1 / bx lr / 1: movs r1, #2 / bx lr
    out = run_snippet(tmp_path, code("08b1", "0121", "7047", "0221", "7047"), {"r0": r0})
    assert out["r1"] == r1


def test_vfp_loads_stores_and_push_pop(tmp_path) -> None:
    # vldr d0, [r0] / vldr s4, [r0, #8] / vstr d0, [r0, #16] / vpush {d8, d9} /
    # vpop {d8, d9} / vstmia r0, {d0} / bx lr
    mem = struct.pack("<dI", 1.5, 0x40490FDB)
    out = run_snippet(
        tmp_path,
        code("90ed000b", "90ed022a", "80ed040b", "2ded048b", "bdec048b", "80ec020b", "7047"),
        {"r0": DATA_BASE, "d8": 0x0123456789ABCDEF, "d9": 9},
        {DATA_BASE: mem},
        read={"lo": (DATA_BASE + 16, 4), "hi": (DATA_BASE + 20, 4)},
    )
    assert out["d0"] == f64(1.5)
    assert out["d2"] & M == 0x40490FDB
    assert out["lo"] | out["hi"] << 32 == f64(1.5)
    assert (out["d8"], out["d9"]) == (0x0123456789ABCDEF, 9)
    assert out["r13"] == 0x200000 - 64


def test_unknown_instruction_becomes_a_trap() -> None:
    snippet = lift_snippet(code("e0ff1000", "7047"), SNIPPET_BASE)  # vshr.u32 d16, d0, #32 (NEON)
    assert snippet.unsupported and snippet.unsupported[0][0] == SNIPPET_BASE
    assert "MT_TRAP(MT_TRAP_UNSUPPORTED" in snippet.source


def test_unsupported_instruction_traps_at_run_time_with_its_address(tmp_path) -> None:
    # movs r0, #5 / mrs r1, apsr (decoded as undefined: no lifter) / movs r0, #6 / bx lr
    body = code("0520", "eff30081", "0620", "7047")
    snippet = lift_snippet(body, SNIPPET_BASE)
    assert snippet.unsupported == ((SNIPPET_BASE + 2, "undefined 0xf3ef8100"),)
    out = run_snippet(tmp_path, body, expect_trap=True)
    trap = (out["trap_kind"], out["trap_pc"], out["trap_detail"])
    assert trap == (1, SNIPPET_BASE + 2, 0xF3EF8100)  # MT_TRAP_UNSUPPORTED, pc, raw
    assert out["r0"] == 5  # the state at the trap


def test_flag_live_into_a_function_is_reported() -> None:
    from recomp.discover import discover_function
    from recomp.lift.core import LiftOptions, lift_function
    from recomp.snippet import _SnippetProgram

    body = code("18bf", "0120", "7047")  # it ne / movne r0, #1 / bx lr: reads Z it never set
    fn = discover_function(body, SNIPPET_BASE, SNIPPET_BASE | 1, SNIPPET_BASE + len(body))
    lifted = lift_function(fn, _SnippetProgram(body, SNIPPET_BASE), LiftOptions())
    assert any("flag Z read where it may be undefined" in p for p in lifted.problems)
