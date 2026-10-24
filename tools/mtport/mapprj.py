"""Parser for the game's ``.mapprj`` tileset projects, and map -> tile resolution (spec:
docs/formats/mapprj.md).

A ``.mapprj`` file is the tileset list of one *area*: every map of area ``a`` (the files
``"%d%d.map" % (a, map)``) draws its tile indices from ``map/<a>.mapprj``. Layout::

    i16 tile_width; i16 tile_height;        // little-endian; 16 x 16 in every shipped file
    u8  count;                              // number of tile sheets (1 in every shipped file)
    count x { u8 length; char name[length]; }   // a PNG in map/tile/, no terminator

The game's loader is ``0xE13D`` (armv7). It builds one ``CMMapTileSet`` per name
(constructor ``0xD019``), which loads ``/map/tile/<name>`` and cuts it into
``(sheet_width / tile_width) x (sheet_height / tile_height)`` tiles. Tile index ``i`` of
a single-sheet project is the rectangle at column ``i % cols``, row ``i / cols``.

The loader checks nothing (it does not even know the file size); this parser rejects
truncation, trailing bytes, non-positive tile sizes, an empty sheet list and names that are
empty, non-ASCII or contain a path separator.

The module also holds the other area-level rules the spec documents: the file names, which
``map/back/<n>.png`` backdrop an area shows, and the game's index walk across sheets.

Command line::

    python3 -m mtport.mapprj info FILE...   # tile size and sheet names of each project
    python3 -m mtport.mapprj areas APPDIR   # per-area table: sheet size, tiles, maps, backdrop
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

HEADER_SIZE = 5
TILE_DIR = "map/tile"  # the loader's prefix is "/map/tile/" (0xE27EA), relative to the bundle
BACK_DIR = "map/back"  # "/map/back/" (0xE59B2)
BAKED_TILE_SIZE = 16  # the .map bake copies 16 x 16 pixel tiles whatever the project says
_FORBIDDEN_NAME_BYTES = b"/\\"  # path separators; control and non-ASCII bytes are refused too

# Areas whose backdrop is not map/back/<area>.png: the remap in the back loader 0x86109.
BACK_OVERRIDES = {5: 4, 7: 11, 8: 12}


@dataclass(frozen=True)
class MapProject:
    """A decoded ``.mapprj`` file: the tile size in pixels and the sheet file names."""

    tile_width: int
    tile_height: int
    tilesets: tuple[str, ...]

    def tileset_paths(self) -> tuple[str, ...]:
        """Bundle-relative paths of the sheets, as the game builds them (``"%s%s"``)."""
        return tuple(f"{TILE_DIR}/{name}" for name in self.tilesets)


@dataclass(frozen=True)
class TileSheet:
    """One ``CMMapTileSet`` as the constructor ``0xD019`` sets it up."""

    name: str
    width: int  # sheet size in pixels (CMMapTileSet+0x1C, +0x20)
    height: int
    cols: int  # +0x2C = width / tile_width (integer division)
    rows: int  # +0x30 = height / tile_height
    first: int  # +0x38: sum of the counts of the sheets before this one

    @property
    def count(self) -> int:
        """+0x34: tiles in the sheet, ``cols * rows``."""
        return self.cols * self.rows


@dataclass(frozen=True)
class TileSource:
    """Where a cell's pixels come from: sheet ``sheet``, rectangle ``(x, y, w, h)`` in it."""

    sheet: int
    x: int
    y: int
    w: int
    h: int


