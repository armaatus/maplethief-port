"""Compendium images: every entity's picture, exported for the site (issue #216, ADR 0123).

Everything is drawn from the original files with the viewers' renderers
(:mod:`mtport.spriteview_render`, :mod:`mtport.mapview_render`) and the icon packs
(:mod:`mtport.pngdat`), and encoded with the stdlib PNG codec (:mod:`mtport.png`). Lossless,
deterministic: the same ``.app`` gives the same bytes.

Output (``build/compendium/site/img/``, owned by this module alone; paths relative to it)::

    monster/<id>/<action>.png        first frame of a mob action (still)
    monster/<id>/<action>.anim.png   the action as an animated PNG (actions of 2+ frames)
    npc/<id>.png, npc/<id>.anim.png  the NPC's sprite, still and animated
    item/<table>/<id>.png, @2x.png   inventory icon (table equip or etc), 1x and 2x
    item/equip/<id>.worn.png         the hero standing, wearing the item (items with a look)
    skill/<id>.png, @2x.png          skill icon
    map/<area>-<map>.png, @2x.png    the map (backdrop and tile layers), 1x and 2x
    map/<area>-<map>.svg             overlay in map pixels: portals, NPCs, spawn points
    map/<area>-<map>.thumb.png       a quarter-size thumbnail of the map (box filter)
    world/worldmap.png               the game's world-map art (key ``world:map``, not an entity)
    manifest.json                    every file above, by entity, with sizes and alt text

Ids are the knowledge base's: monster ``<mob id>``, npc ``<npc id>``, skill ``<id>``, item
``equip:<id>`` / ``etc:<id>``, map ``<area>-<map>``. ``manifest.json`` is
``{"schema": 1, "images": {"<kind>:<id>": [entry, ...]}, "missing": {"<kind>:<id>": why}}``;
an entry has ``path``, ``w``, ``h``, ``alt`` and ``role`` (``still``, ``anim``, ``icon``,
``icon2x``, ``render``, ``render2x``, ``overlay``, ``thumb`` or ``worn``), plus ``action`` and
``anchor``
on sprites and ``delays_ms`` on animations.

How each picture follows the game:

- **Sprites** are drawn as the sprite viewer draws them, facing as drawn (left), each action's
  frames on one canvas cropped to the union of their pixels, so the anchor stays put. A mob's
  project is ``mob.dat``'s ``sprite`` column; an NPC's is ``sprite/mob/<id + 4>``
  (``mtport.objfile``). Mob actions are named from the code that sets them:
  ``CMob::Flush`` ``0x7458D`` action 0 (``move``), ``CMob::TakeHit`` ``0x73715`` action 1
  (``hit``, a hit the mob survives), ``CMob::Die`` ``0x73349`` action 2 (``die``); a boss's
  further actions are ``action<n>``.
- **Animations** are APNG: every frame whole (blend ``source``, dispose ``none``), looping, each
  frame shown ``ticks x 50 ms`` (``sprprj.frame_ticks``, the default game speed of ADR 0087).
- **Icons** are image ``icon`` of the table's pack (``tablebrowser.ICONS``), decoded to RGBA
  (transparent where the PNG is), 2x by nearest neighbour.
- **Worn equipment** is ``hero/0`` action 2 (standing, ``CHero::SetAction``) with the item's
  images swapped in as ``0x62571`` does: by slot (hat, weapon, overall) it loads
  ``/png/cab/cab_<look>_<j>.png``, ``/png/arm/arm_<look>_<j>.png`` or
  ``/png/costume/costume_<look>_<j>.png`` over the ``hero/0`` image listed at entry ``j`` of
  the tables at ``0xEABE0``, ``0xEAC00`` and ``0xEAC90`` (:data:`WORN`). Shoes and earrings
  (``look = -1``) are never drawn on the hero.
- **Maps** are ``mapview export``: the backdrop repeated from the top-left, the tile layers
  baked over it. The overlay is our own drawing in map pixels, so it fits the 1x and the 2x
  image: portals with their numbers, NPCs with their names and each spawn point as the
  monster's still standing on it (embedded once per map, so the SVG works on its own).

Command line (``PYTHONPATH=tools python3 -m mtport.compendium.images ...``)::

    build [--app DIR] [--out DIR]                  write every image and the manifest
    check [--app DIR] [--out DIR] [--kb FILE]      the manifest against the files and entities

``check`` lists every file the manifest lacks or names wrongly (size, frame delays), every
entity without its image and no reason, and prints the reasons. It exits non-zero on any error.
"""

from __future__ import annotations

