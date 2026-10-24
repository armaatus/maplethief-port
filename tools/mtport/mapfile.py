"""Parser (and encoder) for the game's ``.map`` tile-layer files (spec: docs/formats/map.md).

A ``.map`` file holds the tile grid of one map: a width and height in tiles, a layer count,
then each layer as a byte-oriented run-length stream of tile indices. Layout::

    i16 width; i16 height; u8 layers;           // little-endian, 5 bytes
    layers x { RLE tokens until width*height cells are filled }

    token  n in 0x00..0xF0   then one byte v: n copies of v
           n in 0xF1..0xFF   then n - 0xF0 bytes: one cell each (literal run of 1..15)

A cell byte of 0xFF means "no tile" (the game stores it as -1); any other value is an index
into the map's tileset (``.mapprj``, #15). Cells are row-major, ``index = y * width + x``.

The game's decoder is at ``0xD6D1`` in the armv7 slice. It does not check that a run ends at
the layer boundary or that a token's operands are inside the file; this parser rejects both,
and also rejects bytes after the last layer, so a file it accepts is one the game decodes
exactly as described.

Command line::

    python3 -m mtport.mapfile info FILE...   # size, layer count, tokens per file
    python3 -m mtport.mapfile dump FILE      # every layer as a hex grid ('..' = empty)
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

HEADER_SIZE = 5
EMPTY = -1  # a cell with no tile (file byte 0xFF, stored by the game as the i16 -1)
EMPTY_BYTE = 0xFF
LITERAL_BASE = 0xF0  # token bytes above this start a literal run of (token - 0xF0) cells
MAX_RUN = LITERAL_BASE  # longest repeat run one token can encode (240 cells)
MAX_LITERAL = 0xFF - LITERAL_BASE  # longest literal run one token can encode (15 cells)
ENCODER_MAX_RUN = 0xEF  # the shipped files never use a run longer than 239 cells


@dataclass(frozen=True)
class MapFile:
    """A decoded ``.map`` file. ``layers[l][y * width + x]`` is a tile index or ``EMPTY``."""

    width: int
    height: int
    layers: tuple[tuple[int, ...], ...]

    @property
    def cells(self) -> int:
        return self.width * self.height

    def cell(self, layer: int, x: int, y: int) -> int:
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise IndexError(f"cell ({x}, {y}) outside {self.width}x{self.height}")
        return self.layers[layer][y * self.width + x]

    def rows(self, layer: int) -> list[tuple[int, ...]]:
        cells = self.layers[layer]
        return [cells[y * self.width : (y + 1) * self.width] for y in range(self.height)]

    def max_tile(self) -> int:
        """The highest tile index used on any layer, or ``EMPTY`` if every cell is empty."""
        return max((v for layer in self.layers for v in layer), default=EMPTY)


@dataclass(frozen=True)
class DecodeStats:
    """How a file was encoded: token counts, for the spec's validation table."""

    runs: int = 0
    literals: int = 0


def decode(data: bytes, name: str = "<map>") -> MapFile:
    """Decode a whole ``.map`` file. Raises :class:`FormatError` on any malformed input."""
    return decode_with_stats(data, name)[0]


def decode_with_stats(data: bytes, name: str = "<map>") -> tuple[MapFile, DecodeStats]:
    r = ByteReader(data, name)
    width = r.i16("width")
    height = r.i16("height")
    count = r.u8("layer count")
    if width <= 0 or height <= 0:
        raise r.error(f"bad size {width}x{height}", 0)
    cells = width * height
    layers = []
    runs = literals = 0
    for index in range(count):
        layer: list[int] = []
        while len(layer) < cells:
            start = r.pos
            if r.at_end:
                raise r.error(f"layer {index} ends after {len(layer)} of {cells} cells")
            token = r.u8("token")
            if token > LITERAL_BASE:
                size = token - LITERAL_BASE
                values = r.raw(size, f"literal run of {size}")
                literals += 1
            else:
                size = token
                values = bytes([r.u8("run value")]) * size
                runs += 1
            if len(layer) + size > cells:
                raise r.error(
                    f"run of {size} at cell {len(layer)} crosses the end of layer {index} "
                    f"({cells} cells)",
                    start,
                )
            layer.extend(EMPTY if v == EMPTY_BYTE else v for v in values)
        layers.append(tuple(layer))
    r.expect_end(f"{count} layers")
    return MapFile(width, height, tuple(layers)), DecodeStats(runs, literals)


