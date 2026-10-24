"""Unit tests for the bundle format table (synthetic inputs only)."""

import struct
import tempfile
import unittest
import zlib
from pathlib import Path

from mtport import datatables, fat, formats, macho, savedata
from mtport.bytereader import FormatError
from repo_paths import REPO


def png(cgbi: bool = False) -> bytes:
    def chunk(kind: bytes, body: bytes) -> bytes:
        crc = zlib.crc32(kind + body)
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    head = chunk(b"CgBI", b"\x50\x00\x20\x02") if cgbi else b""
    return (
        b"\x89PNG\r\n\x1a\n" + head + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 4, 8, 6, 0, 0, 0))
    )


class ClassifyTest(unittest.TestCase):
    def name(self, rel: str, data: bytes = b"") -> str | None:
        fmt = formats.classify(rel, data)
        return None if fmt is None else fmt.name

    def test_reverse_engineered_formats_by_path(self):
        self.assertEqual(self.name("map/00.map"), "map")
        self.assertEqual(self.name("map/106.att"), "att")
        self.assertEqual(self.name("map/00.obj"), "obj")
        self.assertEqual(self.name("map/0.mapprj"), "mapprj")
        self.assertEqual(self.name("sprite/mob/3.sprprj"), "sprprj")
        self.assertEqual(self.name("script/common/x.out"), "script-out")
        self.assertEqual(self.name("png/static/num.dat"), "png-dat")
        self.assertEqual(self.name("Korean/data/mob.dat"), "data-table")
        self.assertEqual(self.name("MainWindow.nib"), "nib")

    def test_pngs_by_content(self):
        self.assertEqual(self.name("png/ui/a.png", png()), "png")
        self.assertEqual(self.name("Default.png", png()), "png")
        self.assertEqual(self.name("Icon.png", png(cgbi=True)), "cgbi-png")

    def test_standard_and_unknown(self):
        self.assertEqual(self.name("en.lproj/Scripts.strings"), "plist")
        self.assertEqual(self.name("_CodeSignature/CodeResources"), "plist")
        self.assertEqual(self.name("sound/a.mp3"), "mp3")
        self.assertEqual(self.name("MapleThief"), "macho")
        self.assertEqual(self.name("MapleThief_armv6"), "empty")
        self.assertIsNone(self.name("some/new.bin"))
        self.assertIsNone(self.name("other/x.out"))

    def test_framing_checks_reject_wrong_content(self):
        with self.assertRaises(FormatError):
            formats.FORMATS["wav"].decode(b"RIFX0000WAVE", "a.wav")
        with self.assertRaises(FormatError):
            formats.FORMATS["empty"].decode(b"x", "MapleThief_armv6")
        with self.assertRaises(FormatError):
            formats.FORMATS["png"].decode(png(cgbi=True), "a.png")
        self.assertEqual(formats.FORMATS["mp3"].decode(b"ID3\x04", "a.mp3"), b"ID3\x04")

    def test_every_spec_exists_and_is_indexed(self):
        index = (REPO / "docs/formats/README.md").read_text(encoding="utf-8")
        for fmt in formats.FORMATS.values():
            if fmt.kind == formats.SPEC:
                self.assertTrue((REPO / "docs/formats" / fmt.doc).is_file(), fmt.doc)
                self.assertIn(f"]({fmt.doc}", index, fmt.doc)

    def test_parser_errors_are_format_errors(self):
        """check() catches FormatError only, so every parser's error class must be one."""
        for cls in (datatables.DataTableError, savedata.SaveDataError, fat.FatError):
            self.assertTrue(issubclass(cls, FormatError), cls)
        self.assertTrue(issubclass(macho.MachOError, FormatError))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "English" / "data").mkdir(parents=True)
            (root / "English" / "data" / "item.dat").write_bytes(b"\x01")
            (root / "MapleThief").write_bytes(b"\xca\xfe\xba\xbe\x00\x00\x00\x09")
            report = formats.check(root)
        self.assertEqual(
            report.failures,
            [
                "English/data/item.dat: unknown table 'item'",
                "MapleThief: fat header needs 188 bytes, file has 8",
            ],
        )
