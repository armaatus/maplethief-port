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
import json
import re
import sys
import time
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import attfile, mapfile, mapprj, objfile, png
from mtport import datatables as dt
from mtport import htmlpage as hp
from mtport.bytereader import FormatError

VIEWER = "maps"
_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "viewers" / VIEWER

TILE = mapprj.BAKED_TILE_SIZE  # the bake copies 16x16 tiles (0xD9E4)
CELL = attfile.CELL_PIXELS  # an attribute cell is 8x8 pixels
MOB_TABLE = "English/data/mob.dat"
MOB_ID_BASE = objfile.MOB_KIND_BASE - 1  # 0x86289 spawns mob id kind - 59
SPRITE_DIR = "sprite/mob"  # "/sprite/" "mob" "%d" ".sprprj" (0x20005)
SCRIPT_DIR = "script"

# Collision colours (straight-alpha RGBA) and legend text, one per value the shipped files use
# (docs/formats/att.md, "Shipped values"). AIR is left transparent.
A = attfile
ATT_STYLES: dict[int, tuple[tuple[int, int, int, int], str]] = {
    A.AIR: ((0, 0, 0, 0), "air: nothing to stand on (not drawn)"),
    A.FOOTHOLD: ((0, 230, 64, 170), "foothold: one-way floor"),
    A.LADDER: ((255, 200, 0, 150), "ladder or rope"),
    A.FOOTHOLD | A.LADDER: ((0, 200, 255, 190), "foothold + ladder: top step of a ladder"),
    A.LADDER_TOP: ((170, 80, 255, 190), "ladder top: stops climbing up"),
    A.WALL: ((255, 32, 32, 170), "wall: blocks movement"),
    A.FOOTHOLD | A.WALL: ((255, 128, 0, 190), "foothold + wall: wall corner, stair step"),
}
UNKNOWN_COLOR = (255, 0, 255, 220)  # any value the spec doesn't list

# Object marker colours by objfile category.
OBJ_COLORS = {
    "portal": (40, 120, 255, 255),
    "hidden_portal": (150, 150, 150, 255),
    "npc": (0, 190, 80, 255),
    "mob": (230, 30, 30, 255),
    "inert": (255, 255, 255, 255),
}
MARKER_SIZE = 7  # export --objects: a 7x7 square standing on the anchor pixel

_OPAQUE_RUNS = re.compile(rb"[^\x00]+")  # runs of pixels with non-zero alpha (the alpha test)
_OVER_RUNS = re.compile(rb"\xff+|[\x01-\xfe]+")  # opaque runs and partially transparent runs


# --- Loading -------------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class MapId:
    """One map: its area, its number in the area and the file stem (``"%d%d"``)."""

    area: int
    map_no: int
    stem: str


class Tileset:
    """An area's tile sheets, cut like ``CMMapTileSet``, with the blit ops of each tile."""

    def __init__(self, layout: mapprj.TileLayout, sheets: list[png.Image]):
        self.layout = layout
        self.sheets = sheets
        self._ops: dict[int, tuple[tuple[int, int, bytes], ...]] = {}

    def ops(self, index: int) -> tuple[tuple[int, int, bytes], ...]:
        """(row, byte offset in the row, bytes) to copy for tile ``index``: its runs of
        non-zero-alpha pixels. Raises ``IndexError`` for an index outside the sheets."""
        cached = self._ops.get(index)
        if cached is not None:
            return cached
        rect = self.layout.tile_rect(index)
        sheet = self.sheets[rect.sheet]
        w, h = min(rect.w, TILE), min(rect.h, TILE)  # the copy loop is fixed at 16x16
        ops = []
        for row in range(h):
            start = ((rect.y + row) * sheet.width + rect.x) * 4
            line = sheet.rgba[start : start + w * 4]
            for m in _OPAQUE_RUNS.finditer(line[3::4]):
                s, e = m.span()
                ops.append((row, s * 4, line[s * 4 : e * 4]))
        result = tuple(ops)
        self._ops[index] = result
        return result


@dataclass
class LoadedMap:
    """A map and everything it draws from. ``problems`` lists what failed, one line each."""

    id: MapId
    map: mapfile.MapFile | None = None
    att: attfile.AttFile | None = None
    obj: objfile.ObjFile | None = None
    tileset: Tileset | None = None
    tileset_names: tuple[str, ...] = ()
    back_path: str = ""
    back: png.Image | None = None
    objects: list[dict[str, Any]] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)

    @property
    def stem(self) -> str:
        return self.id.stem

    @property
    def pixel_size(self) -> tuple[int, int]:
        assert self.map is not None
        return self.map.width * TILE, self.map.height * TILE

    def problem(self, text: str) -> None:
        self.problems.append(f"map {self.stem}: {text}")


