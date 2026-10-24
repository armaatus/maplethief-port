"""Parser (and encoder) for the game's ``.obj`` map-object files (spec: docs/formats/obj.md).

A ``.obj`` file lists what stands on one map: portals, NPCs and mob spawn points, plus at most
one record that names the map's own script. Layout::

    u8 count;                                   // records that follow
    count x { i16 cell; u8 kind; u8 len; char name[len]; }   // little-endian, no padding

``cell`` is a tile index on the matching ``.map`` grid (``y * width + x``); the record with
``cell == -1`` carries the map script name instead of an object. ``kind`` is a portal number
(0-3), an NPC/sprite id (4-59) or a mob type (60 and up). ``name`` is the script run when the
hero interacts with the object (``/script/chapter%d/<name>.out``, else ``/script/common/``);
an empty name means no script.

The game's loader is at ``0xD259`` in the armv7 slice. It reads with no bounds checks at all;
this parser rejects truncated records, trailing bytes, a second map-script record (which the
game's two passes would mis-size its object array for) and non-ASCII names, so a file it
accepts is one the game loads exactly as described.

Command line::

    python3 -m mtport.objfile info FILE...   # record counts per category
    python3 -m mtport.objfile dump FILE      # one record per line (tile x/y from the .map)
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

MAP_SCRIPT_CELL = -1  # the record with this cell names the map script; its kind is 0xFF
MAX_RECORDS = 0xFF
TILE_PIXELS = 16

# Kind ranges (docs/formats/obj.md lists the code behind each one).
PORTAL_KINDS = range(0, 4)  # portal number; arrival points are matched by it
HIDDEN_PORTAL = 3  # a portal that is neither drawn nor shown on the minimap
SPRITE_KINDS = range(0, 60)  # kinds that load the sprite project /sprite/mob/<kind>.sprprj
NPC_KINDS = range(4, 60)  # NPC id = kind - 4 (sprite npc<id>.png)
MOB_KIND_BASE = 60  # kinds from here up spawn mob type kind - 60

CATEGORIES = ("map_script", "portal", "hidden_portal", "npc", "mob")


@dataclass(frozen=True)
class Record:
    """One record. For an object, ``cell`` is a tile index and ``kind`` its type."""

    cell: int
    kind: int
    name: str

    @property
    def is_map_script(self) -> bool:
        return self.cell == MAP_SCRIPT_CELL

    @property
    def category(self) -> str:
        if self.is_map_script:
            return "map_script"
        if self.kind == HIDDEN_PORTAL:
            return "hidden_portal"
        if self.kind in PORTAL_KINDS:
            return "portal"
        if self.kind in NPC_KINDS:
            return "npc"
        return "mob"

    def tile(self, map_width: int) -> tuple[int, int]:
        """The (x, y) tile the object stands on (``0xBCBD``, ``0xBCE5``)."""
        if self.is_map_script:
            raise ValueError("the map-script record has no position")
        return self.cell % map_width, self.cell // map_width

    def position(self, map_width: int) -> tuple[int, int]:
        """The object's anchor in pixels: the bottom-centre pixel of its tile.

        That is ``(16 * x + 8, 16 * y + 15)``, as ``0xBD0D`` and ``0xBD41`` compute it.
        """
        x, y = self.tile(map_width)
        return x * TILE_PIXELS + TILE_PIXELS // 2, (y + 1) * TILE_PIXELS - 1


@dataclass(frozen=True)
class ObjFile:
    """A decoded ``.obj`` file: its records in file order."""

    records: tuple[Record, ...]

    @property
    def objects(self) -> tuple[Record, ...]:
        """The records the game keeps as objects (everything but the map-script record)."""
        return tuple(r for r in self.records if not r.is_map_script)

    @property
    def map_script(self) -> str | None:
        """The map script name, or None if the file has no map-script record."""
        return next((r.name for r in self.records if r.is_map_script), None)

    def counts(self) -> dict[str, int]:
        out = dict.fromkeys(CATEGORIES, 0)
        for r in self.records:
            out[r.category] += 1
        return out


def decode(data: bytes, name: str = "<obj>") -> ObjFile:
    """Decode a whole ``.obj`` file. Raises :class:`FormatError` on any malformed input."""
    r = ByteReader(data, name)
    count = r.u8("record count")
    records = []
    for index in range(count):
        start = r.pos
        cell = r.i16(f"record {index} cell")
        kind = r.u8(f"record {index} kind")
        raw = r.pstr(f"record {index} name")
        try:
            text = raw.decode("ascii")
        except UnicodeDecodeError:
            raise r.error(f"record {index} name {raw!r} is not ASCII", start) from None
        if "\0" in text:
            raise r.error(f"record {index} name {raw!r} contains a NUL", start)
        if cell < MAP_SCRIPT_CELL:
            raise r.error(f"record {index} has cell {cell}", start)
        if cell == MAP_SCRIPT_CELL and any(x.is_map_script for x in records):
            raise r.error(f"record {index} is a second map-script record", start)
        records.append(Record(cell, kind, text))
    r.expect_end(f"{count} records")
    return ObjFile(tuple(records))


def encode(obj: ObjFile) -> bytes:
    """Encode ``obj`` (the inverse of :func:`decode`). Raises ``ValueError`` if invalid."""
    if len(obj.records) > MAX_RECORDS:
        raise ValueError(f"{len(obj.records)} records, at most {MAX_RECORDS}")
    out = bytearray([len(obj.records)])
    for index, r in enumerate(obj.records):
        raw = r.name.encode("ascii")
        if not (MAP_SCRIPT_CELL <= r.cell <= 0x7FFF and 0 <= r.kind <= 0xFF and len(raw) <= 0xFF):
            raise ValueError(f"record {index} out of range: {r}")
        out += struct.pack("<hBB", r.cell, r.kind, len(raw)) + raw
    return bytes(out)


def load(path: str | Path) -> ObjFile:
    path = Path(path)
    return decode(path.read_bytes(), path.name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="mtport.objfile", description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    info = sub.add_parser("info", help="record counts per category for each file")
    info.add_argument("files", nargs="+", type=Path)
    dump = sub.add_parser("dump", help="print every record")
    dump.add_argument("file", type=Path)
    args = parser.parse_args(argv)

    status = 0
    if args.command == "info":
        for path in args.files:
            try:
                obj = load(path)
            except (OSError, FormatError) as exc:
                print(f"{path}: error: {exc}", file=sys.stderr)
                status = 1
                continue
            counts = " ".join(f"{k}={v}" for k, v in obj.counts().items())
            print(f"{path.name}: records={len(obj.records)} {counts}")
    else:
        obj = load(args.file)
        map_path = args.file.with_suffix(".map")
        width = struct.unpack("<h", map_path.read_bytes()[:2])[0] if map_path.exists() else 0
        for r in obj.records:
            where = ""
            if not r.is_map_script and width > 0:
                where = " tile=({}, {}) px=({}, {})".format(*r.tile(width), *r.position(width))
            print(f"cell={r.cell:5d} kind={r.kind:3d} {r.category:13s} name={r.name!r}{where}")
    return status


if __name__ == "__main__":
    sys.exit(main())