@dataclass(frozen=True)
class TileLayout:
    """A project combined with its sheets' sizes: enough to resolve any tile index."""

    project: MapProject
    sheets: tuple[TileSheet, ...]

    @property
    def tile_count(self) -> int:
        return sum(s.count for s in self.sheets)

    def locate(self, index: int) -> tuple[int, int]:
        """(sheet, index inside it) for a cell value, exactly as the bake at 0xD9E4 does it.

        One sheet: the index is used as it is. Several sheets: the game walks them with
        ``while count[k] <= v: v -= count[k] - 1; k += 1`` (0xDC02..0xDC4C). Note the
        ``- 1``: it does not match the ``first`` offsets the loader computes, so global index
        ``count[0]`` lands on tile 1 of sheet 1, not tile 0. No shipped project has more than
        one sheet, so this path never runs; it is reproduced, not corrected.

        Raises ``IndexError`` for an empty cell (< 0) or an index past the sheet it lands in;
        the game does not check the latter and reads outside the sheet's pixels.
        """
        if index < 0:
            raise IndexError(f"cell value {index} is empty, not a tile")
        if len(self.sheets) == 1:
            sheet, local = 0, index
        else:
            sheet, local = 0, index
            while sheet < len(self.sheets) and self.sheets[sheet].count <= local:
                local -= self.sheets[sheet].count - 1
                sheet += 1
            if sheet == len(self.sheets):
                raise IndexError(f"tile {index} is past the last sheet")
        if local >= self.sheets[sheet].count:
            raise IndexError(
                f"tile {index} is outside sheet {sheet} ({self.sheets[sheet].count} tiles)"
            )
        return sheet, local

    def tile_rect(self, index: int) -> TileSource:
        """The source rectangle of tile ``index`` (0xDC4E..0xDCB2)."""
        sheet, local = self.locate(index)
        s = self.sheets[sheet]
        tw, th = self.project.tile_width, self.project.tile_height
        return TileSource(sheet, (local % s.cols) * tw, (local // s.cols) * th, tw, th)


def decode(data: bytes, name: str = "<mapprj>") -> MapProject:
    """Decode a whole ``.mapprj`` file. Raises :class:`FormatError` on any malformed input."""
    r = ByteReader(data, name)
    tile_width = r.i16("tile width")
    tile_height = r.i16("tile height")
    if tile_width <= 0 or tile_height <= 0:
        raise r.error(f"bad tile size {tile_width}x{tile_height}", 0)
    count = r.u8("tileset count")
    if count == 0:
        raise r.error("no tilesets", 4)
    names = []
    for index in range(count):
        start = r.pos
        raw = r.pstr(f"tileset {index} name")
        if not raw:
            raise r.error(f"tileset {index} has an empty name", start)
        if any(b in _FORBIDDEN_NAME_BYTES or not 0x20 <= b < 0x7F for b in raw) or raw in (
            b".",
            b"..",
        ):
            raise r.error(f"tileset {index} name {raw!r} is not a plain file name", start)
        names.append(raw.decode("ascii"))
    r.expect_end(f"{count} tileset names")
    return MapProject(tile_width, tile_height, tuple(names))


def encode(p: MapProject) -> bytes:
    """Encode ``p`` in the shipped layout. Raises ``ValueError`` if it cannot be stored."""
    if not (0 < p.tile_width <= 0x7FFF and 0 < p.tile_height <= 0x7FFF):
        raise ValueError(f"bad tile size {p.tile_width}x{p.tile_height}")
    if not 0 < len(p.tilesets) <= 0xFF:
        raise ValueError(f"bad tileset count {len(p.tilesets)}")
    out = bytearray(p.tile_width.to_bytes(2, "little") + p.tile_height.to_bytes(2, "little"))
    out.append(len(p.tilesets))
    for name in p.tilesets:
        raw = name.encode("ascii")
        if not 0 < len(raw) <= 0xFF:
            raise ValueError(f"bad tileset name length {len(raw)}")
        out.append(len(raw))
        out += raw
    return bytes(out)


def load(path: str | Path) -> MapProject:
    path = Path(path)
    return decode(path.read_bytes(), path.name)


def layout(project: MapProject, sheet_sizes: list[tuple[int, int]]) -> TileLayout:
    """Cut each sheet the way ``CMMapTileSet`` does, given each sheet's (width, height) in px."""
    if len(sheet_sizes) != len(project.tilesets):
        raise ValueError(f"{len(sheet_sizes)} sheet sizes for {len(project.tilesets)} tilesets")
    sheets = []
    first = 0
    for name, (width, height) in zip(project.tilesets, sheet_sizes, strict=True):
        sheet = TileSheet(
            name, width, height, width // project.tile_width, height // project.tile_height, first
        )
        sheets.append(sheet)
        first += sheet.count
    return TileLayout(project, tuple(sheets))


# --- Area-level naming rules ---------------------------------------------------------------


def project_path(area: int) -> str:
    """``"%s%d%s" % ("/map/", area, ".mapprj")``, bundle-relative."""
    return f"map/{area}.mapprj"


def map_stem(area: int, map_no: int) -> str:
    """The shared stem of a map's ``.map``/``.att``/``.obj`` files: ``"%d%d" % (area, map)``."""
    return f"{area}{map_no}"


def split_map_stem(stem: str, areas: set[int] | frozenset[int]) -> tuple[int, int]:
    """(area, map) for a ``.map`` file stem, given the areas that have a project.

    ``"%d%d"`` is ambiguous on its own (``"110"`` could be area 1 map 10 or area 11 map 0),
    so the split keeps only areas that exist. Raises ``ValueError`` if no split, or more than
    one, is left. Every shipped stem has exactly one.
    """
    found = []
    for cut in range(1, len(stem)):
        head, tail = stem[:cut], stem[cut:]
        if not (head.isdigit() and tail.isdigit()):
            continue
        area, map_no = int(head), int(tail)
        if map_stem(area, map_no) == stem and area in areas:
            found.append((area, map_no))
    if len(found) != 1:
        raise ValueError(
            f"map name {stem!r} splits into {found or 'nothing'} for areas {sorted(areas)}"
        )
    return found[0]


def back_image(area: int) -> int:
    """The ``map/back/<n>.png`` an area shows behind its map (``0x86109``)."""
    return BACK_OVERRIDES.get(area, area)


def back_path(area: int) -> str:
    return f"{BACK_DIR}/{back_image(area)}.png"


# --- Command line --------------------------------------------------------------------------


def _areas(app: Path) -> int:
    from mtport import mapfile, pnginfo

    root = app / "map"
    projects = sorted(root.glob("*.mapprj"), key=lambda p: int(p.stem))
    areas = {int(p.stem) for p in projects}
    maps: dict[int, list[tuple[int, Path]]] = {a: [] for a in areas}
    for path in root.glob("*.map"):
        area, map_no = split_map_stem(path.stem, areas)
        maps[area].append((map_no, path))
    print("area  tileset               sheet     cols rows tiles  maps     max  back")
    for path in projects:
        area = int(path.stem)
        project = load(path)
        sizes = []
        for sheet in project.tileset_paths():
            info = pnginfo.load_info(app / sheet)
            sizes.append((info.width, info.height))
        lay = layout(project, sizes)
        nums = sorted(n for n, _ in maps[area])
        top = max(mapfile.load(p).max_tile() for _, p in maps[area])
        s = lay.sheets[0]
        print(
            f"{area:>4}  {','.join(project.tilesets):<20}  {s.width:>3}x{s.height:<4}  "
            f"{s.cols:>4} {s.rows:>4} {lay.tile_count:>5}  {nums[0]}-{nums[-1]:<5}  "
            f"{top:>4}  {back_image(area)}.png"
        )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.mapprj", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="tile size and tileset names of each project")
    info.add_argument("files", nargs="+", type=Path)
    areas = sub.add_parser("areas", help="per-area table from an extracted .app directory")
    areas.add_argument("app", type=Path, help="e.g. work/ipa/Payload/MapleThief.app")
    args = parser.parse_args(argv)

    if args.command == "areas":
        return _areas(args.app)
    status = 0
    for path in args.files:
        try:
            p = load(path)
        except (OSError, FormatError) as exc:
            print(f"{path}: error: {exc}", file=sys.stderr)
            status = 1
            continue
        print(f"{path.name}: tile {p.tile_width}x{p.tile_height} tilesets={list(p.tilesets)}")
    return status


if __name__ == "__main__":
    sys.exit(main())