import argparse
import base64
import json
import shutil
import struct
import sys
import zlib
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from xml.sax.saxutils import escape

from mtport import cgbi, objfile, png, pngdat, sprprj
from mtport import datatables as dt
from mtport import mapview_render as mvr
from mtport import spriteview_render as svr
from mtport.bytereader import FormatError

SCHEMA = 1
MANIFEST = "manifest.json"
TICK_MS = svr.DEFAULT_TICK_MS  # one frame-delay unit at the default game speed (ADR 0087)
LANG = "English"
NPC_SPRITE_BASE = objfile.NPC_KINDS.start  # NPC id n draws sprite/mob/<n + 4>.sprprj
MOB_ACTIONS = {0: "move", 1: "hit", 2: "die"}  # CMob::Flush, CMob::TakeHit, CMob::Die
HERO = svr.ProjectId("hero", 0)
HERO_STAND = 2  # CHero::SetAction 0x60551: action 2 is standing

# Pack and table per icon kind (the table browser's mapping, docs/formats/png-dat.md).
ICON_PACKS = {
    "equip": "png/item/equip.dat",
    "etc": "png/item/etc.dat",
    "skill": "png/skill/skillico.dat",
}
ITEM_TABLES = {"equip": "item_equip", "etc": "item_etc"}


@dataclass(frozen=True)
class Swap:
    """How ``0x62571`` dresses ``hero/0`` for one equipment slot."""

    file: str  # "%s%d%s%d%s": prefix, look, "_", j, ".png"
    images: tuple[int, ...]  # hero/0 image replaced by file j (-1: none)
    table: int  # where the image indexes live (row 0 is hero/0's; the loop stops after it)


WORN = {
    0: Swap("png/cab/cab_{look}_{j}.png", (29, 30), 0xEABE0),
    1: Swap("png/arm/arm_{look}_{j}.png", (32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 31), 0xEAC00),
    2: Swap("png/costume/costume_{look}_{j}.png", tuple(range(1, 14)), 0xEAC90),
}
SLOT_NAMES = {0: "hat", 1: "weapon", 2: "overall", 3: "earrings", 4: "shoes"}

# What every entity of a kind needs: the role that must be in its manifest entries.
REQUIRED = {"monster": "still", "npc": "still", "item": "icon", "skill": "icon", "map": "render"}

_REPO = Path(__file__).resolve().parents[3]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "build" / "compendium" / "site" / "img"


# --- Animated PNG -----------------------------------------------------------------------------


def encode_apng(
    width: int, height: int, frames: Sequence[bytes], delays_ms: Sequence[int], plays: int = 0
) -> bytes:
    """An animated PNG of whole RGBA frames: frame 0 is also the default image (``IDAT``), so a
    decoder without APNG shows it. Each frame lasts ``delays_ms[i]`` ms; ``plays`` 0 loops."""
    if not frames or len(frames) != len(delays_ms):
        raise ValueError(f"{len(frames)} frames with {len(delays_ms)} delays")
    for rgba in frames:
        if len(rgba) != width * height * 4:
            raise ValueError(f"a {width}x{height} frame needs {width * height * 4} bytes")
    if any(not 0 <= d <= 0xFFFF for d in delays_ms):
        raise ValueError(f"delays {list(delays_ms)} outside 0..65535 ms")
    ihdr = struct.pack(">IIBBBBB", width, height, 8, png.RGBA, 0, 0, 0)
    out = [png.SIGNATURE, png.make_chunk(b"IHDR", ihdr)]
    out.append(png.make_chunk(b"acTL", struct.pack(">II", len(frames), plays)))
    seq = 0
    for i, (rgba, delay) in enumerate(zip(frames, delays_ms, strict=True)):
        fctl = struct.pack(">IIIIIHHBB", seq, width, height, 0, 0, delay, 1000, 0, 0)
        out.append(png.make_chunk(b"fcTL", fctl))
        seq += 1
        data = zlib.compress(png.filter_rows(bytes(rgba), height, width * 4, 4), png.ZLIB_LEVEL)
        if i == 0:
            out.append(png.make_chunk(b"IDAT", data))
        else:
            out.append(png.make_chunk(b"fdAT", struct.pack(">I", seq) + data))
            seq += 1
    out.append(png.make_chunk(b"IEND", b""))
    return b"".join(out)


@dataclass(frozen=True)
class PngSummary:
    width: int
    height: int
    frames: int  # 1 for a still PNG
    delays_ms: tuple[int, ...]  # () for a still PNG; else one per frame, in ms (rounded)
    plays: int = 0


