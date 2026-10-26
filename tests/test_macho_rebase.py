"""Unit tests for the dyld rebase-opcode decoder (mtport.macho_dyld via the mtport.macho face)
and the file <-> VM mapping, on synthetic images only."""

import unittest

from machofixtures import (
    DATA_SEGMENT_INDEX,
    DATA_VMADDR,
    REBASE_ADD_ADDR,
    REBASE_ADD_ADDR_IMM_SCALED,
    REBASE_ADD_ADDR_ULEB,
    REBASE_DONE,
    REBASE_IMM_TIMES,
    REBASE_SEGMENT_OFFSET,
    REBASE_TYPE_IMM,
    REBASE_ULEB_TIMES,
    REBASE_ULEB_TIMES_SKIPPING,
    TEXT_VMADDR,
    ImageBuilder,
    thin_image,
    uleb128,
)
from mtport import macho, macho_dyld

SEG = bytes([REBASE_SEGMENT_OFFSET | DATA_SEGMENT_INDEX])
POINTER = bytes([REBASE_TYPE_IMM | macho.REBASE_TYPE_POINTER])


def image(rebase: bytes) -> macho.MachO:
    return macho.MachO(ImageBuilder().build(rebase=rebase))


def addresses(m: macho.MachO) -> list[int]:
    return [r.address for r in m.rebases]


class RebaseTest(unittest.TestCase):
    def test_face_reexports_the_part(self):
        self.assertIn("Rebase", macho_dyld.__all__)
        for name in macho_dyld.__all__:
            self.assertIs(getattr(macho, name), getattr(macho_dyld, name), name)

    def test_imm_times(self):
        m = image(POINTER + SEG + uleb128(0x10) + bytes([REBASE_IMM_TIMES | 3, REBASE_DONE]))
        self.assertEqual(
            m.rebases,
            tuple(macho.Rebase(DATA_VMADDR + 0x10 + 4 * i, 1) for i in range(3)),
        )

    def test_uleb_times_and_add_addr(self):
        stream = (
            POINTER
            + SEG
            + uleb128(0)
            + bytes([REBASE_ULEB_TIMES])
            + uleb128(2)  # 0x5000, 0x5004 -> 0x5008
            + bytes([REBASE_ADD_ADDR])
            + uleb128(0x18)  # -> 0x5020
            + bytes([REBASE_ADD_ADDR_IMM_SCALED | 2])  # -> 0x5028
            + bytes([REBASE_IMM_TIMES | 1, REBASE_DONE])
        )
        self.assertEqual(addresses(image(stream)), [0x5000, 0x5004, 0x5028])

    def test_add_addr_uleb_and_skipping(self):
        stream = (
            POINTER
            + SEG
            + uleb128(0x100)
            + bytes([REBASE_ADD_ADDR_ULEB])
            + uleb128(0xC)  # 0x5100, then +4+0xC -> 0x5110
            + bytes([REBASE_ULEB_TIMES_SKIPPING])
            + uleb128(3)
            + uleb128(4)  # 0x5110, 0x5118, 0x5120
            + bytes([REBASE_DONE])
        )
        self.assertEqual(addresses(image(stream)), [0x5100, 0x5110, 0x5118, 0x5120])

    def test_type_and_segment_switch(self):
        stream = (
            bytes([REBASE_TYPE_IMM | macho.REBASE_TYPE_TEXT_ABSOLUTE32])
            + bytes([REBASE_SEGMENT_OFFSET | 1])
            + uleb128(0x800)
            + bytes([REBASE_IMM_TIMES | 1])
            + POINTER
            + SEG
            + uleb128(8)
            + bytes([REBASE_IMM_TIMES | 1, REBASE_DONE])
        )
        self.assertEqual(
            image(stream).rebases,
            (macho.Rebase(TEXT_VMADDR + 0x800, 2), macho.Rebase(DATA_VMADDR + 8, 1)),
        )

    def test_offset_wraps_backwards(self):
        back = uleb128((1 << 64) - 0x10)  # ADD_ADDR of -0x10, as ld64 writes it
        stream = POINTER + SEG + uleb128(0x40) + bytes([REBASE_ADD_ADDR]) + back
        self.assertEqual(addresses(image(stream + bytes([REBASE_IMM_TIMES | 1]))), [0x5030])

    def test_done_stops_and_no_stream(self):
        m = image(POINTER + SEG + uleb128(0) + bytes([REBASE_DONE, REBASE_IMM_TIMES | 1]))
        self.assertEqual(m.rebases, ())
        self.assertEqual(image(b"").rebases, ())
        self.assertEqual(macho.MachO(thin_image([0x1401])).rebases, ())


class MalformedRebaseTest(unittest.TestCase):
    def assert_rejected(self, stream: bytes, message: str):
        with self.assertRaisesRegex(macho.MachOError, message):
            image(stream).rebases  # noqa: B018 (forces the lazy decode)

    def test_unknown_opcode(self):
        self.assert_rejected(bytes([0x90]), "unknown rebase opcode 0x90")

    def test_rebase_before_segment(self):
        self.assert_rejected(bytes([REBASE_IMM_TIMES | 1]), "before its segment")

    def test_bad_segment(self):
        self.assert_rejected(bytes([REBASE_SEGMENT_OFFSET | 9]) + uleb128(0), "segment 9 of 4")

    def test_outside_segment(self):
        self.assert_rejected(SEG + uleb128(0x2000) + bytes([REBASE_IMM_TIMES | 1]), "outside")

    def test_runaway_repeat(self):
        stream = SEG + uleb128(0) + bytes([REBASE_ULEB_TIMES]) + uleb128(2**40)
        self.assert_rejected(stream, "overruns its segment")
        stream = SEG + uleb128(0) + bytes([REBASE_ULEB_TIMES_SKIPPING]) + uleb128(2**20)
        self.assert_rejected(stream + uleb128(0x100), "overruns its segment")

    def test_truncated_uleb(self):
        self.assert_rejected(SEG + b"\x80", "ULEB128")
        self.assert_rejected(SEG + uleb128(0) + bytes([REBASE_ADD_ADDR_ULEB]), "ULEB128")


class FileMappingTest(unittest.TestCase):
    def test_round_trip_and_unmapped(self):
        m = macho.MachO(ImageBuilder().build())
        for addr in (TEXT_VMADDR, 0x1800, DATA_VMADDR + 0x10):
            self.assertEqual(m.offset_to_vm(m.vm_to_offset(addr)), addr)
        self.assertEqual(m.offset_to_vm(0x4000), DATA_VMADDR)  # __DATA's fileoff
        with self.assertRaisesRegex(macho.MachOError, "not mapped"):
            m.offset_to_vm(len(m.data) + 4)

    def test_segment_protections(self):
        m = macho.MachO(ImageBuilder().build())
        prot = {s.name: (s.maxprot, s.initprot) for s in m.segments}
        self.assertEqual(prot["__TEXT"], (5, 5))
        self.assertEqual(prot["__DATA"], (3, 3))
        self.assertTrue(m.segment("__DATA").contains(DATA_VMADDR + 0x1FFF))
        self.assertFalse(m.segment("__DATA").contains(DATA_VMADDR + 0x2000))


if __name__ == "__main__":
    unittest.main()
