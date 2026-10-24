"""Parser and writer for ``.sprprj`` sprite projects (spec: docs/formats/sprprj.md).

A sprite project lists the PNGs of one sprite folder, each optionally with a replacement
palette, followed by its actions (animations). An action is a list of frames, a frame
shows for a number of game ticks and is a stack of layers, and a layer places one image
at a signed offset with an alpha, a horizontal flip and a visibility flag. The game reads
the file in ``CMSprite``'s loader at 0x20005. All integers are little-endian. The input is
untrusted data, so every count and length is bounds-checked and trailing bytes are an
error.

Layout::

    u8 image_count
    image_count x { u8 name_len; char name[name_len];          PNG in sprite/<dir>/res/
                    u32 palette_len; u8 palette[palette_len]; }  replaces the PNG's PLTE data
    u8 action_count
    action_count x { u8 frame_count
                     frame_count x { i16 delay; u8 layer_count       delay in game ticks
                                     layer_count x { u8 image; u8 alpha; u8 hflip;
                                                     u8 visible; i16 x; i16 y; } } }

Command line::

    python3 -m mtport.sprprj dump  FILE      # images, actions, frames and layers
    python3 -m mtport.sprprj check DIR...    # parse every *.sprprj below DIR, check PNGs
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections.abc import Iterator, Sequence
from dataclasses import dataclass
from pathlib import Path

from mtport.bytereader import ByteReader, FormatError

SUFFIX = ".sprprj"
RES_DIR = "res"

# CMFrame bounds start from these sentinels in 0x1E77D (min = 10000, max = -10000).
BOUNDS_MIN_START = 10000
BOUNDS_MAX_START = -10000

_U32 = struct.Struct("<I")
_FRAME = struct.Struct("<hB")  # i16 delay, u8 layer_count
_LAYER = struct.Struct("<BBBBhh")  # u8 image, alpha, hflip, visible; i16 x, y


# Parse errors are the shared FormatError (mtport.bytereader); the old name stays for callers.
SprprjError = FormatError


@dataclass(frozen=True)
class Image:
    """One entry of the image list: a PNG file name and an optional palette."""

    name: str
    # None when palette_len is 0. Otherwise raw RGB triples that the engine copies over the
    # PNG's PLTE chunk data before decoding (CMImage at 0xB9F1), which recolours a shared PNG.
    palette: bytes | None = None


@dataclass(frozen=True)
class Layer:
    """One image placed in a frame (``CMLayer``, ctor 0x1E715)."""

    image: int  # index into SpriteProject.images
    alpha: int  # 0-255
    hflip: bool  # draw this layer mirrored horizontally
    visible: bool  # drawn and counted in the frame bounds only when set
    x: int
    y: int


@dataclass(frozen=True)
class Frame:
    """One animation frame (``CMFrame``, ctor 0x1F7A1)."""

    delay: int  # ticks the frame stays on screen; values below 2 mean one tick
    layers: tuple[Layer, ...]


@dataclass(frozen=True)
class Action:
    """One animation (``CMAction``, ctor 0x1FD9D)."""

    frames: tuple[Frame, ...]


@dataclass(frozen=True)
class SpriteProject:
    images: tuple[Image, ...]
    actions: tuple[Action, ...]

    def frames(self) -> Iterator[tuple[int, int, Frame]]:
        """Every ``(action index, frame index, frame)``."""
        for a, action in enumerate(self.actions):
            for f, frame in enumerate(action.frames):
                yield a, f, frame


def _check_name(raw: bytes, where: str) -> str:
    # The name is joined into a path ("/sprite/%s/res/%s"), so keep it a plain file name.
    try:
        name = raw.decode("ascii")
    except UnicodeDecodeError:
        raise SprprjError(f"{where}: image name {raw!r} is not ASCII") from None
    if not name or "\0" in name or "/" in name or "\\" in name or name in (".", ".."):
        raise SprprjError(f"{where}: unsafe image name {raw!r}")
    return name


def parse(data: bytes, name: str = "<sprprj>") -> SpriteProject:
    """Parse a whole ``.sprprj`` file; raises SprprjError on any malformation."""
    r = ByteReader(data, name)
    images = []
    for i in range(r.u8("image_count")):
        image_name = _check_name(r.pstr(f"image {i} name"), f"image {i}")
        palette_len = r.u32(f"image {i} palette_len")
        palette = r.raw(palette_len, f"image {i} palette") if palette_len else None
        images.append(Image(image_name, palette))

    actions = []
    for a in range(r.u8("action_count")):
        frames = []
        for f in range(r.u8(f"action {a} frame_count")):
            delay = r.i16(f"action {a} frame {f} delay")
            layers = []
            for k in range(r.u8(f"action {a} frame {f} layer_count")):
                where = f"action {a} frame {f} layer {k}"
                image, alpha, hflip, visible = (r.u8(where) for _ in range(4))
                x, y = r.i16(where), r.i16(where)
                if image >= len(images):
                    raise r.error(f"{where}: image {image} out of range ({len(images)})")
                layers.append(Layer(image, alpha, hflip != 0, visible != 0, x, y))
            frames.append(Frame(delay, tuple(layers)))
        actions.append(Action(tuple(frames)))

    r.expect_end("sprite project")
    return SpriteProject(tuple(images), tuple(actions))


def build(project: SpriteProject) -> bytes:
    """Serialize ``project``; ``parse(build(p)) == p`` for every valid project."""

    def count(n: int, what: str) -> bytes:
        if not 0 <= n <= 0xFF:
            raise SprprjError(f"{what} {n} does not fit in a u8")
        return bytes([n])

    out = bytearray(count(len(project.images), "image count"))
    for image in project.images:
        name = _check_name(image.name.encode("ascii", "replace"), "build")
        out += count(len(name), "name length") + name.encode("ascii")
        palette = image.palette or b""
        out += _U32.pack(len(palette)) + palette
    out += count(len(project.actions), "action count")
    for action in project.actions:
        out += count(len(action.frames), "frame count")
        for frame in action.frames:
            out += _FRAME.pack(frame.delay, len(frame.layers))
            for layer in frame.layers:
                out += _LAYER.pack(
                    layer.image, layer.alpha, layer.hflip, layer.visible, layer.x, layer.y
                )
    return bytes(out)


def load(path: str | Path) -> SpriteProject:
    return parse(Path(path).read_bytes(), str(path))


# -- engine semantics -------------------------------------------------------------------


@dataclass(frozen=True)
class Bounds:
    """Frame bounding box as CMFrame stores it (0x1E77D, fields +0x10..+0x24)."""

    min_x: int
    max_x: int
    min_y: int
    max_y: int

    @property
    def width(self) -> int:
        return self.max_x - self.min_x

    @property
    def height(self) -> int:
        return self.max_y - self.min_y


def frame_bounds(frame: Frame, sizes: Sequence[tuple[int, int]]) -> Bounds:
    """The box around the frame's visible layers, given each image's ``(width, height)``.

    Mirrors 0x1E77D: hidden layers are skipped, and a frame with no visible layer keeps
    the sentinels (min 10000, max -10000). Flips do not change the box.
    """
    min_x = min_y = BOUNDS_MIN_START
    max_x = max_y = BOUNDS_MAX_START
    for layer in frame.layers:
        if not layer.visible:
            continue
        width, height = sizes[layer.image]
        min_x = min(min_x, layer.x)
        max_x = max(max_x, layer.x + width)
        min_y = min(min_y, layer.y)
        max_y = max(max_y, layer.y + height)
    return Bounds(min_x, max_x, min_y, max_y)


class ActionPlayer:
    """The playback state of one CMAction, advanced once per game tick.

    Mirrors the CMAction methods: reset (0x1E3F5 / 0x1E439), update (0x1E585) and the
    clamp it ends with (0x1E509). Actions never wrap: the last frame holds and ``ended``
    becomes true, and the owner resets the action to loop it.
    """

    def __init__(self, action: Action, use_delay: bool = True) -> None:
        if not action.frames:
            raise ValueError("an action needs at least one frame to play")
        self.action = action
        self.use_delay = use_delay  # +0x1C; when false every frame lasts one tick
        self.reset()

    def reset(self) -> None:
        self.frame = 0  # +0x0C current frame index
        self.ticks = 0  # +0x10 ticks since reset
        self.hold = 1  # +0x18 ticks the current frame has been shown
        self.ended = len(self.action.frames) == 1  # +0x1E

    def update(self) -> None:
        """One game tick (0x1E585)."""
        self.ticks += 1
        if self.use_delay:
            if self.action.frames[self.frame].delay > self.hold:
                self.hold += 1
                return
            self.hold = 1
        self.frame += 1
        last = len(self.action.frames) - 1
        self.ended = False
        if self.frame > last:
            self.frame = last
            self.ended = True


def frame_ticks(frame: Frame) -> int:
    """How many updates the frame stays current while the action plays (at least 1)."""
    return max(frame.delay, 1)


def action_ticks(action: Action) -> int:
    """Updates from reset until the action reports ``ended`` (0 for a 1-frame action).

    ``ended`` is set by the update that tries to step past the last frame, so the last
    frame's delay counts too.
    """
    if len(action.frames) <= 1:
        return 0
    return sum(frame_ticks(f) for f in action.frames)


# -- shipped files ----------------------------------------------------------------------


def missing_images(project: SpriteProject, res_dir: Path) -> list[str]:
    """Names in the image list with no file in ``res_dir``."""
    return [image.name for image in project.images if not (res_dir / image.name).is_file()]


def find_projects(root: Path) -> list[Path]:
    return sorted(root.rglob(f"*{SUFFIX}"))


def _dump(project: SpriteProject) -> None:
    for i, image in enumerate(project.images):
        palette = f" palette {len(image.palette) // 3} colours" if image.palette else ""
        print(f"image {i}: {image.name}{palette}")
    for a, action in enumerate(project.actions):
        print(f"action {a}: {len(action.frames)} frames, {action_ticks(action)} ticks")
        for f, frame in enumerate(action.frames):
            print(f"  frame {f}: delay {frame.delay}")
            for layer in frame.layers:
                flags = ("h" if layer.hflip else "-") + ("v" if layer.visible else "-")
                print(
                    f"    image {layer.image:3d} at ({layer.x:4d},{layer.y:4d}) "
                    f"alpha {layer.alpha:3d} {flags}"
                )


def _check(dirs: Sequence[str]) -> int:
    failures = 0
    totals = {"files": 0, "images": 0, "actions": 0, "frames": 0}
    for d in dirs:
        for path in find_projects(Path(d)):
            totals["files"] += 1
            try:
                project = load(path)
            except SprprjError as e:
                print(f"FAIL {path}: {e}")
                failures += 1
                continue
            missing = missing_images(project, path.parent / RES_DIR)
            if missing:
                print(f"FAIL {path}: missing {', '.join(missing)}")
                failures += 1
            totals["images"] += len(project.images)
            totals["actions"] += len(project.actions)
            totals["frames"] += sum(len(a.frames) for a in project.actions)
    print(" ".join(f"{k}={v}" for k, v in totals.items()), f"failures={failures}")
    return 1 if failures else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.sprprj", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("dump", help="print one file").add_argument("file")
    sub.add_parser("check", help="parse every file below the dirs").add_argument("dirs", nargs="+")
    args = parser.parse_args(argv)
    if args.cmd == "check":
        return _check(args.dirs)
    try:
        _dump(load(args.file))
    except (OSError, SprprjError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
