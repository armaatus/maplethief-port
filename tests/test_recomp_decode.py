"""recomp.decode against hand-checked encodings and against Capstone, the decoder's oracle
(ADR 0003, ADR 0014). Synthetic inputs only."""

from __future__ import annotations

import random
import re
import struct

import capstone
import pytest

from recomp.decode import decode_linear, decode_thumb, is_wide
from recomp.decode.bits import thumb_expand_imm_c, vfp_expand_imm
from recomp.decode.insn import AL, EQ, GT, LE, NE

_CS = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)

# (bytes as hex, text): assembled with clang (thumbv7, cortex-a8, vfpv3) and checked by hand.
ENCODINGS = [
    ("4218", "adds r2, r0, r1"),
    ("431a", "subs r3, r0, r1"),
    ("50eb0104", "adcs.w r4, r0, r1"),
    ("70eb0105", "sbcs.w r5, r0, r1"),
    ("8842", "cmp r0, r1"),
    ("7047", "bx lr"),
    ("90b5", "push {r4, r7, lr}"),
    ("01af", "add r7, sp, #4"),
    ("0446", "mov r4, r0"),
    ("90bd", "pop {r4, r7, pc}"),
    ("0278", "ldrb r2, [r0]"),
    ("90f90130", "ldrsb.w r3, [r0, #1]"),
    ("4488", "ldrh r4, [r0, #2]"),
    ("50f8086b", "ldr r6, [r0], #8"),
    ("40f8046d", "str r6, [r0, #-0x4]!"),
    ("50f82180", "ldr.w r8, [r0, r1, lsl #2]"),
    ("1cc8", "ldm r0!, {r2, r3, r4}"),
    ("21e90c00", "stmdb r1!, {r2, r3}"),
    ("2de90043", "push.w {r8, sb, lr}"),
    ("bde80043", "pop.w {r8, sb, lr}"),
    ("0248", "ldr r0, [pc, #8]"),
    ("dff80810", "ldr.w r1, [pc, #8]"),
    ("00ee100a", "vmov s0, r0"),
    ("30ee201a", "vadd.f32 s2, s0, s1"),
    ("b7eec12a", "vcvt.f64.f32 d2, s2"),
    ("b1eec23b", "vsqrt.f64 d3, d2"),
    ("b7eec34b", "vcvt.f32.f64 s8, d3"),
    ("b4eee00a", "vcmpe.f32 s0, s1"),
    ("f1ee10fa", "vmrs apsr_nzcv, fpscr"),
    ("55ec134b", "vmov r4, r5, d3"),
    ("f1ee404a", "vneg.f32 s9, s0"),
    ("50fb01f2", "smmul r2, r0, r1"),
    ("00fb1123", "mls r3, r0, r1, r2"),
    ("a0fb0145", "umull r4, r5, r0, r1"),
    ("4843", "muls r0, r1, r0"),
    ("c0f30712", "ubfx r2, r0, #4, #8"),
    ("60f30b24", "bfi r4, r0, #8, #4"),
    ("b0fa80f8", "clz r8, r0"),
    ("4fea3026", "ror.w r6, r0, #8"),
    ("4fea3008", "rrx r8, r0"),
    ("10f47f42", "ands r2, r0, #0xff00"),
    ("50ea0113", "orrs.w r3, r0, r1, lsl #4"),
    ("10f0004f", "tst.w r0, #-0x80000000"),
    ("4ff0ff32", "mov.w r2, #-1"),
    ("90ed000b", "vldr d0, [r0]"),
    ("2ded048b", "vpush {d8, d9}"),
    ("bdec048b", "vpop {d8, d9}"),
    ("80ec020b", "vstmia r0, {d0}"),
    ("fede", "trap"),
    ("bff35b8f", "dmb ish"),
]


@pytest.mark.parametrize(("hexcode", "text"), ENCODINGS)
def test_encoding_text(hexcode: str, text: str) -> None:
    code = bytes.fromhex(hexcode)
    insn = decode_thumb(code, 0, 0x1000)
    assert insn.known and insn.size == len(code)
    assert insn.text() == text
    cs = next(_CS.disasm(code, 0x1000))
    assert f"{cs.mnemonic} {cs.op_str}".strip() == text


