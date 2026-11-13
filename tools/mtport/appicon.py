"""The port's own app icon (#182, ADR 0125): a geometric mark we drew, not the game's art.

The player builds (and every other build except the ``mac-app`` preset, whose icon comes from
the IPA, ADR 0064) carry this icon. It is a gold four-pointed star with a hole in its middle,
on a deep blue rounded square that has a vertical gradient. No letters, no maple leaf, nothing
taken from Nexon's art or branding. This script is the icon's source. It draws the shapes from
the constants below with signed distances, anti-aliased over one pixel. It uses only
``+ - * /`` and ``sqrt``, so every platform draws the same pixels, and it writes::

    platform/icon/MapleThief.svg    the same shapes as a vector image, for viewing and editing
    platform/icon/MapleThief.icns   macOS: 16, 32, 64, 128, 256 and 512 px PNG entries
                                    (icp4 icp5 ic11 ic12 ic07 ic08 ic13 ic09 ic14, mtport.macicon)
    platform/icon/MapleThief.ico    Windows: 16, 24, 32, 48, 64 and 256 px PNG entries
    platform/icon/icon.png          Linux: 256 px, the .desktop entry's icon (MapleThief.sh)
    platform/icon/Assets.xcassets/  iOS (ADR 0136): the full-bleed 1024 px app icon, an opaque
                                    square that the system masks, and the launch screen's
                                    256 px image and background colour

and, not committed, the Nintendo Switch's icon (ADR 0138): the iOS icon's full-bleed square at
256 px as a baseline JPEG (mtport.jpeg, integer arithmetic only), which the Switch build writes
into its build tree (``switch-icon OUT``, platform/switch/switch.cmake) and elf2nro puts in the
NRO. Its SHA-256 is in the allowlist too, and tests/test_appicon.py checks it.

The derived files are committed (ADR 0125). The content guard allowlists them by path and
SHA-256 (``mtport.release.ICON_ALLOWLIST``), and compressed PNG bytes can differ between zlib
builds, so a build that drew them again could get other hashes. tests/test_appicon.py checks
that the committed files decode to exactly the pixels this script draws, and that the
allowlist's hashes match the files.

CLI::

    python3 -m mtport.appicon write [--out DIR]   (default platform/icon/)
    python3 -m mtport.appicon check [--out DIR]   exit 1 if a committed file's pixels differ
    python3 -m mtport.appicon switch-icon FILE    writes the Switch's 256 px JPEG to FILE
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from math import sqrt
from pathlib import Path

from mtport import jpeg, macicon, png

REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = REPO / "platform" / "icon"

# Geometry on a unit canvas (0..1, y down), the macOS icon grid: an 824/1024 rounded square,
# corner radius 185.4/1024, centred.
PLATE_HALF = 412 / 1024
PLATE_RADIUS = 185.4 / 1024
STAR_OUTER = 0.30  # the four points' distance from the centre
STAR_INNER = 0.085  # the four inner corners' distance (on the diagonals)
HOLE_RADIUS = 0.05
# Colours (sRGB): the plate's gradient from top to bottom, the star.
PLATE_TOP = (0x1F, 0x4E, 0x79)
PLATE_BOTTOM = (0x0E, 0x22, 0x3A)
STAR = (0xF2, 0xB3, 0x3D)

ICNS_SIZES = (16, 32, 64, 128, 256, 512)
ICO_SIZES = (16, 24, 32, 48, 64, 256)
PNG_SIZE = 256
FILES = ("MapleThief.svg", "MapleThief.icns", "MapleThief.ico", "icon.png")
# The iOS asset catalog (ADR 0136), below platform/icon/Assets.xcassets.
IOS_ICON_SIZE = 1024
IOS_LAUNCH_SIZE = 256
IOS_CATALOG = "Assets.xcassets"
IOS_ICON = "AppIcon.appiconset/icon-1024.png"
IOS_LAUNCH = "LaunchIcon.imageset/launch-icon.png"
IOS_CHECK_STEP = 37  # check() compares this grid of the 1024 px icon, not all million pixels
# The Nintendo Switch's icon (ADR 0138): the Homebrew Menu shows a 256 x 256 JPEG.
SWITCH_ICON_SIZE = 256
SWITCH_ICON_QUALITY = 92

_D = STAR_INNER / sqrt(2.0)  # the inner corners' x and y offsets
STAR_POINTS = (
    (0.0, -STAR_OUTER),
    (_D, -_D),
    (STAR_OUTER, 0.0),
    (_D, _D),
    (0.0, STAR_OUTER),
    (-_D, _D),
    (-STAR_OUTER, 0.0),
    (-_D, -_D),
)


def _rounded_square(x: float, y: float) -> float:
    """Signed distance (negative inside) from the plate, centred at 0."""
    qx = abs(x) - PLATE_HALF + PLATE_RADIUS
    qy = abs(y) - PLATE_HALF + PLATE_RADIUS
    ox, oy = max(qx, 0.0), max(qy, 0.0)
    return sqrt(ox * ox + oy * oy) + min(max(qx, qy), 0.0) - PLATE_RADIUS


def _star(x: float, y: float) -> float:
    """Signed distance from the star polygon (even-odd inside test)."""
    best = 1e9
    inside = False
    n = len(STAR_POINTS)
    for i in range(n):
        ax, ay = STAR_POINTS[i]
        bx, by = STAR_POINTS[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        wx, wy = x - ax, y - ay
        t = (wx * ex + wy * ey) / (ex * ex + ey * ey)
        t = min(max(t, 0.0), 1.0)
        dx, dy = wx - ex * t, wy - ey * t
        best = min(best, dx * dx + dy * dy)
        if (ay > y) != (by > y) and x < ax + (y - ay) * ex / ey:
            inside = not inside
    d = sqrt(best)
    return -d if inside else d


def _coverage(distance: float, size: int) -> float:
    """Anti-aliased coverage of a shape whose signed distance is ``distance`` (unit canvas)
    for a pixel of 1/size."""
    return min(max(0.5 - distance * size, 0.0), 1.0)


def ios_pixel(px: int, py: int, size: int) -> tuple[int, int, int, int]:
    """One pixel of the iOS icon: the plate's gradient over the whole square, no corners (iOS
    cuts them) and the star scaled with the plate, so that it fills the same part of the icon."""
    scale = 0.5 / PLATE_HALF
    x = ((px + 0.5) / size - 0.5) / scale
    y = ((py + 0.5) / size - 0.5) / scale
    t = (py + 0.5) / size
    plate = [PLATE_TOP[c] + (PLATE_BOTTOM[c] - PLATE_TOP[c]) * t for c in range(3)]
    star = 0.0
    if max(abs(x), abs(y)) <= STAR_OUTER + 2.0 / size:
        star = _coverage(_star(x, y) * scale, size)
        if star > 0.0:
            star *= 1.0 - _coverage((sqrt(x * x + y * y) - HOLE_RADIUS) * scale, size)
    return (
        int(plate[0] + (STAR[0] - plate[0]) * star + 0.5),
        int(plate[1] + (STAR[1] - plate[1]) * star + 0.5),
        int(plate[2] + (STAR[2] - plate[2]) * star + 0.5),
        255,
    )


def render_ios(size: int) -> png.Image:
    """The iOS app icon at ``size`` x ``size`` pixels, opaque."""
    out = bytearray(size * size * 4)
    for py in range(size):
        for px in range(size):
            o = (py * size + px) * 4
            out[o : o + 4] = bytes(ios_pixel(px, py, size))
    return png.Image(size, size, bytes(out))


def render(size: int) -> png.Image:
    """The icon at ``size`` x ``size`` pixels, straight alpha."""
    out = bytearray(size * size * 4)
    for py in range(size):
        y = (py + 0.5) / size - 0.5
        t = (py + 0.5) / size
        plate = [PLATE_TOP[c] + (PLATE_BOTTOM[c] - PLATE_TOP[c]) * t for c in range(3)]
        for px in range(size):
            x = (px + 0.5) / size - 0.5
            alpha = _coverage(_rounded_square(x, y), size)
            if alpha <= 0.0:
                continue
            star = _coverage(_star(x, y), size)
            if star > 0.0:
                star *= 1.0 - _coverage(sqrt(x * x + y * y) - HOLE_RADIUS, size)
            o = (py * size + px) * 4
            for c in range(3):
                v = plate[c] + (STAR[c] - plate[c]) * star
                out[o + c] = int(v + 0.5)
            out[o + 3] = int(alpha * 255 + 0.5)
    return png.Image(size, size, bytes(out))


def svg() -> str:
    """The same shapes as an SVG (1024 x 1024 view box)."""

    def num(v: float) -> str:
        return f"{v * 1024:.2f}".rstrip("0").rstrip(".")

    corner = 0.5 - PLATE_HALF
    side = 2 * PLATE_HALF
    points = " ".join(f"{num(0.5 + px)},{num(0.5 + py)}" for px, py in STAR_POINTS)
    top = "#{:02X}{:02X}{:02X}".format(*PLATE_TOP)
    bottom = "#{:02X}{:02X}{:02X}".format(*PLATE_BOTTOM)
    star = "#{:02X}{:02X}{:02X}".format(*STAR)
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        'viewBox="0 0 1024 1024">\n'
        "  <!-- MapleThief port icon (tools/mtport/appicon.py writes this file; ADR 0125) -->\n"
        "  <defs>\n"
        '    <linearGradient id="plate" x1="0" y1="0" x2="0" y2="1024" '
        'gradientUnits="userSpaceOnUse">\n'
        f'      <stop offset="0" stop-color="{top}"/>\n'
        f'      <stop offset="1" stop-color="{bottom}"/>\n'
        "    </linearGradient>\n"
        '    <mask id="hole">\n'
        '      <rect width="1024" height="1024" fill="white"/>\n'
        f'      <circle cx="512" cy="512" r="{num(HOLE_RADIUS)}" fill="black"/>\n'
        "    </mask>\n"
        "  </defs>\n"
        f'  <rect x="{num(corner)}" y="{num(corner)}" width="{num(side)}" height="{num(side)}" '
        f'rx="{num(PLATE_RADIUS)}" fill="url(#plate)"/>\n'
        f'  <polygon points="{points}" fill="{star}" mask="url(#hole)"/>\n'
        "</svg>\n"
    )


def ico(pngs: dict[int, bytes]) -> bytes:
    """A Windows .ico of PNG entries (Vista and later read PNG at every size)."""
    sizes = sorted(pngs)
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    body = b""
    for s in sizes:
        data = pngs[s]
        dim = 0 if s >= 256 else s  # 0 means 256
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    return header + entries + body


def files() -> dict[str, bytes]:
    """Every file of :data:`FILES`, by name."""
    cache: dict[int, bytes] = {}

    def encoded(size: int) -> bytes:
        if size not in cache:
            img = render(size)
            cache[size] = png.encode_rgba(img.width, img.height, img.rgba)
        return cache[size]

    return {
        "MapleThief.svg": svg().encode("utf-8"),
        "MapleThief.icns": macicon.icns({s: encoded(s) for s in ICNS_SIZES}),
        "MapleThief.ico": ico({s: encoded(s) for s in ICO_SIZES}),
        "icon.png": encoded(PNG_SIZE),
    }


def ios_catalog_json() -> dict[str, bytes]:
    """The catalog's Contents.json files, by path inside it."""
    info = {"info": {"author": "xcode", "version": 1}}

    def doc(body: dict) -> bytes:
        return (json.dumps({**body, **info}, indent=2) + "\n").encode("utf-8")

    def channel(value: int) -> str:
        return f"0x{value:02X}"

    return {
        "Contents.json": doc({}),
        "AppIcon.appiconset/Contents.json": doc(
            {
                "images": [
                    {
                        "filename": IOS_ICON.rsplit("/", 1)[1],
                        "idiom": "universal",
                        "platform": "ios",
                        "size": f"{IOS_ICON_SIZE}x{IOS_ICON_SIZE}",
                    }
                ]
            }
        ),
        "LaunchIcon.imageset/Contents.json": doc(
            {"images": [{"filename": IOS_LAUNCH.rsplit("/", 1)[1], "idiom": "universal"}]}
        ),
        "LaunchBackground.colorset/Contents.json": doc(
            {
                "colors": [
                    {
                        "idiom": "universal",
                        "color": {
                            "color-space": "srgb",
                            "components": {
                                "red": channel(PLATE_BOTTOM[0]),
                                "green": channel(PLATE_BOTTOM[1]),
                                "blue": channel(PLATE_BOTTOM[2]),
                                "alpha": "1.000",
                            },
                        },
                    }
                ]
            }
        ),
    }


