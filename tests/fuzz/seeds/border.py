"""Seeds for fuzz_border: the same synthetic PNGs as fuzz_png."""

from __future__ import annotations

from _pngs import pngs

MAX_LEN = 16384


def seeds() -> dict[str, bytes]:
    return pngs()
