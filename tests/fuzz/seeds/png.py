"""Seeds for fuzz_png: every PNG colour type and depth stb_image takes, interlaced, and CgBI."""

from __future__ import annotations

from _pngs import pngs

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    return pngs()
