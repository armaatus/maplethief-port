"""The raw Thumb-2 helpers of mtport.armscan (no Capstone), on code from tests/thumbasm.py.

The Capstone sweep itself is tested in test_imports.py."""

import struct

import pytest

import thumbasm as asm
from mtport import armscan


@pytest.mark.parametrize("imm", [0, 0x1234, 0x0800, 0xF000, 0xFFFF])
@pytest.mark.parametrize("rd", [0, 7, 12])
def test_thumb_mov16_decodes_movw_and_movt(rd, imm):
    for code, movt in ((asm.movw(rd, imm), False), (asm.movt(rd, imm), True)):
        hw1, hw2 = struct.unpack("<HH", code)
        assert armscan.thumb_mov16(hw1, hw2) == (movt, rd, imm)


def test_thumb_mov16_rejects_other_instructions():
    hw1, hw2 = struct.unpack("<HH", asm.thumb_bl(0x1000, 0x2001))
    assert armscan.thumb_mov16(hw1, hw2) is None
    assert armscan.thumb_mov16(0xF240, 0x8000) is None  # bit 15 of hw2 set: not movw


def test_movw_movt_constants():
    code = (
        asm.mov32(0, 0xEDB88320)  # 0x1000
        + asm.movw(1, 0x55)  # 0x1008: no movt follows
        + asm.h(asm.NOP)
        + asm.movw(2, 0x1111)  # 0x100E, movt to another register first
        + asm.movt(3, 0x2222)
        + asm.movt(2, 0x3333)
    )
    got = armscan.movw_movt_constants(code, 0x1000)
    assert dict(got) == {0xEDB88320: [0x1000], 0x55: [0x1008], 0x33331111: [0x100E]}


def test_pc_relative_values():
    t = asm.Thumb(0x2000).movw(0, 0x100).movt(0, 0).add_pc(0)  # add at 0x2008: pc 0x200C
    assert armscan.pc_relative_values(bytes(t.code), 0x2000) == [0x210C]