def read_summary(data: bytes, name: str = "<png>") -> PngSummary:
    """Size, frame count and frame delays of a PNG or APNG, every chunk CRC checked."""
    data = bytes(data)
    if not data.startswith(png.SIGNATURE):
        raise FormatError(f"{name}: not a PNG")
    pos, width, height, frames, plays = len(png.SIGNATURE), None, None, None, 0
    delays: list[int] = []
    while True:
        if pos + 8 > len(data):
            raise FormatError(f"{name}: truncated at 0x{pos:X}")
        n, ctype = struct.unpack(">I4s", data[pos : pos + 8])
        end = pos + 12 + n
        if end > len(data):
            raise FormatError(f"{name}: chunk {ctype!r} runs past the end")
        payload = data[pos + 8 : pos + 8 + n]
        (crc,) = struct.unpack(">I", data[pos + 8 + n : end])
        if zlib.crc32(ctype + payload) != crc:
            raise FormatError(f"{name}: bad CRC in {ctype!r}")
        if ctype == b"IHDR":
            width, height = struct.unpack(">II", payload[:8])
        elif ctype == b"acTL":
            frames, plays = struct.unpack(">II", payload)
        elif ctype == b"fcTL":
            num, den = struct.unpack(">HH", payload[20:24])
            delays.append(round(num * 1000 / (den or 100)))
        pos = end
        if ctype == b"IEND":
            break
    if width is None or height is None:
        raise FormatError(f"{name}: no IHDR")
    if frames is None:
        return PngSummary(width, height, 1, ())
    if frames != len(delays):
        raise FormatError(f"{name}: acTL says {frames} frames, {len(delays)} fcTL chunks")
    return PngSummary(width, height, frames, tuple(delays), plays)


# --- Pixels -----------------------------------------------------------------------------------


def upscale(img: png.Image, k: int) -> png.Image:
    """``img`` enlarged ``k`` times by nearest neighbour (fast enough for whole maps)."""
    if k == 1:
        return img
    stride = img.width * 4
    out = bytearray()
    for y in range(img.height):
        row = img.rgba[y * stride : (y + 1) * stride]
        wide = bytearray(stride * k)
        for c in range(4):
            plane = row[c::4]
            for r in range(k):
                wide[4 * r + c :: 4 * k] = plane
        out += bytes(wide) * k
    return png.Image(img.width * k, img.height * k, bytes(out))