def test_it_block_conditions_and_flags() -> None:
    # cmp r0, r1; ite gt; movgt r2, #1; movle r2, #2; itt eq; addeq r3, r0, #5; moveq r4, #9
    code = bytes.fromhex("8842ccbf0122022204bf431d0924")
    insns = decode_linear(code, 0x1000)
    assert [i.text() for i in insns] == [
        "cmp r0, r1", "ite gt", "movgt r2, #1", "movle r2, #2",
        "itt eq", "addeq r3, r0, #5", "moveq r4, #9",
    ]  # fmt: skip
    assert insns[1].it_conditions() == (GT, LE)
    assert insns[4].it_conditions() == (EQ, EQ)
    assert not insns[2].setflags and insns[2].in_it  # a 16-bit movs inside IT sets no flags
    assert [i.cond for i in insns[5:]] == [EQ, EQ]
    outside = decode_thumb(bytes.fromhex("0122"), 0, 0x1000)  # the same bytes outside IT
    assert outside.text() == "movs r2, #1" and outside.setflags and outside.cond == AL


def test_four_instruction_it_block() -> None:
    insn = decode_thumb(bytes.fromhex("c1bf"), 0, 0x1000)  # itttt gt
    assert insn.text() == "itttt gt"
    assert insn.it_conditions() == (GT, GT, GT, GT)
    ite = decode_thumb(bytes.fromhex("14bf"), 0, 0x1000)  # ite ne
    assert ite.it_conditions() == (NE, EQ)


def test_thumb_expand_imm() -> None:
    assert thumb_expand_imm_c(0x0AB) == (0xAB, None)
    assert thumb_expand_imm_c(0x1AB) == (0x00AB00AB, None)
    assert thumb_expand_imm_c(0x2AB) == (0xAB00AB00, None)
    assert thumb_expand_imm_c(0x3AB) == (0xABABABAB, None)
    assert thumb_expand_imm_c(0x100) is None  # replicated zero byte: UNPREDICTABLE
    assert thumb_expand_imm_c(0x4FF) == (0x7F800000, 0)  # rotated: carry = bit 31
    assert thumb_expand_imm_c(0x400) == (0x80000000, 1)


def test_vfp_expand_imm() -> None:
    assert vfp_expand_imm(0x70, double=False) == 0x3F800000  # 1.0
    assert vfp_expand_imm(0xA2, double=False) == 0xC1100000  # -9.0
    assert vfp_expand_imm(0x70, double=True) == 0x3FF0000000000000


_NUM = re.compile(r"#(-?)(0x[0-9a-f]+|\d+)")


def normalize(text: str) -> str:
    """Capstone's spelling with the number format taken out: it prints some immediates
    signed and some unsigned, and some negative offsets in decimal, others in hex."""

    def number(m: re.Match) -> str:
        value = int(m.group(2), 0) * (-1 if m.group(1) else 1)
        return f"#0x{value & 0xFFFFFFFF:x}"

    return _NUM.sub(number, text).replace(", #0x0]", "]")


def capstone_text(code: bytes) -> str | None:
    found = next(_CS.disasm(code, 0x1000), None)
    return None if found is None else f"{found.mnemonic} {found.op_str}".strip()


def test_every_16bit_encoding_matches_capstone() -> None:
    """All 65536 halfwords outside an IT block: where both decode, the texts are equal, and
    we decode nothing Capstone rejects (except udf #0xf9, which this Capstone refuses)."""
    differ, only_ours = [], []
    for hw in range(0x10000):
        if is_wide(hw) or (hw >> 8 == 0xBF and hw & 0xF):  # 32-bit prefix / IT
            continue
        code = struct.pack("<H", hw)
        ours = decode_thumb(code, 0, 0x1000)
        theirs = capstone_text(code)
        if not ours.known:
            continue
        if theirs is None:
            if hw != 0xDEF9:
                only_ours.append(f"{hw:04x} {ours.text()}")
        elif ours.text() != theirs:
            differ.append(f"{hw:04x}: {ours.text()} vs {theirs}")
    assert not differ, differ[:20]
    assert not only_ours, only_ours[:20]


def test_random_32bit_encodings_match_capstone() -> None:
    """60,000 random 32-bit encodings: everything we decode, Capstone decodes the same way
    (texts equal up to the number format)."""
    rng = random.Random(30)
    differ, only_ours = [], []
    decoded = 0
    for _ in range(60000):
        hw1 = rng.choice((0xE800, 0xF000, 0xF800)) | rng.getrandbits(11)
        code = struct.pack("<HH", hw1, rng.getrandbits(16))
        ours = decode_thumb(code, 0, 0x1000)
        if not ours.known:
            continue
        decoded += 1
        theirs = capstone_text(code)
        if theirs is None:
            only_ours.append(f"{code.hex()} {ours.text()}")
        elif normalize(ours.text()) != normalize(theirs):
            differ.append(f"{code.hex()}: {ours.text()} vs {theirs}")
    assert decoded > 15000
    assert not differ, differ[:20]
    assert not only_ours, only_ours[:20]