def discover(app: Path) -> tuple[list[MapId], list[str], list[str]]:
    """Every map of ``app/map``, sorted by (area, map); problems; orphan files (an ``.att``
    or ``.obj`` without a ``.map``, such as ``106.att``)."""
    root = app / "map"
    problems: list[str] = []
    if not root.is_dir():
        return [], [f"{root}: missing (no map directory)"], []
    areas = {int(p.stem) for p in root.glob("*.mapprj") if p.stem.isdigit()}
    ids = []
    for path in sorted(root.glob("*.map")):
        try:
            area, map_no = mapprj.split_map_stem(path.stem, areas)
        except ValueError as e:
            problems.append(f"{path.name}: {e}")
            continue
        ids.append(MapId(area, map_no, path.stem))
    stems = {m.stem for m in ids}
    orphans = sorted(
        p.name for ext in ("*.att", "*.obj") for p in root.glob(ext) if p.stem not in stems
    )
    return sorted(ids), problems, orphans


class Assets:
    """Loads and caches what maps share: tilesets per area, backdrops, the mob table."""

    def __init__(self, app: Path):
        self.app = Path(app)
        self._tilesets: dict[int, tuple[Tileset | None, tuple[str, ...], list[str]]] = {}
        self._backs: dict[str, tuple[png.Image | None, str]] = {}
        self._mobs: tuple[dict[int, dt.Mob] | None, str] | None = None

    def tileset(self, area: int) -> tuple[Tileset | None, tuple[str, ...], list[str]]:
        if area not in self._tilesets:
            self._tilesets[area] = self._load_tileset(area)
        return self._tilesets[area]

    def _load_tileset(self, area: int) -> tuple[Tileset | None, tuple[str, ...], list[str]]:
        rel = mapprj.project_path(area)
        try:
            project = mapprj.load(self.app / rel)
        except FileNotFoundError:
            return None, (), [f"missing {rel}"]
        except (OSError, FormatError) as e:
            return None, (), [f"{rel}: {e}"]
        problems = []
        if (project.tile_width, project.tile_height) != (TILE, TILE):
            problems.append(
                f"{rel}: tile size {project.tile_width}x{project.tile_height}; the game's bake "
                f"copies {TILE}x{TILE} tiles"
            )
        sheets = []
        for sheet_rel in project.tileset_paths():
            try:
                sheets.append(png.load(self.app / sheet_rel))
            except FileNotFoundError:
                problems.append(f"missing {sheet_rel} (tile sheet of {rel})")
            except (OSError, FormatError) as e:
                problems.append(f"{sheet_rel}: {e}")
        if len(sheets) != len(project.tilesets):
            return None, project.tilesets, problems
        layout = mapprj.layout(project, [(s.width, s.height) for s in sheets])
        return Tileset(layout, sheets), project.tilesets, problems

    def back(self, rel: str) -> tuple[png.Image | None, str]:
        if rel not in self._backs:
            try:
                self._backs[rel] = (png.load(self.app / rel), "")
            except FileNotFoundError:
                self._backs[rel] = (None, f"missing {rel} (backdrop)")
            except (OSError, FormatError) as e:
                self._backs[rel] = (None, f"{rel}: {e}")
        return self._backs[rel]

    def mobs(self) -> tuple[dict[int, dt.Mob] | None, str]:
        """The English mob table by id, or None and why not."""
        if self._mobs is None:
            try:
                table = dt.parse("mob", (self.app / MOB_TABLE).read_bytes())
                self._mobs = ({r.id: r for r in table.records}, "")
            except FileNotFoundError:
                self._mobs = (None, f"missing {MOB_TABLE} (mob names and sprites)")
            except (OSError, FormatError) as e:
                self._mobs = (None, f"{MOB_TABLE}: {e}")
        return self._mobs

    def exists(self, rel: str) -> bool:
        return (self.app / rel).is_file()

    def script_exists(self, name: str) -> bool:
        """``/script/chapter<c>/<name>.out``, falling back to ``/script/common/`` (0x63191):
        the name has a script in some chapter or in common. Names are untrusted: one that is
        not a plain file name has no script."""
        if not name or name in (".", "..") or any(c in name for c in "/\\:\0"):
            return False
        root = self.app / SCRIPT_DIR
        if (root / "common" / f"{name}.out").is_file():
            return True
        return any((d / f"{name}.out").is_file() for d in root.glob("chapter*"))


def _load_file(lm: LoadedMap, app: Path, ext: str, decode) -> Any:
    rel = f"map/{lm.stem}{ext}"
    try:
        data = (app / rel).read_bytes()
    except FileNotFoundError:
        lm.problem(f"missing {rel}")
        return None
    except OSError as e:
        lm.problem(f"{rel}: {e}")
        return None
    try:
        return decode(data, Path(rel).name)
    except FormatError as e:
        lm.problem(str(e))
        return None