def downscale(img: png.Image, k: int) -> png.Image:
    """``img`` shrunk ``k`` times by averaging each ``k`` by ``k`` block (a box filter; edge
    pixels that do not fill a block are dropped). For thumbnails of opaque maps: the channels
    are averaged as stored, without weighting by alpha."""
    if k == 1:
        return img
    w, h = img.width // k, img.height // k
    stride = img.width * 4
    n = k * k
    out = bytearray(w * h * 4)
    for y in range(h):
        sums: list[list[int]] | None = None
        for r in range(k):
            row = img.rgba[(y * k + r) * stride : (y * k + r) * stride + w * k * 4]
            planes = []
            for c in range(4):
                plane = row[c::4]
                parts = [plane[i::k] for i in range(k)]
                planes.append([sum(t) for t in zip(*parts, strict=True)])
            if sums is None:
                sums = planes
            else:
                sums = [
                    [a + b for a, b in zip(s, p, strict=True)]
                    for s, p in zip(sums, planes, strict=True)
                ]
        assert sums is not None
        base = y * w * 4
        for c in range(4):
            out[base + c : base + w * 4 : 4] = bytes((v + n // 2) // n for v in sums[c])
    return png.Image(w, h, bytes(out))


def crop(rgba: bytes, width: int, box: svr.Box) -> bytes:
    w = box.x1 - box.x0
    return b"".join(
        rgba[(y * width + box.x0) * 4 : (y * width + box.x0 + w) * 4] for y in range(box.y0, box.y1)
    )


@dataclass(frozen=True)
class Clip:
    """One sprite action ready to save: cropped frames, their delays, the anchor."""

    width: int
    height: int
    anchor: tuple[int, int]
    frames: tuple[bytes, ...]
    delays_ms: tuple[int, ...]

    def still(self) -> bytes:
        return png.encode_rgba(self.width, self.height, self.frames[0])

    def anim(self) -> bytes:
        return encode_apng(self.width, self.height, self.frames, self.delays_ms)


def clip(action: sprprj.Action, images: Sequence[png.Image]) -> Clip | None:
    """``action`` facing as drawn, cropped to the union of its frames' pixels (``None`` if no
    frame draws a pixel)."""
    render = svr.render_action(action, images)
    w, h = render.width, render.height
    box = svr.union(svr.visible_box(f[3::4], w, h) for f in render.frames[False])
    if box is None:
        return None
    frames = tuple(crop(f, w, box) for f in render.frames[False])
    return Clip(
        box.x1 - box.x0,
        box.y1 - box.y0,
        (render.anchor_x - box.x0, render.anchor_y - box.y0),
        frames,
        tuple(sprprj.frame_ticks(f) * TICK_MS for f in action.frames),
    )


# --- Entities ---------------------------------------------------------------------------------


@dataclass
class Sources:
    """What the images are drawn from: the English tables and the maps."""

    app: Path
    tables: dict[str, dt.Table] = field(default_factory=dict)
    maps: list[mvr.MapId] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)

    def rows(self, table: str) -> tuple[Any, ...]:
        t = self.tables.get(table)
        return tuple(t.records) if t else ()


def load_sources(app: Path) -> Sources:
    src = Sources(Path(app))
    d = src.app / LANG / "data"
    for name in ("mob", "npc", "item_equip", "item_etc", "skill"):
        try:
            src.tables[name] = dt.parse(name, (d / f"{name}.dat").read_bytes())
        except (OSError, FormatError) as e:
            src.errors.append(f"{LANG}/data/{name}.dat: {e}")
    src.maps, problems, _orphans = mvr.discover(src.app)
    src.errors += problems
    return src


def display_name(name: str) -> str:
    """A table name as text: ``$`` marks a line break."""
    return " ".join(name.replace("$", " ").split())


def map_id(mid: mvr.MapId) -> str:
    return f"{mid.area}-{mid.map_no}"


def entities(src: Sources) -> dict[str, dict[str, dict[str, Any]]]:
    """``{kind: {id: entity}}`` from the tables and maps, the shape of the knowledge base's
    ``load_kb`` (only the kinds and ids matter to :func:`check`)."""
    out: dict[str, dict[str, dict[str, Any]]] = {k: {} for k in REQUIRED}
    for r in src.rows("mob"):
        out["monster"][str(r.id)] = {"name": display_name(r.name)}
    for r in src.rows("npc"):
        out["npc"][str(r.id)] = {"name": display_name(r.name)}
    for short, table in ITEM_TABLES.items():
        for r in src.rows(table):
            out["item"][f"{short}:{r.id}"] = {"name": display_name(r.name)}
    for r in src.rows("skill"):
        out["skill"][str(r.id)] = {"name": display_name(r.name)}
    for mid in src.maps:
        out["map"][map_id(mid)] = {"name": f"Map {map_id(mid)}"}
    return out


# --- Export -----------------------------------------------------------------------------------


@dataclass
class Export:
    """Files by path, manifest entries by entity key, reasons for missing images."""

    files: dict[str, bytes] = field(default_factory=dict)
    images: dict[str, list[dict[str, Any]]] = field(default_factory=dict)
    missing: dict[str, str] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)

    def add(self, key: str, path: str, data: bytes, w: int, h: int, alt: str, role: str, **extra):
        if path in self.files:
            raise ValueError(f"{path}: written twice")
        self.files[path] = data
        self.images.setdefault(key, []).append(
            {"path": path, "w": w, "h": h, "alt": alt, "role": role, **extra}
        )

    def manifest(self) -> dict[str, Any]:
        images = {k: sorted(v, key=lambda e: e["path"]) for k, v in sorted(self.images.items())}
        return {"schema": SCHEMA, "images": images, "missing": dict(sorted(self.missing.items()))}


class Sprites:
    """Sprite projects of the ``.app``, with their decoded images shared per folder."""

    def __init__(self, app: Path):
        self.app = Path(app)
        self.cache: svr.ImageCache = {}

    def load(self, pid: svr.ProjectId) -> tuple[sprprj.SpriteProject | None, list, str]:
        """(project, images, problem): the problem is "" when every image loaded."""
        if not pid.path(self.app).is_file():
            return None, [], f"sprite/{pid.key}{sprprj.SUFFIX} missing"
        loaded = svr.load_project(self.app, pid, self.cache)
        if loaded.project is None:
            return None, [], "; ".join(loaded.errors)
        return loaded.project, loaded.images, "; ".join(loaded.errors)


def _action_clip(project, images, a: int) -> tuple[Clip | None, str]:
    action = project.actions[a]
    if not action.frames:
        return None, f"action {a} has no frames"
    used = {la.image for fr in action.frames for la in fr.layers}
    if any(images[i] is None for i in used):
        return None, f"action {a} uses an image that failed to load"
    c = clip(action, images)
    return (c, "") if c is not None else (None, f"action {a} draws no pixel")