def ios_files() -> dict[str, bytes]:
    """Every file of the iOS asset catalog, by path inside it."""
    icon = render_ios(IOS_ICON_SIZE)
    launch = render(IOS_LAUNCH_SIZE)
    return {
        **ios_catalog_json(),
        IOS_ICON: png.encode_rgba(icon.width, icon.height, icon.rgba),
        IOS_LAUNCH: png.encode_rgba(launch.width, launch.height, launch.rgba),
    }


def switch_icon() -> bytes:
    """The Switch's icon: the iOS icon's opaque square at 256 px, as a baseline JPEG."""
    img = render_ios(SWITCH_ICON_SIZE)
    rgb = bytearray()
    for i in range(0, len(img.rgba), 4):
        rgb += img.rgba[i : i + 3]
    return jpeg.encode_rgb(img.width, img.height, bytes(rgb), SWITCH_ICON_QUALITY)


def check_ios(catalog: Path) -> list[str]:
    """The iOS catalog's files that aren't what this script writes (or are missing)."""
    problems = []
    for name, data in ios_catalog_json().items():
        path = catalog / name
        if not path.is_file() or path.read_bytes().replace(b"\r\n", b"\n") != data:
            problems.append(f"{path}: differs from ios_catalog_json()")
    icon, launch = catalog / IOS_ICON, catalog / IOS_LAUNCH
    if not icon.is_file() or not launch.is_file():
        return [*problems, f"{catalog}: the icon or the launch image is missing"]
    img = png.decode(icon.read_bytes())
    if (img.width, img.height) != (IOS_ICON_SIZE, IOS_ICON_SIZE):
        problems.append(f"{icon}: {img.width}x{img.height}, expected {IOS_ICON_SIZE}")
    else:
        grid = range(0, IOS_ICON_SIZE, IOS_CHECK_STEP)
        if any(img.pixel(px, py) != ios_pixel(px, py, IOS_ICON_SIZE) for py in grid for px in grid):
            problems.append(f"{icon}: differs from ios_pixel()")
    if png.decode(launch.read_bytes()).rgba != render(IOS_LAUNCH_SIZE).rgba:
        problems.append(f"{launch}: differs from render({IOS_LAUNCH_SIZE})")
    return problems


