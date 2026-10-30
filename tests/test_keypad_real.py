"""The hit-mask table of docs/controls/keypad.md against the shipped KeyPadArea.png (#66).

The table parse runs everywhere; the comparison with the image is skipped when work/ipa/ is
absent (CI): run scripts/bootstrap first.
"""

from __future__ import annotations

import re
from collections import Counter

import pytest

from mtport import cgbi
from repo_paths import IPA_DIR, REPO

DOC = REPO / "docs" / "controls" / "keypad.md"
MASK = IPA_DIR / "Payload" / "MapleThief.app" / "KeyPadArea.png"
SCREEN_H = 320

ROW = re.compile(
    r"\| \((\d+), (\d+), (\d+)\) \| ([\w ]+?) \((\d+)\) \| (\d+)–(\d+) \| (\d+)–(\d+) \| (\d+) \|"
)

# Panel 0 key images (initKeyPadType0:), screen points (x, y, w, h), and the pad type of the
# mask region each stands on.
BUTTONS = {
    "jump": ((335, 277, 70, 34), 10),
    "attack": ((405, 277, 70, 34), 11),
    "short 1": ((446, 224, 34, 31), 20),
    "short 2": ((446, 178, 34, 31), 21),
    "short 3": ((446, 132, 34, 31), 22),
    "short 4": ((446, 86, 34, 31), 23),
}


def _doc_table() -> dict[tuple[int, int, int], tuple[str, int, tuple[int, int, int, int], int]]:
    """Colour -> (key, pad type, (x0, x1, y0, y1) screen box, pixel count)."""
    text = DOC.read_text(encoding="utf-8")
    block = text.split("<!-- keypad-mask-table -->")[1].split("<!-- /keypad-mask-table -->")[0]
    out = {}
    for m in ROW.finditer(block):
        r, g, b, key, kind, x0, x1, y0, y1, n = m.groups()
        box = (int(x0), int(x1), int(y0), int(y1))
        out[(int(r), int(g), int(b))] = (key, int(kind), box, int(n))
    return out


def test_doc_table_parses():
    table = _doc_table()
    assert len(table) == 10
    assert sorted(kind for _, kind, _, _ in table.values()) == [1, 2, 3, 4, 10, 11, 20, 21, 22, 23]


def test_buttons_stand_on_their_mask_region():
    by_kind = {kind: box for _, kind, box, _ in _doc_table().values()}
    for name, ((x, y, w, h), kind) in BUTTONS.items():
        x0, x1, y0, y1 = by_kind[kind]
        assert x0 <= x and x + w - 1 <= x1, name
        assert y0 <= y and y + h - 1 <= y1, name


@pytest.mark.real_assets
def test_mask_matches_the_doc():
    img = cgbi.decode(MASK.read_bytes(), MASK.name, premultiplied=True)
    assert (img.width, img.height) == (480, SCREEN_H)
    counts: Counter = Counter()
    boxes: dict[tuple[int, int, int], list[int]] = {}
    for y in range(img.height):
        for x in range(img.width):
            i = (y * img.width + x) * 4
            r, g, b, a = img.rgba[i : i + 4]
            if a == 0:
                continue
            assert a == 255, (x, y)
            px = (r, g, b)
            counts[px] += 1
            # CGBitmapContext is bottom-up: file row y is screen row 319 - y.
            sy = SCREEN_H - 1 - y
            box = boxes.setdefault(px, [x, x, sy, sy])
            box[0], box[1] = min(box[0], x), max(box[1], x)
            box[2], box[3] = min(box[2], sy), max(box[3], sy)
    found = {px: (tuple(boxes[px]), counts[px]) for px in counts}
    want = {px: (box, n) for px, (_, _, box, n) in _doc_table().items()}
    assert found == want