def _sprite_entries(
    ex: Export, key: str, stem: str, clips: list[tuple[str, Clip]], label: str, single: bool
) -> None:
    for name, c in clips:
        base = stem if single else f"{stem}/{name}"
        what = label if single else f"{label}, {name}"
        anchor = list(c.anchor)
        ex.add(
            key, f"{base}.png", c.still(), c.width, c.height, f"{what} (first frame)", "still",
            action=name, anchor=anchor,
        )  # fmt: skip
        if len(c.frames) > 1:
            ex.add(
                key, f"{base}.anim.png", c.anim(), c.width, c.height, f"{what} (animation)",
                "anim", action=name, anchor=anchor, delays_ms=list(c.delays_ms),
            )  # fmt: skip


def export_monsters(ex: Export, src: Sources, sprites: Sprites) -> dict[int, Clip]:
    """Every mob action; returns each mob's first still clip (for the map overlays)."""
    firsts: dict[int, Clip] = {}
    for r in src.rows("mob"):
        key, name = f"monster:{r.id}", display_name(r.name)
        project, images, problem = sprites.load(svr.ProjectId("mob", r.sprite))
        if project is None or not project.actions:
            ex.missing[key] = problem or f"sprite/mob/{r.sprite}{sprprj.SUFFIX} has no actions"
            continue
        clips, why = [], []
        for a in range(len(project.actions)):
            c, reason = _action_clip(project, images, a)
            if c is None:
                why.append(reason)
                continue
            clips.append((MOB_ACTIONS.get(a, f"action{a}"), c))
        for reason in why:
            ex.errors.append(f"{key} ({name}, sprite/mob/{r.sprite}): {reason}")
        if not clips:
            ex.missing[key] = "; ".join(why)
            continue
        firsts[r.id] = clips[0][1]
        _sprite_entries(ex, key, f"monster/{r.id}", clips, name, single=False)
    return firsts


def export_npcs(ex: Export, src: Sources, sprites: Sprites) -> None:
    for r in src.rows("npc"):
        key, name = f"npc:{r.id}", display_name(r.name)
        pid = svr.ProjectId("mob", r.id + NPC_SPRITE_BASE)
        project, images, problem = sprites.load(pid)
        if project is None or not project.actions:
            ex.missing[key] = problem or f"sprite/{pid.key}{sprprj.SUFFIX} has no actions"
            continue
        c, reason = _action_clip(project, images, 0)
        if c is None:
            ex.missing[key] = reason
            ex.errors.append(f"{key} ({name}, sprite/{pid.key}): {reason}")
            continue
        _sprite_entries(ex, key, f"npc/{r.id}", [(MOB_ACTIONS[0], c)], name, single=True)


def _pack_images(ex: Export, app: Path, rel: str) -> list[png.Image | None]:
    try:
        subs = pngdat.split((app / rel).read_bytes(), rel)
    except (OSError, FormatError) as e:
        ex.errors.append(f"{rel}: {e}")
        return []
    out: list[png.Image | None] = []
    for s in subs:
        try:
            out.append(cgbi.decode(s.data, f"{rel} image {s.index}"))
        except FormatError as e:
            ex.errors.append(str(e))
            out.append(None)
    return out


def _icon(ex: Export, key: str, stem: str, icons, index: int, label: str, rel: str) -> bool:
    img = icons[index] if 0 <= index < len(icons) else None
    if img is None:
        ex.missing[key] = f"icon {index}: no such image in {rel} ({len(icons)} images)"
        return False
    big = upscale(img, 2)
    ex.add(key, f"{stem}.png", png.encode_rgba(img.width, img.height, img.rgba),
           img.width, img.height, f"{label} icon", "icon")  # fmt: skip
    ex.add(key, f"{stem}@2x.png", png.encode_rgba(big.width, big.height, big.rgba),
           big.width, big.height, f"{label} icon", "icon2x")  # fmt: skip
    return True


def export_icons(ex: Export, src: Sources) -> None:
    for short, table in ITEM_TABLES.items():
        rel = ICON_PACKS[short]
        icons = _pack_images(ex, src.app, rel)
        for r in src.rows(table):
            _icon(ex, f"item:{short}:{r.id}", f"item/{short}/{r.id}", icons, r.icon,
                  display_name(r.name), rel)  # fmt: skip
    rel = ICON_PACKS["skill"]
    icons = _pack_images(ex, src.app, rel)
    for r in src.rows("skill"):
        _icon(ex, f"skill:{r.id}", f"skill/{r.id}", icons, r.icon, display_name(r.name), rel)


