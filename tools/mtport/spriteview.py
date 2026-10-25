"""Sprite/animation viewer for the ``.sprprj`` sprite projects (issue #27, ADR 0010).

Plays every action of every sprite project with the offsets, flips and timing of the game
(docs/formats/sprprj.md). Python does all the pixel work: it composites every frame of an
action onto **one canvas per action**, with the sprite position (the *anchor*) at the same
canvas pixel in every frame and both facings, so playback cannot jitter by construction. The
page's JavaScript only swaps those frame PNGs at the spec's durations and toggles overlays.

Drawing follows the ``CMFrame`` draw at 0x1F119 (called by the ``CMAction`` draw 0x1F2B9):

- the frame is drawn 1 px below the sprite position (``y + 1``);
- layers in file order (first at the back), hidden layers skipped;
- a layer's own alpha is applied (the action alpha is 255, its constructor default);
- unflipped, a layer's top-left is at ``(x + layer.x, y + layer.y)``; with the action flipped
  (``facing == 2``, 0x1E619) at ``(x - layer.x - (image width - 1), y + layer.y)``;
- a layer is mirrored when exactly one of the action flip and the layer's ``hflip`` is set.

Images are ``CMSpriteImage`` (0x218F1): a palette from the project replaces the PNG's
``PLTE`` data before decoding (0xB9F1), ``tRNS`` is kept. Layers are blended "over" in
straight alpha (see :func:`blend`).

Timing: a frame is current for ``max(delay, 1)`` game ticks and an action ends after the sum
over all its frames (``CMAction`` update 0x1E585, :class:`mtport.sprprj.ActionPlayer`). A tick
is 65, 50 or 40 ms (game speed table 0x108C58; 50 ms by default, docs/re/timing-threading.md).
Actions never wrap; the page loops them the way an owner does that resets an action when it
reports *ended*, or holds the last frame when looping is off.

Output (``assets-out/viewers/sprites/``, owned by this command alone)::

    index.html                       every project and its actions
    <folder>-<n>.html                one player per action (data inlined: works from file://)
    <folder>-<n>.json                the same data: canvas, anchor, frame ticks, bounds, files
    <folder>-<n>/a<k>-f<i>.png       frame i of action k, normal facing
    <folder>-<n>/a<k>-f<i>-flip.png  the same frame with the action flipped

Command line (``PYTHONPATH=tools python3 -m mtport.spriteview ...``)::

    build [--app DIR] [--out DIR] [--projects P...]   write the pages (P: mob/3 or hero)
    strip PROJECT ACTION --out FILE.png [--flip] [--guides] [--background RRGGBB] [--scale N]
    batch [--app DIR] [--projects P...]               load and render everything, check it

``batch`` loads every project, decodes every image, renders every frame of every action in
both facings and runs the anchor check (:func:`check_action`) on each, printing one line per
error and exiting 1 on any. The output is deterministic. Input files are untrusted data:
they are parsed, never executed.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import sys
from array import array
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import cgbi, png, sprprj
from mtport import htmlpage as hp
from mtport.bytereader import FormatError

VIEWER = "sprites"
SPRITE_DIR = "sprite"

_REPO = Path(__file__).resolve().parents[2]
DEFAULT_APP = _REPO / "work" / "ipa" / "Payload" / "MapleThief.app"
DEFAULT_OUT = _REPO / "assets-out" / "viewers" / VIEWER

DRAW_Y_OFFSET = 1  # the CMFrame draw (0x1F119) adds 1 to y before drawing any layer
TICK_MS = (65, 50, 40)  # g_gameSpeedIntervals 0x108C58, indexed by the game-speed option
DEFAULT_TICK_MS = 50  # the speed LoadConfig writes
FACINGS = (False, True)  # action flip off (normal) / on (CMAction +0x14, facing == 2)

_NAME = re.compile(r"[A-Za-z0-9_]+\Z")


class ViewerError(FormatError):
    """A sprite project or one of its images cannot be loaded or rendered."""


# --- Projects and images -------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class ProjectId:
    """``sprite/<folder>/<index>.sprprj``."""

    folder: str
    index: int

    @property
    def key(self) -> str:
        return f"{self.folder}/{self.index}"

    @property
    def slug(self) -> str:
        return f"{self.folder}-{self.index}"

    def path(self, app: Path) -> Path:
        return Path(app) / SPRITE_DIR / self.folder / f"{self.index}{sprprj.SUFFIX}"

    def res(self, app: Path) -> Path:
        return Path(app) / SPRITE_DIR / self.folder / sprprj.RES_DIR


def parse_id(text: str) -> ProjectId:
    folder, _, index = text.partition("/")
    if not _NAME.match(folder) or not index.isdigit():
        raise ValueError(f"{text!r}: expected <folder>/<n>, e.g. mob/3")
    return ProjectId(folder, int(index))


def find_projects(app: Path, errors: list[str]) -> list[ProjectId]:
    """Every ``*.sprprj`` below ``app/sprite``, sorted by folder and number. Files whose path
    is not ``<folder>/<n>.sprprj`` with a plain folder name are reported in ``errors``."""
    root = Path(app) / SPRITE_DIR
    found = []
    for path in sprprj.find_projects(root):
        rel = path.relative_to(root)
        folder, stem = rel.parent.as_posix(), path.stem
        if not _NAME.match(folder) or not stem.isdigit() or str(int(stem)) != stem:
            errors.append(f"{SPRITE_DIR}/{rel.as_posix()}: not <folder>/<n>{sprprj.SUFFIX}")
            continue
        found.append(ProjectId(folder, int(stem)))
    return sorted(found)


def select(ids: Sequence[ProjectId], wanted: Sequence[str] | None) -> list[ProjectId]:
    """``ids`` filtered by ``wanted`` entries (``mob/3`` or a whole folder ``hero``)."""
    if not wanted:
        return list(ids)
    keys = set(wanted)
    return [p for p in ids if p.key in keys or p.folder in keys]


def decode_image(data: bytes, palette: bytes | None, name: str = "<png>") -> png.Image:
    """Decode one sprite image as ``CMSpriteImage`` (0x218F1) does.

    With a palette, the PNG's ``PLTE`` data is replaced before decoding (0xB9F1: it copies
    ``PLTE length / 3`` entries; every shipped palette is exactly that long, and a different
    length is an error here). ``tRNS`` stays as it is.
    """
    if palette is None:
        img = cgbi.decode(data, name)
        return png.Image(img.width, img.height, clean(img.rgba))
    f = png.read(data, name)
    if f.cgbi or f.color_type != png.PALETTE:
        raise ViewerError(f"{name}: a palette swap needs an indexed (colour type 3) PNG")
    if len(palette) != len(f.palette or b""):
        raise ViewerError(
            f"{name}: palette of {len(palette)} bytes for a PLTE of {len(f.palette or b'')}"
        )
    f = dataclasses.replace(f, palette=bytes(palette))
    raw = png.inflate(f.idat, f.height * (f.stride + 1), name)
    pixels = png.unfilter(raw, f.height, f.stride, f.bpp, name)
    return png.Image(f.width, f.height, clean(png.to_rgba(f, pixels, name)))


_ZERO = re.compile(rb"\x00+")


def clean(rgba: bytes) -> bytes:
    """``rgba`` with every fully transparent pixel set to ``(0, 0, 0, 0)``, so that copying a
    row onto an empty canvas needs no per-pixel work (:func:`composite`)."""
    out = bytearray(rgba)
    for m in _ZERO.finditer(rgba[3::4]):
        out[4 * m.start() : 4 * m.end()] = bytes(4 * (m.end() - m.start()))
    return bytes(out)


@dataclass
class Loaded:
    """A parsed project and its decoded images (``None`` where an image failed)."""

    pid: ProjectId
    project: sprprj.SpriteProject | None
    images: list[png.Image | None] = field(default_factory=list)
    errors: list[str] = field(default_factory=list)


ImageCache = dict  # (folder, name, palette) -> png.Image | str (the error)


def load_project(app: Path, pid: ProjectId, cache: ImageCache | None = None) -> Loaded:
    """Parse ``pid`` and decode its images. Images are shared between the projects of a
    folder through ``cache``, keyed like the engine's image list (0x21BB5): folder, name and
    palette."""
    cache = {} if cache is None else cache
    try:
        project = sprprj.load(pid.path(app))
    except (OSError, FormatError) as e:
        return Loaded(pid, None, errors=[f"{pid.key}: {e}"])
    loaded = Loaded(pid, project)
    for i, image in enumerate(project.images):
        key = (pid.folder, image.name, image.palette)
        if key not in cache:
            path = pid.res(app) / image.name
            try:
                cache[key] = decode_image(path.read_bytes(), image.palette, image.name)
            except FileNotFoundError:
                cache[key] = f"{sprprj.RES_DIR}/{image.name}: missing"
            except (OSError, FormatError) as e:
                cache[key] = str(e)
        got = cache[key]
        if isinstance(got, str):
            loaded.errors.append(f"{pid.key}: image {i}: {got}")
            loaded.images.append(None)
        else:
            loaded.images.append(got)
    return loaded


# --- Rendering ------------------------------------------------------------------------------


@dataclass(frozen=True)
class Box:
    """A pixel rectangle ``[x0, x1) x [y0, y1)``."""

    x0: int
    y0: int
    x1: int
    y1: int

    def union(self, other: Box | None) -> Box:
        if other is None:
            return self
        return Box(
            min(self.x0, other.x0),
            min(self.y0, other.y0),
            max(self.x1, other.x1),
            max(self.y1, other.y1),
        )

    def shift(self, dx: int, dy: int) -> Box:
        return Box(self.x0 + dx, self.y0 + dy, self.x1 + dx, self.y1 + dy)

    def inside(self, other: Box) -> bool:
        return (
            other.x0 <= self.x0
            and other.y0 <= self.y0
            and self.x1 <= other.x1
            and self.y1 <= other.y1
        )

    def as_list(self) -> list[int]:
        return [self.x0, self.y0, self.x1, self.y1]


def union(boxes: Iterable[Box | None]) -> Box | None:
    out = None
    for b in boxes:
        if b is not None:
            out = b.union(out)
    return out


@dataclass(frozen=True)
class Placement:
    """Where the frame draw puts one visible layer, relative to the sprite position."""

    layer: sprprj.Layer
    x: int  # top-left of the image
    y: int
    width: int
    height: int
    mirrored: bool

    @property
    def box(self) -> Box:
        return Box(self.x, self.y, self.x + self.width, self.y + self.height)


def placements(
    frame: sprprj.Frame, sizes: Sequence[tuple[int, int]], flip: bool
) -> list[Placement]:
    """The visible layers of ``frame`` as 0x1F119 draws them, back to front."""
    out = []
    for layer in frame.layers:
        if not layer.visible:
            continue
        w, h = sizes[layer.image]
        x = -layer.x - (w - 1) if flip else layer.x
        out.append(Placement(layer, x, layer.y + DRAW_Y_OFFSET, w, h, flip != layer.hflip))
    return out


def spec_box(frame: sprprj.Frame, sizes: Sequence[tuple[int, int]], flip: bool) -> Box | None:
    """The engine's frame bounds (0x1E77D, :func:`mtport.sprprj.frame_bounds`) where the draw
    puts them: 1 px down, and mirrored like a flipped layer. ``None`` without visible layers."""
    b = sprprj.frame_bounds(frame, sizes)
    if b.min_x == sprprj.BOUNDS_MIN_START and b.max_x == sprprj.BOUNDS_MAX_START:
        return None
    if flip:
        # A flipped layer spans [-x - w + 1, -x + 1).
        return Box(1 - b.max_x, b.min_y + DRAW_Y_OFFSET, 1 - b.min_x, b.max_y + DRAW_Y_OFFSET)
    return Box(b.min_x, b.min_y + DRAW_Y_OFFSET, b.max_x, b.max_y + DRAW_Y_OFFSET)


def layer_alpha(pixel_alpha: int, alpha: int) -> int:
    """A source pixel's alpha after the layer's alpha: ``round(a * alpha / 255)``."""
    return pixel_alpha if alpha == 255 else (pixel_alpha * alpha + 127) // 255


def blend(
    dst: tuple[int, int, int, int], src: tuple[int, int, int], sa: int
) -> tuple[int, int, int, int]:
    """``src`` (colour, alpha ``sa`` > 0) over ``dst``, straight alpha, rounded to nearest.

    With ``D = sa * 255 + da * (255 - sa)``: ``out_a = round(D / 255)`` and
    ``out_c = round((sc * sa * 255 + dc * da * (255 - sa)) / D)``.
    """
    da = dst[3]
    if sa == 255 or da == 0:
        return (*src, sa)
    k = da * (255 - sa)
    d = sa * 255 + k
    r, g, b = ((s * sa * 255 + t * k + d // 2) // d for s, t in zip(src, dst[:3], strict=True))
    return (r, g, b, (d + 127) // 255)


def mirror_rows(img: png.Image) -> bytes:
    """``img.rgba`` mirrored horizontally."""
    out = bytearray()
    stride = img.width * 4
    for y in range(img.height):
        row = array("I")
        row.frombytes(img.rgba[y * stride : (y + 1) * stride])
        row.reverse()
        out += row.tobytes()
    return bytes(out)


_VISIBLE = re.compile(rb"[^\x00]")


def composite(
    canvas: bytearray, cw: int, ch: int, rgba: bytes, w: int, h: int, x0: int, y0: int, alpha: int
) -> None:
    """Draw the ``w`` x ``h`` image ``rgba`` with its top-left at canvas ``(x0, y0)``, layer
    alpha ``alpha``, clipped to the ``cw`` x ``ch`` canvas."""
    if alpha == 0:
        return
    sx0, sx1 = max(0, -x0), min(w, cw - x0)
    if sx0 >= sx1:
        return
    n = (sx1 - sx0) * 4
    zero = bytes(n)
    for sy in range(max(0, -y0), min(h, ch - y0)):
        src = rgba[(sy * w + sx0) * 4 : (sy * w + sx1) * 4]
        d = ((y0 + sy) * cw + x0 + sx0) * 4
        if alpha == 255 and canvas[d : d + n] == zero:
            canvas[d : d + n] = src  # nothing below: copy (transparent pixels are all zero)
            continue
        for m in _VISIBLE.finditer(src[3::4]):
            o = 4 * m.start()
            sa = layer_alpha(src[o + 3], alpha)
            if sa == 0:
                continue
            p = d + o
            dst = (canvas[p], canvas[p + 1], canvas[p + 2], canvas[p + 3])
            canvas[p : p + 4] = bytes(blend(dst, (src[o], src[o + 1], src[o + 2]), sa))


@dataclass(frozen=True)
class ActionRender:
    """Every frame of one action, in both facings, on one canvas.

    The sprite position is canvas pixel ``(anchor_x, anchor_y)`` in every frame and facing.
    ``frames[flip][i]`` is frame ``i``'s RGBA; ``boxes[flip][i]`` its :func:`spec_box` in
    canvas coordinates (``None`` without visible layers).
    """

    width: int
    height: int
    anchor_x: int
    anchor_y: int
    frames: dict[bool, tuple[bytes, ...]]
    boxes: dict[bool, tuple[Box | None, ...]]

    def to_canvas(self, box: Box | None) -> Box | None:
        return None if box is None else box.shift(self.anchor_x, self.anchor_y)


def sizes_of(images: Sequence[png.Image]) -> list[tuple[int, int]]:
    return [(i.width, i.height) for i in images]


class _Mirrors(dict):
    """Lazily mirrored image pixels, by image index."""

    def __init__(self, images: Sequence[png.Image]) -> None:
        super().__init__()
        self.images = images

    def __missing__(self, i: int) -> bytes:
        self[i] = mirror_rows(self.images[i])
        return self[i]


def render_action(action: sprprj.Action, images: Sequence[png.Image]) -> ActionRender:
    """Composite every frame of ``action`` (both facings) onto one shared canvas: the union of
    every layer's rectangle and of the anchor pixel itself."""
    sizes = sizes_of(images)
    plans = {f: [placements(fr, sizes, f) for fr in action.frames] for f in FACINGS}
    area = Box(0, 0, 1, 1)  # the anchor pixel is always on the canvas
    for f in FACINGS:
        for plan in plans[f]:
            area = area.union(union(p.box for p in plan))
    cw, ch = area.x1 - area.x0, area.y1 - area.y0
    ax, ay = -area.x0, -area.y0
    mirrors = _Mirrors(images)
    frames: dict[bool, tuple[bytes, ...]] = {}
    boxes: dict[bool, tuple[Box | None, ...]] = {}
    for f in FACINGS:
        out = []
        for plan in plans[f]:
            canvas = bytearray(cw * ch * 4)
            for p in plan:
                img = images[p.layer.image]
                rgba = mirrors[p.layer.image] if p.mirrored else img.rgba
                composite(
                    canvas, cw, ch, rgba, p.width, p.height, ax + p.x, ay + p.y, p.layer.alpha
                )
            out.append(bytes(canvas))
        frames[f] = tuple(out)
        boxes[f] = tuple(
            None if b is None else b.shift(ax, ay)
            for b in (spec_box(fr, sizes, f) for fr in action.frames)
        )
    return ActionRender(cw, ch, ax, ay, frames, boxes)


