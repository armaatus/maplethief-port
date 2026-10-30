"""The macOS app icon, made at build time from the IPA's own artwork (#81, ADR 0064).

The game app (``mac-app`` preset) needs an ``.icns``. The icon is Nexon's art, so it is never
committed: the build writes it below ``build/`` from the user's extracted IPA::

    python3 -m mtport.macicon --ipa work/ipa --out build/mac-app/platform/MapleThief.icns

Source, in order of preference:

1. ``iTunesArtwork`` at the IPA's root (next to ``Payload/``): the 1024 x 1024 store artwork
   (a JPEG in version 1.5);
2. otherwise the largest icon that the bundle's ``Info.plist`` names (``CFBundleIcons`` ->
   ``CFBundlePrimaryIcon`` -> ``CFBundleIconFiles``, ``CFBundleIconFiles``,
   ``CFBundleIconFile``), CgBI or standard PNG (``Icon@2x.png``, 114 x 114, in 1.5).

``--source FILE`` names one directly. The geometry follows the macOS app icon grid: the art is
scaled into an 824 x 824 rounded rectangle (corner radius 185.4) centred on a transparent
1024 x 1024 canvas, then halved down to 16 x 16. The non-integer scale of the source is done by
``sips`` (ImageIO: it reads JPEG and CgBI), everything else here: the anti-aliased mask, the
premultiplied 2 x 2 box filter and the ``icns`` container. That container is a big-endian
``'icns'`` header and length, then one ``<type><length><PNG>`` entry per size of a full
``.iconset`` (``iconutil -c iconset`` reads them back)::

    icp4 16   icp5 32   ic07 128   ic08 256   ic09 512   ic10 1024 (512@2x)
    ic11 32 (16@2x)   ic12 64 (32@2x)   ic13 256 (128@2x)   ic14 512 (256@2x)

(``icp6`` is left out: ``iconutil`` reads it as 48 x 48, not 64.)

The inputs are untrusted data: they are parsed (``plistlib``, ``mtport.png``, ``sips``), never
executed. Exit status 1 with an ``error:`` line on any failure.
"""

from __future__ import annotations

import argparse
import math
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path

from mtport import bootstrap, cgbi, png

CANVAS = 1024  # the largest icon, in pixels
# The macOS icon grid (Big Sur and later) for a 1024 canvas: 824 content, 100 margin.
CONTENT_FRACTION = 824 / 1024
RADIUS_FRACTION = 185.4 / 824  # corner radius over the content's side
SUPERSAMPLE = 4  # per axis, for pixels the rounded corner's edge crosses

# (type, pixel size): the sizes of a full .iconset.
ICNS_TYPES: tuple[tuple[bytes, int], ...] = (
    (b"icp4", 16),
    (b"icp5", 32),
    (b"ic07", 128),
    (b"ic08", 256),
    (b"ic09", 512),
    (b"ic10", 1024),
    (b"ic11", 32),
    (b"ic12", 64),
    (b"ic13", 256),
    (b"ic14", 512),
)

# resize(source, size, out): write ``source`` scaled to size x size as a standard PNG to ``out``.
Resize = Callable[[Path, int, Path], None]


class IconError(Exception):
    """No usable icon source, or a tool failed."""


# --- Source --------------------------------------------------------------------------------


def _plist_icon_names(info: dict) -> list[str]:
    names: list[str] = []
    primary = info.get("CFBundleIcons", {})
    if isinstance(primary, dict):
        files = primary.get("CFBundlePrimaryIcon", {})
        if isinstance(files, dict):
            names += [n for n in files.get("CFBundleIconFiles", []) if isinstance(n, str)]
    names += [n for n in info.get("CFBundleIconFiles", []) if isinstance(n, str)]
    single = info.get("CFBundleIconFile")
    if isinstance(single, str):
        names.append(single)
    return names


def _png_size(path: Path) -> int:
    """The smaller side of a (CgBI or standard) PNG, 0 if it is not one."""
    try:
        f = png.read(path.read_bytes(), str(path))
    except (OSError, png.PngError):
        return 0
    return min(f.width, f.height)


