"""Flag semantics of the lifted integer instructions against a Python transcription of the
ARM ARM pseudocode (#33): AddWithCarry (A2.2.1) for the arithmetic, Shift_C (A8.4.3) with the
register-controlled amounts 0, 1-31, 32, 33-255 and the bottom-byte rule, the shifter
carry-out of the logical instructions, and V left alone where the pseudocode leaves it.

Each instruction is lifted once and run over every pair of a set of edge values (0, 1, the
signed limits, all ones, ...) with C clear and set, and N/Z/V preset, so a flag an
instruction must keep is checked too. The encodings were assembled with clang; Capstone
checks the same forms in the snippet suite. Synthetic code only."""

from __future__ import annotations

import itertools

import pytest

from recomp_harness import run_cases

M = 0xFFFFFFFF
EDGES = [0, 1, 2, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF,
         0x12345678, 0xF0000001]  # fmt: skip
AMOUNTS = [0, 1, 2, 15, 31, 32, 33, 63, 64, 127, 128, 255, 256, 257, 0xFFFFFF20, 0x80000001]


def add_with_carry(a: int, b: int, c: int) -> tuple[int, int, int]:
    """A2.2.1 AddWithCarry: (result, carry_out, overflow)."""
    unsigned = a + b + c
    signed = (a ^ 0x80000000) - 0x80000000 + (b ^ 0x80000000) - 0x80000000 + c
    result = unsigned & M
    return result, int(unsigned != result), int(signed != (result ^ 0x80000000) - 0x80000000)


def shift_c(x: int, kind: str, n: int, c: int) -> tuple[int, int]:
    """A8.4.3 Shift_C for a shift amount n (already the bottom byte, or an immediate)."""
    if kind == "rrx":
        return (c << 31) | (x >> 1), x & 1
    if n == 0:
        return x, c
    if kind == "lsl":
        return (x << n) & M if n < 32 else 0, (x >> (32 - n)) & 1 if n <= 32 else 0
    if kind == "lsr":
        return x >> n if n < 32 else 0, (x >> (n - 1)) & 1 if n <= 32 else 0
    if kind == "asr":
        sx = x - (1 << 32) if x >> 31 else x
        return (sx >> min(n, 32)) & M, (sx >> min(n - 1, 31)) & 1
    m = n % 32  # ror
    result = ((x >> m) | (x << (32 - m))) & M if m else x
    return result, result >> 31


def flags(n: int, z: int, c: int, v: int) -> int:
    return n << 31 | z << 30 | c << 29 | v << 28


def nz(result: int) -> tuple[int, int]:
    return result >> 31, int(result == 0)


ARITH = {  # mnemonic: (Thumb encoding, (a, b, c) -> AddWithCarry operands)
    "adds.w r2, r0, r1": ("10eb0102", lambda a, b, c: (a, b, 0)),
    "adcs.w r2, r0, r1": ("50eb0102", lambda a, b, c: (a, b, c)),
    "subs.w r2, r0, r1": ("b0eb0102", lambda a, b, c: (a, ~b & M, 1)),
    "sbcs.w r2, r0, r1": ("70eb0102", lambda a, b, c: (a, ~b & M, c)),
    "rsbs.w r2, r0, r1": ("d0eb0102", lambda a, b, c: (b, ~a & M, 1)),
    "cmp.w r0, r1": ("b0eb010f", lambda a, b, c: (a, ~b & M, 1)),
    "cmn.w r0, r1": ("10eb010f", lambda a, b, c: (a, b, 0)),
    "adds.w r2, r0, r1, asr #32": ("10eb2102", lambda a, b, c: (a, shift_c(b, "asr", 32, 0)[0], 0)),
    "rsbs.w r2, r0, r1, lsr #32": ("d0eb1102", lambda a, b, c: (~a & M, 0, 1)),
}
A32_ARITH = {
    "adds r2, r0, r1": ("012090e0", lambda a, b, c: (a, b, 0)),
    "sbcs r2, r0, r1": ("0120d0e0", lambda a, b, c: (a, ~b & M, c)),
    "rscs r2, r0, r1": ("0120f0e0", lambda a, b, c: (b, ~a & M, c)),
}


def _pairs(values_b: list[int]) -> list[tuple[int, int, int, int]]:
    """(a, b, carry in, NZ/V preset) cases: every pair, both carries, other flags varied."""
    out = []
    for i, (a, b, c) in enumerate(itertools.product(EDGES, values_b, (0, 1))):
        out.append((a, b, c, (i * 5) % 16))  # N Z V presets cycle through all patterns
    return out


def _run(tmp_path, hexcode: str, cases, arm: bool = False):
    code = bytes.fromhex(hexcode + ("1eff2fe1" if arm else "7047"))
    states = [{"r0": a, "r1": b, "r2": 0xDEAD, "cpsr": (pre & 0b1101 | c << 1) << 28}
              for a, b, c, pre in cases]  # fmt: skip
    return run_cases(tmp_path, code, states, arm=arm)


