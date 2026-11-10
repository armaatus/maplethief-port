"""A small baseline JPEG encoder and header reader, for the port's own Nintendo Switch icon.

The Homebrew Menu shows an NRO's icon from a 256 x 256 JPEG (docs/formats/nro.md). The port's
icon is drawn by tools/mtport/appicon.py (ADR 0125); :func:`encode_rgb` turns its pixels into a
baseline JFIF file: 8-bit YCbCr, no subsampling, the quantisation tables of the JPEG standard's
Annex K scaled like the IJG library's quality setting, and Annex K's Huffman tables.

Everything is integer arithmetic (the DCT's cosines are fixed constants), so the same pixels
give the same bytes on every machine and Python. The release guard allowlists the icon by its
SHA-256 (mtport.release.ICON_ALLOWLIST), and tests/test_appicon.py checks that hash.

:func:`info` reads a JPEG's frame header (size, components, baseline or not) without decoding.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

# Annex K.1: the example luminance and chrominance quantisation tables, in natural order.
_LUMA_Q = (
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
)  # fmt: skip
_CHROMA_Q = (
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
) + (99,) * 32  # fmt: skip

# The zig-zag order: ZIGZAG[i] is the natural index of the i-th coefficient.
ZIGZAG = (
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
)  # fmt: skip

# Annex K.3: the typical Huffman tables (counts of codes per length 1..16, then the symbols).
_DC_LUMA = ((0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0), tuple(range(12)))
_DC_CHROMA = ((0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0), tuple(range(12)))
_AC_LUMA = (
    (0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7D),
    bytes.fromhex(
        "01020300041105122131410613516107227114328191a1082342b1c11552d1f0"
        "2433627282090a161718191a25262728292a3435363738393a43444546474849"
        "4a535455565758595a636465666768696a737475767778797a83848586878889"
        "8a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5"
        "c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8"
        "f9fa"
    ),
)
_AC_CHROMA = (
    (0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77),
    bytes.fromhex(
        "000102031104052131061241510761711322328108144291a1b1c109233352f0"
        "156272d10a162434e125f11718191a262728292a35363738393a434445464748"
        "494a535455565758595a636465666768696a737475767778797a828384858687"
        "88898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3"
        "c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8"
        "f9fa"
    ),
)

# cos(k * pi / 16) * 8192 for k = 0..8, rounded, and (1 / sqrt(2)) * 8192: the DCT's constants,
# fixed so that no libm is involved.
_COS = (8192, 8035, 7568, 6811, 5793, 4551, 3135, 1598, 0)
_INV_SQRT2 = 5793
_SCALE_BITS = 13


def _cos_index(m: int) -> int:
    """cos(m * pi / 16) * 8192 for any integer m, from :data:`_COS`."""
    m %= 32
    if m > 16:
        m = 32 - m
    return _COS[m] if m <= 8 else -_COS[16 - m]


# _BASIS[x][u] = c(u) * cos((2x + 1) u pi / 16) * 8192, c(0) = 1/sqrt(2), c(u > 0) = 1.
_BASIS = tuple(
    tuple(
        (_INV_SQRT2 * _cos_index((2 * x + 1) * u) + 4096) >> 13 if u == 0 else
        _cos_index((2 * x + 1) * u)
        for u in range(8)
    )
    for x in range(8)
)  # fmt: skip


def quant_table(base: tuple[int, ...], quality: int) -> tuple[int, ...]:
    """``base`` scaled for ``quality`` (1..100) as the IJG library does, natural order."""
    if not 1 <= quality <= 100:
        raise ValueError(f"JPEG quality {quality} is not 1..100")
    scale = 5000 // quality if quality < 50 else 200 - 2 * quality
    return tuple(min(max((q * scale + 50) // 100, 1), 255) for q in base)


def _huffman_codes(
    table: tuple[tuple[int, ...], bytes | tuple[int, ...]],
) -> dict[int, tuple[int, int]]:
    """Symbol -> (code, length) of a table given as Annex C's BITS and HUFFVAL."""
    bits, values = table
    if sum(bits) != len(values):
        raise ValueError("Huffman table: the counts and the symbols disagree")
    codes: dict[int, tuple[int, int]] = {}
    code = 0
    k = 0
    for length, count in enumerate(bits, 1):
        for _ in range(count):
            codes[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return codes


class _BitWriter:
    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def write(self, value: int, length: int) -> None:
        self.acc = (self.acc << length) | (value & ((1 << length) - 1))
        self.n += length
        while self.n >= 8:
            self.n -= 8
            byte = (self.acc >> self.n) & 0xFF
            self.out.append(byte)
            if byte == 0xFF:
                self.out.append(0)  # byte stuffing
        self.acc &= (1 << self.n) - 1

    def flush(self) -> bytes:
        if self.n:
            self.write((1 << (8 - self.n)) - 1, 8 - self.n)  # pad with 1 bits
        return bytes(self.out)


def _magnitude(v: int) -> tuple[int, int]:
    """The (size category, extra bits) of a coefficient (Annex F.1.2.1)."""
    a = -v if v < 0 else v
    size = a.bit_length()
    return size, (v if v >= 0 else v + (1 << size) - 1)


def _fdct_quantize(block: list[int], qt: tuple[int, ...]) -> list[int]:
    """The level-shifted 8 x 8 ``block`` (natural order) -> quantised coefficients, zig-zag.

    F(v, u) = 1/4 c(u) c(v) sum f(y, x) cos((2x+1)u pi/16) cos((2y+1)v pi/16), then rounded
    to the nearest multiple of the quantiser, halves away from zero, all in exact integers."""
    rows = [
        [sum(block[y * 8 + x] * _BASIS[x][u] for x in range(8)) for u in range(8)] for y in range(8)
    ]
    coef = [0] * 64
    for v in range(8):
        for u in range(8):
            s = sum(rows[y][u] * _BASIS[y][v] for y in range(8))
            d = qt[v * 8 + u] << (2 * _SCALE_BITS + 2)
            q = (abs(s) * 2 + d) // (2 * d)
            coef[v * 8 + u] = q if s >= 0 else -q
    return [coef[i] for i in ZIGZAG]


def encode_rgb(width: int, height: int, rgb: bytes, quality: int = 90) -> bytes:
    """A baseline JFIF JPEG of an 8-bit RGB image (rows top to bottom), 4:4:4, ``quality``
    as the IJG library's (1..100)."""
    if len(rgb) != width * height * 3:
        raise ValueError(f"{width}x{height} image needs {width * height * 3} RGB bytes")
    if not (0 < width < 65536 and 0 < height < 65536):
        raise ValueError("JPEG size out of range")
    planes = [bytearray(width * height) for _ in range(3)]
    for i in range(width * height):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        ycc = (
            (19595 * r + 38470 * g + 7471 * b + 32768) >> 16,
            ((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16) + 128,
            ((32768 * r - 27439 * g - 5329 * b + 32768) >> 16) + 128,
        )
        for c in range(3):
            planes[c][i] = min(max(ycc[c], 0), 255)

    qts = (quant_table(_LUMA_Q, quality), quant_table(_CHROMA_Q, quality))
    dc = (_huffman_codes(_DC_LUMA), _huffman_codes(_DC_CHROMA))
    ac = (_huffman_codes(_AC_LUMA), _huffman_codes(_AC_CHROMA))
    bits = _BitWriter()
    pred = [0, 0, 0]
    for by in range(0, height, 8):
        for bx in range(0, width, 8):
            for c in range(3):
                t = 0 if c == 0 else 1
                plane = planes[c]
                block = []
                for y in range(8):
                    row = min(by + y, height - 1) * width  # edge blocks repeat the last pixels
                    for x in range(8):
                        block.append(plane[row + min(bx + x, width - 1)] - 128)
                zz = _fdct_quantize(block, qts[t])
                diff = zz[0] - pred[c]
                pred[c] = zz[0]
                size, extra = _magnitude(diff)
                bits.write(*dc[t][size])
                if size:
                    bits.write(extra, size)
                run = 0
                for k in range(1, 64):
                    v = zz[k]
                    if v == 0:
                        run += 1
                        continue
                    while run > 15:
                        bits.write(*ac[t][0xF0])  # ZRL: sixteen zeros
                        run -= 16
                    size, extra = _magnitude(v)
                    bits.write(*ac[t][(run << 4) | size])
                    bits.write(extra, size)
                    run = 0
                if run:
                    bits.write(*ac[t][0x00])  # EOB
    scan = bits.flush()

    def segment(marker: int, payload: bytes) -> bytes:
        return struct.pack(">BBH", 0xFF, marker, len(payload) + 2) + payload

    out = bytearray(b"\xff\xd8")
    out += segment(0xE0, b"JFIF\0" + struct.pack(">BBBHHBB", 1, 1, 0, 1, 1, 0, 0))
    for tid, qt in enumerate(qts):
        out += segment(0xDB, bytes([tid]) + bytes(qt[i] for i in ZIGZAG))
    out += segment(
        0xC0,
        struct.pack(">BHHB", 8, height, width, 3) + bytes([1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1]),
    )
    for cls_id, table in (
        (0x00, _DC_LUMA),
        (0x10, _AC_LUMA),
        (0x01, _DC_CHROMA),
        (0x11, _AC_CHROMA),
    ):
        out += segment(0xC4, bytes([cls_id]) + bytes(table[0]) + bytes(table[1]))
    out += segment(0xDA, bytes([3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]))
    out += scan
    out += b"\xff\xd9"
    return bytes(out)


@dataclass(frozen=True)
class JpegInfo:
    width: int
    height: int
    components: int
    baseline: bool  # SOF0 (baseline DCT), the kind the Homebrew Menu shows


class JpegError(ValueError):
    pass


def info(data: bytes) -> JpegInfo:
    """The frame header of the JPEG ``data``. Raises :class:`JpegError` if it is not one."""
    if not data.startswith(b"\xff\xd8"):
        raise JpegError("not a JPEG: no SOI marker")
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 0xFF:
            raise JpegError(f"no marker at offset {pos}")
        marker = data[pos + 1]
        if marker == 0xFF:  # fill byte
            pos += 1
            continue
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            pos += 2
            continue
        (length,) = struct.unpack_from(">H", data, pos + 2)
        if length < 2 or pos + 2 + length > len(data):
            raise JpegError(f"segment 0x{marker:02X} at {pos} runs past the end")
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if length < 8:
                raise JpegError("frame header too short")
            _, height, width, comps = struct.unpack_from(">BHHB", data, pos + 4)
            return JpegInfo(width, height, comps, marker == 0xC0)
        if marker in (0xD9, 0xDA):
            break
        pos += 2 + length
    raise JpegError("no frame header (SOF) before the scan")
