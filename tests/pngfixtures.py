"""Synthetic PNG and CgBI files built in code (nothing derived from the IPA).

A deliberately naive, per-byte reference encoder and decoder, independent of
``mtport.png``'s row-parallel arithmetic, so the tests check one against the other.
"""

from __future__ import annotations

import random
import struct
import zlib

SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
CGBI_FLAGS = b"\x30\x00\x20\x02"  # the value 105 of the 113 shipped CgBI files carry


def chunk(ctype: bytes, payload: bytes, crc: int | None = None) -> bytes:
    if crc is None:
        crc = zlib.crc32(ctype + payload)
    return struct.pack(">I", len(payload)) + ctype + payload + struct.pack(">I", crc)


def pack_row(samples: list[int], depth: int) -> bytes:
    """Sample values (channels interleaved) to the packed bytes of one scanline."""
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out, acc, bits = bytearray(), 0, 0
    for s in samples:
        acc, bits = (acc << depth) | s, bits + depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def _predict(ftype: int, cur: bytes | bytearray, prev: bytes, i: int, bpp: int) -> int:
    a = cur[i - bpp] if i >= bpp else 0
    b = prev[i]
    c = prev[i - bpp] if i >= bpp else 0
    return [0, a, b, (a + b) // 2, _paeth(a, b, c)][ftype]


def filter_row(ftype: int, line: bytes, prev: bytes, bpp: int) -> bytes:
    if ftype > 4:  # an invalid type, for error tests: bytes stored as they are
        return line
    return bytes((line[i] - _predict(ftype, line, prev, i, bpp)) & 0xFF for i in range(len(line)))


def unfilter_naive(raw: bytes, height: int, stride: int, bpp: int) -> bytes:
    out, prev = bytearray(), bytes(stride)
    for y in range(height):
        ftype = raw[y * (stride + 1)]
        line = raw[y * (stride + 1) + 1 : (y + 1) * (stride + 1)]
        cur = bytearray(stride)
        for i in range(stride):
            cur[i] = (line[i] + _predict(ftype, cur, prev, i, bpp)) & 0xFF
        out += cur
        prev = bytes(cur)
    return bytes(out)


def make_png(
    width: int,
    height: int,
    color: int,
    depth: int,
    rows: list[bytes],
    filters: tuple[int, ...] = (0,),
    *,
    palette: bytes | None = None,
    trns: bytes | None = None,
    idat_parts: int = 1,
    cgbi: bytes | None = None,
    extra: tuple[bytes, ...] = (),
    interlace: int = 0,
) -> bytes:
    """A PNG from packed scanlines; row ``y`` uses ``filters[y % len(filters)]``.

    ``cgbi`` puts a CgBI chunk with that payload first and deflates the stream raw (no zlib
    header or Adler-32), as Apple's crusher does. ``extra`` chunks go after IHDR.
    """
    bpp = max(1, CHANNELS[color] * depth // 8)
    stream, prev = bytearray(), bytes(len(rows[0]))
    for y, line in enumerate(rows):
        ftype = filters[y % len(filters)]
        stream.append(ftype)
        stream += filter_row(ftype, line, prev, bpp)
        prev = line
    if cgbi is not None:
        z = zlib.compressobj(9, zlib.DEFLATED, -15)
        compressed = z.compress(bytes(stream)) + z.flush()
    else:
        compressed = zlib.compress(bytes(stream), 9)
    cut = -(-len(compressed) // idat_parts)
    parts = [compressed[i : i + cut] for i in range(0, len(compressed), cut)] or [b""]
    out = SIGNATURE
    if cgbi is not None:
        out += chunk(b"CgBI", cgbi)
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, interlace))
    out += b"".join(extra)
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if trns is not None:
        out += chunk(b"tRNS", trns)
    out += b"".join(chunk(b"IDAT", p) for p in parts)
    return out + chunk(b"IEND", b"")


def random_rgba(width: int, height: int, seed: int, alphas: tuple[int, ...] | None = None) -> bytes:
    """Random straight RGBA; alpha drawn from ``alphas`` if given. Transparent pixels are
    (0, 0, 0, 0), the only colour a premultiplied image can hold at alpha 0."""
    rng = random.Random(seed)
    out = bytearray()
    for _ in range(width * height):
        a = rng.choice(alphas) if alphas else rng.randrange(256)
        out += bytes([rng.randrange(256) for _ in range(3)] + [a]) if a else bytes(4)
    return bytes(out)


def make_cgbi(width: int, height: int, rgba: bytes, filters: tuple[int, ...] = (1,), **kw) -> bytes:
    """Crush straight RGBA the way Xcode does: premultiply, swap to BGRA, raw deflate."""
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            o = (y * width + x) * 4
            r, g, b, a = rgba[o : o + 4]
            r, g, b = ((v * a + 127) // 255 for v in (r, g, b))
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    return make_png(width, height, 6, 8, rows, filters, cgbi=kw.pop("cgbi", CGBI_FLAGS), **kw)
