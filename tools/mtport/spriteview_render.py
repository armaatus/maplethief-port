"""The pixel half of :mod:`mtport.spriteview`: loading a sprite project and its images,
compositing every frame of an action onto one canvas per action, the anchor check, the
per-project data and the frame strips. The module docstring of
``tools/mtport/spriteview.py`` describes the drawing rules; that module re-exports every
name defined here.
"""

from __future__ import annotations

import dataclasses
import json
import re
from array import array
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from mtport import cgbi, png, sprprj
from mtport.bytereader import FormatError

VIEWER = "sprites"
SPRITE_DIR = "sprite"


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
