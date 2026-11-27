"""``mtport.compendium.world map-name`` (#397): one map's knowledge-base name per language,
for the minimap's name check, on a synthetic bundle (made-up names)."""

from __future__ import annotations

import plistlib
from pathlib import Path

from mtport.compendium import world


def _bundle(tmp_path: Path) -> Path:
    app = tmp_path / "MapleThief.app"
    for lang, prefix in (("en", "Spot"), ("ko", "장소")):
        d = app / f"{lang}.lproj"
        d.mkdir(parents=True)
        table = {world.SPOT_KEY.format(n): f"{prefix} {n}" for n in range(1, 68)}
        (d / "ScriptExtra.strings").write_bytes(plistlib.dumps(table, fmt=plistlib.FMT_BINARY))
    return app


def test_map_name_follows_the_spot_tables(tmp_path: Path) -> None:
    src = world.Sources(_bundle(tmp_path))
    # 2.3: Henesys (group 0), index 11 -> key 12.
    assert world.map_name(src, 2, 3) == {"en": "Spot 12", "ko": "장소 12"}
    # Group 1 (Orbis, 4.1 -> index 1 -> key 26) and group 2 (Magatia, 7.0 -> index 1 -> key 58).
    assert world.map_name(src, 4, 1)["en"] == "Spot 26"
    assert world.map_name(src, 7, 0)["en"] == "Spot 58"
    # Past its row's end: the group's entry 0, as the game.
    assert world.map_name(src, 2, 10)["en"] == "Spot 1"
    assert world.map_name(src, 11, 0) == {}
    assert world.map_name(src, 2, 30) == {}


def test_map_name_command(tmp_path: Path) -> None:
    app = _bundle(tmp_path)
    out = tmp_path / "out" / "name.txt"
    assert world.main(["map-name", "--app", str(app), "2", "3", str(out)]) == 0
    assert out.read_text(encoding="utf-8") == "en\tSpot 12\nko\t장소 12\n"
    assert world.main(["map-name", "--app", str(tmp_path / "none"), "2", "3", str(out)]) == 77
    assert world.main(["map-name", "--app", str(app), "11", "0", str(out)]) == 1
