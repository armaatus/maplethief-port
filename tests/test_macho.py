"""Unit tests for the thin Mach-O reader (synthetic fixtures only)."""

import struct
import unittest

from machofixtures import ARMV7_THIN, thin_image, uleb128
from mtport import macho

STARTS = [0x1401, 0x1421, 0x1480, 0x14F1]
IMAGE = thin_image(STARTS)


class ParseTest(unittest.TestCase):
    def setUp(self):
        self.image = macho.MachO(IMAGE)

    def test_segments_and_sections(self):
        self.assertEqual(
            [s.name for s in self.image.segments], ["__PAGEZERO", "__TEXT", "__DATA", "__LINKEDIT"]
        )
        text = self.image.section("__TEXT", "__text")
        self.assertEqual((text.addr, text.offset), (0x1400, 0x400))
        self.assertIsNone(self.image.section("__TEXT", "__cstring"))
        self.assertIs(self.image.section_at(0x1440), text)
        self.assertIsNone(self.image.section_at(0x2000))

    def test_function_starts_keep_the_thumb_bit(self):
        self.assertEqual(self.image.function_starts, tuple(STARTS))

    def test_entry_point_and_dylibs(self):
        self.assertEqual(self.image.entry_point, 0x1400)
        self.assertEqual(self.image.dylibs, (("/usr/lib/libSystem.B.dylib", False),))

    def test_reads_by_vm_address(self):
        self.assertEqual(self.image.read(0x1400, 2), b"\x70\x47")
        self.assertEqual(self.image.cstring(0x3006), b"world")
        self.assertEqual(self.image.u32(0x300C), 0xCAFEF00D)
        self.assertEqual(self.image.u16(0x300C), 0xF00D)
        self.assertEqual(self.image.vm_to_offset(0x3000), 0x1000)

    def test_unbacked_addresses_raise(self):
        for addr in (0x0, 0x3100, 0x9000):  # __PAGEZERO, __DATA zero-fill, unmapped
            with self.assertRaises(macho.MachOError):
                self.image.read(addr, 1)
        with self.assertRaises(macho.MachOError):
            self.image.read(0x30FE, 4)  # runs off the file-backed part of __DATA

    def test_no_function_starts_command(self):
        self.assertEqual(macho.MachO(ARMV7_THIN).function_starts, ())
        self.assertIsNone(macho.MachO(ARMV7_THIN).entry_point)


class MalformedTest(unittest.TestCase):
    def assert_rejected(self, data: bytes, message: str):
        with self.assertRaisesRegex(macho.MachOError, message):
            macho.MachO(data).function_starts  # noqa: B018 (forces the lazy decode)

    def test_wrong_magic(self):
        self.assert_rejected(b"\xca\xfe\xba\xbe" + IMAGE[4:], "not a thin")

    def test_truncated(self):
        self.assert_rejected(IMAGE[:20], "too short")
        self.assert_rejected(IMAGE[:100], "load commands need")

    def test_bad_cmdsize(self):
        data = bytearray(IMAGE)
        struct.pack_into("<I", data, 28 + 4, 6)  # first command's cmdsize
        self.assert_rejected(bytes(data), "bad cmdsize")

    def test_segment_past_end_of_file(self):
        self.assert_rejected(IMAGE[:0x1080], "past end of file")

    def test_unterminated_uleb(self):
        span = macho.MachO(IMAGE).linkedit_data(macho.LC_FUNCTION_STARTS)
        data = bytearray(IMAGE)
        data[span[0] : span[0] + span[1]] = b"\x80" * span[1]
        self.assert_rejected(bytes(data), "ULEB128")


class Uleb128Test(unittest.TestCase):
    def test_round_trip(self):
        for value in (0, 1, 127, 128, 0x27E0, 2**32 - 1):
            encoded = uleb128(value)
            self.assertEqual(macho.read_uleb128(encoded, 0, len(encoded)), (value, len(encoded)))


class CommandLineTest(unittest.TestCase):
    def test_info_and_starts(self):
        import contextlib
        import io
        import tempfile
        from pathlib import Path

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "image"
            path.write_bytes(IMAGE)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(macho.main(["info", str(path)]), 0)
                self.assertEqual(macho.main(["starts", str(path)]), 0)
        text = out.getvalue()
        self.assertIn("entry 0x1400", text)
        self.assertIn("functions 4 (3 Thumb, 1 ARM)", text)
        self.assertIn("0x14F1\n", text)