def _bundle(ipa: Path) -> Path:
    apps = sorted(p for p in (ipa / "Payload").glob("*.app") if p.is_dir())
    if len(apps) != 1:
        raise IconError(f"{ipa}/Payload holds {len(apps)} .app bundles, expected 1")
    return apps[0]


def find_source(ipa: Path) -> Path:
    """The best icon source of the extracted IPA ``ipa`` (the directory holding Payload/)."""
    artwork = ipa / "iTunesArtwork"
    if artwork.is_file():
        return artwork
    app = _bundle(ipa)
    try:
        with (app / "Info.plist").open("rb") as f:
            info = plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError) as e:
        raise IconError(f"{app}/Info.plist: {e}") from e
    best: tuple[int, Path] | None = None
    for name in _plist_icon_names(info if isinstance(info, dict) else {}):
        # Names may leave out ".png"; never leave the bundle.
        for candidate in (name, f"{name}.png"):
            if "/" in candidate or "\\" in candidate or candidate in ("", ".", ".."):
                continue
            path = app / candidate
            size = _png_size(path) if path.is_file() else 0
            if size and (best is None or size > best[0]):
                best = (size, path)
    if best is None:
        raise IconError(f"no iTunesArtwork in {ipa} and no icon named by {app}/Info.plist")
    return best[1]


# --- Geometry --------------------------------------------------------------------------------


def content_size(canvas: int) -> int:
    """The rounded rectangle's side on a ``canvas`` x ``canvas`` icon (even, so it centres)."""
    return 2 * round(canvas * CONTENT_FRACTION / 2)


