"""KB stage: areas and maps (docs/re/map-names.md, docs/formats/obj.md, docs/compendium/kb.md).

Kinds: ``area`` (id ``"<area>"``, one per ``map/<area>.mapprj``) and ``map`` (id
``"<area>-<map>"``, one per ``map/<area><map>.map``, the ``"%d%d"`` stem split with the areas
that exist, :func:`mtport.mapprj.split_map_stem`). From each map's ``.obj``: the NPCs that
stand on it, the monsters it spawns (with the count of spawn points) and its portals. A
portal's destination is the constant ``changeMap(area, map, portal)`` of its script; the
full script analysis (conditions, quests) is the quests stage's (#217).

Names come from ``ScriptExtra.strings`` through tables transcribed from the code below
(``tests/test_compendium_kb_real.py`` checks them against the armv7 slice).

Command line::

    python3 -m mtport.compendium.world names [OUT]   # work/re/names/mapnames.json
"""

from __future__ import annotations

import argparse
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import Any

from mtport import ghidra_common as gc
from mtport import mapfile, mapprj, objfile, scriptout
from mtport.audio_analysis import BOSS_MAPS, FIELD_BGM, TOWN_BGM, map_bgm
from mtport.autosave import NO_SAVE_MAPS
from mtport.compendium.kb import KB, Sources, natural_key, relation

relation("map", "area", "area", "maps")
relation("npc", "stands_on", "map", "npcs")
relation("monster", "spawns_on", "map", "spawns")
relation("map", "exits", "map", "entrances")

# -- the name tables (docs/re/map-names.md) --------------------------------------------------

# CDataMgr_AreaWorldSpot 0x36E65: area -> world spot (index of m_strWorldSpotName, from 0).
AREA_WORLD_SPOT = {0: 0, 1: 1, 2: 3, 3: 2, 4: 4, 5: 4, 6: 6, 7: 6, 8: 6, 9: 5, 10: 5}
# CDataMgr_WorldSpotGroup 0x36EE5: world spot -> spot-name group (0-3 -> 0, 4-5 -> 1, 6 -> 2).
WORLD_SPOT_GROUP = {0: 0, 1: 0, 2: 0, 3: 0, 4: 1, 5: 1, 6: 2}
# The groups CDataMgr::CDataMgr 0x5AE39 fills: char[50][50] each at 0x111A50 + group * 0x9C4,
# from m_strMiddleSpotName.<n>, n numbered on across groups from 1. (count, first n).
SPOT_GROUPS = ((24, 1), (32, 25), (11, 57))
WORLD_SPOTS = 7  # m_strWorldSpotName.1-7 -> char[7][50] at 0x116DA0
WORLD_KEY = "CDataMgr.m_strWorldSpotName.{}"
SPOT_KEY = "CDataMgr.m_strMiddleSpotName.{}"
# CDataMgr_MapSpotIndex 0x36F3D returns int[11][30] at 0x1077E0: [area][map] -> index in the
# area's group. Trailing zeros of each row are left out here (the slot is 0 there too).
SPOT_TABLE_ADDR = 0x1077E0
SPOT_TABLE_SHAPE = (11, 30)
SPOT_TABLE: dict[int, tuple[int, ...]] = {
    0: (0, 1, 2, 14, 13),
    1: (5, 3, 4, 6, 7, 7),
    2: (10, 8, 9, 11, 12, 12),
    3: (17, 15, 16, 18, 19, 20, 21, 22, 23),
    4: (0, 1, 2, 3, 4, 3, 9, 5, 6, 10, 6, 7, 11, 12, 13, 14, 12, 8),
    5: (15, 16, 17, 18),
    6: (),
    7: (1, 4, 2, 3, 5, 3),
    8: (6, 7, 9, 8, 10, 8),
    9: (20, 19, 21, 22, 23, 24, 25, 26, 22),
    10: (27, 28, 29, 30, 30, 31),
}
# CStateGame::Paint 0x87501 adds the map's name to the text list at +0x54 on entering a map
# (CNotifyTextList::Add 0x78339, call at 0x87E28) except on these maps (the comparisons on
# CStateGame +0x84 / +0x88 just before the first call, at 0x87DB0).
BANNER_SKIPPED = frozenset(
    {
        (2, 5),
        (2, 6),
        (2, 7),
        (2, 8),
        (2, 9),
        (4, 8),
        (6, 1),
        (6, 2),
        (7, 5),
        (9, 8),
        (9, 9),
        (9, 10),
        (3, 8),
        (4, 17),
        (9, 5),
        (7, 4),
        (10, 5),
    }
)


