"""Unit tests for the PNG header reader (PNG framing built in code, no image files)."""

import struct
import zlib

import pytest

from mtport import pnginfo
from mtport.bytereader import FormatError


def chunk(ctype: bytes, payload: bytes, crc: int | None = None) -> bytes:
    if crc is None:
        crc = zlib.crc32(ctype + payload)
    return struct.pack(">I", len(payload)) + ctype + payload + struct.pack(">I", crc)


def ihdr(w: int, h: int, depth: int = 8, color: int = 3, interlace: int = 0) -> bytes:
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color, 0, 0, interlace))


def png(*chunks: bytes) -> bytes:
    return pnginfo.SIGNATURE + b"".join(chunks) + chunk(b"IEND", b"")


def test_reads_size_and_format():
    info = pnginfo.read_info(png(ihdr(320, 128)))
    assert (info.width, info.height, info.bit_depth, info.color_type) == (320, 128, 8, 3)
    assert info.color == "palette"
    assert not info.cgbi


def test_cgbi_variant_is_flagged():
    info = pnginfo.read_info(png(chunk(b"CgBI", b"\x50\x00\x20\x02"), ihdr(57, 57, 8, 6)))
    assert (info.width, info.height, info.color, info.cgbi) == (57, 57, "rgba", True)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"GIF89a", "not a PNG"),
        (pnginfo.SIGNATURE + b"\x00\x00", "chunk header runs past the end"),
        (pnginfo.SIGNATURE + ihdr(1, 1)[:-6], "runs past the end"),
        (png(chunk(b"IHDR", bytes(13), crc=0)), "bad CRC"),
        (png(chunk(b"tEXt", b"a\0b"), ihdr(1, 1)), "not IHDR"),
        (png(chunk(b"IHDR", bytes(12))), "not IHDR"),
        (png(ihdr(0, 5)), "empty image"),
    ],
)
def test_rejects_malformed_headers(data, message):
    with pytest.raises(FormatError, match=message):
        pnginfo.read_info(data, "x.png")


def test_png_end_finds_each_png_of_a_concatenation():
    a = png(ihdr(2, 2), chunk(b"IDAT", b"x"))
    b = png(ihdr(3, 1))
    data = a + b
    assert pnginfo.png_end(data, 0) == len(a)
    assert pnginfo.png_end(data, len(a)) == len(data)
    ctype, payload, nxt = pnginfo.chunk_at(data, len(pnginfo.SIGNATURE))
    assert (ctype, len(payload), nxt) == (b"IHDR", 13, len(pnginfo.SIGNATURE) + 25)


@pytest.mark.parametrize(
    ("data", "message"),
    [
        (b"junkjunk" + png(ihdr(1, 1)), "no PNG signature"),
        (png(ihdr(1, 1))[:-12], "runs past the end"),  # IEND missing
        (pnginfo.SIGNATURE + chunk(b"IDAT", b"") + chunk(b"IEND", b""), "not IHDR"),
        (png(ihdr(1, 1), chunk(b"IDAT", b"x", crc=1)), "bad CRC"),
    ],
)
def test_png_end_rejects_broken_framing(data, message):
    with pytest.raises(FormatError, match=message):
        pnginfo.png_end(data, 0, "p.dat")
