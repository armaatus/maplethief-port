"""PNG and CgBI seeds from tests/pngfixtures.py (synthetic: nothing from the game)."""

from __future__ import annotations

import pngfixtures as pf


def pngs() -> dict[str, bytes]:
    rgba = pf.random_rgba(4, 3, seed=1)
    rows = [rgba[y * 16 : (y + 1) * 16] for y in range(3)]
    gray = [bytes([0x0F, 0xF0])]  # 16 x 1 at depth 1
    return {
        "rgba8.png": pf.make_png(4, 3, 6, 8, rows, (0, 1, 2, 3, 4)),
        "rgba8-split.png": pf.make_png(4, 3, 6, 8, rows, (4,), idat_parts=3),
        "gray1.png": pf.make_png(16, 1, 0, 1, gray),
        "rgb16.png": pf.make_png(2, 2, 2, 16, [bytes(range(12)), bytes(range(12, 24))]),
        "palette.png": pf.make_png(
            4, 1, 3, 2, [bytes([0b00011011])], palette=bytes(range(12)), trns=b"\x00\x80"
        ),
        "gray-alpha.png": pf.make_png(2, 1, 4, 8, [b"\x10\x20\x30\x40"]),
        "interlaced.png": pf.make_png(4, 3, 6, 8, rows, interlace=1),
        "cgbi.png": pf.make_cgbi(4, 3, rgba),
    }