@pytest.mark.parametrize("asm", [*ARITH, *A32_ARITH])
def test_arithmetic_flags_match_add_with_carry(tmp_path, asm: str) -> None:
    arm = asm in A32_ARITH
    hexcode, operands = (A32_ARITH if arm else ARITH)[asm]
    cases = _pairs(EDGES)
    compare = asm.startswith(("cmp", "cmn"))
    for (a, b, c, _), got in zip(cases, _run(tmp_path, hexcode, cases, arm), strict=True):
        result, carry, overflow = add_with_carry(*operands(a, b, c))
        want = flags(*nz(result), carry, overflow)
        where = f"{asm} with r0={a:#x} r1={b:#x} C={c}"
        assert got["cpsr"] >> 28 == want >> 28, where
        assert got["r2"] == (0xDEAD if compare else result), where


SHIFTS = {  # register-controlled shifts: (encoding, kind, sets flags)
    "lsls.w r2, r0, r1": ("10fa01f2", "lsl", True),
    "lsrs.w r2, r0, r1": ("30fa01f2", "lsr", True),
    "asrs.w r2, r0, r1": ("50fa01f2", "asr", True),
    "rors.w r2, r0, r1": ("70fa01f2", "ror", True),
    "lsl.w r2, r0, r1": ("00fa01f2", "lsl", False),
    "asr.w r2, r0, r1": ("40fa01f2", "asr", False),
}
A32_SHIFTS = {
    "lsls r2, r0, r1": ("1021b0e1", "lsl", True),
    "rors r2, r0, r1": ("7021b0e1", "ror", True),
}


@pytest.mark.parametrize("asm", [*SHIFTS, *A32_SHIFTS])
def test_register_shifts_match_shift_c(tmp_path, asm: str) -> None:
    arm = asm in A32_SHIFTS
    hexcode, kind, setflags = (A32_SHIFTS if arm else SHIFTS)[asm]
    cases = _pairs(AMOUNTS)
    for (a, b, c, pre), got in zip(cases, _run(tmp_path, hexcode, cases, arm), strict=True):
        result, carry = shift_c(a, kind, b & 0xFF, c)  # the bottom byte of Rm
        before = (pre & 0b1101 | c << 1) << 28
        v = (before >> 28) & 1
        want = flags(*nz(result), carry, v) if setflags else before
        where = f"{asm} with r0={a:#x} r1={b:#x} C={c}"
        assert got["r2"] == result, where
        assert got["cpsr"] >> 28 == want >> 28, where


# Logical instructions with an immediate-shifted register: C is the shifter carry-out
# (unchanged for lsl #0), V is unchanged. (encoding, op, kind, amount); mvn/mov have no rn.
LOGIC = {
    "ands.w r2, r0, r1, lsl #3": ("10eac102", lambda a, s: a & s, "lsl", 3),
    "orrs.w r2, r0, r1, lsr #32": ("50ea1102", lambda a, s: a | s, "lsr", 32),
    "eors.w r2, r0, r1, asr #32": ("90ea2102", lambda a, s: a ^ s, "asr", 32),
    "bics.w r2, r0, r1, ror #7": ("30eaf112", lambda a, s: a & ~s & M, "ror", 7),
    "mvns.w r2, r1, rrx": ("7fea3102", lambda a, s: ~s & M, "rrx", 1),
    "lsls.w r2, r1, #1": ("5fea4102", lambda a, s: s, "lsl", 1),
    "teq.w r0, r1, asr #5": ("90ea611f", lambda a, s: a ^ s, "asr", 5),
    "lsrs r2, r1, #32 (A32)": ("2120b0e1", lambda a, s: s, "lsr", 32),
}


@pytest.mark.parametrize("asm", list(LOGIC))
def test_logical_shifter_carry(tmp_path, asm: str) -> None:
    hexcode, op, kind, amount = LOGIC[asm]
    arm = asm.endswith("(A32)")
    cases = _pairs(EDGES)
    for (a, b, c, pre), got in zip(cases, _run(tmp_path, hexcode, cases, arm), strict=True):
        shifted, carry = shift_c(b, kind, amount, c)
        result = op(a, shifted)
        v = pre & 1
        want = flags(*nz(result), carry, v)
        where = f"{asm} with r0={a:#x} r1={b:#x} C={c}"
        assert got["cpsr"] >> 28 == want >> 28, where
        assert got["r2"] == (0xDEAD if asm.startswith("teq") else result), where


def test_model_matches_the_arm_arm_examples() -> None:
    """The model itself, on values worked out by hand from the pseudocode."""
    assert add_with_carry(0xFFFFFFFF, 1, 0) == (0, 1, 0)
    assert add_with_carry(0x7FFFFFFF, 1, 0) == (0x80000000, 0, 1)
    assert add_with_carry(0x80000000, 0x7FFFFFFF, 1) == (0, 1, 0)  # subs 0x80000000 - 0x80000000
    assert shift_c(0x80000001, "lsl", 32, 0) == (0, 1)
    assert shift_c(0x80000001, "lsl", 33, 1) == (0, 0)
    assert shift_c(0x80000001, "lsr", 32, 0) == (0, 1)
    assert shift_c(0x80000000, "asr", 200, 0) == (M, 1)
    assert shift_c(0x80000001, "ror", 32, 0) == (0x80000001, 1)
    assert shift_c(0x00000001, "ror", 1, 0) == (0x80000000, 1)
    assert shift_c(0x12345678, "lsl", 0, 1) == (0x12345678, 1)