# --- Anchor check ---------------------------------------------------------------------------


def visible_box(alpha_plane: bytes, w: int, h: int) -> Box | None:
    """The box around the non-zero bytes of a ``w`` x ``h`` alpha plane."""
    x0, y0, x1, y1 = w, h, 0, 0
    for y in range(h):
        row = alpha_plane[y * w : (y + 1) * w]
        rest = row.lstrip(b"\0")
        if not rest:
            continue
        x0 = min(x0, w - len(rest))
        x1 = max(x1, len(row.rstrip(b"\0")))
        y0 = min(y0, y)
        y1 = y + 1
    return Box(x0, y0, x1, y1) if x1 else None


_THRESHOLDS: dict[int, bytes] = {}


def _threshold_table(alpha: int) -> bytes:
    """Maps a pixel alpha to 1 where it stays visible under layer alpha ``alpha``."""
    if alpha not in _THRESHOLDS:
        _THRESHOLDS[alpha] = bytes(1 if layer_alpha(a, alpha) else 0 for a in range(256))
    return _THRESHOLDS[alpha]


def image_box(img: png.Image, alpha: int, mirrored: bool) -> Box | None:
    """The box around the pixels of ``img`` that a layer with ``alpha`` draws."""
    b = visible_box(img.rgba[3::4].translate(_threshold_table(alpha)), img.width, img.height)
    if b is None or not mirrored:
        return b
    return Box(img.width - b.x1, b.y0, img.width - b.x0, b.y1)


