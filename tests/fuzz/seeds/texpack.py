"""Seeds for fuzz_texpack: byte 0 is the upload (bits 0-1: width - 1, bits 2-3: height - 1,
bit 4: the .raw.png name), the rest the pack's PNG for it, 1 to 3 times its size."""

from __future__ import annotations

import pngfixtures as pf

MAX_LEN = 16384


def png_of(w: int, h: int, seed: int) -> bytes:
    rgba = pf.random_rgba(w, h, seed)
    return pf.make_png(w, h, 6, 8, [rgba[y * w * 4 : (y + 1) * w * 4] for y in range(h)], (1,))


def seeds() -> dict[str, bytes]:
    out = {}
    for w, h, k, raw in (
        (1, 1, 1, False),
        (2, 2, 2, False),
        (4, 3, 3, True),
        (3, 1, 2, False),
        (2, 4, 1, True),
    ):
        b0 = (w - 1) | (h - 1) << 2 | (16 if raw else 0)
        out[f"{w}x{h}-k{k}{'-raw' if raw else ''}"] = bytes([b0]) + png_of(k * w, k * h, w * h)
    out["wrong-size"] = bytes([0x05]) + png_of(3, 3, 9)  # 2 x 2: 3 is no multiple
    out["cgbi"] = bytes([0x05]) + pf.make_cgbi(4, 4, pf.random_rgba(4, 4, 4))
    return out
