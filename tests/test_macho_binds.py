"""Unit tests for the dyld bind-opcode decoder in mtport.macho (synthetic streams only)."""

import unittest

from machofixtures import (
    BIND_ADD_ADDR,
    BIND_ADDEND_SLEB,
    BIND_DO,
    BIND_DO_ADD_ADDR,
    BIND_DO_IMM_SCALED,
    BIND_DO_TIMES_SKIPPING,
    BIND_DONE,
    BIND_ORDINAL_IMM,
    BIND_ORDINAL_ULEB,
    BIND_SEGMENT_OFFSET,
    BIND_SPECIAL_IMM,
    BIND_SYMBOL,
    BIND_TYPE_IMM,
    DATA_SEGMENT_INDEX,
    DATA_VMADDR,
    ImageBuilder,
    bind_stream,
    sleb128,
    thin_image,
    uleb128,
)
from mtport import macho

SEG = bytes([BIND_SEGMENT_OFFSET | DATA_SEGMENT_INDEX])


def symbol(name: str, flags: int = 0) -> bytes:
    return bytes([BIND_SYMBOL | flags]) + name.encode() + b"\0"


def image(bind: bytes = b"", weak: bytes = b"", lazy: bytes = b"") -> macho.MachO:
    return macho.MachO(ImageBuilder().build(bind=bind, weak=weak, lazy=lazy))


class Sleb128Test(unittest.TestCase):
    def test_round_trip(self):
        for value in (0, 1, -1, 63, -64, 64, -65, 0x7FFFFFFF, -(2**31)):
            encoded = sleb128(value)
            self.assertEqual(macho.read_sleb128(encoded, 0, len(encoded)), (value, len(encoded)))

    def test_truncated(self):
        with self.assertRaisesRegex(macho.MachOError, "SLEB128"):
            macho.read_sleb128(b"\x80", 0, 1)


class BindTest(unittest.TestCase):
    def test_plain_stream_and_bound_symbol(self):
        stream = bind_stream([(0x5000, "_OBJC_CLASS_$_NSObject"), (0x5010, "_free")])
        m = image(stream)
        self.assertEqual(
            m.binds,
            (
                macho.Bind("bind", 0x5000, "_OBJC_CLASS_$_NSObject", 1, 1, 0, 0),
                macho.Bind("bind", 0x5010, "_free", 1, 1, 0, 0),
            ),
        )
        self.assertEqual(m.bound_symbol(0x5010), "_free")
        self.assertIsNone(m.bound_symbol(0x5004))
        self.assertEqual(m.dyld_info()["bind"], (0x7000, len(stream)))

    def test_every_opcode(self):
        stream = (
            bytes([BIND_ORDINAL_ULEB])
            + uleb128(300)
            + symbol("_a", flags=macho.BIND_SYMBOL_FLAGS_WEAK_IMPORT)
            + bytes([BIND_TYPE_IMM | 1, BIND_ADDEND_SLEB])
            + sleb128(-4)
            + SEG
            + uleb128(0x10)
            + bytes([BIND_DO])  # 0x5010, then +4
            + bytes([BIND_DO_ADD_ADDR])
            + uleb128(8)  # 0x5014, then +4+8
            + bytes([BIND_DO_IMM_SCALED | 2])  # 0x5020, then +4+8
            + bytes([BIND_ADD_ADDR])
            + uleb128(2**64 - 0x1C)  # back by 0x1C (64-bit wrap) to 0x5010
            + bytes([BIND_SPECIAL_IMM | 0xF])  # ordinal -1 (main executable)
            + symbol("_b")
            + bytes([BIND_ADDEND_SLEB])
            + sleb128(0)
            + bytes([BIND_DO_TIMES_SKIPPING])
            + uleb128(3)
            + uleb128(4)  # 0x5010, 0x5018, 0x5020
            + bytes([BIND_SPECIAL_IMM, BIND_ORDINAL_IMM | 2])
            + bytes([BIND_DO])  # 0x5028
            + bytes([BIND_DONE, 0x55, 0x66])  # bytes after DONE are ignored
        )
        got = [(b.address, b.symbol, b.ordinal, b.addend, b.flags) for b in image(stream).binds]
        self.assertEqual(
            got,
            [
                (0x5010, "_a", 300, -4, 1),
                (0x5014, "_a", 300, -4, 1),
                (0x5020, "_a", 300, -4, 1),
                (0x5010, "_b", -1, 0, 0),
                (0x5018, "_b", -1, 0, 0),
                (0x5020, "_b", -1, 0, 0),
                (0x5028, "_b", 2, 0, 0),
            ],
        )

    def test_lazy_stream_continues_past_done(self):
        record = (
            SEG
            + uleb128(0x40)
            + bytes([BIND_ORDINAL_IMM | 1])
            + symbol("_x")
            + bytes([BIND_DO, BIND_DONE])
        )
        record2 = SEG + uleb128(0x44) + symbol("_y") + bytes([BIND_DO, BIND_DONE])
        m = image(lazy=record + record2)
        self.assertEqual(
            [(b.kind, b.address, b.symbol) for b in m.binds],
            [("lazy", 0x5040, "_x"), ("lazy", 0x5044, "_y")],
        )
        self.assertIsNone(m.bound_symbol(0x5040))  # lazy binds are not "bound" pointers

    def test_streams_in_kind_order(self):
        m = image(
            bind=bind_stream([(0x5000, "_a")]),
            weak=symbol("__Znwm")
            + bytes([BIND_TYPE_IMM | 1])
            + SEG
            + uleb128(4)
            + bytes([BIND_DO]),
            lazy=bind_stream([(0x5008, "_c")]),
        )
        self.assertEqual(
            [(b.kind, b.symbol, b.ordinal) for b in m.binds],
            [("bind", "_a", 1), ("weak", "__Znwm", 0), ("lazy", "_c", 1)],
        )
        self.assertEqual(m.bound_symbol(0x5004), "__Znwm")

    def test_no_dyld_info(self):
        m = macho.MachO(thin_image([0x1401]))
        self.assertIsNone(m.dyld_info())
        self.assertEqual(m.binds, ())
        self.assertIsNone(m.bound_symbol(0x3000))