def embedded_pngs(name: str, data: bytes) -> dict[int, bytes]:
    """The PNG entries of a written file by pixel size (icon.png: itself)."""
    if name == "icon.png":
        return {png.decode(data).width: data}
    out: dict[int, bytes] = {}
    if name.endswith(".icns"):
        pos = 8
        while pos < len(data):
            length = struct.unpack(">I", data[pos + 4 : pos + 8])[0]
            entry = data[pos + 8 : pos + length]
            out[png.decode(entry).width] = entry
            pos += length
    elif name.endswith(".ico"):
        count = struct.unpack("<H", data[4:6])[0]
        for i in range(count):
            size, offset = struct.unpack("<II", data[6 + 16 * i + 8 : 6 + 16 * i + 16])
            entry = data[offset : offset + size]
            out[png.decode(entry).width] = entry
    return out


def check(out: Path) -> list[str]:
    """The committed files that don't decode to this script's pixels (or are missing)."""
    problems = []
    for name in FILES:
        path = out / name
        if not path.is_file():
            problems.append(f"{path}: missing")
            continue
        data = path.read_bytes()
        if name.endswith(".svg"):
            if data.replace(b"\r\n", b"\n") != svg().encode("utf-8"):
                problems.append(f"{path}: differs from appicon.svg()")
            continue
        entries = embedded_pngs(name, data)
        want = {"MapleThief.icns": ICNS_SIZES, "MapleThief.ico": ICO_SIZES}.get(name, (PNG_SIZE,))
        if sorted(entries) != sorted(set(want)):
            problems.append(f"{path}: sizes {sorted(entries)}, expected {sorted(set(want))}")
            continue
        for size, entry in entries.items():
            if png.decode(entry).rgba != render(size).rgba:
                problems.append(f"{path}: the {size} px image differs from render({size})")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python3 -m mtport.appicon", description=__doc__)
    parser.add_argument("cmd", choices=("write", "check", "switch-icon"))
    parser.add_argument("file", nargs="?", type=Path, help="switch-icon: the JPEG to write")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args(argv)
    if args.cmd == "switch-icon":
        if args.file is None:
            parser.error("switch-icon needs the FILE to write")
        data = switch_icon()
        args.file.parent.mkdir(parents=True, exist_ok=True)
        args.file.write_bytes(data)
        print(f"appicon: {args.file} ({len(data)} bytes)")
        return 0
    if args.cmd == "write":
        args.out.mkdir(parents=True, exist_ok=True)
        for name, data in files().items():
            (args.out / name).write_bytes(data)
            print(f"appicon: {args.out / name} ({len(data)} bytes)")
        for name, data in ios_files().items():
            path = args.out / IOS_CATALOG / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f"appicon: {path} ({len(data)} bytes)")
        return 0
    problems = check(args.out) + check_ios(args.out / IOS_CATALOG)
    for p in problems:
        print(f"appicon: {p}", file=sys.stderr)
    if not problems:
        print(f"appicon: {args.out}: every file matches")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