def export_worn(ex: Export, src: Sources, sprites: Sprites) -> None:
    """The standing hero wearing each equip that has a look (``0x62571``)."""
    project, images, problem = sprites.load(HERO)
    if project is None or len(project.actions) <= HERO_STAND:
        ex.errors.append(f"sprite/{HERO.key}: {problem or 'no standing action'}")
        return
    for r in src.rows("item_equip"):
        key, name = f"item:equip:{r.id}", display_name(r.name)
        swap = WORN.get(r.slot)
        if r.look < 0 or swap is None:
            continue  # shoes and earrings: never drawn on the hero
        dressed = list(images)
        why = ""
        for j, target in enumerate(swap.images):
            if target < 0:
                continue
            rel = swap.file.format(look=r.look, j=j)
            if not 0 <= target < len(dressed):
                why = f"hero/0 has no image {target} for {rel}"
                break
            try:
                dressed[target] = svr.decode_image((src.app / rel).read_bytes(), None, rel)
            except FileNotFoundError:
                why = f"{rel} missing"
                break
            except (OSError, FormatError) as e:
                why = str(e)
                break
        c = None
        if not why:
            c, why = _action_clip(project, dressed, HERO_STAND)
        if c is None:
            ex.errors.append(f"{key} ({name}) worn: {why}")
            continue
        ex.add(key, f"item/equip/{r.id}.worn.png", c.still(), c.width, c.height,
               f"The hero wearing {name}", "worn", anchor=list(c.anchor))  # fmt: skip


# Overlay drawing (our own, in map pixels).
SVG_STYLE = (
    "text{font:bold 9px sans-serif;paint-order:stroke;stroke:#000;stroke-width:2px;"
    "fill:#fff;text-anchor:middle}"
    ".portal rect{fill:#2878ff;stroke:#000}.hidden rect{fill:#969696;stroke-dasharray:2 1}"
    ".npc circle{fill:#00be50;stroke:#000}"
)


def overlay_svg(lm: mvr.LoadedMap, src: Sources, firsts: dict[int, Clip]) -> str:
    """The map's portals, NPCs and spawn points as an SVG the size of the 1x map."""
    width, height = lm.pixel_size
    npcs = {r.id: display_name(r.name) for r in src.rows("npc")}
    mobs = {r.id: display_name(r.name) for r in src.rows("mob")}
    defs, portals, people, spawns = [], [], [], []
    used: set[int] = set()
    for o in lm.objects:
        ax, ay = o["anchor"]
        cat = o["category"]
        if cat in ("portal", "hidden_portal"):
            hidden = cat == "hidden_portal"
            label = f"{'hidden portal' if hidden else 'portal'} {o['kind']}"
            cls = "portal hidden" if hidden else "portal"
            portals.append(
                f'<g class="{cls}" data-kind="{o["kind"]}"><title>{escape(label)}</title>'
                f'<rect x="{ax - 6}" y="{ay - 13}" width="12" height="13" rx="2"/>'
                f'<text x="{ax}" y="{ay - 3}">{o["kind"]}</text></g>'
            )
        elif cat == "npc":
            npc_id = o["kind"] - NPC_SPRITE_BASE
            name = npcs.get(npc_id, f"NPC {npc_id}")
            people.append(
                f'<g class="npc" data-id="{npc_id}"><title>{escape(name)}</title>'
                f'<circle cx="{ax}" cy="{ay - 4}" r="4"/>'
                f'<text x="{ax}" y="{ay - 11}">{escape(name)}</text></g>'
            )
        elif cat == "mob":
            mob_id = o.get("mob_id")
            name = mobs.get(mob_id, f"mob {mob_id}")
            c = firsts.get(mob_id)
            if c is None:
                spawns.append(
                    f'<g class="spawn" data-id="{mob_id}"><title>{escape(name)}</title>'
                    f'<circle cx="{ax}" cy="{ay - 4}" r="4" fill="#e61e1e" stroke="#000"/></g>'
                )
                continue
            if mob_id not in used:
                used.add(mob_id)
                b64 = base64.b64encode(c.still()).decode("ascii")
                defs.append(
                    f'<image id="m{mob_id}" width="{c.width}" height="{c.height}" '
                    f'href="data:image/png;base64,{b64}"/>'
                )
            spawns.append(
                f'<g class="spawn" data-id="{mob_id}"><title>{escape(name)}</title>'
                f'<use href="#m{mob_id}" x="{ax - c.anchor[0]}" y="{ay - c.anchor[1]}"/></g>'
            )
    title = escape(f"Map {map_id(lm.id)}: portals, NPCs and spawn points")
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        f"<title>{title}</title>",
        f"<style>{SVG_STYLE}</style>",
        f"<defs>{''.join(defs)}</defs>",
        f'<g class="spawns">{"".join(spawns)}</g>',
        f'<g class="portals">{"".join(portals)}</g>',
        f'<g class="npcs">{"".join(people)}</g>',
        "</svg>",
    ]
    return "\n".join(parts) + "\n"