def check_action(
    where: str, action: sprprj.Action, images: Sequence[png.Image], render: ActionRender
) -> list[str]:
    """Anchor stability and placement of a rendered action, one line per problem.

    For every frame and facing: the frame is on the action's one canvas (same size, same
    anchor, anchor pixel on the canvas); no layer is cropped by the canvas; and the box
    around the frame's visible pixels, relative to the anchor, equals the box predicted
    independently from each layer's own visible pixels placed at its spec offset (flips
    included) and lies inside the engine's frame bounds (0x1E77D). A frame drawn at a moved
    anchor, a wrong offset or flip, or a cropped canvas fails this.
    """
    errors = []
    sizes = sizes_of(images)
    w, h = render.width, render.height
    boxes: dict[tuple[int, int, bool], Box | None] = {}

    def own_box(p: Placement) -> Box | None:
        key = (p.layer.image, p.layer.alpha, p.mirrored)
        if key not in boxes:
            boxes[key] = image_box(images[p.layer.image], p.layer.alpha, p.mirrored)
        return boxes[key]

    canvas = Box(0, 0, w, h)
    if not (0 <= render.anchor_x < w and 0 <= render.anchor_y < h):
        errors.append(f"{where}: anchor ({render.anchor_x}, {render.anchor_y}) off the canvas")
    for flip in FACINGS:
        facing = "flipped" if flip else "normal"
        frames = render.frames.get(flip, ())
        if len(frames) != len(action.frames):
            errors.append(f"{where} {facing}: {len(frames)} frames rendered, {len(action.frames)}")
            continue
        for i, (frame, rgba) in enumerate(zip(action.frames, frames, strict=True)):
            at = f"{where} frame {i} {facing}"
            if len(rgba) != w * h * 4:
                errors.append(f"{at}: not on the action's {w}x{h} canvas")
                continue
            plan = placements(frame, sizes, flip)
            for p in plan:
                if not p.box.shift(render.anchor_x, render.anchor_y).inside(canvas):
                    errors.append(f"{at}: layer image {p.layer.image} cropped by the canvas")
            want = union(b.shift(p.x, p.y) for p in plan if (b := own_box(p)))
            got = visible_box(rgba[3::4], w, h)
            got = None if got is None else got.shift(-render.anchor_x, -render.anchor_y)
            if got != want:
                errors.append(
                    f"{at}: pixels at {_fmt(got)} relative to the anchor, layer offsets give "
                    f"{_fmt(want)}"
                )
            spec = spec_box(frame, sizes, flip)
            if got is not None and (spec is None or not got.inside(spec)):
                errors.append(f"{at}: pixels at {_fmt(got)} outside the frame bounds {_fmt(spec)}")
    return errors


