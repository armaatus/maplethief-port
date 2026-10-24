"""Unit tests for the portable fat Mach-O splitter (synthetic fixtures only)."""

import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from machofixtures import (
    ARMV6_THIN,
    ARMV7_THIN,
    CPU_SUBTYPE_ARM_V6,
    CPU_SUBTYPE_ARM_V7,
    CPU_TYPE_ARM,
    SAMPLE_FAT,
    fat_macho,
    thin_macho,
)
from mtport import fat

TOOLS = Path(__file__).resolve().parents[1] / "tools"


def patch_entry(data: bytes, index: int, **fields) -> bytes:
    """Rewrite fields of fat_arch[index] in a 32-bit fat header."""
    names = ("cputype", "cpusubtype", "offset", "size", "align")
    pos = 8 + 20 * index
    values = dict(zip(names, struct.unpack_from(">iiIII", data, pos), strict=True))
    values.update(fields)
    out = bytearray(data)
    struct.pack_into(">iiIII", out, pos, *(values[n] for n in names))
    return bytes(out)


class ParseTest(unittest.TestCase):
    def test_lists_both_slices_in_file_order(self):
        archs = fat.parse_fat(SAMPLE_FAT)
        self.assertEqual([a.name for a in archs], ["armv6", "armv7"])
        self.assertEqual(archs[0].offset, 4096)
        self.assertEqual(archs[0].size, len(ARMV6_THIN))
        self.assertEqual(archs[1].offset % 4096, 0)
        self.assertEqual(archs[1].align, 12)

    def test_split_returns_exact_slice_bytes(self):
        parts = fat.split(SAMPLE_FAT)
        self.assertEqual(parts["armv6"][1], ARMV6_THIN)
        self.assertEqual(parts["armv7"][1], ARMV7_THIN)

    def test_fat_magic_64(self):
        thin = thin_macho(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, b"x" * 64)
        data = struct.pack(">II", 0xCAFEBABF, 1)
        data += struct.pack(">iiQQII", CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, 4096, len(thin), 12, 0)
        data += b"\0" * (4096 - len(data)) + thin
        (arch,) = fat.parse_fat(data)
        self.assertEqual(arch.name, "armv7")
        self.assertEqual(fat.extract(data, arch), thin)

    def test_arch_names(self):
        self.assertEqual(fat.arch_name(12, 6), "armv6")
        self.assertEqual(fat.arch_name(12, 9), "armv7")
        self.assertEqual(fat.arch_name(12 | 0x01000000, 0), "arm64")
        # Capability bits in the high byte of cpusubtype are ignored.
        self.assertEqual(fat.arch_name(12 | 0x01000000, -0x80000000 | 2), "arm64e")
        self.assertEqual(fat.arch_name(99, 1), "cputype99_subtype1")

    def test_find_arch(self):
        archs = fat.parse_fat(SAMPLE_FAT)
        self.assertEqual(fat.find_arch(archs, "armv7").cpusubtype, CPU_SUBTYPE_ARM_V7)
        with self.assertRaisesRegex(fat.FatError, "no arm64 slice"):
            fat.find_arch(archs, "arm64")


class RejectTest(unittest.TestCase):
    def assertRejects(self, data: bytes, message: str):
        with self.assertRaisesRegex(fat.FatError, message):
            fat.parse_fat(data)

    def test_thin_file(self):
        self.assertFalse(fat.is_fat(ARMV7_THIN))
        self.assertRejects(ARMV7_THIN, "not a fat Mach-O")

    def test_too_short(self):
        self.assertRejects(b"\xca\xfe", "too short")

    def test_zero_archs(self):
        self.assertRejects(struct.pack(">II", 0xCAFEBABE, 0), "no architectures")

    def test_implausible_count_like_java_class(self):
        self.assertRejects(struct.pack(">II", 0xCAFEBABE, 0x32) + b"\0" * 2000, "implausible")

    def test_truncated_header(self):
        self.assertRejects(SAMPLE_FAT[:30], "fat header needs")

    def test_slice_past_end(self):
        self.assertRejects(SAMPLE_FAT[:-1], "past end of file")

    def test_misaligned_offset(self):
        self.assertRejects(patch_entry(SAMPLE_FAT, 0, offset=4100), "not aligned")

    def test_offset_inside_header(self):
        self.assertRejects(patch_entry(SAMPLE_FAT, 0, offset=0, align=0), "overlaps the fat header")

    def test_overlapping_slices(self):
        second = struct.unpack_from(">I", SAMPLE_FAT, 8 + 20 + 8)[0]
        self.assertRejects(patch_entry(SAMPLE_FAT, 0, size=second - 4096 + 1), "overlap")

    def test_empty_slice(self):
        self.assertRejects(patch_entry(SAMPLE_FAT, 0, size=0), "empty slice")

    def test_huge_alignment(self):
        self.assertRejects(patch_entry(SAMPLE_FAT, 0, align=40), "alignment")

    def test_duplicate_arch(self):
        data = fat_macho([(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V7, ARMV7_THIN)] * 2)
        self.assertRejects(data, "duplicate architectures: armv7")

    def test_slice_without_macho_header(self):
        data = fat_macho([(CPU_TYPE_ARM, CPU_SUBTYPE_ARM_V6, b"\0" * 64)])
        self.assertRejects(data, "does not start with a Mach-O header")

    def test_cputype_mismatch(self):
        data = fat_macho([(CPU_TYPE_ARM | 0x01000000, 0, ARMV6_THIN)])
        self.assertRejects(data, "cputype")


class CommandLineTest(unittest.TestCase):
    def run_cli(self, *args):
        env = dict(os.environ, PYTHONPATH=str(TOOLS))
        return subprocess.run(
            [sys.executable, "-m", "mtport.fat", *args], capture_output=True, text=True, env=env
        )

    def test_list_and_thin(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "fat"
            src.write_bytes(SAMPLE_FAT)
            listed = self.run_cli("list", str(src))
            self.assertEqual(listed.returncode, 0, listed.stderr)
            self.assertIn("armv6\toffset=4096", listed.stdout)
            out = Path(tmp) / "MapleThief.armv7"
            thinned = self.run_cli("thin", str(src), "armv7", str(out))
            self.assertEqual(thinned.returncode, 0, thinned.stderr)
            self.assertEqual(out.read_bytes(), ARMV7_THIN)

    def test_error_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "thin"
            src.write_bytes(ARMV7_THIN)
            proc = self.run_cli("list", str(src))
            self.assertEqual(proc.returncode, 1)
            self.assertIn("not a fat Mach-O", proc.stderr)


if __name__ == "__main__":
    unittest.main()
