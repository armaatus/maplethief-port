"""Texture-pack builder: upscale your own texture dump into a pack (#153, ADR 0081).

The game writes a dump with ``MapleThief --dump-textures DUMP`` (``maplethief/texpack.h``):

- ``DUMP/<hash>_<W>x<H>.png``: every texture it uploads, straight-alpha RGBA (``.raw.png``:
  texels that are not premultiplied, as stored);
- ``DUMP/ui/<bundle path>``: every UIKit image the shell loads by file name;
- ``DUMP/manifest.jsonl``: one JSON object a line for each of those files.

``python3 -m mtport.texpack build DUMP PACK --scale K`` writes each of them, K times larger,
under the same name into PACK, which ``texture_pack = PACK`` (settings.ini, or
``--texture-pack PACK``) then loads. The game checks every file: a PNG of exactly K times the
size its name (or the UIKit image) says, K from 1 to 8, at most 4096 pixels a side.

Scalers (standard library only, ADR 0003; xBRZ is not available as a pure-Python package):

- ``scale`` (the default): the Scale2x / Scale3x rules (AdvMAME2x/3x, EPX), which copy
  neighbours into the corners where two edges meet, so diagonal edges stay sharp and nothing is
  blended. K = 4 and 8 apply Scale2x two or three times, K = 6 Scale2x then Scale3x; K = 5 and
  7 have no such rule and fall back to nearest.
- ``nearest``: every pixel becomes K x K.

The rules compare whole pixels for equality, so alpha is handled like any colour and a pack
made from a straight-alpha dump stays straight alpha (the game premultiplies it at load, as its
own loader does). A dump and a pack are copies of the game's art: keep them to yourself, in
``work/`` or elsewhere outside the repository, never in git.

``build`` also takes ``--jobs N`` (processes; default the CPU count), ``--no-ui`` (only the
GL textures) and ``--skip-without-dump`` (exit 77, CTest's skip, when DUMP holds no dump).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from array import array
from collections.abc import Sequence
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

from mtport import png

MAX_SCALE = 8
MAX_SIZE = 4096
SCALERS = ("scale", "nearest")
SKIPPED = 77  # build --skip-without-dump: CTest's skip status

NAME_RE = re.compile(r"^[0-9a-fA-F]{16}_([1-9][0-9]{0,3})x([1-9][0-9]{0,3})\.(raw\.)?png$")
UI_PART_RE = re.compile(r"^[A-Za-z0-9 _\-+.@]+$")


class TexpackError(ValueError):
    """A dump or a request the builder cannot use."""


# --- Scalers ---------------------------------------------------------------------------------
#
# An image is a list of rows, each a list of pixels as 32-bit integers (the four RGBA bytes in
# the machine's order: only compared and copied, never taken apart).


Rows = list[list[int]]


def to_rows(image: png.Image) -> Rows:
    px = array("I", image.rgba)
    w = image.width
    return [px[y * w : (y + 1) * w].tolist() for y in range(image.height)]


def from_rows(rows: Rows) -> png.Image:
    height = len(rows)
    width = len(rows[0]) if rows else 0
    px = array("I")
    for row in rows:
        px.extend(row)
    return png.Image(width, height, px.tobytes())


def nearest(rows: Rows, k: int) -> Rows:
    out: Rows = []
    for row in rows:
        wide = [p for p in row for _ in range(k)]
        out.extend(list(wide) for _ in range(k))
    return out


def _neighbours(rows: Rows, y: int) -> tuple[list[int], list[int], list[int], list[int]]:
    """Above, left, right and below each pixel of row y (the border repeated)."""
    row = rows[y]
    up = rows[y - 1] if y > 0 else row
    down = rows[y + 1] if y + 1 < len(rows) else row
    left = [row[0], *row[:-1]]
    right = [*row[1:], row[-1]]
    return up, left, right, down


def scale2x(rows: Rows) -> Rows:
    """Scale2x (AdvMAME2x): E's four corners take D, F (left, right) where an edge meets."""
    out: Rows = []
    for y, row in enumerate(rows):
        b_row, d_row, f_row, h_row = _neighbours(rows, y)
        top: list[int] = []
        bottom: list[int] = []
        for b, d, e, f, h in zip(b_row, d_row, row, f_row, h_row, strict=True):
            if b != h and d != f:
                top.append(d if d == b else e)
                top.append(f if b == f else e)
                bottom.append(d if d == h else e)
                bottom.append(f if h == f else e)
            else:
                top.append(e)
                top.append(e)
                bottom.append(e)
                bottom.append(e)
        out.append(top)
        out.append(bottom)
    return out