def load_map(assets: Assets, mid: MapId) -> LoadedMap:
    """Load one map and check every asset it draws from; problems go to ``lm.problems``."""
    lm = LoadedMap(mid)
    lm.map = _load_file(lm, assets.app, ".map", mapfile.decode)
    lm.att = _load_file(lm, assets.app, ".att", attfile.decode_any)
    lm.obj = _load_file(lm, assets.app, ".obj", objfile.decode)
    lm.tileset, lm.tileset_names, problems = assets.tileset(mid.area)
    for p in problems:
        lm.problem(p)
    lm.back_path = mapprj.back_path(mid.area)
    lm.back, problem = assets.back(lm.back_path)
    if problem:
        lm.problem(problem)
    if lm.map is None:
        return lm
    _check_tiles(lm)
    _check_att(lm)
    _collect_objects(lm, assets)
    return lm


def _check_tiles(lm: LoadedMap) -> None:
    """One problem per (layer, tile index) that resolves outside the tile sheets."""
    m, ts = lm.map, lm.tileset
    assert m is not None
    if ts is None:
        return
    for layer, cells in enumerate(m.layers):
        bad: dict[int, list[Any]] = {}
        for i, v in enumerate(cells):
            if v < 0:
                continue
            if v in bad:
                bad[v][0] += 1
                continue
            try:
                ts.ops(v)
            except IndexError as e:
                bad[v] = [1, (i % m.width, i // m.width), str(e)]
        for v, (count, (x, y), why) in sorted(bad.items()):
            lm.problem(
                f"layer {layer}: missing tile {v} ({why}): {count} cells, first at tile ({x}, {y})"
            )


def _check_att(lm: LoadedMap) -> None:
    m, att = lm.map, lm.att
    assert m is not None
    if att is None:
        return
    gw, gh = attfile.grid_size(m.width, m.height)
    if len(att.cells) != gw * gh:
        lm.problem(f"{lm.stem}.att has {len(att.cells)} cells, the map needs {gw}x{gh}")
        return
    for value, count in att.histogram().items():
        if value not in ATT_STYLES:
            lm.problem(
                f"{lm.stem}.att: unknown attribute value 0x{value:02X} "
                f"({attfile.flag_names(value)}) in {count} cells"
            )


def _collect_objects(lm: LoadedMap, assets: Assets) -> None:
    m, obj = lm.map, lm.obj
    assert m is not None
    if obj is None:
        return
    if obj.map_script and not assets.script_exists(obj.map_script):
        lm.problem(f"map script {obj.map_script!r}: no script/<chapter|common>/*.out")
    mobs = None
    for n, r in enumerate(obj.objects):
        cat = r.category
        entry: dict[str, Any] = {"index": n, "category": cat, "kind": r.kind, "name": r.name}
        if not 0 <= r.cell < m.cells:
            lm.problem(f"object {n} (kind {r.kind}): cell {r.cell} outside the map")
            continue
        entry["tile"] = list(r.tile(m.width))
        entry["anchor"] = list(r.position(m.width))
        sprite = None
        if cat in ("portal", "hidden_portal"):
            label = f"{'hidden portal' if cat == 'hidden_portal' else 'portal'} {r.kind}"
            sprite = f"{SPRITE_DIR}/{r.kind}.sprprj"
        elif cat == "npc":
            label = f"NPC {r.kind - 4}"
            sprite = f"{SPRITE_DIR}/{r.kind}.sprprj"
        elif cat == "mob":
            mob_id = r.kind - MOB_ID_BASE
            entry["mob_id"] = mob_id
            label = f"mob {mob_id}"
            if mobs is None:
                mobs, problem = assets.mobs()
                if problem:
                    lm.problem(problem)
                    mobs = {}
            mob = mobs.get(mob_id)
            if mob is not None:
                entry["mob_name"] = mob.name
                label += f" {mob.name}"
                sprite = f"{SPRITE_DIR}/{mob.sprite}.sprprj"
            elif mobs:
                lm.problem(f"object {n}: mob id {mob_id} (kind {r.kind}) is not in {MOB_TABLE}")
        else:
            label = f"inert (kind {r.kind})"
        if r.name:
            label += f" {r.name}"
            if not assets.script_exists(r.name):
                lm.problem(f"object {n} ({label}): no script/<chapter|common>/{r.name}.out")
        if sprite is not None:
            entry["sprite"] = sprite
            if not assets.exists(sprite):
                lm.problem(f"object {n} ({label}): missing {sprite}")
        entry["label"] = label
        lm.objects.append(entry)


# --- Rendering -----------------------------------------------------------------------------


def render_tiles(lm: LoadedMap, layers: Iterable[int] | None = None) -> bytearray:
    """The given tile layers (default: all) baked into one RGBA buffer, like ``0xD9E4``:
    lower layers first, a pixel copied only where its alpha is non-zero, no blending. Empty
    cells and tiles outside the sheet (reported by :func:`load_map`) are skipped."""
    m, ts = lm.map, lm.tileset
    assert m is not None
    width, height = lm.pixel_size
    buf = bytearray(width * height * 4)
    if ts is None:
        return buf
    stride = width * 4
    for layer in range(len(m.layers)) if layers is None else layers:
        cells = m.layers[layer]
        for y in range(m.height):
            row_base = y * TILE * stride
            for x in range(m.width):
                v = cells[y * m.width + x]
                if v < 0:
                    continue
                try:
                    ops = ts.ops(v)
                except IndexError:
                    continue
                base = row_base + x * TILE * 4
                for dy, dx, data in ops:
                    o = base + dy * stride + dx
                    buf[o : o + len(data)] = data
    return buf


def att_color(value: int) -> tuple[int, int, int, int]:
    style = ATT_STYLES.get(value)
    return style[0] if style else UNKNOWN_COLOR


def render_att(lm: LoadedMap) -> bytearray:
    """The collision overlay: every 8x8 cell filled with :func:`att_color` of its value."""
    m, att = lm.map, lm.att
    assert m is not None
    width, height = lm.pixel_size
    gw, gh = attfile.grid_size(m.width, m.height)
    if att is None or len(att.cells) != gw * gh:
        return bytearray(width * height * 4)
    cell_px = {v: bytes(att_color(v)) * CELL for v in set(att.cells)}
    out = bytearray()
    for row in att.rows(gw):
        line = b"".join(cell_px[v] for v in row)
        out += line * CELL
    return out


def render_back(lm: LoadedMap) -> bytearray:
    """The backdrop repeated from the map's top-left corner (opaque black without one)."""
    width, height = lm.pixel_size
    back = lm.back
    if back is None:
        return bytearray(b"\0\0\0\xff" * (width * height))
    stride = back.width * 4
    reps = -(-width // back.width)
    rows = [
        (back.rgba[y * stride : (y + 1) * stride] * reps)[: width * 4] for y in range(back.height)
    ]
    out = bytearray()
    for y in range(height):
        out += rows[y % back.height]
    return out


def over(dst: bytearray, src: bytes | bytearray) -> None:
    """Source-over compositing of ``src`` onto ``dst`` (same size, straight alpha). Opaque
    runs are slice copies; only partially transparent pixels are blended."""
    alpha = bytes(src[3::4])
    for m in _OVER_RUNS.finditer(alpha):
        s, e = m.span()
        if alpha[s] == 255:
            dst[s * 4 : e * 4] = src[s * 4 : e * 4]
            continue
        for i in range(s * 4, e * 4, 4):
            sa, da = src[i + 3], dst[i + 3]
            keep = da * (255 - sa)  # destination weight, scaled by 255
            oa255 = sa * 255 + keep
            for c in range(3):
                dst[i + c] = (src[i + c] * sa * 255 + dst[i + c] * keep + oa255 // 2) // oa255
            dst[i + 3] = (oa255 + 127) // 255


def draw_markers(lm: LoadedMap, buf: bytearray) -> None:
    """Objects as 7x7 squares (category colour, black border) standing on their anchor."""
    width, height = lm.pixel_size
    half = MARKER_SIZE // 2
    for o in lm.objects:
        ax, ay = o["anchor"]
        color = bytes(OBJ_COLORS[o["category"]])
        for y in range(ay - MARKER_SIZE + 1, ay + 1):
            for x in range(ax - half, ax + half + 1):
                if 0 <= x < width and 0 <= y < height:
                    edge = y in (ay - MARKER_SIZE + 1, ay) or x in (ax - half, ax + half)
                    i = (y * width + x) * 4
                    buf[i : i + 4] = b"\0\0\0\xff" if edge else color


def export_rgba(lm: LoadedMap, back: bool = True, att: bool = False, objects: bool = False):
    """The full composited map: backdrop, the baked tile layers, then optional overlays."""
    tiles = render_tiles(lm)
    if back:
        out = render_back(lm)
        over(out, tiles)
    else:
        out = tiles
    if att:
        over(out, render_att(lm))
    if objects:
        draw_markers(lm, out)
    return out


# --- Pages ---------------------------------------------------------------------------------


def _encode(lm: LoadedMap, rgba: bytes | bytearray) -> bytes:
    width, height = lm.pixel_size
    return png.encode_rgba(width, height, bytes(rgba))


def _css_rgba(color: tuple[int, int, int, int]) -> str:
    r, g, b, a = color
    return f"rgba({r}, {g}, {b}, {a / 255:.3f})"


def _legend(lm: LoadedMap) -> str:
    if lm.att is None:
        return '<p class="muted">No collision grid.</p>'
    counts = lm.att.histogram()
    rows = []
    for value in sorted(set(counts) | set(ATT_STYLES)):
        color = att_color(value)
        known = value in ATT_STYLES
        meaning = ATT_STYLES[value][1] if known else "unknown value (not in docs/formats/att.md)"
        cls = "" if known else ' class="error"'
        rows.append(
            f'<li{cls}><span class="sw" style="background:{_css_rgba(color)}"></span>'
            f"<code>0x{value:02X}</code> {hp.esc(attfile.flag_names(value))}: {hp.esc(meaning)}"
            f' <span class="muted">({counts.get(value, 0)} cells)</span></li>'
        )
    return '<ul class="legend">' + "".join(rows) + "</ul>"


def _marker(o: dict[str, Any]) -> str:
    x, y = o["anchor"]
    return (
        f'<div class="mk {hp.esc(o["category"])}" style="left:{x}px;top:{y}px"'
        f' title="{hp.esc(o["label"])}"><i></i><b>{hp.esc(o["label"])}</b></div>'
    )


OBJECT_COLUMNS = (
    hp.Column("#", numeric=True),
    hp.Column("Category"),
    hp.Column("Kind", "the .obj kind byte", numeric=True),
    hp.Column("Label"),
    hp.Column("Script", "name: script/chapter<c>/ or script/common/<name>.out"),
    hp.Column("Tile", "x, y in tiles"),
    hp.Column("Anchor", "x, y in pixels: bottom centre of the tile"),
    hp.Column("Sprite"),
)


def _objects_table(lm: LoadedMap) -> str:
    rows = []
    for o in lm.objects:
        rows.append(
            [
                hp.num(o["index"]),
                hp.text(o["category"]),
                hp.num(o["kind"]),
                hp.text(o["label"]),
                hp.text(o["name"]),
                hp.text("{}, {}".format(*o["tile"])),
                hp.text("{}, {}".format(*o["anchor"])),
                hp.text(o.get("sprite", "")),
            ]
        )
    if not rows:
        return '<p class="muted">No objects.</p>'
    return hp.table(OBJECT_COLUMNS, rows)


MAP_CSS = """
.mv-tools { display: flex; flex-wrap: wrap; gap: 4px 14px; align-items: center; margin: 8px 0; }
.mv-tools button { font: inherit; min-width: 2.2em; }
.mv-view { position: relative; overflow: hidden; height: 72vh; border: 1px solid var(--line);
  background: #000; cursor: grab; touch-action: none; outline: none; }
.mv-view.drag { cursor: grabbing; }
.mv-back { position: absolute; inset: 0; background-repeat: repeat; image-rendering: pixelated; }
.mv-stage { position: absolute; left: 0; top: 0; transform-origin: 0 0; }
.mv-stage img { position: absolute; left: 0; top: 0; image-rendering: pixelated;
  pointer-events: none; }
.mv-objs { position: absolute; left: 0; top: 0; }
.mk { position: absolute; transform: scale(var(--inv, 1)); transform-origin: 0 0; }
.mk i { position: absolute; left: -4px; top: -8px; width: 7px; height: 7px;
  border: 1px solid #000; background: var(--c); }
.mk b { position: absolute; left: 6px; top: -10px; white-space: nowrap; font: 600 11px/1.2
  system-ui, sans-serif; color: #fff; background: rgba(0, 0, 0, 0.6); padding: 0 3px;
  border-left: 3px solid var(--c); }
.mk.portal { --c: rgb(40, 120, 255); } .mk.hidden_portal { --c: rgb(150, 150, 150); }
.mk.npc { --c: rgb(0, 190, 80); } .mk.mob { --c: rgb(230, 30, 30); }
.mk.inert { --c: #fff; }
.mv-nolabels .mk b { display: none; }
.legend { list-style: none; padding: 0; columns: 2 22em; }
.sw { display: inline-block; width: 14px; height: 14px; border: 1px solid var(--line);
  vertical-align: -2px; margin-right: 6px; }
kbd { border: 1px solid var(--line); border-radius: 3px; padding: 0 4px; font-size: 12px; }
"""

# Toggles, pan (drag, arrow keys), zoom (wheel, + / -, fit, 1:1) and map cycling (n / p).
# The view state travels in the URL hash, so cycling keeps zoom and toggles. No game pixels are
# touched: only CSS transforms and visibility.
MAP_SCRIPT = """
(function () {
  var view = document.getElementById("view"), stage = document.getElementById("stage");
  var back = document.getElementById("back"), zoomOut = document.getElementById("zoom");
  var W = +stage.dataset.w, H = +stage.dataset.h, BW = +back.dataset.w, BH = +back.dataset.h;
  var ZOOMS = [0.25, 0.5, 0.75, 1, 1.5, 2, 3, 4, 6, 8];
  var z = 1, tx = 0, ty = 0;
  var boxes = Array.prototype.slice.call(document.querySelectorAll("input[data-layer]"));
  function apply() {
    stage.style.transform = "translate(" + tx + "px," + ty + "px) scale(" + z + ")";
    stage.style.setProperty("--inv", 1 / z);
    back.style.backgroundSize = BW * z + "px " + BH * z + "px";
    zoomOut.textContent = Math.round(z * 100) + "%";
  }
  function toggle() {
    boxes.forEach(function (b) {
      var on = b.checked, sel = "[data-layer='" + b.dataset.layer + "']:not(input)";
      Array.prototype.forEach.call(document.querySelectorAll(sel), function (el) {
        if (b.dataset.layer === "labels") view.classList.toggle("mv-nolabels", !on);
        else el.hidden = !on;
      });
    });
  }
  function zoomAt(nz, cx, cy) {
    nz = Math.max(ZOOMS[0], Math.min(ZOOMS[ZOOMS.length - 1], nz));
    tx = cx - (cx - tx) * nz / z; ty = cy - (cy - ty) * nz / z; z = nz; apply();
  }
  function step(d) {
    var i = 0;
    while (i < ZOOMS.length - 1 && ZOOMS[i] < z - 1e-9) i++;
    if (d > 0) i = ZOOMS[i] > z + 1e-9 ? i : i + 1; else i = i - 1;
    i = Math.max(0, Math.min(ZOOMS.length - 1, i));
    zoomAt(ZOOMS[i], view.clientWidth / 2, view.clientHeight / 2);
  }
  function fit() {
    z = Math.min(view.clientWidth / W, view.clientHeight / H);
    tx = (view.clientWidth - W * z) / 2; ty = (view.clientHeight - H * z) / 2; apply();
  }
  function state() {
    var off = boxes.filter(function (b) { return !b.checked; })
      .map(function (b) { return b.dataset.layer; });
    var on = boxes.filter(function (b) { return b.checked && b.dataset.off; })
      .map(function (b) { return b.dataset.layer; });
    return "#z=" + z + "&off=" + off.join(",") + "&on=" + on.join(",");
  }
  function go(rel) {
    var a = document.querySelector("a[rel='" + rel + "']");
    if (a) location.href = a.getAttribute("href") + state();
  }
  var h = {};
  location.hash.slice(1).split("&").forEach(function (kv) {
    var p = kv.split("="); if (p[0]) h[p[0]] = decodeURIComponent(p[1] || "");
  });
  boxes.forEach(function (b) {
    if ((h.off || "").split(",").indexOf(b.dataset.layer) >= 0) b.checked = false;
    if ((h.on || "").split(",").indexOf(b.dataset.layer) >= 0) b.checked = true;
    b.addEventListener("change", toggle);
  });
  toggle();
  if (h.z && +h.z > 0) { z = +h.z; apply(); } else fit();
  Array.prototype.forEach.call(document.querySelectorAll("button[data-zoom]"), function (b) {
    b.addEventListener("click", function () {
      var k = b.dataset.zoom;
      if (k === "fit") fit(); else if (k === "1") zoomAt(1, 0, 0); else step(k === "+" ? 1 : -1);
    });
  });
  view.addEventListener("wheel", function (e) {
    e.preventDefault();
    var r = view.getBoundingClientRect();
    zoomAt(z * (e.deltaY < 0 ? 1.25 : 0.8), e.clientX - r.left, e.clientY - r.top);
  }, { passive: false });
  var drag = null;
  view.addEventListener("pointerdown", function (e) {
    drag = [e.clientX - tx, e.clientY - ty]; view.classList.add("drag");
    view.setPointerCapture(e.pointerId); view.focus();
  });
  view.addEventListener("pointermove", function (e) {
    if (drag) { tx = e.clientX - drag[0]; ty = e.clientY - drag[1]; apply(); }
  });
  function end() { drag = null; view.classList.remove("drag"); }
  view.addEventListener("pointerup", end);
  view.addEventListener("pointercancel", end);
  document.addEventListener("keydown", function (e) {
    if (e.target.tagName === "INPUT" && e.target.type !== "checkbox") return;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    var k = e.key, P = 64;
    if (k === "n" || k === "PageDown") go("next");
    else if (k === "p" || k === "PageUp") go("prev");
    else if (k === "+" || k === "=") step(1);
    else if (k === "-") step(-1);
    else if (k === "0") fit();
    else if (k === "ArrowLeft") { tx += P; apply(); }
    else if (k === "ArrowRight") { tx -= P; apply(); }
    else if (k === "ArrowUp") { ty += P; apply(); }
    else if (k === "ArrowDown") { ty -= P; apply(); }
    else {
      var b = boxes.filter(function (x) { return x.dataset.key === k; })[0];
      if (!b) return;
      b.checked = !b.checked; toggle();
    }
    e.preventDefault();
  });
})();
"""

INDEX_SCRIPT = """
document.addEventListener("keydown", function (e) {
  if (e.target.tagName === "INPUT" || e.metaKey || e.ctrlKey || e.altKey) return;
  var a = document.querySelector(e.key === "n" ? "a[rel='first']" : e.key === "p" ?
    "a[rel='last']" : null);
  if (a) location.href = a.getAttribute("href");
});
"""


def _back_file(lm: LoadedMap) -> str:
    return f"back/{Path(lm.back_path).name}"


def map_page(lm: LoadedMap, prev: str | None, next_: str | None) -> str:
    m = lm.map
    assert m is not None
    width, height = lm.pixel_size
    mid = lm.id
    nav = [hp.link("index.html", "All maps")]
    if prev:
        nav.append(f'<a rel="prev" href="{hp.esc(prev)}.html">&larr; {hp.esc(prev)} (p)</a>')
    if next_:
        nav.append(f'<a rel="next" href="{hp.esc(next_)}.html">{hp.esc(next_)} (n) &rarr;</a>')
    script = lm.obj.map_script if lm.obj else None
    facts = (
        f"Area {mid.area}, map {mid.map_no}: {m.width} &times; {m.height} tiles "
        f"({width} &times; {height} px), {len(m.layers)} tile layers. Tileset "
        f"<code>{hp.esc(', '.join(lm.tileset_names) or '?')}</code>, backdrop "
        f"<code>{hp.esc(lm.back_path)}</code>, map script "
        + (f"<code>{hp.esc(script)}</code>" if script else "none")
        + f", {len(lm.objects)} objects."
    )
    toggles = []

    def toggle(layer: str, label: str, key: str, on: bool = True) -> None:
        checked = " checked" if on else ' data-off="1"'
        toggles.append(
            f'<label><input type="checkbox" data-layer="{layer}" data-key="{key}"{checked}> '
            f"{hp.esc(label)} <kbd>{hp.esc(key)}</kbd></label>"
        )

    toggle("back", "backdrop", "b")
    for n in range(len(m.layers)):
        toggle(f"l{n}", f"layer {n}", str(n + 1))
    toggle("att", "collision", "a", on=False)
    toggle("obj", "objects", "o")
    toggle("labels", "labels", "l")
    imgs = "".join(
        f'<img data-layer="l{n}" src="{lm.stem}/layer{n}.png" width="{width}" height="{height}"'
        f' alt="tile layer {n}">'
        for n in range(len(m.layers))
    )
    imgs += (
        f'<img data-layer="att" src="{lm.stem}/att.png" width="{width}" height="{height}"'
        ' alt="collision overlay" hidden>'
    )
    markers = "".join(_marker(o) for o in lm.objects)
    back_w, back_h = (lm.back.width, lm.back.height) if lm.back else (0, 0)
    back_style = f' style="background-image:url({_back_file(lm)})"' if lm.back else ""
    problems = (
        "<ul>" + "".join(f'<li class="error">{hp.esc(p)}</li>' for p in lm.problems) + "</ul>"
        if lm.problems
        else "<p>No problems.</p>"
    )
    body = (
        f"<style>{MAP_CSS}</style>\n"
        f"<p>{facts}</p>\n"
        '<div class="mv-tools">' + " ".join(toggles) + " <span>zoom "
        '<button data-zoom="-" title="zoom out (-)">&minus;</button> <span id="zoom">100%</span> '
        '<button data-zoom="+" title="zoom in (+)">+</button> '
        '<button data-zoom="fit" title="fit (0)">fit</button> '
        '<button data-zoom="1">1:1</button></span></div>\n'
        '<div class="mv-view" id="view" tabindex="0">'
        f'<div class="mv-back" id="back" data-layer="back" data-w="{back_w}" data-h="{back_h}"'
        f"{back_style}></div>"
        f'<div class="mv-stage" id="stage" data-w="{width}" data-h="{height}"'
        f' style="width:{width}px;height:{height}px">{imgs}'
        f'<div class="mv-objs" data-layer="obj">{markers}</div></div></div>\n'
        '<p class="muted">Drag or arrow keys to pan, wheel or <kbd>+</kbd>/<kbd>-</kbd> to zoom, '
        "<kbd>0</kbd> to fit, <kbd>n</kbd>/<kbd>p</kbd> for the next/previous map. The backdrop "
        "stays put like in the game (drawn at screen 0, 0, never scrolled), repeated past the "
        "480 &times; 320 iPhone screen. Tile layers are drawn in order with an alpha test, as "
        f'the game bakes them. <a href="{lm.stem}/objects.json">objects.json</a></p>\n'
        "<h2>Collision</h2>\n"
        + _legend(lm)
        + "\n<h2>Objects</h2>\n"
        + _objects_table(lm)
        + "\n<h2>Problems</h2>\n"
        + problems
        + f"\n<script>{MAP_SCRIPT}</script>\n"
    )
    return hp.page(f"Map {lm.stem}", body, " · ".join(nav))


INDEX_COLUMNS = (
    hp.Column("Map", "file stem: map/<stem>.map"),
    hp.Column("Area", numeric=True),
    hp.Column("No.", "map number in the area", numeric=True),
    hp.Column("Tileset"),
    hp.Column("Tiles", "width x height in tiles"),
    hp.Column("Layers", numeric=True),
    hp.Column("Portals", numeric=True),
    hp.Column("NPCs", numeric=True),
    hp.Column("Mobs", "mob spawn points", numeric=True),
    hp.Column("Map script"),
    hp.Column("Problems", numeric=True),
)


def index_page(maps: list[LoadedMap], problems: list[str], orphans: list[str]) -> str:
    rows = []
    for lm in maps:
        cats = [o["category"] for o in lm.objects]
        m = lm.map
        rows.append(
            [
                hp.Cell(hp.link(f"{lm.stem}.html", lm.stem) if m else hp.esc(lm.stem), lm.stem),
                hp.num(lm.id.area),
                hp.num(lm.id.map_no),
                hp.text(", ".join(lm.tileset_names)),
                hp.text(f"{m.width} x {m.height}" if m else "?"),
                hp.num(len(m.layers) if m else 0),
                hp.num(cats.count("portal") + cats.count("hidden_portal")),
                hp.num(cats.count("npc")),
                hp.num(cats.count("mob")),
                hp.text((lm.obj.map_script or "") if lm.obj else ""),
                hp.num(len(lm.problems)),
            ]
        )
    problems = problems + [p for lm in maps for p in lm.problems]
    pages = [lm for lm in maps if lm.map is not None]
    first = (
        f'<a rel="first" href="{pages[0].stem}.html">first map (n)</a>, '
        f'<a rel="last" href="{pages[-1].stem}.html">last map (p)</a>'
        if pages
        else ""
    )
    summary = f"{len(maps)} maps, {len(problems)} problems. " + (
        "No problems." if not problems else '<span class="error">Listed below.</span>'
    )
    body = (
        f"<p>{summary} Open a map to see its tile layers, backdrop, collision grid and objects; "
        f"on a map page <kbd>n</kbd>/<kbd>p</kbd> go to the next/previous map. {first}.</p>\n"
        + (
            "<ul>" + "".join(f'<li class="error">{hp.esc(p)}</li>' for p in problems) + "</ul>"
            if problems
            else ""
        )
        + (
            f'<p class="muted">Files with no .map (not maps): {hp.esc(", ".join(orphans))}.</p>\n'
            if orphans
            else ""
        )
        + hp.table(INDEX_COLUMNS, rows, [lm.stem for lm in maps])
        + f"<style>kbd {{ border: 1px solid var(--line); padding: 0 4px; }}</style>"
        f"<script>{INDEX_SCRIPT}</script>\n"
    )
    return hp.page("Maps", body)


# --- Commands ------------------------------------------------------------------------------


def _select(ids: list[MapId], maps: str | None) -> tuple[list[MapId], list[str]]:
    if not maps:
        return ids, []
    wanted = [s.strip() for s in maps.split(",") if s.strip()]
    by_stem = {m.stem: m for m in ids}
    missing = [f"no map {s!r} in map/" for s in wanted if s not in by_stem]
    return [by_stem[s] for s in sorted(set(wanted) & set(by_stem), key=lambda s: by_stem[s])], (
        missing
    )


def load_all(app: Path, maps: str | None = None) -> tuple[list[LoadedMap], list[str], list[str]]:
    """Load the selected maps (default: all). Returns the maps, global problems, orphans."""
    ids, problems, orphans = discover(Path(app))
    ids, missing = _select(ids, maps)
    assets = Assets(Path(app))
    return [load_map(assets, mid) for mid in ids], problems + missing, orphans


def render(app: Path, maps: str | None = None) -> tuple[dict[str, bytes], list[str]]:
    """Every file of the viewer directory, and every problem (one line each)."""
    loaded, general, orphans = load_all(app, maps)
    problems = list(general)
    files: dict[str, bytes] = {}
    for lm in loaded:
        problems += lm.problems
    pages = [lm for lm in loaded if lm.map is not None]  # a map that didn't parse has no page
    for n, lm in enumerate(pages):
        assert lm.map is not None
        for layer in range(len(lm.map.layers)):
            files[f"{lm.stem}/layer{layer}.png"] = _encode(lm, render_tiles(lm, [layer]))
        files[f"{lm.stem}/att.png"] = _encode(lm, render_att(lm))
        files[f"{lm.stem}/objects.json"] = (
            json.dumps(lm.objects, indent=1, sort_keys=True, ensure_ascii=False) + "\n"
        ).encode("utf-8")
        if lm.back is not None and _back_file(lm) not in files:
            files[_back_file(lm)] = png.encode_rgba(lm.back.width, lm.back.height, lm.back.rgba)
        prev = pages[n - 1].stem if n > 0 else None
        next_ = pages[n + 1].stem if n + 1 < len(pages) else None
        files[f"{lm.stem}.html"] = map_page(lm, prev, next_).encode("utf-8")
    files["index.html"] = index_page(loaded, general, orphans).encode("utf-8")
    return files, problems


def batch(app: Path, maps: str | None = None) -> tuple[int, list[str]]:
    """Load and render every selected map in memory. Returns (maps rendered, problems)."""
    loaded, problems, _ = load_all(app, maps)
    for lm in loaded:
        problems += lm.problems
        if lm.map is None:
            continue
        for layer in range(len(lm.map.layers)):
            render_tiles(lm, [layer])
        export_rgba(lm, back=True, att=True, objects=True)
    return len(loaded), problems


def export(app: Path, stem: str, out: Path, **kw: bool) -> list[str]:
    """Write the composited map ``stem`` to ``out``. Returns the map's problems."""
    loaded, problems, _ = load_all(app, stem)
    if not loaded:
        return problems
    lm = loaded[0]
    if lm.map is None:
        return problems + lm.problems
    width, height = lm.pixel_size
    Path(out).write_bytes(png.encode_rgba(width, height, bytes(export_rgba(lm, **kw))))
    return problems + lm.problems


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