class MalformedBindTest(unittest.TestCase):
    def assert_rejected(self, stream: bytes, message: str):
        with self.assertRaisesRegex(macho.MachOError, message):
            image(stream).binds  # noqa: B018 (forces the lazy decode)

    def test_unknown_opcode(self):
        self.assert_rejected(bytes([0xD0]), "unknown bind opcode 0xD0")

    def test_bind_before_state(self):
        self.assert_rejected(bytes([BIND_DO]), "before its segment or symbol")
        self.assert_rejected(SEG + uleb128(0) + bytes([BIND_DO]), "before its segment or symbol")

    def test_bad_segment(self):
        self.assert_rejected(bytes([BIND_SEGMENT_OFFSET | 9]) + uleb128(0), "segment 9 of 4")

    def test_address_outside_segment(self):
        self.assert_rejected(symbol("_a") + SEG + uleb128(0x2000) + bytes([BIND_DO]), "outside")

    def test_runaway_repeat(self):
        stream = symbol("_a") + SEG + uleb128(0) + bytes([BIND_DO_TIMES_SKIPPING])
        self.assert_rejected(stream + uleb128(2**40) + uleb128(0), "overruns its segment")

    def test_unterminated_symbol_and_uleb(self):
        self.assert_rejected(bytes([BIND_SYMBOL]) + b"_abc", "symbol name runs past")
        self.assert_rejected(SEG + b"\x80", "ULEB128")

    def test_dyld_info_past_end_of_file(self):
        data = bytearray(ImageBuilder().build(bind=bind_stream([(DATA_VMADDR, "_a")])))
        m = macho.MachO(bytes(data))
        cmd = m.commands_of(macho.LC_DYLD_INFO_ONLY)[0]
        data[cmd.offset + 20 : cmd.offset + 24] = (0x7FFFFFFF).to_bytes(4, "little")
        with self.assertRaisesRegex(macho.MachOError, "bind data runs past end of file"):
            macho.MachO(bytes(data)).binds  # noqa: B018