def _fmt(b: Box | None) -> str:
    return "nothing" if b is None else f"[{b.x0}, {b.x1}) x [{b.y0}, {b.y1})"


# --- Whole projects -------------------------------------------------------------------------


@dataclass
class Rendered:
    """A loaded project with every action it could render."""

    loaded: Loaded
    actions: dict[int, ActionRender] = field(default_factory=dict)
    errors: list[str] = field(default_factory=list)


def render_project(loaded: Loaded, check: bool = True) -> Rendered:
    """Render every action of ``loaded`` (skipping actions that use a failed image) and, with
    ``check``, run :func:`check_action` on each."""
    out = Rendered(loaded, errors=list(loaded.errors))
    project = loaded.project
    if project is None:
        return out
    for a, action in enumerate(project.actions):
        where = f"{loaded.pid.key} action {a}"
        if not action.frames:
            out.errors.append(f"{where}: no frames")
            continue
        used = {la.image for fr in action.frames for la in fr.layers}
        if any(loaded.images[i] is None for i in used):
            out.errors.append(f"{where}: not rendered (an image failed to load)")
            continue
        images = [img if img is not None else _EMPTY for img in loaded.images]
        render = render_action(action, images)
        out.actions[a] = render
        if check:
            out.errors += check_action(where, action, images, render)
    return out


_EMPTY = png.Image(1, 1, bytes(4))  # stands in for a failed image no rendered action uses


def frame_file(pid: ProjectId, action: int, frame: int, flip: bool) -> str:
    return f"{pid.slug}/a{action}-f{frame}{'-flip' if flip else ''}.png"


