"""Real-asset check of the CoreGraphics / UIKit drawing subset (#51, ADR 0036) against the slice.

Skipped when work/ipa/ is absent (CI). Acceptance of #51: every CoreGraphics import of the game
and every UIKit C function (UIGraphics*, UIImagePNGRepresentation, UIRectFill) has an entry in
the runtime's HLE registry, as a function or as a data-object init hook; none is left to the
unimplemented-import handler. UIApplicationMain is the process start-up's (#43). The behaviour
of each entry is tested by tests/cpp/cg_test.cpp (CI, golden images of synthetic drawings) and
tests/cpp/cg_real_test.cpp (local, every PNG of the bundle).

The bundled font (NanumGothic, ADR 0036) must have a glyph for every character of the game's
text: the localized ``Scripts.strings`` / ``ScriptExtra.strings`` (binary plists) and the
string columns of the English and Korean data tables. The font is the pinned download of
cmake/Dependencies.cmake (skipped until a build has fetched it).
"""

from __future__ import annotations

import hashlib
import os
import plistlib
import re
import struct
from pathlib import Path

import pytest

from mtport import bootstrap as bs
from mtport import datatables
from recomp import loader
from recomp.imports import ImportTable
from repo_paths import REPO

pytestmark = pytest.mark.real_assets

SLICE = REPO / bs.slice_path("armv7")
REGISTRY = REPO / "runtime" / "src" / "hle" / "registry.cpp"
CG = "/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics"
UIKIT = "/System/Library/Frameworks/UIKit.framework/UIKit"
CLASS_PREFIXES = ("_OBJC_CLASS_$_", "_OBJC_METACLASS_$_")
APP = REPO / bs.IPA_DIR / "Payload" / "MapleThief.app"
DEPENDENCIES = REPO / "cmake" / "Dependencies.cmake"


def registry_entries() -> dict[str, tuple[str, str]]:
    """name -> (function, init hook) of every registry line."""
    text = REGISTRY.read_text(encoding="utf-8")
    return {
        m[1]: (m[2], m[3]) for m in re.finditer(r'^\s*\{"(\w+)",\s*(\w+),\s*(\w+)\}', text, re.M)
    }


def test_every_coregraphics_and_uikit_drawing_import_has_an_hle_entry():
    table = ImportTable.from_image(loader.Image.load(SLICE))
    cg = [e for e in table.entries if e.library == CG]
    uikit = [
        e for e in table.entries if e.library == UIKIT and not e.name.startswith(CLASS_PREFIXES)
    ]
    functions = {e.name for e in cg + uikit if e.function}
    data = {e.name for e in cg + uikit if not e.function}
    # 37 CG functions + 3 CG constants; UIKit: UIApplicationMain + 9 drawing functions.
    assert (len(functions), len(data)) == (47, 3)
    entries = registry_entries()
    missing = sorted(n for n in functions if entries.get(n, ("nullptr",))[0] == "nullptr")
    assert not missing, missing
    no_hook = sorted(n for n in data if entries.get(n, ("", "nullptr"))[1] == "nullptr")
    assert not no_hook, no_hook
    assert entries["_UIApplicationMain"][0] == "mt_hle_UIApplicationMain"
    assert data == {"_CGAffineTransformIdentity", "_CGRectNull", "_CGRectZero"}


def pinned(name: str) -> str:
    m = re.search(rf"^set\({name} (\w+)\)", DEPENDENCIES.read_text(encoding="utf-8"), re.M)
    assert m, name
    return m[1]


def font_file() -> Path | None:
    """The pinned NanumGothic-Regular.ttf in a download directory, or None."""
    rel = Path(f"nanumgothic-{pinned('MAPLETHIEF_FONT_COMMIT')[:8]}") / "NanumGothic-Regular.ttf"
    dirs = (
        [Path(os.environ["MAPLETHIEF_DOWNLOAD_DIR"])]
        if "MAPLETHIEF_DOWNLOAD_DIR" in os.environ
        else []
    )
    dirs.append(REPO / "build" / "_downloads")
    for d in dirs:
        if (d / rel).is_file():
            return d / rel
    return None


def cmap(font: bytes) -> set[int]:
    """The code points of a TrueType font's format 4 and 12 cmap subtables."""
    (count,) = struct.unpack_from(">H", font, 4)
    tables = {}
    for i in range(count):
        tag, _, offset, _ = struct.unpack_from(">4sIII", font, 12 + 16 * i)
        tables[tag] = offset
    base = tables[b"cmap"]
    (subtables,) = struct.unpack_from(">H", font, base + 2)
    points: set[int] = set()
    for i in range(subtables):
        _, _, offset = struct.unpack_from(">HHI", font, base + 4 + 8 * i)
        sub = base + offset
        (fmt,) = struct.unpack_from(">H", font, sub)
        if fmt == 4:
            (seg2,) = struct.unpack_from(">H", font, sub + 6)
            n = seg2 // 2
            ends = struct.unpack_from(f">{n}H", font, sub + 14)
            starts = struct.unpack_from(f">{n}H", font, sub + 16 + seg2)
            deltas = struct.unpack_from(f">{n}h", font, sub + 16 + 2 * seg2)
            ranges_at = sub + 16 + 3 * seg2
            ranges = struct.unpack_from(f">{n}H", font, ranges_at)
            for k in range(n):
                for c in range(starts[k], ends[k] + 1):
                    if c == 0xFFFF:
                        continue
                    if ranges[k] == 0:
                        glyph = (c + deltas[k]) & 0xFFFF
                    else:
                        at = ranges_at + 2 * k + ranges[k] + 2 * (c - starts[k])
                        (glyph,) = struct.unpack_from(">H", font, at)
                        glyph = (glyph + deltas[k]) & 0xFFFF if glyph else 0
                    if glyph:
                        points.add(c)
        elif fmt == 12:
            (groups,) = struct.unpack_from(">I", font, sub + 12)
            for g in range(groups):
                first, last, glyph = struct.unpack_from(">III", font, sub + 16 + 12 * g)
                points.update(range(first, last + 1) if glyph else range(first + 1, last + 1))
    return points


def game_text() -> set[str]:
    """Every character of the game's localized strings and data-table strings."""
    chars: set[str] = set()
    for strings in sorted(APP.glob("*.lproj/*.strings")):
        for key, value in plistlib.loads(strings.read_bytes()).items():
            chars.update(str(key))
            chars.update(str(value))
    for language in ("English", "Korean"):
        for table in datatables.load_dir(APP / language / "data").values():
            for record in table.records:
                for value in vars(record).values():
                    if isinstance(value, str):
                        chars.update(value)
    return {c for c in chars if c >= " "}


def test_the_bundled_font_covers_the_game_text():
    font_path = font_file()
    if font_path is None:
        pytest.skip("the pinned font is not downloaded yet (configure a CMake preset)")
    font = font_path.read_bytes()
    assert hashlib.sha256(font).hexdigest() == pinned("MAPLETHIEF_FONT_SHA256")
    points = cmap(font)
    text = game_text()
    hangul = {c for c in text if "\uac00" <= c <= "\ud7a3"}
    assert len(text) > 900 and len(hangul) > 700
    missing = sorted(f"U+{ord(c):04X}" for c in text if ord(c) not in points)
    assert not missing, missing