def _coverage(px: float, py: float, cx: float, cy: float, r: float) -> int:
    """Alpha 0-255 of the unit pixel at (px, py) inside the circle (cx, cy, r)."""
    d = math.hypot(px + 0.5 - cx, py + 0.5 - cy)
    if d <= r - 0.75:
        return 255
    if d >= r + 0.75:
        return 0
    n = SUPERSAMPLE
    inside = 0
    for sy in range(n):
        for sx in range(n):
            x = px + (sx + 0.5) / n - cx
            y = py + (sy + 0.5) / n - cy
            if x * x + y * y <= r * r:
                inside += 1
    return (inside * 255 + n * n // 2) // (n * n)


def compose(art: png.Image, canvas: int) -> png.Image:
    """``art`` (content_size(canvas) square) centred on a transparent canvas, masked to the
    rounded rectangle; straight alpha."""
    side = content_size(canvas)
    if (art.width, art.height) != (side, side):
        raise ValueError(f"art is {art.width}x{art.height}, needs {side}x{side}")
    margin = (canvas - side) // 2
    radius = side * RADIUS_FRACTION
    out = bytearray(canvas * canvas * 4)
    row = side * 4
    for y in range(side):
        o = ((margin + y) * canvas + margin) * 4
        out[o : o + row] = art.rgba[y * row : (y + 1) * row]
    # The corners: the top-left one's coverage, mirrored to the other three, scales alpha.
    centre = margin + radius  # the top-left circle's centre, on both axes
    last = 2 * margin + side - 1  # x -> last - x mirrors a column (and a row)
    for y in range(margin, math.ceil(centre)):
        for x in range(margin, math.ceil(centre)):
            cov = _coverage(x, y, centre, centre, radius)
            if cov == 255:
                continue
            for qx, qy in ((x, y), (last - x, y), (x, last - y), (last - x, last - y)):
                o = (qy * canvas + qx) * 4
                if cov == 0:
                    out[o : o + 4] = b"\0\0\0\0"
                else:
                    out[o + 3] = (out[o + 3] * cov + 127) // 255
    return png.Image(canvas, canvas, bytes(out))


def _pair_sums(row: bytes) -> list[int]:
    """The sums of each pair of horizontally adjacent RGBA pixels, channel by channel: all R
    sums, then G, B and A."""
    even = row[0::8] + row[1::8] + row[2::8] + row[3::8]
    odd = row[4::8] + row[5::8] + row[6::8] + row[7::8]
    return [p + q for p, q in zip(even, odd, strict=True)]


def halve(img: png.Image) -> png.Image:
    """Half the size, each pixel the premultiplied mean of a 2 x 2 block (rounded)."""
    if img.width % 2 or img.height % 2:
        raise ValueError(f"{img.width}x{img.height} does not halve")
    w, h = img.width // 2, img.height // 2
    pre = cgbi.premultiply(img.rgba)
    stride = img.width * 4
    out = bytearray(w * h * 4)
    for y in range(h):
        a = pre[2 * y * stride : (2 * y + 1) * stride]
        b = pre[(2 * y + 1) * stride : (2 * y + 2) * stride]
        sums = bytes((t + u + 2) // 4 for t, u in zip(_pair_sums(a), _pair_sums(b), strict=True))
        o = y * w * 4
        for c in range(4):
            out[o + c : o + w * 4 : 4] = sums[c * w : (c + 1) * w]
    return png.Image(w, h, cgbi.unpremultiply(bytes(out)))


def icns(pngs: dict[int, bytes]) -> bytes:
    """An icns container of the PNGs by pixel size: every ICNS_TYPES entry whose size is in
    ``pngs``."""
    body = b""
    for kind, size in ICNS_TYPES:
        data = pngs.get(size)
        if data is not None:
            body += kind + struct.pack(">I", len(data) + 8) + data
    if not body:
        raise ValueError("no icon sizes")
    return b"icns" + struct.pack(">I", len(body) + 8) + body


# --- Building --------------------------------------------------------------------------------


def sips_resize(source: Path, size: int, out: Path) -> None:
    """``sips``: any image ImageIO reads (JPEG, CgBI PNG, ...) to a size x size PNG."""
    sips = shutil.which("sips")
    if sips is None:
        raise IconError("sips not found: the icon is built on macOS only")
    result = subprocess.run(
        [sips, "-s", "format", "png", "-z", str(size), str(size), str(source), "--out", str(out)],
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not out.is_file():
        raise IconError(f"sips failed on {source}: {result.stderr.strip() or result.stdout}")


def build(source: Path, canvas: int = CANVAS, resize: Resize = sips_resize) -> bytes:
    """The icns bytes for ``source``; ``canvas`` (a power of two, 16 or more) is the largest
    size."""
    if canvas < 16 or canvas & (canvas - 1):
        raise ValueError(f"canvas {canvas} is not a power of two of at least 16")
    side = content_size(canvas)
    with tempfile.TemporaryDirectory(prefix="macicon-") as tmp:
        scaled = Path(tmp) / "content.png"
        resize(source, side, scaled)
        try:
            art = cgbi.decode(scaled.read_bytes(), str(scaled))
        except png.PngError as e:
            raise IconError(f"{source}: {e}") from e
    if (art.width, art.height) != (side, side):
        raise IconError(f"{source}: scaled to {art.width}x{art.height}, expected {side}x{side}")
    img = compose(art, canvas)
    pngs: dict[int, bytes] = {}
    while True:
        pngs[img.width] = png.encode_rgba(img.width, img.height, img.rgba)
        if img.width == 16:
            break
        img = halve(img)
    return icns(pngs)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.macicon", description=__doc__.split("\n")[0])
    parser.add_argument(
        "--ipa",
        type=Path,
        default=Path(bootstrap.IPA_DIR),
        help="the extracted IPA, the directory holding Payload/ (default: work/ipa)",
    )
    parser.add_argument("--source", type=Path, help="this image instead of the IPA's artwork")
    parser.add_argument("--out", type=Path, required=True, help="the .icns to write")
    args = parser.parse_args(argv)
    try:
        source = args.source if args.source is not None else find_source(args.ipa)
        data = build(source)
    except (IconError, OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    tmp = args.out.with_name(args.out.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, args.out)
    print(f"macicon: {source} -> {args.out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