def project_data(r: Rendered) -> dict:
    """The JSON of one project: what the page's player needs, and nothing it computes."""
    project = r.loaded.project
    assert project is not None
    pid = r.loaded.pid
    actions = []
    for a, render in sorted(r.actions.items()):
        action = project.actions[a]
        frames = []
        for i, frame in enumerate(action.frames):
            frames.append(
                {
                    "delay": frame.delay,
                    "ticks": sprprj.frame_ticks(frame),
                    "layers": len(frame.layers),
                    "files": {
                        "normal": frame_file(pid, a, i, False),
                        "flipped": frame_file(pid, a, i, True),
                    },
                    "bounds": {
                        ("flipped" if f else "normal"): (
                            None if render.boxes[f][i] is None else render.boxes[f][i].as_list()
                        )
                        for f in FACINGS
                    },
                }
            )
        actions.append(
            {
                "index": a,
                "width": render.width,
                "height": render.height,
                "anchor": [render.anchor_x, render.anchor_y],
                "loop_ticks": sum(f["ticks"] for f in frames),
                "ended_after_ticks": sprprj.action_ticks(action),
                "frames": frames,
            }
        )
    return {
        "project": pid.key,
        "draw_y_offset": DRAW_Y_OFFSET,
        "tick_ms": list(TICK_MS),
        "default_tick_ms": DEFAULT_TICK_MS,
        "images": [
            {
                "name": im.name,
                "palette_colours": len(im.palette) // 3 if im.palette else 0,
                "size": None if img is None else [img.width, img.height],
            }
            for im, img in zip(project.images, r.loaded.images, strict=True)
        ],
        "actions": actions,
    }