def spot(area: int, map_no: int) -> tuple[int, int, bool]:
    """(group, index in the group, explicit) of a map's name. ``explicit`` is False when the
    slot lies past the last non-zero entry of the area's row (and is not map 0): the game
    uses entry 0 of the group (a town: Kerning City Village, Orbis Village or Magatia Village),
    which probably is not the map's own name."""
    group = WORLD_SPOT_GROUP[AREA_WORLD_SPOT[area]]
    row = SPOT_TABLE[area]
    index = row[map_no] if map_no < len(row) else 0
    return group, index, map_no == 0 or map_no < len(row)


def spot_key(group: int, index: int) -> int:
    return SPOT_GROUPS[group][1] + index


NAMES_OUT = "work/re/names/mapnames.json"
# (address, kind, name, comment) for Ghidra (scripts/re-analyze). Function pointers carry the
# Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x36E65", "function", "CDataMgr::AreaWorldSpot", "(area): world spot 0-6, else -1"),
    ("0x36EE5", "function", "CDataMgr::WorldSpotGroup", "(spot): 0-3 -> 0, 4-5 -> 1, 6 -> 2"),
    ("0x36F3D", "function", "CDataMgr::MapSpotIndex", "(area, map): g_mapSpotIndex[area][map]"),
    ("0x1077E0", "data", "g_mapSpotIndex", "int[11][30]: [area][map] -> spot in the group"),
    ("0x111A50", "data", "g_middleSpotNames", "char[3][50][50]: m_strMiddleSpotName by group"),
    ("0x116DA0", "data", "g_worldSpotNames", "char[7][50]: m_strWorldSpotName"),
)

NO_SAVE = {(a, m): why for a, m, why in NO_SAVE_MAPS}


# -- the stage ------------------------------------------------------------------------------


def stage(kb: KB, src: Sources) -> None:
    root = src.app / "map"
    areas = sorted(int(p.stem) for p in root.glob("*.mapprj") if p.stem.isdigit())
    extra = src.script_extra
    for a in areas:
        project = mapprj.load(root / mapprj.project_path(a).split("/", 1)[1])
        ws = AREA_WORLD_SPOT.get(a)
        name, conf = _string(
            kb,
            extra,
            WORLD_KEY.format(ws + 1) if ws is not None else None,
            f"area {a}",
            (f"Area {a}", f"지역 {a}"),
        )
        kb.add(
            "area",
            str(a),
            name,
            {
                "world_spot": (ws, "code"),
                "tilesets": (list(project.tilesets), "code"),
                "backdrop": (mapprj.back_image(a), "code"),
                "town_bgm": (TOWN_BGM.get(a), "code"),
                "field_bgm": (FIELD_BGM.get(a), "code"),
            },
            name_confidence=conf,
        )
    stems = sorted((p.stem for p in root.glob("*.map")), key=natural_key)
    maps = {}
    for stem in stems:
        a, m = mapprj.split_map_stem(stem, frozenset(areas))
        maps[(a, m)] = stem
    for (a, m), stem in sorted(maps.items()):
        _map(kb, src, a, m, stem)
    for (a, m), stem in sorted(maps.items()):
        _exits(kb, src, a, m, stem)


def _string(
    kb: KB, extra: dict, key: str | None, what: str, fallback: tuple[str, str]
) -> tuple[dict[str, str], str]:
    got = {lang: extra[lang].get(key) if key else None for lang in ("en", "ko")}
    if all(got.values()):
        return got, "code"
    kb.problems.append(f"{what}: no ScriptExtra string {key}")
    return {"en": fallback[0], "ko": fallback[1]}, "unknown"


