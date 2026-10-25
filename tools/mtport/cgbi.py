"""Apple CgBI ("crushed") PNGs: decode them and convert them to standard PNGs.

Spec: docs/formats/cgbi.md. Xcode's ``pngcrush -iphone`` step rewrote the 113 shell UI
images of the bundle (112 at the root, 1 in ``English.lproj``) into Apple's CgBI variant,
which only Apple's ImageIO reads. It differs from a standard PNG in four ways::

    89 50 4E 47 0D 0A 1A 0A
    CgBI chunk (4 bytes: 30 00 20 02 or 30 00 20 06)   before IHDR
    IHDR, ancillary chunks                               as in a standard PNG
    IDAT...  raw deflate (RFC 1951): no zlib header, no Adler-32
    IEND
    pixels: B, G, R, A byte order, colour premultiplied by alpha

Decoding drops the ``CgBI`` chunk, inflates the IDAT stream as raw deflate, undoes the
scanline filters (``mtport.png.unfilter``; the filters work on bytes, so the channel order
does not matter to them), swaps B and R, and un-premultiplies:
``c = min(255, (c * 255 + a // 2) // a)``, and ``(0, 0, 0, 0)`` where ``a = 0``. That is
stb_image's rounding, and stb_image matches ``sips`` on every shipped file (ADR 0003).

API::

    is_cgbi(data) -> bool
    decode(data, name, premultiplied=False) -> png.Image   # CgBI or standard PNG
    convert(data, name, premultiplied=False) -> bytes      # standard PNG bytes

``convert`` returns a standard PNG for CgBI input (``mtport.png.encode_rgba``, so the output
is deterministic and has no ancillary chunks) and returns any other PNG **unchanged**. The
``premultiplied`` mode keeps premultiplied RGBA, the layout an OpenGL upload with
``GL_ONE, GL_ONE_MINUS_SRC_ALPHA`` blending wants (#57). In that mode standard PNGs are
premultiplied too (``c = (c * a + 127) // 255``), so every output has the same meaning; a
PNG written in this mode is a GPU buffer in PNG framing, not a normal image.

Command line (``PYTHONPATH=tools python3 -m mtport.cgbi ...``)::

    convert IN... --out DIR [--premultiplied]   files or directories (searched for *.png)
    check [DIR]                                  every *.png under DIR (default: the bundle)

``check`` converts every PNG under DIR in memory, re-decodes each output with
``mtport.png`` (zlib-based), checks that standard PNGs pass through byte-identical, and exits
1 on any failure. Input files are untrusted data: they are parsed, never executed.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from functools import cache
from pathlib import Path

from mtport import bootstrap, png
from mtport.bytereader import FormatError

CHUNK = b"CgBI"


class CgbiError(png.PngError):
    """Malformed CgBI data."""


def is_cgbi(data: bytes) -> bool:
    """True if ``data`` is a PNG whose first chunk is ``CgBI`` (framing only, no CRC check)."""
    return data[:8] == png.SIGNATURE and data[12:16] == CHUNK


# --- Alpha -----------------------------------------------------------------------------------

_NOT_OPAQUE = re.compile(rb"[^\xff]")


@cache
def _unpremultiply_table(a: int) -> bytes:
    return bytes(min(255, (c * 255 + a // 2) // a) for c in range(256))


@cache
def _premultiply_table(a: int) -> bytes:
    return bytes((c * a + 127) // 255 for c in range(256))


def _scale(rgba: bytes, table) -> bytes:
    """Apply ``table(alpha)`` to R, G, B of every pixel whose alpha is not 255."""
    out = bytearray(rgba)
    alpha = rgba[3::4]
    for m in _NOT_OPAQUE.finditer(alpha):
        o = 4 * m.start()
        a = out[o + 3]
        if a == 0:
            out[o : o + 3] = b"\0\0\0"
        else:
            t = table(a)
            out[o], out[o + 1], out[o + 2] = t[out[o]], t[out[o + 1]], t[out[o + 2]]
    return bytes(out)


def unpremultiply(rgba: bytes) -> bytes:
    """Premultiplied RGBA8 to straight alpha (stb_image's rounding, clamped to 255)."""
    return _scale(rgba, _unpremultiply_table)


def premultiply(rgba: bytes) -> bytes:
    """Straight RGBA8 to premultiplied, rounding to nearest."""
    return _scale(rgba, _premultiply_table)


def swap_rb(pixels: bytes, step: int) -> bytes:
    """Swap the first and third byte of every ``step``-byte pixel (BGR(A) <-> RGB(A))."""
    out = bytearray(pixels)
    out[0::step], out[2::step] = pixels[2::step], pixels[0::step]
    return bytes(out)


# --- Decode / convert ------------------------------------------------------------------------


def decode(data: bytes, name: str = "<png>", *, premultiplied: bool = False) -> png.Image:
    """Decode a CgBI or standard PNG to RGBA8, straight alpha unless ``premultiplied``."""
    if not is_cgbi(data):
        img = png.decode(data, name)
        return png.Image(img.width, img.height, premultiply(img.rgba)) if premultiplied else img
    f = png.read(data, name)
    if not f.cgbi:
        raise CgbiError(f"{name}: CgBI chunk not first")
    if f.color_type not in (png.RGB, png.RGBA) or f.bit_depth != 8:
        raise CgbiError(
            f"{name}: CgBI colour type {f.color_type} / depth {f.bit_depth}: only 8-bit "
            f"BGR and BGRA are known"
        )
    try:
        raw = png.inflate(f.idat, f.height * (f.stride + 1), name, raw=True)
        pixels = png.unfilter(raw, f.height, f.stride, f.bpp, name)
    except png.PngError as e:
        raise CgbiError(str(e)) from e
    rgba = png.to_rgba(f, swap_rb(pixels, f.channels), name)
    if f.color_type == png.RGBA and not premultiplied:
        rgba = unpremultiply(rgba)
    return png.Image(f.width, f.height, rgba)


def convert(data: bytes, name: str = "<png>", *, premultiplied: bool = False) -> bytes:
    """Standard PNG bytes for ``data``. Non-CgBI input comes back unchanged, unless
    ``premultiplied`` asks for it to be premultiplied."""
    if not is_cgbi(data) and not premultiplied:
        png.read(data, name)  # still a PNG: fail on anything else
        return data
    img = decode(data, name, premultiplied=premultiplied)
    return png.encode_rgba(img.width, img.height, img.rgba)


# --- Command line ----------------------------------------------------------------------------


def _inputs(paths: list[Path]) -> list[tuple[Path, Path]]:
    """(file, output path relative to --out) for each input; directories give their *.png."""
    out = []
    for p in paths:
        if p.is_dir():
            out += [(f, f.relative_to(p)) for f in sorted(p.rglob("*.png")) if f.is_file()]
        else:
            out.append((p, Path(p.name)))
    return out


def _convert_cmd(paths: list[Path], out_dir: Path, premultiplied: bool) -> int:
    jobs = _inputs(paths)
    seen: dict[Path, Path] = {}
    errors = converted = passed = 0
    for src, rel in jobs:
        if rel in seen:
            print(f"error: {src}: same output name as {seen[rel]}", file=sys.stderr)
            errors += 1
            continue
        seen[rel] = src
        data = src.read_bytes()
        try:
            result = convert(data, str(src), premultiplied=premultiplied)
        except FormatError as e:
            print(f"error: {e}", file=sys.stderr)
            errors += 1
            continue
        dst = out_dir / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(result)
        if result is data:
            passed += 1
        else:
            converted += 1
    print(f"converted={converted} passed-through={passed} errors={errors} -> {out_dir}")
    return 1 if errors else 0


@dataclass
class Report:
    cgbi: int = 0
    standard: int = 0
    failures: list[str] = field(default_factory=list)


def check(root: Path) -> Report:
    """Convert every *.png under ``root`` in memory and verify each result (see module doc)."""
    report = Report()
    for path in sorted(p for p in root.rglob("*.png") if p.is_file()):
        rel = path.relative_to(root).as_posix()
        data = path.read_bytes()
        try:
            out = convert(data, rel)
            img = png.decode(out, rel)
            if is_cgbi(data):
                report.cgbi += 1
                header = png.read(data, rel)
                if (img.width, img.height) != (header.width, header.height):
                    raise CgbiError(f"{rel}: output is {img.width}x{img.height}")
            else:
                report.standard += 1
                if out != data:
                    raise CgbiError(f"{rel}: standard PNG was not passed through unchanged")
        except FormatError as e:
            report.failures.append(str(e))
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.cgbi", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    cv = sub.add_parser("convert", help="write standard PNGs for CgBI inputs, copy the rest")
    cv.add_argument("inputs", nargs="+", type=Path, metavar="IN")
    cv.add_argument("--out", type=Path, required=True)
    cv.add_argument("--premultiplied", action="store_true", help="keep premultiplied RGBA")
    ck = sub.add_parser("check", help="convert and verify every *.png under DIR")
    ck.add_argument("root", nargs="?", type=Path, default=bootstrap.APP_DIR, metavar="DIR")
    args = parser.parse_args(argv)
    if args.command == "convert":
        return _convert_cmd(args.inputs, args.out, args.premultiplied)
    report = check(args.root)
    for line in report.failures:
        print(f"failed: {line}", file=sys.stderr)
    print(f"cgbi={report.cgbi} standard={report.standard} failures={len(report.failures)}")
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