def dumps(data: object) -> str:
    return json.dumps(data, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def inline_json(data: object) -> str:
    """:func:`dumps` safe inside a ``<script>`` element: ``<``, ``>`` and ``&`` (only in
    strings, e.g. untrusted image names) become ``\\u`` escapes, so no ``</script>``."""
    return dumps(data).replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")


# --- Pages ----------------------------------------------------------------------------------

CSS = """
.controls { position: sticky; top: 0; z-index: 2; background: var(--bg); padding: 6px 0;
  border-bottom: 1px solid var(--line); display: flex; flex-wrap: wrap; gap: 6px 16px; }
.controls label { white-space: nowrap; }
section.action { margin: 16px 0 24px; }
section.action h2 { font-size: 16px; margin: 8px 0 4px; }
.player { display: flex; flex-wrap: wrap; align-items: flex-start; gap: 12px; }
.stage { position: relative; outline: 1px solid var(--line); }
.stage img, .cell img { display: block; image-rendering: pixelated; }
.bg-checker .stage, .bg-checker .cell .box { background-color: #fff;
  background-image: linear-gradient(45deg, #ccc 25%, transparent 25%),
  linear-gradient(-45deg, #ccc 25%, transparent 25%),
  linear-gradient(45deg, transparent 75%, #ccc 75%),
  linear-gradient(-45deg, transparent 75%, #ccc 75%);
  background-size: 16px 16px; background-position: 0 0, 0 8px, 8px -8px, -8px 0; }
.bg-dark .stage, .bg-dark .cell .box { background: #202020; }
.bg-light .stage, .bg-light .cell .box { background: #f0f0f0; }
.bg-green .stage, .bg-green .cell .box { background: #6c8; }
.ov { position: absolute; pointer-events: none; }
.cross { background: #ff00c8; }
.bounds { outline: 1px solid rgba(0, 160, 255, .9); }
.hide-cross .cross, .hide-bounds .bounds { display: none; }
.buttons button { font: inherit; min-width: 3.5em; }
.cells { display: flex; flex-wrap: wrap; gap: 6px; }
.cell { font-size: 12px; text-align: center; cursor: pointer; }
.cell .box { position: relative; outline: 1px solid var(--line); }
.cell.cur .box { outline: 2px solid var(--link); }
.status { font-variant-numeric: tabular-nums; }
"""

# Plays the pre-rendered frames. Per action: frame i shows for ticks[i] ticks; after the last
# frame the action is ended and, when looping, reset to frame 0 (as the game's owners do).
# One timer drives every player at the chosen tick length. No network, works from file://.
SCRIPT = """
(function () {
  var data = JSON.parse(document.getElementById("sprite-data").textContent);
  var body = document.body, players = [], timer = null;
  var opts = { zoom: 3, tick: data.default_tick_ms, flip: false, loop: true };
  function px(v) { return (v * opts.zoom) + "px"; }
  function place(el, x, y, w, h) {
    el.style.left = px(x); el.style.top = px(y); el.style.width = px(w); el.style.height = px(h);
  }
  function Player(sec, act) {
    var self = this;
    this.act = act; this.cur = 0; this.hold = 0;
    this.playing = act.frames.length > 1;
    this.stage = sec.querySelector(".stage"); this.img = this.stage.querySelector("img");
    this.box = this.stage.querySelector(".bounds");
    this.h = this.stage.querySelector(".cross.h"); this.v = this.stage.querySelector(".cross.v");
    this.status = sec.querySelector(".status");
    this.play = sec.querySelector(".play");
    this.cells = sec.querySelectorAll(".cell");
    sec.querySelector(".prev").onclick = function () { self.step(-1); };
    sec.querySelector(".next").onclick = function () { self.step(1); };
    this.play.onclick = function () {
      if (!self.playing && self.ended()) { self.cur = 0; self.hold = 0; }
      self.playing = !self.playing; self.draw();
    };
    Array.prototype.forEach.call(this.cells, function (c, i) {
      c.onclick = function () { self.playing = false; self.cur = i; self.hold = 0; self.draw(); };
    });
  }
  Player.prototype.ended = function () {
    return this.cur === this.act.frames.length - 1 && this.hold >= this.act.frames[this.cur].ticks;
  };
  Player.prototype.step = function (d) {
    var n = this.act.frames.length;
    this.playing = false; this.cur = (this.cur + d + n) % n; this.hold = 0; this.draw();
  };
  Player.prototype.tick = function () {
    var f = this.act.frames;
    if (!this.playing || f.length < 2) return;
    this.hold++;
    if (this.hold >= f[this.cur].ticks) {
      if (this.cur + 1 < f.length) { this.cur++; this.hold = 0; }
      else if (opts.loop) { this.cur = 0; this.hold = 0; }
      else { this.playing = false; }
    }
    this.draw();
  };
  Player.prototype.draw = function () {
    var a = this.act, f = a.frames[this.cur], face = opts.flip ? "flipped" : "normal";
    var b = f.bounds[face];
    this.stage.style.width = px(a.width); this.stage.style.height = px(a.height);
    this.img.src = f.files[face];
    this.img.style.width = px(a.width); this.img.style.height = px(a.height);
    place(this.h, 0, a.anchor[1] + 0.5, a.width, 0);
    this.h.style.height = "1px";
    place(this.v, a.anchor[0] + 0.5, 0, 0, a.height);
    this.v.style.width = "1px";
    if (b) { this.box.hidden = false; place(this.box, b[0], b[1], b[2] - b[0], b[3] - b[1]); }
    else { this.box.hidden = true; }
    var t = 0;
    for (var i = 0; i < this.cur; i++) t += a.frames[i].ticks;
    this.status.textContent = "frame " + this.cur + " / " + (a.frames.length - 1) +
      " \\u00b7 delay " + f.delay + " (" + f.ticks + " tick" + (f.ticks === 1 ? "" : "s") +
      ", " + f.ticks * opts.tick + " ms) \\u00b7 tick " + (t + Math.min(this.hold, f.ticks - 1)) +
      " of " + a.loop_ticks;
    this.play.textContent = this.playing ? "Pause" : "Play";
    Array.prototype.forEach.call(this.cells, function (c, i) {
      c.classList.toggle("cur", i === this.cur);
      var cimg = c.querySelector("img");
      cimg.src = a.frames[i].files[face];
    }, this);
  };
  data.actions.forEach(function (act) {
    var sec = document.getElementById("a" + act.index);
    if (sec) players.push(new Player(sec, act));
  });
  function run() {
    players.forEach(function (p) { p.tick(); });
    timer = setTimeout(run, opts.tick);
  }
  function redraw() { players.forEach(function (p) { p.draw(); }); }
  function bind(id, fn) {
    var el = document.getElementById(id);
    if (!el) return;
    el.addEventListener("change", function () { fn(el); redraw(); });
    fn(el);
  }
  bind("opt-zoom", function (el) { opts.zoom = Number(el.value); });
  bind("opt-tick", function (el) {
    opts.tick = Number(el.value);
    if (timer !== null) { clearTimeout(timer); timer = setTimeout(run, opts.tick); }
  });
  bind("opt-flip", function (el) { opts.flip = el.checked; });
  bind("opt-loop", function (el) { opts.loop = el.checked; });
  bind("opt-cross", function (el) { body.classList.toggle("hide-cross", !el.checked); });
  bind("opt-bounds", function (el) { body.classList.toggle("hide-bounds", !el.checked); });
  bind("opt-bg", function (el) {
    ["checker", "dark", "light", "green"].forEach(function (k) {
      body.classList.toggle("bg-" + k, el.value === k);
    });
  });
  document.getElementById("all-play").onclick = function () {
    players.forEach(function (p) { p.playing = p.act.frames.length > 1; p.cur = 0; p.hold = 0; });
    redraw();
  };
  document.getElementById("all-pause").onclick = function () {
    players.forEach(function (p) { p.playing = false; });
    redraw();
  };
  redraw();
  timer = setTimeout(run, opts.tick);
})();
"""


def _controls() -> str:
    ticks = "".join(
        f'<option value="{ms}"{" selected" if ms == DEFAULT_TICK_MS else ""}>{ms} ms</option>'
        for ms in TICK_MS
    )
    zooms = "".join(
        f'<option value="{z}"{" selected" if z == 3 else ""}>{z}x</option>' for z in (1, 2, 3, 4, 6)
    )
    return (
        '<div class="controls">'
        '<span class="buttons"><button id="all-play" type="button">Play all</button> '
        '<button id="all-pause" type="button">Pause all</button></span>'
        f'<label>Tick <select id="opt-tick" title="Game speed option: 65, 50 (default) or 40 ms '
        f'per tick (0x108C58)">{ticks}</select></label>'
        f'<label>Zoom <select id="opt-zoom">{zooms}</select></label>'
        '<label title="Flip the whole action (CMAction +0x14, facing == 2)">'
        '<input type="checkbox" id="opt-flip"> Flipped</label>'
        '<label title="Reset the action when it ends, as its owner does; off: hold the last '
        'frame"><input type="checkbox" id="opt-loop" checked> Loop</label>'
        '<label title="The sprite position (the anchor)"><input type="checkbox" id="opt-cross" '
        "checked> Anchor</label>"
        '<label title="The frame bounds (0x1E77D), where the draw puts them">'
        '<input type="checkbox" id="opt-bounds"> Bounds</label>'
        '<label>Background <select id="opt-bg"><option value="checker">checker</option>'
        '<option value="dark">dark</option><option value="light">light</option>'
        '<option value="green">green</option></select></label>'
        "</div>\n"
    )


def _action_section(pid: ProjectId, a: int, action: sprprj.Action, render: ActionRender) -> str:
    n = len(action.frames)
    loop = sum(sprprj.frame_ticks(f) for f in action.frames)
    timing = (
        "1 frame (static)"
        if n == 1
        else f"{n} frames, {loop} ticks ({loop * DEFAULT_TICK_MS} ms at {DEFAULT_TICK_MS} ms/tick)"
    )
    cells = []
    for i, frame in enumerate(action.frames):
        ticks = sprprj.frame_ticks(frame)
        cells.append(
            f'<div class="cell" title="delay {frame.delay}, {len(frame.layers)} layers">'
            f'<div class="box"><img src="{hp.esc(frame_file(pid, a, i, False))}" '
            f'width="{render.width}" height="{render.height}" alt="frame {i}"></div>'
            f"f{i} &middot; {ticks}t</div>"
        )
    first = hp.esc(frame_file(pid, a, 0, False))
    return (
        f'<section class="action" id="a{a}">\n'
        f'<h2>Action {a} <span class="muted">{hp.esc(timing)}; canvas {render.width}x'
        f"{render.height}, anchor ({render.anchor_x}, {render.anchor_y})</span></h2>\n"
        '<div class="player">'
        f'<div class="stage"><img src="{first}" alt="action {a}">'
        '<div class="ov bounds"></div><div class="ov cross h"></div>'
        '<div class="ov cross v"></div></div>'
        '<div><p class="buttons"><button type="button" class="prev">&lt; Step</button> '
        '<button type="button" class="play">Play</button> '
        '<button type="button" class="next">Step &gt;</button></p>'
        '<p class="status"></p></div></div>\n'
        f'<div class="cells">{"".join(cells)}</div>\n'
        "</section>\n"
    )


def render_page(r: Rendered, data: dict) -> str:
    pid, project = r.loaded.pid, r.loaded.project
    assert project is not None
    rows = []
    for i, (im, img) in enumerate(zip(project.images, r.loaded.images, strict=True)):
        size = "failed" if img is None else f"{img.width}x{img.height}"
        palette = f"{len(im.palette) // 3} colours" if im.palette else ""
        rows.append([hp.num(i), hp.text(im.name), hp.text(size), hp.text(palette)])
    images = hp.table(
        [hp.Column("#", numeric=True), hp.Column("PNG"), hp.Column("Size"), hp.Column("Palette")],
        rows,
    )
    errors = "".join(f'<li class="error">{hp.esc(e)}</li>' for e in r.errors)
    sections = "".join(
        _action_section(pid, a, project.actions[a], render)
        for a, render in sorted(r.actions.items())
    )
    body = (
        f'<p class="muted">sprite/{hp.esc(pid.folder)}/{pid.index}.sprprj: '
        f"{len(project.images)} images, {len(project.actions)} actions. Frames are composited "
        "in Python onto one canvas per action, so the anchor (the sprite position) is the same "
        "canvas pixel in every frame. A frame shows for max(delay, 1) game ticks; the frame "
        "draw puts the layers 1 px below the anchor (0x1F119).</p>\n"
        + (f"<ul>{errors}</ul>\n" if errors else "")
        + _controls()
        + sections
        + "<h2>Images</h2>\n"
        + images
        + '<script type="application/json" id="sprite-data">'
        + inline_json(data)
        + "</script>\n"
        + f"<style>{CSS}</style><script>{SCRIPT}</script>\n"
    )
    nav = hp.link("index.html", "All sprite projects")
    return hp.page(f"Sprite {pid.key}", body, nav)


def render_index(results: Sequence[Rendered], errors: Sequence[str]) -> str:
    rows, ids = [], []
    for r in results:
        pid, project = r.loaded.pid, r.loaded.project
        if project is None:
            rows.append(
                [
                    hp.Cell(hp.esc(pid.key), pid.key),
                    hp.text(pid.folder),
                    hp.num(0),
                    hp.num(0),
                    hp.num(0),
                    hp.Cell('<span class="error">failed to load</span>'),
                ]
            )
        else:
            frames = sum(len(a.frames) for a in project.actions)
            links = " ".join(
                hp.link(
                    f"{pid.slug}.html#a{a}",
                    str(a),
                    f"{len(project.actions[a].frames)} frames",
                )
                for a in sorted(r.actions)
            )
            name = hp.link(f"{pid.slug}.html", pid.key) if r.actions else hp.esc(pid.key)
            note = links or '<span class="muted">empty</span>'
            rows.append(
                [
                    hp.Cell(name, pid.key),
                    hp.text(pid.folder),
                    hp.num(len(project.images)),
                    hp.num(len(project.actions)),
                    hp.num(frames),
                    hp.Cell(note),
                ]
            )
        ids.append(pid.slug)
    table = hp.table(
        [
            hp.Column("Project"),
            hp.Column("Folder"),
            hp.Column("Images", numeric=True),
            hp.Column("Actions", numeric=True),
            hp.Column("Frames", numeric=True),
            hp.Column("Actions to play", "click an action number to open its player"),
        ],
        rows,
        ids,
    )
    errs = (
        "<ul>" + "".join(f'<li class="error">{hp.esc(e)}</li>' for e in errors) + "</ul>\n"
        if errors
        else ""
    )
    body = (
        f'<p class="muted" data-projects="{len(results)}">{len(results)} sprite projects from '
        "sprite/&lt;folder&gt;/&lt;n&gt;.sprprj (docs/formats/sprprj.md), "
        f"{len(errors)} errors.</p>\n" + errs + table
    )
    return hp.page("MapleStory Thief sprites", body)


def render(app: Path, wanted: Sequence[str] | None = None) -> tuple[dict[str, bytes], list[str]]:
    """Every output file (relative path -> bytes) and every error."""
    files: dict[str, bytes] = {}
    errors: list[str] = []
    results = []
    for r in run(app, wanted, errors):
        results.append(r)
        if r.loaded.project is None or not r.actions:
            continue
        pid = r.loaded.pid
        for a, ar in r.actions.items():
            for flip in FACINGS:
                for i, rgba in enumerate(ar.frames[flip]):
                    files[frame_file(pid, a, i, flip)] = png.encode_rgba(ar.width, ar.height, rgba)
        data = project_data(r)
        files[f"{pid.slug}.json"] = (dumps(data) + "\n").encode()
        files[f"{pid.slug}.html"] = render_page(r, data).encode()
    files["index.html"] = render_index(results, errors).encode()
    return files, errors


def run(
    app: Path, wanted: Sequence[str] | None, errors: list[str], check: bool = True
) -> Iterable[Rendered]:
    """Load and render every selected project, adding every error to ``errors``."""
    ids = select(find_projects(app, errors), wanted)
    if wanted:
        for w in wanted:
            if not any(p.key == w or p.folder == w for p in ids):
                errors.append(f"{w}: no such sprite project or folder")
    cache: ImageCache = {}
    for pid in ids:
        r = render_project(load_project(app, pid, cache), check)
        errors += r.errors
        yield r


@dataclass
class BatchReport:
    projects: int = 0
    empty: int = 0
    actions: int = 0
    frames: int = 0
    errors: list[str] = field(default_factory=list)

    def summary(self) -> str:
        return (
            f"projects={self.projects} empty={self.empty} actions={self.actions} "
            f"frames={self.frames} errors={len(self.errors)}"
        )


def batch(app: Path, wanted: Sequence[str] | None = None) -> BatchReport:
    """Load every project, render every frame of every action in both facings and check it."""
    report = BatchReport()
    for r in run(app, wanted, report.errors):
        report.projects += 1
        project = r.loaded.project
        if project is None:
            continue
        report.empty += not project.images and not project.actions
        report.actions += len(r.actions)
        report.frames += sum(len(project.actions[a].frames) for a in r.actions)
    return report


def build(app: Path, out: Path, wanted: Sequence[str] | None = None):
    files, errors = render(app, wanted)
    hp.write_dir(out, files, VIEWER)
    return files, errors


# --- Frame strips ---------------------------------------------------------------------------


def strip(
    render: ActionRender,
    flip: bool = False,
    gap: int = 1,
    guides: bool = False,
    background: tuple[int, int, int] | None = None,
    scale: int = 1,
) -> png.Image:
    """All frames of an action left to right, ``gap`` transparent columns apart, each on the
    action's canvas (so the anchor is at the same place in every cell). ``guides`` draws a
    magenta cross through the anchor behind each frame; ``background`` fills each cell first.
    ``scale`` enlarges the result with nearest-neighbour sampling."""
    w, h = render.width, render.height
    frames = render.frames[flip]
    sw = len(frames) * w + (len(frames) - 1) * gap
    out = bytearray(sw * h * 4)
    for i, rgba in enumerate(frames):
        x0 = i * (w + gap)
        cell = bytearray(w * h * 4)
        if background is not None:
            cell[:] = bytes((*background, 255)) * (w * h)
        if guides:
            magenta = bytes((255, 0, 255, 255))
            for x in range(w):
                o = (render.anchor_y * w + x) * 4
                cell[o : o + 4] = magenta
            for y in range(h):
                o = (y * w + render.anchor_x) * 4
                cell[o : o + 4] = magenta
        composite(cell, w, h, rgba, w, h, 0, 0, 255)
        for y in range(h):
            d = (y * sw + x0) * 4
            out[d : d + w * 4] = cell[y * w * 4 : (y + 1) * w * 4]
    img = png.Image(sw, h, bytes(out))
    return upscale(img, scale) if scale > 1 else img


def upscale(img: png.Image, k: int) -> png.Image:
    out = bytearray()
    stride = img.width * 4
    for y in range(img.height):
        row = img.rgba[y * stride : (y + 1) * stride]
        wide = b"".join(row[x : x + 4] * k for x in range(0, stride, 4))
        out += wide * k
    return png.Image(img.width * k, img.height * k, bytes(out))


def _color(text: str) -> tuple[int, int, int]:
    if not re.fullmatch(r"[0-9A-Fa-f]{6}", text):
        raise argparse.ArgumentTypeError(f"{text!r}: expected RRGGBB")
    v = int(text, 16)
    return (v >> 16, (v >> 8) & 0xFF, v & 0xFF)


def _strip_cmd(args: argparse.Namespace) -> int:
    try:
        pid = parse_id(args.project)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    loaded = load_project(args.app, pid)
    project = loaded.project
    if project is None or not 0 <= args.action < len(project.actions):
        problem = "; ".join(loaded.errors) or f"{pid.key}: no action {args.action}"
        print(f"error: {problem}", file=sys.stderr)
        return 1
    action = project.actions[args.action]
    used = {la.image for fr in action.frames for la in fr.layers}
    if not action.frames or any(loaded.images[i] is None for i in used):
        print(f"error: {'; '.join(loaded.errors) or 'no frames'}", file=sys.stderr)
        return 1
    images = [img if img is not None else _EMPTY for img in loaded.images]
    render = render_action(action, images)
    problems = check_action(f"{pid.key} action {args.action}", action, images, render)
    img = strip(render, args.flip, args.gap, args.guides, args.background, args.scale)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    png.save(args.out, img)
    for p in problems:
        print(f"error: {p}", file=sys.stderr)
    print(
        f"{args.out}: {len(action.frames)} frames of {render.width}x{render.height}, anchor "
        f"({render.anchor_x}, {render.anchor_y}), ticks "
        f"{[sprprj.frame_ticks(f) for f in action.frames]}"
    )
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.spriteview", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="write the viewer pages")
    c = sub.add_parser("batch", help="load, render and check every project")
    s = sub.add_parser("strip", help="export the frames of one action as a PNG strip")
    for p in (b, c, s):
        p.add_argument("--app", type=Path, default=DEFAULT_APP, help="MapleThief.app directory")
    b.add_argument("--out", type=Path, default=DEFAULT_OUT)
    for p in (b, c):
        p.add_argument("--projects", nargs="+", metavar="P", help="mob/3 or a folder (hero)")
    s.add_argument("project", help="<folder>/<n>, e.g. hero/0")
    s.add_argument("action", type=int)
    s.add_argument("--out", type=Path, required=True)
    s.add_argument("--flip", action="store_true", help="flip the whole action")
    s.add_argument("--gap", type=int, default=1, help="transparent columns between frames")
    s.add_argument("--guides", action="store_true", help="anchor cross behind each frame")
    s.add_argument("--background", type=_color, help="fill colour RRGGBB behind each frame")
    s.add_argument("--scale", type=int, default=1, help="nearest-neighbour enlargement")
    args = parser.parse_args(argv)
    if not args.app.is_dir():
        print(f"error: {args.app}: not a directory (run scripts/bootstrap)", file=sys.stderr)
        return 1
    if args.cmd == "strip":
        if args.gap < 0 or not 1 <= args.scale <= 16:
            print("error: --gap must be >= 0 and --scale 1..16", file=sys.stderr)
            return 1
        return _strip_cmd(args)
    if args.cmd == "batch":
        report = batch(args.app, args.projects)
        for e in report.errors:
            print(f"error: {e}")
        print(report.summary())
        return 1 if report.errors else 0
    try:
        files, errors = build(args.app, args.out, args.projects)
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    for e in errors:
        print(f"error: {e}", file=sys.stderr)
    print(f"{len(files)} files -> {args.out}, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