def export_maps(ex: Export, src: Sources, firsts: dict[int, Clip]) -> None:
    assets = mvr.Assets(src.app)
    for mid in src.maps:
        key, ident = f"map:{map_id(mid)}", map_id(mid)
        lm = mvr.load_map(assets, mid)
        ex.errors += lm.problems
        if lm.map is None:
            ex.missing[key] = "; ".join(lm.problems) or "the map did not load"
            continue
        w, h = lm.pixel_size
        img = png.Image(w, h, bytes(mvr.export_rgba(lm)))
        big = upscale(img, 2)
        ex.add(key, f"map/{ident}.png", png.encode_rgba(w, h, img.rgba), w, h,
               f"Map {ident}", "render")  # fmt: skip
        ex.add(key, f"map/{ident}@2x.png", png.encode_rgba(big.width, big.height, big.rgba),
               big.width, big.height, f"Map {ident}", "render2x")  # fmt: skip
        svg = overlay_svg(lm, src, firsts).encode("utf-8")
        ex.add(key, f"map/{ident}.svg", svg, w, h,
               f"Map {ident}: portals, NPCs and spawn points", "overlay")  # fmt: skip
        small = downscale(img, THUMB_SHRINK)
        ex.add(key, f"map/{ident}.thumb.png", png.encode_rgba(small.width, small.height,
               small.rgba), small.width, small.height, f"Map {ident}", "thumb")  # fmt: skip


# The world map's picture (#232): the game's own world-map screen art, exported as it is for
# the compendium's world map. Not an entity: its manifest key is WORLD_KEY.
WORLD_ART = "png/map/worldMap.png"
WORLD_KEY = "world:map"
THUMB_SHRINK = 4  # map thumbnails (role "thumb") are a quarter of the 1x render


def export_world(ex: Export, src: Sources) -> None:
    path = src.app / WORLD_ART
    if not path.is_file():  # optional: the world map then draws its own layout
        return
    try:
        img = cgbi.decode(path.read_bytes(), WORLD_ART)
    except FormatError as e:
        ex.errors.append(f"{WORLD_ART}: {e}")
        return
    ex.add(WORLD_KEY, "world/worldmap.png", png.encode_rgba(img.width, img.height, img.rgba),
           img.width, img.height, "The game's world map", "render")  # fmt: skip


def build(app: Path) -> tuple[Export, Sources]:
    """Every image of the compendium, in memory."""
    src = load_sources(app)
    ex = Export(errors=list(src.errors))
    sprites = Sprites(src.app)
    firsts = export_monsters(ex, src, sprites)
    export_npcs(ex, src, sprites)
    export_icons(ex, src)
    export_worn(ex, src, sprites)
    export_maps(ex, src, firsts)
    export_world(ex, src)
    return ex, src


def manifest_bytes(ex: Export) -> bytes:
    return (json.dumps(ex.manifest(), indent=1, sort_keys=True, ensure_ascii=False) + "\n").encode(
        "utf-8"
    )


def write(ex: Export, out: Path) -> None:
    """Replace ``out`` with the export (the directory is this module's alone)."""
    out = Path(out)
    if out.exists():
        if any(out.iterdir()) and not (out / MANIFEST).is_file():
            raise FileExistsError(f"{out}: not empty and not an image export ({MANIFEST})")
        shutil.rmtree(out)
    for rel, data in sorted(ex.files.items()):
        path = out / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (out / MANIFEST).write_bytes(manifest_bytes(ex))


# --- Check ------------------------------------------------------------------------------------


def _svg_size(data: bytes) -> tuple[int, int] | None:
    head = data[:400].decode("utf-8", "replace")
    try:
        w = int(head.split(' width="', 1)[1].split('"', 1)[0])
        h = int(head.split(' height="', 1)[1].split('"', 1)[0])
    except (IndexError, ValueError):
        return None
    return w, h


