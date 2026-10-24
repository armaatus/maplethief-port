"""Parser and encoder for the game's ``.att`` attribute files (spec: docs/formats/att.md).

A ``.att`` file holds the collision grid of one map: one byte of attribute flags per 8x8-pixel
cell, so the grid is ``2 * width`` by ``2 * height`` cells for a ``.map`` of ``width`` by
``height`` 16x16 tiles. Layout::

    u8 3; char "ATT"[3]; u32 cells;            // little-endian, 8 bytes; the game skips them
    { u8 value; u8 count; } ...                // runs, row-major, until the end of the file

A run with ``value == 1`` (``AIR``) only advances the cursor, because the game fills the grid
with 1 before it decodes. The value is a set of flags (``AIR``, ``FOOTHOLD``, ``LADDER``,
``LADDER_TOP``, ``WALL``); see the spec for the code that tests each one.

The game's decoder is at ``0xD5B1`` in the armv7 slice. It ignores the header and does not
check that the runs fit the grid. This parser requires the magic, requires the runs to add up
to exactly the header's cell count, and rejects a truncated pair, so a file it accepts is one
the game decodes exactly as described. :func:`decode_any` also accepts the one shipped file
that is not in this format: ``106.att`` is a headerless raw grid, one byte per cell.

Command line::

    python3 -m mtport.attfile info FILE...          # cell count, runs, flag histogram
    python3 -m mtport.attfile dump FILE [--width W] # the grid as text (width from the .map)
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

MAGIC = b"\x03ATT"
HEADER_SIZE = 8
CELL_PIXELS = 8  # an attribute cell is half a 16x16 tile in each direction
MAX_RUN = 0xFF

# Attribute flags (docs/formats/att.md lists the code behind each one).
AIR = 0x01  # nothing to stand on: an object here starts falling, mobs turn back at it
FOOTHOLD = 0x02  # one-way floor: lands objects that fall onto it from above
LADDER = 0x04  # climbable ladder or rope (2 cells wide)
LADDER_TOP = 0x08  # stops climbing upwards (caps ladders that hang from nothing)
WALL = 0x10  # blocks movement in every direction
KNOWN_FLAGS = AIR | FOOTHOLD | LADDER | LADDER_TOP | WALL
FLAG_NAMES = ((AIR, "AIR"), (FOOTHOLD, "FOOTHOLD"), (LADDER, "LADDER"))
FLAG_NAMES += ((LADDER_TOP, "LADDER_TOP"), (WALL, "WALL"))
DEFAULT = AIR  # the value the game fills the grid with before it decodes a file
OUT_OF_RANGE = FOOTHOLD  # what the game's cell lookup (0xBBFD) returns past the end of the grid

# One character per value for the text dump; the shipped files use exactly these seven.
DUMP_CHARS = {AIR: ".", FOOTHOLD: "=", LADDER: "#", FOOTHOLD | LADDER: "H"}
DUMP_CHARS.update({LADDER_TOP: "^", WALL: "|", WALL | FOOTHOLD: "+"})


def flag_names(value: int) -> str:
    """``value`` as ``FLAG|FLAG``, with unknown bits in hex, or ``0`` for no flags."""
    names = [name for bit, name in FLAG_NAMES if value & bit]
    if value & ~KNOWN_FLAGS:
        names.append(f"0x{value & ~KNOWN_FLAGS:02X}")
    return "|".join(names) or "0"


def grid_size(map_width: int, map_height: int) -> tuple[int, int]:
    """The attribute grid size in cells for a ``.map`` of ``map_width`` x ``map_height`` tiles."""
    return 2 * map_width, 2 * map_height


@dataclass(frozen=True)
class AttFile:
    """A decoded ``.att`` file: ``cells[y * grid_width + x]`` is the flag byte of one 8x8 cell.

    The file does not store the grid width; it comes from the matching ``.map`` (see
    :func:`grid_size`). ``packed`` is False for the headerless raw grid (``106.att``).
    """

    cells: bytes
    packed: bool = True

    def rows(self, width: int) -> list[bytes]:
        if width <= 0 or len(self.cells) % width:
            raise ValueError(f"{len(self.cells)} cells do not make rows of {width}")
        return [self.cells[y : y + width] for y in range(0, len(self.cells), width)]

    def cell(self, width: int, x: int, y: int) -> int:
        """The flags at cell (x, y), the way the game looks them up (``0xBBFD``).

        A cell index at or past the end of the grid gives ``OUT_OF_RANGE``; the game reads out
        of bounds for a negative index, which this raises ``IndexError`` for instead.
        """
        index = y * width + x
        if index >= len(self.cells):
            return OUT_OF_RANGE
        if index < 0:
            raise IndexError(f"cell ({x}, {y}) is before the start of the grid")
        return self.cells[index]

    def histogram(self) -> dict[int, int]:
        return dict(sorted(Counter(self.cells).items()))


@dataclass(frozen=True)
class DecodeStats:
    """How a file was encoded: run counts, for the spec's validation table."""

    runs: int = 0
    air_runs: int = 0  # runs of value 1, which the game skips instead of writing


