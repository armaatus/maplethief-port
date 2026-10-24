"""Unit tests for the export-trie reader in mtport.macho (synthetic tries only)."""

import unittest

from machofixtures import TEXT_VMADDR, ImageBuilder, export_trie, uleb128
from mtport import macho


def image(export: bytes) -> macho.MachO:
    return macho.MachO(ImageBuilder().build(export=export))


class ExportTrieTest(unittest.TestCase):
    def test_flat_trie(self):
        m = image(export_trie([("start", 0x1800), ("_bson_init", 0x1811)]))
        self.assertEqual(
            m.exports,
            (
                macho.Export("_bson_init", 0x1811, 0),
                macho.Export("start", 0x1800, 0),
            ),
        )

    def test_shared_prefixes(self):
        # root --"_bson_"--> node1 {--"init"--> leaf A, --"free"--> leaf B}
        leaf_a = uleb128(0) + uleb128(0x811)
        leaf_b = uleb128(0) + uleb128(0x4100)
        root = b"\0\x01_bson_\0"  # child offset patched below
        node1_off = len(root) + 1
        # Every offset fits one ULEB byte, so node1 has the same length with offsets 0.
        a_off = node1_off + len(b"\0\x02init\0\x00free\0\x00")
        a = uleb128(len(leaf_a)) + leaf_a + b"\0"
        b_off = a_off + len(a)
        node1 = b"\0\x02init\0" + uleb128(a_off) + b"free\0" + uleb128(b_off)
        trie = root + uleb128(node1_off) + node1 + a + uleb128(len(leaf_b)) + leaf_b + b"\0"
        m = image(trie)
        self.assertEqual(
            [(e.name, e.address) for e in m.exports],
            [("_bson_free", TEXT_VMADDR + 0x4100), ("_bson_init", TEXT_VMADDR + 0x811)],
        )

    def test_reexport_has_no_address(self):
        info = uleb128(macho.EXPORT_SYMBOL_FLAGS_REEXPORT) + uleb128(1) + b"_other\0"
        leaf = uleb128(len(info)) + info + b"\0"
        root = b"\0\x01_x\0"
        trie = root + uleb128(len(root) + 1) + leaf
        (e,) = image(trie).exports
        self.assertEqual((e.name, e.address, e.flags), ("_x", None, 0x08))

    def test_no_trie(self):
        self.assertEqual(image(b"").exports, ())

    def test_loop_is_an_error(self):
        trie = b"\0\x01a\0\x00"  # the only child points back at the root
        with self.assertRaises(macho.MachOError):
            _ = image(trie).exports

    def test_child_out_of_range(self):
        trie = b"\0\x01a\0\x7f"
        with self.assertRaises(macho.MachOError):
            _ = image(trie).exports

    def test_unterminated_label(self):
        with self.assertRaises(macho.MachOError):
            _ = image(b"\0\x01abc").exports


if __name__ == "__main__":
    unittest.main()
