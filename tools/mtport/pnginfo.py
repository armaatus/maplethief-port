"""Minimal PNG header reader: image size and pixel format, without decoding any pixels.

The format specs need a PNG's size in several places (a tile sheet's size gives its tile
count, see docs/formats/mapprj.md). This module reads only the chunk framing and ``IHDR``,
with the standard library. Apple's "CgBI" variant (an extra ``CgBI`` chunk before ``IHDR``,
used by the iOS-crushed PNGs in the bundle root, #24) is recognised and flagged, not decoded.

Layout (big-endian)::

    89 50 4E 47 0D 0A 1A 0A                         signature
    { u32 length; char type[4]; u8 data[length]; u32 crc; }  chunks, IHDR first
                                                    (CgBI files: CgBI first, then IHDR)
    IHDR = { u32 width; u32 height; u8 bit_depth; u8 color_type;
             u8 compression; u8 filter; u8 interlace; }

The input is untrusted: every length is bounds-checked, and the IHDR CRC is verified.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import FormatError

SIGNATURE = b"\x89PNG\r\n\x1a\n"
IHDR_SIZE = 13
COLOR_TYPES = {0: "gray", 2: "rgb", 3: "palette", 4: "gray+alpha", 6: "rgba"}


@dataclass(frozen=True)
class PngInfo:
    width: int
    height: int
    bit_depth: int
    color_type: int
    interlace: int
    cgbi: bool  # Apple's crushed variant: needs un-crushing before a normal decoder reads it

    @property
    def color(self) -> str:
        return COLOR_TYPES.get(self.color_type, f"type {self.color_type}")


def _chunk(data: bytes, pos: int, name: str) -> tuple[bytes, bytes, int]:
    """The chunk at ``pos``: (type, payload, offset of the next chunk)."""
    if pos + 8 > len(data):
        raise FormatError(f"{name}: offset 0x{pos:X}: chunk header runs past the end")
    length, ctype = struct.unpack_from(">I4s", data, pos)
    end = pos + 8 + length + 4
    if end > len(data):
        raise FormatError(
            f"{name}: offset 0x{pos:X}: chunk {ctype!r} of {length} bytes runs past the end"
        )
    payload = data[pos + 8 : pos + 8 + length]
    (crc,) = struct.unpack_from(">I", data, pos + 8 + length)
    if zlib.crc32(ctype + payload) != crc:
        raise FormatError(f"{name}: offset 0x{pos:X}: bad CRC on chunk {ctype!r}")
    return ctype, payload, end


def chunk_at(data: bytes, pos: int, name: str = "<png>") -> tuple[bytes, bytes, int]:
    """The CRC-checked chunk at ``pos``: (type, payload, offset of the next chunk)."""
    return _chunk(data, pos, name)


def png_end(data: bytes, pos: int = 0, name: str = "<png>") -> int:
    """Offset just past the ``IEND`` chunk of the PNG that starts at ``pos``.

    Every chunk on the way is bounds- and CRC-checked. Used to split files that hold several
    PNGs back to back (``png/**/*.dat``, docs/formats/png-dat.md).
    """
    if data[pos : pos + len(SIGNATURE)] != SIGNATURE:
        raise FormatError(f"{name}: offset 0x{pos:X}: no PNG signature")
    p = pos + len(SIGNATURE)
    first = True
    while True:
        ctype, _payload, p = _chunk(data, p, name)
        if first and ctype not in (b"IHDR", b"CgBI"):
            raise FormatError(f"{name}: offset 0x{pos:X}: first chunk is {ctype!r}, not IHDR")
        first = False
        if ctype == b"IEND":
            return p


def read_info(data: bytes, name: str = "<png>") -> PngInfo:
    """Size and format from a PNG's ``IHDR``. Raises :class:`FormatError` if it is not a PNG."""
    data = bytes(data)
    if not data.startswith(SIGNATURE):
        raise FormatError(f"{name}: not a PNG (no signature)")
    ctype, payload, pos = _chunk(data, len(SIGNATURE), name)
    cgbi = ctype == b"CgBI"
    if cgbi:
        ctype, payload, pos = _chunk(data, pos, name)
    if ctype != b"IHDR" or len(payload) != IHDR_SIZE:
        raise FormatError(f"{name}: first chunk is {ctype!r} ({len(payload)} bytes), not IHDR")
    width, height, depth, color, _compression, _filter, interlace = struct.unpack(
        ">IIBBBBB", payload
    )
    if width == 0 or height == 0:
        raise FormatError(f"{name}: empty image {width}x{height}")
    return PngInfo(width, height, depth, color, interlace, cgbi)


def load_info(path: str | Path) -> PngInfo:
    path = Path(path)
    return read_info(path.read_bytes(), path.name)
