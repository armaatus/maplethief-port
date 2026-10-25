"""Standard-library PNG codec: decode any non-interlaced PNG to RGBA8, encode RGBA8.

The one image codec of the Python tools (ADR 0010): the CgBI un-crusher (``mtport.cgbi``),
the map and sprite renderers and the asset pipeline all build on it. Only ``zlib``,
``struct`` and ``re`` are used (ADR 0003).

Public API::

    img = png.decode(data, name)        # -> Image(width, height, rgba); raises PngError
    img = png.load(path)                # same, from a file
    data = png.encode_rgba(w, h, rgba)  # -> standard 8-bit RGBA PNG bytes, deterministic
    png.save(path, img)                 # encode_rgba() to a file

``Image.rgba`` is ``width * height * 4`` bytes, rows top to bottom, pixels left to right,
channels R, G, B, A, **straight** (not premultiplied) alpha.

Decoding covers every colour type and bit depth of ISO/IEC 15948: grey (1/2/4/8/16 bits),
RGB (8/16), palette (1/2/4/8) with or without ``tRNS``, grey+alpha (8/16) and RGBA (8/16), all
five scanline filters, and IDAT split over any number of chunks. Low bit depths are scaled up
to 8 bits (``v * 255 / (2**depth - 1)``); 16-bit samples keep their high byte, as libpng's
"strip 16" does. Ancillary chunks (gamma, colour profiles, text) are ignored, so pixels are
the stored values. Not supported, with a clear :class:`PngError`: interlaced (Adam7) files
and Apple CgBI files (decode those with ``mtport.cgbi``). The shipped game uses only palette
(4/8-bit, mostly with ``tRNS``), RGB 8 and RGBA 8, none interlaced (docs/ANALYSIS.md).

Encoding writes ``IHDR`` (8-bit RGBA, non-interlaced), one ``IDAT`` and ``IEND``: no
ancillary chunks, no timestamps. Each row gets the filter (None, Sub or Up, lowest type on a
tie) with the smallest sum of absolute byte values, and the stream is compressed at the fixed
``ZLIB_LEVEL``, so the same pixels always give the same bytes with the same zlib.

Lower-level helpers shared with ``mtport.cgbi``: :func:`read` (chunk walk + header checks,
returns a :class:`PngFile`), :func:`inflate`, :func:`unfilter` and :func:`to_rgba`, plus
:func:`make_chunk`. The input is untrusted: every chunk is bounds- and CRC-checked (via
``mtport.pnginfo``), the inflated size must match the header exactly and is capped before
decompression, and palette indices are range-checked.

Speed: filters None, Sub and Up work on whole rows as big integers (SIMD-within-a-register
byte additions), so they cost a few C-level operations per row; Average and Paeth run one
Python step per byte. Colour conversion uses ``bytes.translate`` and extended slices.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from functools import cache
from pathlib import Path

from mtport import pnginfo
from mtport.bytereader import FormatError

SIGNATURE = pnginfo.SIGNATURE
ZLIB_LEVEL = 6  # fixed so that encode_rgba() is deterministic
MAX_PIXELS = 1 << 26  # 64 Mpx (256 MB of RGBA): a sanity cap for untrusted headers

GRAY, RGB, PALETTE, GRAY_ALPHA, RGBA = 0, 2, 3, 4, 6
CHANNELS = {GRAY: 1, RGB: 3, PALETTE: 1, GRAY_ALPHA: 2, RGBA: 4}
BIT_DEPTHS = {
    GRAY: (1, 2, 4, 8, 16),
    RGB: (8, 16),
    PALETTE: (1, 2, 4, 8),
    GRAY_ALPHA: (8, 16),
    RGBA: (8, 16),
}
FILTER_NONE, FILTER_SUB, FILTER_UP, FILTER_AVERAGE, FILTER_PAETH = range(5)


class PngError(FormatError):
    """Malformed or unsupported PNG data."""


@dataclass(frozen=True)
class Image:
    """A decoded image: 8-bit RGBA, straight alpha, rows top to bottom."""

    width: int
    height: int
    rgba: bytes

    def __post_init__(self) -> None:
        if len(self.rgba) != self.width * self.height * 4:
            raise ValueError(
                f"{self.width}x{self.height} image needs {self.width * self.height * 4} "
                f"RGBA bytes, got {len(self.rgba)}"
            )

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        o = (y * self.width + x) * 4
        r, g, b, a = self.rgba[o : o + 4]
        return r, g, b, a


@dataclass(frozen=True)
class PngFile:
    """The parts of a PNG that :func:`decode` needs, after the chunk walk."""

    width: int
    height: int
    bit_depth: int
    color_type: int
    cgbi: bool  # an Apple CgBI chunk came before IHDR (see mtport.cgbi)
    palette: bytes | None  # PLTE payload (RGB triples)
    trns: bytes | None  # tRNS payload
    idat: bytes  # all IDAT payloads, concatenated
    chunks: tuple[bytes, ...]  # chunk types in file order

    @property
    def channels(self) -> int:
        return CHANNELS[self.color_type]

    @property
    def stride(self) -> int:
        """Bytes per unfiltered scanline (without the filter-type byte)."""
        return row_bytes(self.width, self.color_type, self.bit_depth)

    @property
    def bpp(self) -> int:
        """Filter distance: bytes per complete pixel, at least 1."""
        return max(1, self.channels * self.bit_depth // 8)


def row_bytes(width: int, color_type: int, bit_depth: int) -> int:
    return (width * CHANNELS[color_type] * bit_depth + 7) // 8


def make_chunk(ctype: bytes, payload: bytes) -> bytes:
    """One chunk: length, type, payload and CRC."""
    return (
        struct.pack(">I", len(payload))
        + ctype
        + payload
        + struct.pack(">I", zlib.crc32(ctype + payload))
    )


# --- Reading ---------------------------------------------------------------------------------


def read(data: bytes, name: str = "<png>") -> PngFile:
    """Walk the chunks of a PNG (standard or CgBI) and check the header. Pixels stay packed."""
    data = bytes(data)
    if not data.startswith(SIGNATURE):
        raise PngError(f"{name}: not a PNG (no signature)")
    pos = len(SIGNATURE)
    types: list[bytes] = []
    header = palette = trns = None
    idat: list[bytes] = []
    while True:
        try:
            ctype, payload, pos = pnginfo.chunk_at(data, pos, name)
        except FormatError as e:
            raise PngError(str(e)) from e
        types.append(ctype)
        if ctype == b"IHDR":
            if header is not None or len(payload) != pnginfo.IHDR_SIZE:
                raise PngError(f"{name}: bad or repeated IHDR")
            header = struct.unpack(">IIBBBBB", payload)
        elif header is None and ctype != b"CgBI":
            raise PngError(f"{name}: first chunk is {ctype!r}, not IHDR")
        elif ctype == b"PLTE":
            palette = payload
        elif ctype == b"tRNS":
            trns = payload
        elif ctype == b"IDAT":
            idat.append(payload)
        elif ctype == b"IEND":
            break
    if header is None:
        raise PngError(f"{name}: no IHDR")
    width, height, depth, color, compression, filter_method, interlace = header
    cgbi = types[0] == b"CgBI"
    if width == 0 or height == 0 or width * height > MAX_PIXELS:
        raise PngError(f"{name}: unsupported image size {width}x{height}")
    if depth not in BIT_DEPTHS.get(color, ()):
        raise PngError(f"{name}: invalid colour type {color} / bit depth {depth}")
    if compression != 0 or filter_method != 0:
        raise PngError(f"{name}: unknown compression {compression} / filter method {filter_method}")
    if interlace != 0:
        raise PngError(f"{name}: interlaced (Adam7) PNGs are not supported")
    if color == PALETTE:
        # More entries than the bit depth can index is tolerated (as libpng does).
        if palette is None or len(palette) % 3 or not 0 < len(palette) <= 768:
            raise PngError(f"{name}: palette image without a valid PLTE")
        if trns is not None and len(trns) > len(palette) // 3:
            raise PngError(f"{name}: tRNS has more entries than PLTE")
    elif trns is not None and color in (GRAY, RGB) and len(trns) != 2 * CHANNELS[color]:
        raise PngError(f"{name}: tRNS of {len(trns)} bytes for colour type {color}")
    if not idat:
        raise PngError(f"{name}: no IDAT")
    return PngFile(width, height, depth, color, cgbi, palette, trns, b"".join(idat), tuple(types))


def inflate(stream: bytes, size: int, name: str = "<png>", *, raw: bool = False) -> bytes:
    """Decompress exactly ``size`` bytes: a zlib stream, or raw deflate (``raw``, CgBI)."""
    d = zlib.decompressobj(-15 if raw else 15)
    try:
        out = d.decompress(stream, size + 1)
    except zlib.error as e:
        raise PngError(f"{name}: corrupt image data ({e})") from e
    if len(out) > size:
        raise PngError(f"{name}: image data is too long (more than the {size} bytes expected)")
    if len(out) < size:
        raise PngError(f"{name}: image data inflates to {len(out)} bytes, expected {size}")
    if not d.eof:
        try:
            rest = d.decompress(d.unconsumed_tail, 1) + d.flush()
        except zlib.error as e:
            raise PngError(f"{name}: corrupt image data ({e})") from e
        if rest or not d.eof:
            raise PngError(f"{name}: image data stream is truncated or too long")
    return out


# --- Byte-parallel row arithmetic (SWAR on Python ints) --------------------------------------


@cache
def _masks(n: int) -> tuple[int, int, int]:
    """(all ones, high bit of every byte, low 7 bits of every byte) for an ``n``-byte row."""
    ones = (1 << (8 * n)) - 1
    high = int.from_bytes(b"\x80" * n, "little")
    return ones, high, ones ^ high


def _add(a: int, b: int, high: int, low: int) -> int:
    """Byte-wise ``(a + b) mod 256`` of two rows packed as little-endian ints."""
    return ((a & low) + (b & low)) ^ ((a ^ b) & high)


def _sub(a: int, b: int, high: int, low: int) -> int:
    """Byte-wise ``(a - b) mod 256``."""
    return ((a | high) - (b & low)) ^ ((a ^ b ^ high) & high)


def _prefix(x: int, bpp: int, n: int, ones: int, high: int, low: int) -> int:
    """Byte-wise running sum with distance ``bpp`` (undoes filter Sub), in log2(n) steps."""
    k = bpp
    while k < n:
        x = _add(x, (x << (8 * k)) & ones, high, low)
        k <<= 1
    return x


def _int(b: bytes) -> int:
    return int.from_bytes(b, "little")


def unfilter(raw: bytes, height: int, stride: int, bpp: int, name: str = "<png>") -> bytes:
    """Undo the scanline filters: ``height`` rows of 1 filter byte + ``stride`` bytes in,
    ``height * stride`` bytes out."""
    if len(raw) != height * (stride + 1):
        raise PngError(f"{name}: {len(raw)} bytes of image data, expected {height * (stride + 1)}")
    ones, high, low = _masks(stride)
    out = bytearray(height * stride)
    prev = bytes(stride)
    for y in range(height):
        pos = y * (stride + 1)
        ftype = raw[pos]
        line = raw[pos + 1 : pos + 1 + stride]
        if ftype == FILTER_NONE:
            cur = line
        elif ftype == FILTER_SUB:
            cur = _prefix(_int(line), bpp, stride, ones, high, low).to_bytes(stride, "little")
        elif ftype == FILTER_UP:
            cur = _add(_int(line), _int(prev), high, low).to_bytes(stride, "little")
        elif ftype == FILTER_AVERAGE:
            c = bytearray(line)
            for i in range(min(bpp, stride)):
                c[i] = (c[i] + (prev[i] >> 1)) & 0xFF
            for i in range(bpp, stride):
                c[i] = (c[i] + ((c[i - bpp] + prev[i]) >> 1)) & 0xFF
            cur = bytes(c)
        elif ftype == FILTER_PAETH:
            c = bytearray(line)
            for i in range(min(bpp, stride)):  # left and upper-left are 0: Paeth picks up
                c[i] = (c[i] + prev[i]) & 0xFF
            for i in range(bpp, stride):
                a, b, u = c[i - bpp], prev[i], prev[i - bpp]
                pa, pb, pc = abs(b - u), abs(a - u), abs(a + b - 2 * u)
                if pa <= pb and pa <= pc:
                    c[i] = (c[i] + a) & 0xFF
                elif pb <= pc:
                    c[i] = (c[i] + b) & 0xFF
                else:
                    c[i] = (c[i] + u) & 0xFF
            cur = bytes(c)
        else:
            raise PngError(f"{name}: row {y}: unknown filter type {ftype}")
        out[y * stride : (y + 1) * stride] = cur
        prev = cur
    return bytes(out)


# --- Samples to RGBA -------------------------------------------------------------------------


@cache
def _unpack_table(depth: int) -> tuple[bytes, ...]:
    """For each byte value, its ``8 // depth`` samples (MSB first), one byte each."""
    per = 8 // depth
    mask = (1 << depth) - 1
    return tuple(bytes((v >> (8 - depth * (i + 1))) & mask for i in range(per)) for v in range(256))