def decode(data: bytes, name: str = "<att>") -> AttFile:
    """Decode a ``03 "ATT"`` run-length file. Raises :class:`FormatError` if malformed."""
    return decode_with_stats(data, name)[0]


def decode_with_stats(data: bytes, name: str = "<att>") -> tuple[AttFile, DecodeStats]:
    r = ByteReader(data, name)
    magic = r.raw(len(MAGIC), "magic")
    if magic != MAGIC:
        raise r.error(f"bad magic {magic.hex(' ')}, expected {MAGIC.hex(' ')}", 0)
    count = r.u32("cell count")
    cells = bytearray()
    runs = air_runs = 0
    while not r.at_end:
        start = r.pos
        value = r.u8("run value")
        size = r.u8("run length")
        if len(cells) + size > count:
            raise r.error(
                f"run of {size} at cell {len(cells)} passes the {count} cells of the header",
                start,
            )
        cells += bytes([value]) * size
        runs += 1
        air_runs += value == DEFAULT
    if len(cells) != count:
        raise r.error(f"runs end after {len(cells)} of {count} cells")
    return AttFile(bytes(cells)), DecodeStats(runs, air_runs)


def decode_raw(data: bytes, name: str = "<att>") -> AttFile:
    """Decode a headerless raw grid (one flag byte per cell), the format of ``106.att``.

    Only bytes that are combinations of the known flags are accepted, so this does not
    mistake arbitrary data for a grid. The width is not stored (``106.att`` is 44 wide).
    """
    if not data:
        raise FormatError(f"{name}: empty raw attribute grid")
    for pos, value in enumerate(data):
        if value == 0 or value & ~KNOWN_FLAGS:
            raise FormatError(f"{name}: offset 0x{pos:X}: 0x{value:02X} is not an attribute")
    return AttFile(bytes(data), packed=False)


def decode_any(data: bytes, name: str = "<att>") -> AttFile:
    """:func:`decode` for files with the ``03 "ATT"`` magic, else :func:`decode_raw`."""
    if data[: len(MAGIC)] == MAGIC:
        return decode(data, name)
    return decode_raw(data, name)


def encode(att: AttFile) -> bytes:
    """Encode ``att`` the way the shipped files are encoded (runs of at most 255 equal cells).

    This reproduces all 90 packed shipped files byte for byte (tests/test_attobj_real.py).
    A raw grid (``packed=False``) is returned as its bare bytes.
    """
    if not att.packed:
        return att.cells
    out = bytearray(MAGIC + struct.pack("<I", len(att.cells)))
    cells = att.cells
    pos = 0
    while pos < len(cells):
        start = pos
        while pos < len(cells) and cells[pos] == cells[start] and pos - start < MAX_RUN:
            pos += 1
        out += bytes([cells[start], pos - start])
    return bytes(out)


def load(path: str | Path) -> AttFile:
    path = Path(path)
    return decode_any(path.read_bytes(), path.name)


def _map_width(path: Path) -> int | None:
    """The attribute grid width from the ``.map`` next to ``path``, if there is one."""
    map_path = path.with_suffix(".map")
    try:
        head = map_path.read_bytes()[:2]
    except OSError:
        return None
    return 2 * struct.unpack("<h", head)[0] if len(head) == 2 else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.attfile", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="cell count, runs and flag histogram of each file")
    info.add_argument("files", nargs="+", type=Path)
    dump = sub.add_parser("dump", help="print the grid, one character per cell")
    dump.add_argument("file", type=Path)
    dump.add_argument("--width", type=int, help="grid width (default: 2 x the .map width)")
    args = parser.parse_args(argv)

    status = 0
    if args.command == "info":
        for path in args.files:
            try:
                data = path.read_bytes()
                if data[: len(MAGIC)] == MAGIC:
                    att, stats = decode_with_stats(data, path.name)
                    kind = f"runs={stats.runs} air_runs={stats.air_runs}"
                else:
                    att, kind = decode_raw(data, path.name), "raw (no header)"
            except (OSError, FormatError) as exc:
                print(f"{path}: error: {exc}", file=sys.stderr)
                status = 1
                continue
            hist = " ".join(f"{flag_names(v)}={n}" for v, n in att.histogram().items())
            print(f"{path.name}: cells={len(att.cells)} {kind} {hist}")
    else:
        att = load(args.file)
        width = args.width or _map_width(args.file)
        if not width:
            parser.error(f"{args.file}: no .map next to it; pass --width")
        for row in att.rows(width):
            print("".join(DUMP_CHARS.get(v, "?") for v in row))
    return status


if __name__ == "__main__":
    sys.exit(main())
