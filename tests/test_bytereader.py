"""Unit tests for the bounds-checked byte reader shared by the format parsers."""

import pytest

from mtport.bytereader import ByteReader, FormatError


def test_little_endian_fields_in_order():
    r = ByteReader(bytes.fromhex("01 FF 34 12 FE FF 78 56 34 12 FF FF FF FF"), "f")
    assert r.u8() == 1
    assert r.i8() == -1
    assert r.u16() == 0x1234
    assert r.i16() == -2
    assert r.u32() == 0x12345678
    assert r.i32() == -1
    assert r.at_end
    r.expect_end()


def test_raw_and_pstr():
    r = ByteReader(b"\x03abcXY", "f")
    assert r.pstr() == b"abc"
    assert r.remaining == 2
    assert r.raw(2) == b"XY"
    assert len(r) == 6


def test_short_read_names_file_offset_and_field():
    r = ByteReader(b"\x01\x02\x03", "00.map")
    r.u16()
    with pytest.raises(FormatError, match=r"00\.map: offset 0x2: width needs 2 bytes, 1 left"):
        r.u16("width")
    assert r.pos == 2  # a failed read does not move the cursor


def test_pstr_past_end():
    with pytest.raises(FormatError, match="runs past the end"):
        ByteReader(b"\x05ab").pstr()


def test_negative_size_and_bad_start():
    with pytest.raises(FormatError):
        ByteReader(b"abc").raw(-1)
    with pytest.raises(FormatError):
        ByteReader(b"abc", pos=4)


def test_expect_end_reports_trailing_bytes():
    r = ByteReader(b"\x00\x01\x02", "f")
    r.u8()
    with pytest.raises(FormatError, match="2 trailing bytes after the header"):
        r.expect_end("header")
