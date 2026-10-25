"""Map viewer: render the 90 maps with their layers, collision and objects (issue #26, ADR 0010).

Every map of ``map/`` (docs/formats/map.md) is drawn from the original files: the tile layers of
its ``.map`` on the area's tile sheet (``.mapprj``, docs/formats/mapprj.md), the area backdrop
from ``map/back/``, the collision grid of its ``.att`` (docs/formats/att.md) and the objects of
its ``.obj`` (docs/formats/obj.md). Nothing is decoded here; the parsers do that, and
:mod:`mtport.png` decodes and encodes the pixels.

How a map is drawn follows the game:

- **Tile layers** are composed like the bake in the ``.map`` loader (``0xD9E4``-``0xDE1C``):
  layer 0 first, each 16x16 tile copied to ``(16x, 16y)``, and a source pixel is copied only if
  its alpha is non-zero, overwriting the destination (an alpha test, no blending). Opaque runs
  of each tile row are precomputed, so a tile costs one slice assignment per run.
- **The backdrop** ``map/back/<n>.png`` (480x320, the area remap of ``0x86109``) is drawn at
  screen (0, 0) before the map and never scrolls (``0x884C4``, then ``CMMap::Draw`` ``0xD225``).
  The pages pin it to the view and repeat it to fill a window larger than the iPhone screen.
  ``export`` repeats it from the map's top-left corner, which is exactly what the game shows
  while the camera is at (0, 0).
- **Collision**: one 8x8 cell per ``.att`` byte, coloured by its value (:data:`ATT_STYLES`).
  A value the spec doesn't know is drawn in :data:`UNKNOWN_COLOR` and listed as unknown.
- **Objects** are markers at their anchor, the bottom-centre pixel of their tile
  (``0xBD0D``/``0xBD41``), labelled with category, kind, script name and, for mob spawns, the
  ``mob.dat`` name (mob id = kind - 59, ``0x86289``).

Output (``assets-out/viewers/maps/``, owned by this command alone)::

    index.html                       the map picker: every map with its size, layers, objects
    <stem>.html                      one page per map (stem = the file name, e.g. 00, 105)
    <stem>/layer<N>.png              tile layer N alone, transparent where empty
    <stem>/att.png                   the collision overlay
    <stem>/objects.json              the objects with positions and labels
    back/<n>.png                     the backdrops, re-encoded from the decoded pixels

A page stacks the PNGs with CSS. Its small inline script only toggles layers, pans, zooms and
moves to the next or previous map; every pixel comes from this module.

Command line (``PYTHONPATH=tools python3 -m mtport.mapview ...``)::

    build  [--app DIR] [--out DIR] [--maps 00,105]   write the pages
    check  [--app DIR] [--out DIR] [--maps ...]      compare the pages on disk with a fresh build
    export MAP --out FILE.png [--app DIR] [--no-back] [--att] [--objects]
    batch  [--app DIR] [--maps ...]                  load and render every map, report problems

Every command prints one line per error (a parse error, a missing file, a tile outside its
sheet, an unknown collision value, ...) and exits non-zero if there was any. Output is
deterministic.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from mtport import htmlpage as hp
from mtport.mapview_page import (
    INDEX_COLUMNS,
    INDEX_SCRIPT,
    MAP_CSS,
    MAP_SCRIPT,
    OBJECT_COLUMNS,
    _legend,
    batch,
    export,
    index_page,
    load_all,
    map_page,
    render,
)
from mtport.mapview_render import (
    ATT_STYLES,
    CELL,
    MARKER_SIZE,
    MOB_ID_BASE,
    MOB_TABLE,
    OBJ_COLORS,
    SCRIPT_DIR,
    SPRITE_DIR,
    TILE,
    UNKNOWN_COLOR,
    VIEWER,
    A,
    Assets,
    LoadedMap,
    MapId,
    Tileset,
    att_color,
    discover,
    draw_markers,
    export_rgba,
    load_map,
    over,
    render_att,
    render_back,
    render_tiles,
)

# mtport.mapview is the public face of the loader and renderer (mapview_render) and the
# pages (mapview_page): it re-exports their names and keeps the command line.
__all__ = [
    "A",
    "ATT_STYLES",
    "Assets",
    "CELL",
    "INDEX_COLUMNS",
    "INDEX_SCRIPT",
    "LoadedMap",
    "MAP_CSS",
    "MAP_SCRIPT",
    "MARKER_SIZE",
    "MOB_ID_BASE",
    "MOB_TABLE",
    "MapId",
    "OBJECT_COLUMNS",
    "OBJ_COLORS",
    "SCRIPT_DIR",
    "SPRITE_DIR",
    "TILE",
    "Tileset",
    "UNKNOWN_COLOR",
    "VIEWER",
    "_legend",
    "att_color",
    "batch",
    "discover",
    "draw_markers",
    "export",
    "export_rgba",
    "index_page",
    "load_all",
    "load_map",
    "map_page",
    "over",
    "render",
    "render_att",
    "render_back",
    "render_tiles",
]


_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "viewers" / VIEWER


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.mapview", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    cmds = {
        "build": "write the pages",
        "check": "compare the pages on disk with a fresh build",
        "batch": "load and render every map, print problems",
        "export": "write one composited map as a PNG",
    }
    for cmd, help_ in cmds.items():
        p = sub.add_parser(cmd, help=help_)
        p.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
        if cmd == "export":
            p.add_argument("map", help="map file stem, e.g. 00 or 105")
            p.add_argument("--out", type=Path, required=True, help="PNG file to write")
            p.add_argument("--no-back", action="store_true", help="leave out the backdrop")
            p.add_argument("--att", action="store_true", help="draw the collision overlay")
            p.add_argument("--objects", action="store_true", help="draw object markers")
            continue
        if cmd != "batch":
            p.add_argument("--out", type=Path, default=DEFAULT_OUT)
        p.add_argument("--maps", help="comma-separated map stems (default: all)")
    args = parser.parse_args(argv)
    if not args.app.is_dir():
        print(f"error: {args.app}: not a directory (run scripts/bootstrap)", file=sys.stderr)
        return 1
    start = time.monotonic()
    try:
        if args.cmd == "build":
            files, errors = render(args.app, args.maps)
            hp.write_dir(args.out, files, VIEWER)
            n = sum(1 for f in files if f.endswith(".html")) - 1
            summary = f"{n} maps, {len(files)} files -> {args.out}"
        elif args.cmd == "check":
            files, errors = render(args.app, args.maps)
            errors = errors + hp.diff_dir(args.out, files)
            summary = f"checked {args.out}"
        elif args.cmd == "batch":
            n, errors = batch(args.app, args.maps)
            summary = f"{n} maps loaded and rendered"
        else:
            errors = export(
                args.app,
                args.map,
                args.out,
                back=not args.no_back,
                att=args.att,
                objects=args.objects,
            )
            summary = f"map {args.map} -> {args.out}"
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{summary}, {len(errors)} errors ({time.monotonic() - start:.1f} s)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