def _encode_layer(cells: tuple[int, ...]) -> bytes:
    """RLE one layer exactly the way the shipped files were encoded (see map.md, Encoder).

    A cell that equals the next one starts a run (up to 239 cells); any other cell starts a
    literal, which grows until the cell at the cursor equals the one after it (or it holds
    15). Two quirks of the original tool are kept: a token that starts on the last cell of a
    layer is a run (of 1), and a literal that holds 14 cells always takes one more cell, even
    the first cell of a repeat. This reproduces all 90 shipped files byte for byte
    (tests/test_mapfile_real.py).
    """
    raw = bytes(EMPTY_BYTE if v == EMPTY else v for v in cells)
    n = len(raw)
    out = bytearray()
    pos = 0
    while pos < n:
        start = pos
        pos += 1
        if pos < n and raw[start] != raw[pos]:
            while pos < n and pos - start < MAX_LITERAL:
                if pos - start != MAX_LITERAL - 1 and pos + 1 < n and raw[pos] == raw[pos + 1]:
                    break
                pos += 1
            out.append(LITERAL_BASE + pos - start)
            out += raw[start:pos]
        else:
            while pos < n and raw[pos] == raw[start] and pos - start < ENCODER_MAX_RUN:
                pos += 1
            out += bytes([pos - start, raw[start]])
    return bytes(out)


def encode(m: MapFile) -> bytes:
    """Encode ``m`` the way the shipped files are encoded. Raises ``ValueError`` if invalid."""
    if not (0 < m.width <= 0x7FFF and 0 < m.height <= 0x7FFF):
        raise ValueError(f"bad size {m.width}x{m.height}")
    if len(m.layers) > 0xFF:
        raise ValueError(f"bad layer count {len(m.layers)}")
    out = bytearray(m.width.to_bytes(2, "little") + m.height.to_bytes(2, "little"))
    out.append(len(m.layers))
    for index, layer in enumerate(m.layers):
        if len(layer) != m.cells:
            raise ValueError(f"layer {index} has {len(layer)} cells, expected {m.cells}")
        if any(v != EMPTY and not 0 <= v < EMPTY_BYTE for v in layer):
            raise ValueError(f"layer {index} has a tile index outside 0..254")
        out += _encode_layer(layer)
    return bytes(out)


def load(path: str | Path) -> MapFile:
    path = Path(path)
    return decode(path.read_bytes(), path.name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.mapfile", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="size, layer count and token counts of each file")
    info.add_argument("files", nargs="+", type=Path)
    dump = sub.add_parser("dump", help="print every layer as a hex grid")
    dump.add_argument("file", type=Path)
    args = parser.parse_args(argv)

    status = 0
    if args.command == "info":
        for path in args.files:
            try:
                m, stats = decode_with_stats(path.read_bytes(), path.name)
            except (OSError, FormatError) as exc:
                print(f"{path}: error: {exc}", file=sys.stderr)
                status = 1
                continue
            print(
                f"{path.name}: {m.width}x{m.height} layers={len(m.layers)} "
                f"runs={stats.runs} literals={stats.literals} max_tile={m.max_tile()}"
            )
    else:
        m = load(args.file)
        for index in range(len(m.layers)):
            print(f"layer {index}")
            for row in m.rows(index):
                print(" ".join(".." if v == EMPTY else f"{v:02X}" for v in row))
    return status


if __name__ == "__main__":
    sys.exit(main())