def scale3x(rows: Rows) -> Rows:
    """Scale3x (AdvMAME3x), with the diagonal neighbours A, C, G, I for the edge pixels."""
    out: Rows = []
    for y, row in enumerate(rows):
        b_row, d_row, f_row, h_row = _neighbours(rows, y)
        a_row = [b_row[0], *b_row[:-1]]
        c_row = [*b_row[1:], b_row[-1]]
        g_row = [h_row[0], *h_row[:-1]]
        i_row = [*h_row[1:], h_row[-1]]
        r0: list[int] = []
        r1: list[int] = []
        r2: list[int] = []
        for a, b, c, d, e, f, g, h, i in zip(
            a_row, b_row, c_row, d_row, row, f_row, g_row, h_row, i_row, strict=True
        ):
            if b != h and d != f:
                r0.append(d if d == b else e)
                r0.append(b if (d == b and e != c) or (b == f and e != a) else e)
                r0.append(f if b == f else e)
                r1.append(d if (d == b and e != g) or (d == h and e != a) else e)
                r1.append(e)
                r1.append(f if (b == f and e != i) or (h == f and e != c) else e)
                r2.append(d if d == h else e)
                r2.append(h if (d == h and e != i) or (h == f and e != g) else e)
                r2.append(f if h == f else e)
            else:
                r0 += (e, e, e)
                r1 += (e, e, e)
                r2 += (e, e, e)
        out += (r0, r1, r2)
    return out


def scale_steps(k: int, scaler: str) -> list[int]:
    """The passes for factor k: 2 and 3 for ``scale`` (empty for k = 1); [k] means nearest."""
    if not 1 <= k <= MAX_SCALE:
        raise TexpackError(f"scale {k}: must be 1 to {MAX_SCALE}")
    if scaler not in SCALERS:
        raise TexpackError(f"scaler {scaler!r}: must be one of {', '.join(SCALERS)}")
    if k == 1:
        return []
    if scaler == "scale":
        steps = {2: [2], 3: [3], 4: [2, 2], 6: [2, 3], 8: [2, 2, 2]}.get(k)
        if steps is not None:
            return steps
    return [-k]  # nearest


def upscale(image: png.Image, k: int, scaler: str = "scale") -> png.Image:
    """The image k times larger."""
    steps = scale_steps(k, scaler)
    if not steps:
        return image
    if image.width * k > MAX_SIZE or image.height * k > MAX_SIZE:
        raise TexpackError(
            f"{image.width}x{image.height} at {k}x is above {MAX_SIZE} pixels a side"
        )
    rows = to_rows(image)
    for step in steps:
        rows = nearest(rows, -step) if step < 0 else scale2x(rows) if step == 2 else scale3x(rows)
    return from_rows(rows)


# --- The dump ---------------------------------------------------------------------------------


@dataclass(frozen=True)
class Entry:
    """One file of the dump: its path relative to the dump and its size there."""

    file: str  # "<hash>_<W>x<H>.png" or "ui/<bundle path>"
    width: int
    height: int


def safe_ui_path(rel: str) -> bool:
    """A bundle path the game names below ui/: plain '/'-separated parts ending in .png."""
    if not rel.endswith(".png") or len(rel) > 255:
        return False
    parts = rel.split("/")
    return all(p not in ("", ".", "..") and UI_PART_RE.match(p) for p in parts)


def entry_of(record: object) -> Entry | None:
    """An Entry for a manifest object, or None if it is not one the builder can trust."""
    if not isinstance(record, dict):
        return None
    file = record.get("file")
    width = record.get("width")
    height = record.get("height")
    if not isinstance(file, str) or not isinstance(width, int) or not isinstance(height, int):
        return None
    if not (1 <= width <= MAX_SIZE and 1 <= height <= MAX_SIZE):
        return None
    if file.startswith("ui/"):
        return Entry(file, width, height) if safe_ui_path(file[3:]) else None
    m = NAME_RE.match(file)
    if m is None or (int(m.group(1)), int(m.group(2))) != (width, height):
        return None
    return Entry(file, width, height)


