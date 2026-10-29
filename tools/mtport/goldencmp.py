"""Whole-screen golden comparison: device screenshots against our frames (#60, ADR 0044).

The Python side of the golden-image helper of ADR 0005 (``tests/golden/golden.h``), for
screens: the same rule (a pixel differs when any of R, G, B, A is off by more than
``channel``; the images match when at most ``ratio`` of the compared pixels differ) and the
same diff colours, plus two things screenshots need:

- **normalisation** (:func:`normalise`): a screenshot from the device becomes the 480x320
  landscape screen of ADR 0037, whatever form iOS saved it in:

  ============  ===================================================================
  480x320       as is (a non-Retina device that saved it landscape)
  320x480       portrait (the window's orientation; iOS saves a portrait window's
                screenshots so): turned onto the screen with ADR 0037's mapping,
                screen pixel (x, y) = window pixel (y, 479 - x), a quarter turn
                clockwise (:func:`portrait_to_screen`)
  960x640       a Retina device (the game is not Retina-aware, so iOS shows it at 2x):
                each 2x2 block averaged into one pixel
  640x960       both: turned, then averaged
  1136x640      a 4-inch device (iPhone 5, iPod touch 5): the 3.5-inch game in the
  640x1136      middle 960x640 (or 640x960) with black bars of 88 pixels, which must
                be black; cropped, then as above
  ============  ===================================================================

  Anything else is rejected with :class:`NormaliseError`. Alpha becomes 255 (the screen is
  opaque).
- **masks** (:class:`Rect`): screen rectangles left out of the comparison, for parts that
  differ for a known reason (text drawn with another font, #118; an animation the
  screenshot caught at another moment). The ratio is taken over the pixels compared.

Command line: ``python3 -m mtport.goldencmp ACTUAL EXPECTED [--channel N] [--ratio R]
[--mask X,Y,W,H]... [--diff OUT.png] [--normalised OUT.png] [--no-normalise]``; exit status
0 if they match, 1 if not, 2 for bad input.
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from . import png

SCREEN_WIDTH = 480
SCREEN_HEIGHT = 320

# The diff colours of golden.h: within the channel tolerance yellow, differing red, outside
# the other image magenta; matching pixels the expected pixel's luma dimmed to 64..127 grey.
# Masked pixels (not in golden.h) are dark blue.
DIFF_WITHIN = (255, 255, 0, 255)
DIFF_DIFFERING = (255, 0, 0, 255)
DIFF_OUTSIDE = (255, 0, 255, 255)
DIFF_MASKED = (0, 0, 128, 255)

# A 4-inch device shows the 3.5-inch game centred, with black bars this wide on each side.
LETTERBOX_BAR = 88
# A bar pixel counts as black up to this channel value.
LETTERBOX_BLACK = 16


class NormaliseError(ValueError):
    """A screenshot that is not of a 480x320 screen in any form the device saves."""


@dataclass(frozen=True)
class Rect:
    """A rectangle in screen pixels: x, y of the top left corner, width, height."""

    x: int
    y: int
    w: int
    h: int

    @classmethod
    def parse(cls, text: str) -> Rect:
        parts = text.split(",")
        try:
            x, y, w, h = (int(p) for p in parts)
        except ValueError:
            raise ValueError(f"a mask is X,Y,W,H in whole pixels, got {text!r}") from None
        return cls.of([x, y, w, h])

    @classmethod
    def of(cls, values: Sequence[int]) -> Rect:
        if len(values) != 4 or not all(isinstance(v, int) for v in values):
            raise ValueError(f"a rectangle is four integers [x, y, w, h], got {values!r}")
        x, y, w, h = values
        if w <= 0 or h <= 0:
            raise ValueError(f"a rectangle needs a positive size, got {values!r}")
        return cls(x, y, w, h)

    def inside(self, width: int, height: int) -> bool:
        return self.x >= 0 and self.y >= 0 and self.x + self.w <= width and self.y + self.h <= height


@dataclass(frozen=True)
class Tolerance:
    """golden.h's Tolerance: per-channel slack and the allowed ratio of differing pixels."""

    channel: int = 0
    ratio: float = 0.0

    def __post_init__(self) -> None:
        if not 0 <= self.channel <= 255:
            raise ValueError(f"channel tolerance must be 0..255, got {self.channel}")
        if not 0.0 <= self.ratio <= 1.0:
            raise ValueError(f"ratio must be 0..1, got {self.ratio}")