def _map(kb: KB, src: Sources, a: int, m: int, stem: str) -> None:
    root = src.app / "map"
    grid = mapfile.load(root / f"{stem}.map")
    obj = objfile.load(root / f"{stem}.obj")
    ident = f"{a}-{m}"
    name, conf = {"en": f"Area {a} map {m}", "ko": f"지역 {a} 맵 {m}"}, "unknown"
    group = index = None
    explicit = False
    note = "the area has no name table"
    if a in SPOT_TABLE:
        group, index, explicit = spot(a, m)
        name, conf = _string(
            kb,
            src.script_extra,
            SPOT_KEY.format(spot_key(group, index)),
            f"map {ident}",
            (name["en"], name["ko"]),
        )
        note = None
        if not explicit:
            conf = "probable" if conf == "code" else conf
            note = (
                "its slot in the name table is 0 past the end of the area's row, so the "
                "game uses entry 0 of the area's name group, a town; it probably is not the "
                "map's own name"
            )
    source = "spot" if explicit else ("default slot" if a in SPOT_TABLE else "none")
    sound, resume = map_bgm(a, m)
    portals = []
    for r in obj.objects:
        if r.category in ("portal", "hidden_portal"):
            portals.append(
                {
                    "number": r.kind,
                    "hidden": r.category == "hidden_portal",
                    "script": r.name or None,
                    "tile": list(r.tile(grid.width)),
                }
            )
    kb.add(
        "map",
        ident,
        name,
        {
            "area_no": (a, "code"),
            "map_no": (m, "code"),
            "file": (stem, "code"),
            "width": (grid.width, "code"),
            "height": (grid.height, "code"),
            "name_source": (source, "code"),
            "name_note": (note, "probable" if note else "code"),
            "name_spot": (
                {
                    "group": group,
                    "index": index,
                    "key": spot_key(group, index) if group is not None else None,
                },
                "code",
            ),
            "banner": ((a, m) not in BANNER_SKIPPED, "code"),
            "map_script": (obj.map_script, "code"),
            "bgm": ({"sound": sound, "resume": resume}, "code"),
            "boss_map": ((a, m) in BOSS_MAPS, "code"),
            "no_save": (NO_SAVE.get((a, m)), "code"),
            "portals": (portals, "code"),
        },
        name_confidence=conf,
    )
    kb.link("map", ident, "area", "area", str(a))
    spawns: dict[int, int] = {}
    for r in obj.objects:
        if r.category == "npc":
            kb.link(
                "npc",
                str(r.kind - objfile.NPC_KINDS.start),
                "stands_on",
                "map",
                ident,
                script=r.name or None,
                tile=list(r.tile(grid.width)),
            )
        elif r.category == "mob":
            mob = r.kind - (objfile.MOB_KIND_BASE - 1)
            spawns[mob] = spawns.get(mob, 0) + 1
    for mob, count in sorted(spawns.items()):
        kb.link("monster", str(mob), "spawns_on", "map", ident, count=count)


def portal_targets(chunk: scriptout.Chunk) -> tuple[list[tuple[int, int, int]], bool]:
    """The constant ``changeMap(area, map, portal)`` calls of a script, and whether the
    script does anything else (a condition, another call): ``gated``."""
    targets, gated = [], len(chunk.functions) > 1
    for func in chunk.functions:
        if any(i.target is not None for i in chunk.instructions(func)):
            gated = True
        for call in scriptout.calls(chunk, func):
            if (
                call.callee == "changeMap"
                and len(call.args) == 3
                and all(v.kind == "number" for v in call.args)
            ):
                targets.append(tuple(int(v.text) for v in call.args))
            else:
                gated = True
    return targets, gated


def _exits(kb: KB, src: Sources, a: int, m: int, stem: str) -> None:
    ident = f"{a}-{m}"
    entity = kb.get("map", ident)
    for portal in entity["portals"]:
        targets = []
        for script, path in src.script_variants(portal["script"]) if portal["script"] else []:
            found, gated = portal_targets(scriptout.load(path))
            for ta, tm, tp in found:
                targets.append(
                    {"script": script, "map": f"{ta}-{tm}", "portal": tp, "gated": gated}
                )
                kb.link(
                    "map",
                    ident,
                    "exits",
                    "map",
                    f"{ta}-{tm}",
                    portal=portal["number"],
                    target_portal=tp,
                    script=script,
                    gated=gated,
                )
        portal["targets"] = sorted(
            targets, key=lambda t: (natural_key(t["script"]), natural_key(t["map"]), t["portal"])
        )


def name_report(entities: dict[str, dict[str, Any]]) -> list[str]:
    """One line per map whose name is not its own spot name, with the reason."""
    out = []
    for ident in sorted(entities.get("map", {}), key=natural_key):
        e = entities["map"][ident]
        if e.get("name_source") != "spot":
            out.append(
                f"map {ident} ({e['file']}): named {e['name']['en']!r} "
                f"({e['name_source']}): {e['name_note']}"
            )
    return out


def names_json() -> str:
    return gc.names_json("mapnames", NAMES)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.compendium.world", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    args = parser.parse_args(argv)
    try:
        out = Path(args.out)
        gc.write_text(out, names_json())
        print(f"wrote {out} ({len(NAMES)} names)")
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