def check(out: Path, ents: dict[str, dict[str, Any]] | None) -> tuple[list[str], list[str]]:
    """(errors, notes): the manifest against the files in ``out`` and against ``ents``
    (``{kind: {id: entity}}``; ``None`` skips the coverage part)."""
    out = Path(out)
    errors: list[str] = []
    try:
        manifest = json.loads((out / MANIFEST).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        return [f"{out / MANIFEST}: {e}"], []
    if manifest.get("schema") != SCHEMA:
        errors.append(f"{MANIFEST}: schema {manifest.get('schema')!r}, expected {SCHEMA}")
    images = manifest.get("images", {})
    missing = manifest.get("missing", {})
    listed: dict[str, str] = {}
    for key, entries in images.items():
        for e in entries:
            path = e.get("path", "")
            if path in listed:
                errors.append(f"{path}: listed under {listed[path]} and {key}")
            listed[path] = key
            for k in ("w", "h", "alt", "role"):
                if not e.get(k):
                    errors.append(f"{key} {path}: no {k}")
            try:
                data = (out / path).read_bytes()
            except OSError:
                errors.append(f"{key} {path}: listed, no such file")
                continue
            errors += _check_file(key, path, data, e)
    on_disk = {
        p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file() and p.name != MANIFEST
    }
    for path in sorted(on_disk - set(listed)):
        errors.append(f"{path}: on disk, not in {MANIFEST}")
    notes = []
    for kind, role in REQUIRED.items():
        for ident in sorted((ents or {}).get(kind, {})):
            key = f"{kind}:{ident}"
            if any(e.get("role") == role for e in images.get(key, ())):
                continue
            if key in missing:
                notes.append(f"{key}: no {role} image: {missing[key]}")
            else:
                errors.append(f"{key}: no {role} image")
    return errors, sorted(notes)


def _check_file(key: str, path: str, data: bytes, e: dict[str, Any]) -> list[str]:
    where = f"{key} {path}"
    if path.endswith(".svg"):
        size = _svg_size(data)
        if size != (e.get("w"), e.get("h")):
            return [f"{where}: SVG size {size}, manifest {e.get('w')}x{e.get('h')}"]
        return []
    try:
        s = read_summary(data, path)
    except FormatError as err:
        return [f"{where}: {err}"]
    problems = []
    if (s.width, s.height) != (e.get("w"), e.get("h")):
        problems.append(f"{where}: {s.width}x{s.height}, manifest {e.get('w')}x{e.get('h')}")
    if e.get("role") == "anim":
        if list(s.delays_ms) != e.get("delays_ms"):
            problems.append(f"{where}: frame delays {list(s.delays_ms)}, manifest "
                            f"{e.get('delays_ms')}")  # fmt: skip
    elif s.frames != 1:
        problems.append(f"{where}: {s.frames} frames in a {e.get('role')} image")
    return problems


def load_entities(app: Path, kb: Path | None) -> dict[str, dict[str, Any]]:
    """The knowledge base's entities (``--kb``), or the ones the tables and maps give."""
    if kb is not None:
        from mtport.compendium import kb as kbmod  # noqa: PLC0415  (#213, optional here)

        return kbmod.load_kb(kb)
    return entities(load_sources(app))


# --- Command line -----------------------------------------------------------------------------


def counts(ex: Export) -> dict[str, int]:
    out: dict[str, int] = {}
    for key in ex.images:
        kind = key.split(":", 1)[0]
        out[kind] = out.get(kind, 0) + 1
    return dict(sorted(out.items()))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        prog="mtport.compendium.images", description=__doc__.split("\n")[0]
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "check"):
        p = sub.add_parser(name)
        p.add_argument("--app", type=Path, default=DEFAULT_APP)
        p.add_argument("--out", type=Path, default=DEFAULT_OUT)
        if name == "check":
            p.add_argument("--kb", type=Path, help="knowledge base file (default: the tables)")
    args = ap.parse_args(argv)
    if not args.app.is_dir():
        print(f"{args.app}: no such .app directory", file=sys.stderr)
        return 2
    if args.cmd == "build":
        ex, _src = build(args.app)
        write(ex, args.out)
        for line in ex.errors:
            print(line)
        for key, why in sorted(ex.missing.items()):
            print(f"{key}: no image: {why}")
        summary = ", ".join(f"{n} {k}" for k, n in counts(ex).items())
        print(f"{args.out}: {len(ex.files)} files for {summary}")
        return 1 if ex.errors else 0
    errors, notes = check(args.out, load_entities(args.app, args.kb))
    for line in errors + notes:
        print(line)
    print(f"{args.out}: {len(errors)} errors, {len(notes)} entities without an image (reasons)")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
