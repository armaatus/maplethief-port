"""recomp.decode.arm (A32, #33) against hand-checked encodings and against Capstone, the
decoder's oracle (ADR 0003, ADR 0014). Synthetic inputs only."""

from __future__ import annotations

import random

import capstone
import pytest

from recomp.decode import decode_arm
from recomp.decode.arm import decode_word
from recomp.decode.bits import arm_expand_imm_text
from recomp.decode.insn import AL, EQ, NE

_CS = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)

# (word as little-endian hex, text): assembled with clang (armv7-a, .arm) and checked by hand.
ENCODINGS = [
    ("041080e2", "add r1, r0, #4"),
    ("032181e0", "add r2, r1, r3, lsl #2"),
    ("0740c5e3", "bic r4, r5, #7"),
    ("0260a0e1", "mov r6, r2"),
    ("08708fe2", "add r7, pc, #8"),
    ("200056e3", "cmp r6, #0x20"),
    ("01808812", "addne r8, r8, #1"),
    ("11a050e2", "subs sl, r0, #0x11"),
    ("1eff2fe1", "bx lr"),
    ("001090e5", "ldr r1, [r0]"),
    ("042093e4", "ldr r2, [r3], #4"),
    ("10402de9", "push {r4, lr}"),
    ("1040bde8", "pop {r4, lr}"),
    ("04402de5", "str r4, [sp, #-4]!"),  # Capstone has no single-register push alias
    ("04409de4", "pop {r4}"),  # ldr r4, [sp], #4
    ("fd0300fa", "blx #0x2000"),  # at 0x1004: PC + 8 + 0xFF4, Thumb target
    ("fb0700ea", "b #0x3000"),  # at 0x100C
    ("0000a0e1", "mov r0, r0"),
    ("0201a0e1", "lsl r0, r2, #2"),
    ("3102a0e1", "lsr r0, r1, r2"),
    ("6000a0e1", "rrx r0, r0"),
    ("910203e0", "mul r3, r1, r2"),
    ("923164e0", "mls r4, r2, r1, r3"),
    ("921083e0", "umull r1, r3, r2, r0"),
    ("110f6fe1", "clz r0, r1"),
    ("b2005ee1", "ldrh r0, [lr, #-2]"),
    ("d02041e1", "ldrd r2, r3, [r1, #-0]"),
    ("0300c0e7", "strb r0, [r0, r3]"),
]


def _at(hexcode: str) -> int:
    return 0x100C if hexcode == "fb0700ea" else 0x1004 if hexcode == "fd0300fa" else 0x1000


@pytest.mark.parametrize(("hexcode", "text"), ENCODINGS, ids=[t for _, t in ENCODINGS])
def test_encoding_text(hexcode: str, text: str) -> None:
    code = bytes.fromhex(hexcode)
    insn = decode_arm(code, 0, _at(hexcode))
    assert insn.known and insn.arm and insn.size == 4
    assert insn.text() == text
    cs = next(_CS.disasm(code, _at(hexcode)))
    assert f"{cs.mnemonic} {cs.op_str}" == text


def test_conditions_and_pc() -> None:
    bne = decode_word(0x1AFFFFFC, 0x1008)  # bne back to 0x1000: PC (0x1010) - 16
    assert (bne.mnemonic, bne.cond, bne.operands[0].target) == ("b", NE, 0x1000)
    assert bne.pc == 0x1010
    assert decode_word(0x02899001, 0x1000).cond == EQ  # addeq r9, r9, #1
    assert decode_word(0xFA0003FD, 0x1004).cond == AL  # cond 1111: blx is unconditional


def test_modified_immediate_carry_and_spelling() -> None:
    movs = decode_word(0xE3B004FF, 0)  # movs r0, #0xff000000: rotated, carry = bit 31 = 1
    assert movs.operands[1].value == 0xFF000000 and movs.operands[1].carry == 1
    plain = decode_word(0xE3B000FF, 0)  # movs r0, #0xff: not rotated, carry unchanged
    assert plain.operands[1].carry is None
    assert arm_expand_imm_text(0x4FF) == "#0xff000000"
    assert arm_expand_imm_text(0x204) == "#4, #4"  # 4 ror 4: not LLVM's canonical encoding


def test_unmodelled_forms_are_undefined() -> None:
    for word in (
        0xE0810312,  # add r0, r1, r2, lsl r3 (register-shifted register)
        0xE7910122,  # ldr r0, [r1, r2, lsr #2] (index shift other than lsl)
        0xE8900006 | 1 << 22,  # ldm r0, {r1, r2}^ (user registers)
        0xE9900006,  # ldmib r0, {r1, r2}
        0xE4B10004,  # ldrt r0, [r1], #4
        0xE0B10392,  # umulls r1, r0, r2, r3 (flag-setting long multiply)
    ):
        insn = decode_word(word, 0)
        assert not insn.known and insn.arm, f"{word:#x}: {insn.text()}"


# (name, mask, value): the encoding classes the decoder handles (ARM ARM A5.1-A5.5): the bits
# in mask are fixed to value, the others random.
CLASSES = [
    ("dp-register", 0x0E000010, 0x00000000),
    ("mov-register-shifted", 0x0FEF0090, 0x01A00010),  # lsl/lsr/asr/ror rd, rm, rs
    ("dp-immediate", 0x0E000000, 0x02000000),
    ("ldst-immediate", 0x0E000000, 0x04000000),
    ("ldst-register", 0x0E000010, 0x06000000),
    ("extra-ldst", 0x0E000090, 0x00000090),
    ("multiply", 0x0F0000F0, 0x00000090),
    ("bx-blx-register", 0x0FFFFFD0, 0x012FFF10),
    ("clz", 0x0FFF0FF0, 0x016F0F10),
    ("ldm-stm", 0x0E000000, 0x08000000),
    ("branch", 0x0E000000, 0x0A000000),
]


@pytest.mark.parametrize(("name", "mask", "value"), CLASSES, ids=[c[0] for c in CLASSES])
def test_random_encodings_match_capstone(name: str, mask: int, value: int) -> None:
    """Every word of the class that the decoder decodes, Capstone decodes to the same text
    (at a fixed address, with random conditions and the unconditional space for branches)."""
    rng = random.Random(f"a32-{name}")
    decoded = 0
    differ = []
    for _ in range(4000):
        word = (rng.getrandbits(32) & ~mask) | value
        cond = rng.choice((0xE, 0xE, rng.randrange(15), 0xF if name == "branch" else 0xE))
        word = (word & 0x0FFFFFFF) | cond << 28
        ours = decode_word(word, 0x10000)
        if not ours.known:
            continue
        decoded += 1
        cs = list(_CS.disasm(word.to_bytes(4, "little"), 0x10000))
        theirs = f"{cs[0].mnemonic} {cs[0].op_str}".strip() if cs else "<nothing>"
        if theirs != ours.text():
            differ.append(f"{word:08x}: ours {ours.text()!r}, Capstone {theirs!r}")
    assert not differ, differ[:10]
    assert decoded >= 40, decoded  # the class is really exercised