@dataclass
class Normalised:
    """A screenshot as the 480x320 screen, and what was done to it."""

    image: png.Image
    source_size: tuple[int, int]
    steps: list[str] = field(default_factory=list)

    @property
    def scale(self) -> int:
        """2 if the screenshot came from a Retina (2x) screen, else 1."""
        return 2 if max(self.source_size) >= 2 * SCREEN_WIDTH else 1


@dataclass
class Comparison:
    matches: bool
    size_matches: bool
    differing: int  # compared pixels beyond the channel tolerance, plus outside pixels
    compared: int  # pixels of the union of both sizes that no mask covers
    masked: int
    max_delta: int  # over the compared pixels both images have
    diff: png.Image
    tolerance: Tolerance

    @property
    def ratio(self) -> float:
        return self.differing / self.compared if self.compared else 0.0

    def summary(self) -> str:
        verdict = "match" if self.matches else "DIFFER"
        size = "" if self.size_matches else ", sizes differ"
        return (
            f"{verdict}: {self.differing} of {self.compared} compared pixels differ "
            f"({self.ratio:.4%}; allowed {self.tolerance.ratio:.4%} beyond {self.tolerance.channel} "
            f"per channel), max channel delta {self.max_delta}, {self.masked} masked{size}"
        )


def _opaque(rgba: bytes | bytearray) -> bytes:
    out = bytearray(rgba)
    out[3::4] = b"\xff" * (len(out) // 4)
    return bytes(out)


def portrait_to_screen(image: png.Image) -> png.Image:
    """A portrait (window-oriented) image turned onto the landscape screen of ADR 0037.

    ``screen_from_window`` maps window point (x, y) to screen point (H - y, x) for a window
    H points tall (LandscapeLeft: the window's top edge on the screen's right). For pixels,
    screen pixel (sx, sy) is window pixel (sy, H - 1 - sx): a quarter turn clockwise. The
    result is ``image.height`` x ``image.width``.
    """
    w, h = image.width, image.height
    src = image.rgba
    out = bytearray(w * h * 4)
    # Screen row sy is window column sy, read from the window's bottom row upwards.
    for sy in range(w):
        row = bytearray(h * 4)
        for sx in range(h):
            o = ((h - 1 - sx) * w + sy) * 4
            row[sx * 4 : sx * 4 + 4] = src[o : o + 4]
        out[sy * h * 4 : (sy + 1) * h * 4] = row
    return png.Image(h, w, bytes(out))


def screen_to_portrait(image: png.Image) -> png.Image:
    """The inverse of :func:`portrait_to_screen` (for tests and for making portrait input)."""
    w, h = image.width, image.height
    src = image.rgba
    out = bytearray(w * h * 4)
    # Window pixel (px, py) is screen pixel (sx = H - 1 - py, sy = px), H = the window height = w.
    for py in range(w):
        for px in range(h):
            o = (px * w + (w - 1 - py)) * 4
            d = (py * h + px) * 4
            out[d : d + 4] = src[o : o + 4]
    return png.Image(h, w, bytes(out))


def downsample2(image: png.Image) -> png.Image:
    """Each 2x2 block averaged into one pixel (rounded half up), per channel."""
    if image.width % 2 or image.height % 2:
        raise NormaliseError(f"cannot halve an odd size {image.width}x{image.height}")
    w, h = image.width // 2, image.height // 2
    src = image.rgba
    stride = image.width * 4
    out = bytearray(w * h * 4)
    for y in range(h):
        r0 = 2 * y * stride
        r1 = r0 + stride
        d = y * w * 4
        for i in range(w * 4):
            c = (i // 4) * 8 + i % 4
            out[d + i] = (src[r0 + c] + src[r0 + c + 4] + src[r1 + c] + src[r1 + c + 4] + 2) >> 2
    return png.Image(w, h, bytes(out))


def crop(image: png.Image, rect: Rect) -> png.Image:
    if not rect.inside(image.width, image.height):
        raise ValueError(f"crop {rect} outside a {image.width}x{image.height} image")
    stride = image.width * 4
    rows = [
        image.rgba[(rect.y + y) * stride + rect.x * 4 : (rect.y + y) * stride + (rect.x + rect.w) * 4]
        for y in range(rect.h)
    ]
    return png.Image(rect.w, rect.h, b"".join(rows))


def _is_black(image: png.Image, rect: Rect) -> bool:
    part = crop(image, rect).rgba
    return all(
        part[i] <= LETTERBOX_BLACK and part[i + 1] <= LETTERBOX_BLACK and part[i + 2] <= LETTERBOX_BLACK
        for i in range(0, len(part), 4)
    )


ACCEPTED = "480x320, 320x480, 960x640, 640x960, 1136x640 or 640x1136"


def normalise(image: png.Image) -> Normalised:
    """The screenshot as the 480x320 landscape screen (see the module docstring)."""
    size = (image.width, image.height)
    result = Normalised(image, size)
    img = image
    if size in ((1136, 640), (640, 1136)):
        landscape = size == (1136, 640)
        bars = (
            (Rect(0, 0, LETTERBOX_BAR, 640), Rect(1136 - LETTERBOX_BAR, 0, LETTERBOX_BAR, 640))
            if landscape
            else (Rect(0, 0, 640, LETTERBOX_BAR), Rect(0, 1136 - LETTERBOX_BAR, 640, LETTERBOX_BAR))
        )
        if not all(_is_black(img, bar) for bar in bars):
            raise NormaliseError(
                f"{size[0]}x{size[1]}: a 4-inch screenshot, but its {LETTERBOX_BAR}-pixel side "
                "bars are not black, so it is not the 3.5-inch game letterboxed"
            )
        inner = Rect(LETTERBOX_BAR, 0, 960, 640) if landscape else Rect(0, LETTERBOX_BAR, 640, 960)
        img = crop(img, inner)
        result.steps.append(f"cropped the 4-inch letterbox to {img.width}x{img.height}")
    if (img.width, img.height) in ((320, 480), (640, 960)):
        img = portrait_to_screen(img)
        result.steps.append(f"turned the portrait screenshot onto the screen ({img.width}x{img.height})")
    if (img.width, img.height) == (960, 640):
        img = downsample2(img)
        result.steps.append("averaged each 2x2 block (Retina 2x)")
    if (img.width, img.height) != (SCREEN_WIDTH, SCREEN_HEIGHT):
        raise NormaliseError(
            f"a {size[0]}x{size[1]} image is no screenshot of the 480x320 game; expected {ACCEPTED}"
        )
    if any(a != 255 for a in img.rgba[3::4]):
        result.steps.append("made it opaque")
    result.image = png.Image(img.width, img.height, _opaque(img.rgba))
    return result


def mask_bitmap(width: int, height: int, masks: Iterable[Rect]) -> bytearray:
    """One byte per pixel: 1 where a mask covers it (masks are clipped to the image)."""
    bits = bytearray(width * height)
    for m in masks:
        x0, y0 = max(0, m.x), max(0, m.y)
        x1, y1 = min(width, m.x + m.w), min(height, m.y + m.h)
        if x0 >= x1:
            continue
        for y in range(y0, y1):
            bits[y * width + x0 : y * width + x1] = b"\x01" * (x1 - x0)
    return bits


def _dimmed(r: int, g: int, b: int) -> tuple[int, int, int, int]:
    v = 64 + (299 * r + 587 * g + 114 * b) // 1000 // 4
    return v, v, v, 255


def compare(
    actual: png.Image,
    expected: png.Image,
    tolerance: Tolerance = Tolerance(),
    masks: Iterable[Rect] = (),
) -> Comparison:
    """golden.h's compare(), with masks: masked pixels are left out of the counts."""
    width = max(actual.width, expected.width)
    height = max(actual.height, expected.height)
    covered = mask_bitmap(width, height, masks)
    diff = bytearray(width * height * 4)
    differing = compared = masked = max_delta = 0
    a_px, e_px = actual.rgba, expected.rgba
    for y in range(height):
        for x in range(width):
            i = y * width + x
            d = i * 4
            if covered[i]:
                masked += 1
                diff[d : d + 4] = bytes(DIFF_MASKED)
                continue
            compared += 1
            if x >= actual.width or y >= actual.height or x >= expected.width or y >= expected.height:
                differing += 1
                diff[d : d + 4] = bytes(DIFF_OUTSIDE)
                continue
            ao = (y * actual.width + x) * 4
            eo = (y * expected.width + x) * 4
            delta = max(abs(a_px[ao + c] - e_px[eo + c]) for c in range(4))
            if delta > max_delta:
                max_delta = delta
            if delta > tolerance.channel:
                differing += 1
                diff[d : d + 4] = bytes(DIFF_DIFFERING)
            elif delta:
                diff[d : d + 4] = bytes(DIFF_WITHIN)
            else:
                diff[d : d + 4] = bytes(_dimmed(e_px[eo], e_px[eo + 1], e_px[eo + 2]))
    size_matches = (actual.width, actual.height) == (expected.width, expected.height)
    matches = size_matches and differing <= tolerance.ratio * compared
    return Comparison(
        matches, size_matches, differing, compared, masked, max_delta,
        png.Image(width, height, bytes(diff)), tolerance,
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.goldencmp",
        description="Compare one screen PNG with another (e.g. our frame with a device screenshot).",
    )
    parser.add_argument("actual", type=Path, help="our frame (480x320)")
    parser.add_argument("expected", type=Path, help=f"the reference, e.g. a device screenshot ({ACCEPTED})")
    parser.add_argument("--channel", type=int, default=0, help="per-channel tolerance (0..255, default 0)")
    parser.add_argument("--ratio", type=float, default=0.0, help="allowed ratio of differing pixels (default 0)")
    parser.add_argument("--mask", action="append", default=[], metavar="X,Y,W,H", help="leave a screen rectangle out (repeatable)")
    parser.add_argument("--diff", type=Path, help="write the diff image here")
    parser.add_argument("--normalised", type=Path, help="write the normalised reference here")
    parser.add_argument("--no-normalise", action="store_true", help="compare the files as they are")
    args = parser.parse_args(argv)
    try:
        tolerance = Tolerance(args.channel, args.ratio)
        masks = [Rect.parse(m) for m in args.mask]
        actual = png.load(args.actual)
        expected = png.load(args.expected)
        if not args.no_normalise:
            n = normalise(expected)
            for step in n.steps:
                print(f"{args.expected}: {step}")
            expected = n.image
            a = normalise(actual)
            for step in a.steps:
                print(f"{args.actual}: {step}")
            actual = a.image
    except (OSError, ValueError) as e:  # PngError and NormaliseError are ValueErrors
        print(f"goldencmp: {e}", file=sys.stderr)
        return 2
    if args.normalised:
        png.save(args.normalised, expected)
    result = compare(actual, expected, tolerance, masks)
    if args.diff:
        png.save(args.diff, result.diff)
    print(result.summary())
    return 0 if result.matches else 1


if __name__ == "__main__":
    sys.exit(main())