def _unpack(pixels: bytes, width: int, height: int, stride: int, depth: int) -> bytes:
    """Sub-byte samples to one byte per sample (unscaled), ``width`` per row."""
    table = _unpack_table(depth)
    return b"".join(
        b"".join(map(table.__getitem__, pixels[y * stride : (y + 1) * stride]))[:width]
        for y in range(height)
    )


def _key_alpha(samples: bytes, key: bytes, n: int) -> bytearray:
    """Alpha plane for a tRNS colour key: 0 where a pixel's bytes equal ``key``, else 255."""
    alpha = bytearray(b"\xff" * n)
    step = len(key)
    j = samples.find(key)
    while j != -1:
        if j % step == 0:
            alpha[j // step] = 0
            j = samples.find(key, j + step)
        else:
            j = samples.find(key, j + 1)
    return alpha


def _interleave(n: int, r: bytes, g: bytes, b: bytes, a: bytes) -> bytes:
    out = bytearray(4 * n)
    out[0::4], out[1::4], out[2::4], out[3::4] = r, g, b, a
    return bytes(out)


def to_rgba(f: PngFile, pixels: bytes, name: str = "<png>") -> bytes:
    """Unfiltered scanlines of ``f`` to 8-bit RGBA (straight alpha, as stored)."""
    w, h, depth, color = f.width, f.height, f.bit_depth, f.color_type
    n = w * h
    if depth < 8:
        pixels, depth = _unpack(pixels, w, h, f.stride, depth), 0  # 0: unscaled samples
    opaque = b"\xff" * n

    if color == PALETTE:
        pal = f.palette or b""
        entries = len(pal) // 3
        if pixels and max(pixels) >= entries:
            raise PngError(f"{name}: palette index {max(pixels)} >= {entries} PLTE entries")
        pad = bytes(256 - entries)
        r, g, b = (pal[i::3] + pad for i in range(3))
        a = (f.trns or b"") + b"\xff" * (256 - len(f.trns or b""))
        planes = (pixels.translate(t) for t in (r, g, b, a))
        return _interleave(n, *planes)

    if color in (GRAY, RGB) and depth == 16:
        if f.trns is not None:
            alpha = bytes(_key_alpha(pixels, f.trns, n))
        else:
            alpha = opaque
        pixels = pixels[0::2]  # high bytes
    elif color in (GRAY, RGB):
        if f.trns is not None:
            key = bytes(v & 0xFF for v in struct.unpack(f">{len(f.trns) // 2}H", f.trns))
            alpha = bytes(_key_alpha(pixels, key, n))
        else:
            alpha = opaque
    elif depth == 16:  # grey+alpha, RGBA
        pixels = pixels[0::2]
    if color == GRAY:
        if depth == 0:  # 1/2/4-bit: scale 0..2**d-1 to 0..255
            top = (1 << f.bit_depth) - 1
            scale = bytes(min(255, v * 255 // top) for v in range(256))
            pixels = pixels.translate(scale)
        return _interleave(n, pixels, pixels, pixels, alpha)
    if color == RGB:
        return _interleave(n, pixels[0::3], pixels[1::3], pixels[2::3], alpha)
    if color == GRAY_ALPHA:
        g = pixels[0::2]
        return _interleave(n, g, g, g, pixels[1::2])
    return bytes(pixels)  # RGBA 8 (or 16 reduced to 8)


def decode(data: bytes, name: str = "<png>") -> Image:
    """Decode a standard PNG to RGBA8. Raises :class:`PngError` (a ``FormatError``)."""
    f = read(data, name)
    if f.cgbi:
        raise PngError(f"{name}: Apple CgBI PNG; decode it with mtport.cgbi")
    raw = inflate(f.idat, f.height * (f.stride + 1), name)
    pixels = unfilter(raw, f.height, f.stride, f.bpp, name)
    return Image(f.width, f.height, to_rgba(f, pixels, name))


def load(path: str | Path) -> Image:
    path = Path(path)
    return decode(path.read_bytes(), str(path))


# --- Encoding --------------------------------------------------------------------------------

# |signed byte| for the filter heuristic: 0..128.
_ABS = bytes(min(v, 256 - v) for v in range(256))


def filter_rows(pixels: bytes, height: int, stride: int, bpp: int) -> bytes:
    """Filter each row with None, Sub or Up, whichever has the smallest sum of |bytes|."""
    ones, high, low = _masks(stride)
    out = bytearray()
    prev = 0
    for y in range(height):
        line = pixels[y * stride : (y + 1) * stride]
        x = _int(line)
        candidates = (
            line,
            _sub(x, (x << (8 * bpp)) & ones, high, low).to_bytes(stride, "little"),
            _sub(x, prev, high, low).to_bytes(stride, "little"),
        )
        costs = [sum(c.translate(_ABS)) for c in candidates]
        best = costs.index(min(costs))
        out.append(best)
        out += candidates[best]
        prev = x
    return bytes(out)


def encode_rgba(width: int, height: int, rgba: bytes, *, level: int = ZLIB_LEVEL) -> bytes:
    """A standard 8-bit RGBA PNG: IHDR, one IDAT, IEND. Same input, same bytes."""
    if width <= 0 or height <= 0 or len(rgba) != width * height * 4:
        raise ValueError(f"{width}x{height} RGBA needs {width * height * 4} bytes, got {len(rgba)}")
    ihdr = struct.pack(">IIBBBBB", width, height, 8, RGBA, 0, 0, 0)
    idat = zlib.compress(filter_rows(bytes(rgba), height, width * 4, 4), level)
    return (
        SIGNATURE + make_chunk(b"IHDR", ihdr) + make_chunk(b"IDAT", idat) + make_chunk(b"IEND", b"")
    )


def save(path: str | Path, image: Image) -> None:
    Path(path).write_bytes(encode_rgba(image.width, image.height, image.rgba))
