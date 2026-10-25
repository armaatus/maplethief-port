"""The loading and pixel half of :mod:`mtport.mapview`: finding and decoding a map with its
tilesets, collision and objects, the checks, and the layer, collision, backdrop and
marker renderers. The module docstring of ``tools/mtport/mapview.py`` describes the
drawing rules; that module re-exports every name defined here.
"""

from __future__ import annotations

import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport import attfile, mapfile, mapprj, objfile, png
from mtport import datatables as dt
from mtport.bytereader import FormatError

VIEWER = "maps"
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