def read_dump(dump: Path, *, ui: bool = True) -> tuple[list[Entry], list[str]]:
    """The dump's files (from its manifest, each once) and the manifest lines it skipped."""
    manifest = dump / "manifest.jsonl"
    if not manifest.is_file():
        raise TexpackError(f"{dump}: no manifest.jsonl (not a --dump-textures directory)")
    entries: dict[str, Entry] = {}
    skipped: list[str] = []
    with manifest.open(encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                record = None
            entry = entry_of(record)
            if entry is None:
                skipped.append(f"line {number}: {line[:80]}")
                continue
            if entry.file.startswith("ui/") and not ui:
                continue
            entries.setdefault(entry.file, entry)
    return list(entries.values()), skipped


def build_one(dump: str, pack: str, entry: Entry, k: int, scaler: str) -> str:
    """Upscales one file of the dump into the pack. Returns "" or what went wrong."""
    src = Path(dump, *PurePosixPath(entry.file).parts)
    dst = Path(pack, *PurePosixPath(entry.file).parts)
    try:
        image = png.load(src)
        if (image.width, image.height) != (entry.width, entry.height):
            return f"{entry.file}: {image.width}x{image.height}, the manifest says " + (
                f"{entry.width}x{entry.height}"
            )
        out = upscale(image, k, scaler)
        dst.parent.mkdir(parents=True, exist_ok=True)
        png.save(dst, out)
    except (OSError, ValueError) as e:  # PngError and TexpackError are ValueErrors
        return f"{entry.file}: {e}"
    return ""


def build(
    dump: Path, pack: Path, k: int, scaler: str = "scale", *, jobs: int = 0, ui: bool = True
) -> tuple[int, list[str]]:
    """Builds the pack. Returns the files written and the problems (skipped lines, failures)."""
    scale_steps(k, scaler)  # check the request before reading anything
    if pack.resolve() == dump.resolve():
        raise TexpackError("the pack must be another directory than the dump")
    entries, problems = read_dump(dump, ui=ui)
    problems = [f"manifest {p}: skipped" for p in problems]
    pack.mkdir(parents=True, exist_ok=True)
    workers = jobs if jobs > 0 else (os.cpu_count() or 1)
    args = [(str(dump), str(pack), e, k, scaler) for e in entries]
    if workers == 1 or len(args) < 2:
        results = [build_one(*a) for a in args]
    else:
        with ProcessPoolExecutor(max_workers=workers) as pool:
            results = list(pool.map(build_one, *zip(*args, strict=True), chunksize=4))
    failures = [r for r in results if r]
    return len(results) - len(failures), problems + failures


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.texpack", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="upscale a --dump-textures directory into a texture pack")
    b.add_argument("dump", type=Path, metavar="DUMP")
    b.add_argument("pack", type=Path, metavar="PACK")
    b.add_argument("--scale", type=int, default=2, metavar="K", help="1 to 8 (default 2)")
    b.add_argument("--scaler", choices=SCALERS, default="scale")
    b.add_argument("--jobs", type=int, default=0, metavar="N", help="processes (0: CPU count)")
    b.add_argument("--no-ui", action="store_true", help="only the GL textures")
    b.add_argument(
        "--skip-without-dump",
        action="store_true",
        help=f"exit {SKIPPED} (a skipped test) when DUMP has no manifest (CTest without assets)",
    )
    args = parser.parse_args(argv)
    if args.skip_without_dump and not (args.dump / "manifest.jsonl").is_file():
        print(f"skipped: {args.dump} holds no dump")
        return SKIPPED
    try:
        written, problems = build(
            args.dump, args.pack, args.scale, args.scaler, jobs=args.jobs, ui=not args.no_ui
        )
    except TexpackError as e:
        print(f"mtport.texpack: {e}", file=sys.stderr)
        return 2
    for p in problems:
        print(f"mtport.texpack: {p}", file=sys.stderr)
    print(f"texpack: {written} files at {args.scale}x ({args.scaler}) in {args.pack}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
