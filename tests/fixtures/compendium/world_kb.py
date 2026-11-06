"""A synthetic world for the World section tests (#221): two areas, nine maps, portals (one
hidden), an NPC warp, conditional warps, a boss event, an unused Monster Carnival round and
maps no warp reaches.

Every name and number is made up; nothing comes from the game.
"""

from __future__ import annotations

import copy


def _n(en: str, ko: str) -> dict:
    return {"en": en, "ko": ko}


def _ref(kind: str, eid: str, **kw) -> dict:
    return {"kind": kind, "id": eid, **kw}


def _map(
    mid: str,
    en: str,
    ko: str,
    *,
    portals=(),
    exits=(),
    entrances=(),
    npcs=(),
    spawns=(),
    events=(),
    boss=False,
    no_save=None,
    name_source="spot",
    name_note=None,
    sound=7,
    resume=True,
) -> dict:
    a, m = (int(x) for x in mid.split("-"))
    return {
        "id": mid,
        "kind": "map",
        "name": _n(en, ko),
        "area": [_ref("area", str(a))],
        "area_no": a,
        "map_no": m,
        "width": 40,
        "height": 20,
        "bgm": {"sound": sound, "resume": resume},
        "boss_map": boss,
        "no_save": no_save,
        "name_source": name_source,
        "name_note": name_note,
        "banner": True,
        "portals": list(portals),
        "exits": list(exits),
        "entrances": list(entrances),
        "npcs": list(npcs),
        "spawns": list(spawns),
        "events": list(events),
    }


def _portal(number: int, script, targets=(), hidden=False, tile=(1, 2)) -> dict:
    return {
        "number": number,
        "hidden": hidden,
        "script": script,
        "tile": list(tile),
        "targets": [{"script": s, "map": m, "portal": p, "gated": g} for s, m, p, g in targets],
    }


def _exit(mid: str, portal, script: str, target_portal=0, gated=False) -> dict:
    return _ref("map", mid, portal=portal, script=script, target_portal=target_portal, gated=gated)


QUEST_COND = [
    [
        {
            "native": "getQuestState",
            "args": [0, 1],
            "quest": [0, 1],
            "op": "==",
            "value": 1,
            "term": "getQuestState(0, 1)",
            "text": "main quest 1 is in progress",
        }
    ]
]
BOSS_COND = [
    [
        {
            "native": "checkRequire",
            "args": [0, 1],
            "op": "==",
            "value": 0,
            "term": "checkRequire(0, 1)",
            "text": "checkRequire(0, 1) == 0",
        }
    ]
]
ANSWER_COND = [
    [{"answer": 1, "op": "==", "value": 1, "term": "answer#1", "text": "answer#1 == 1"}],
    [
        {"answer": 1, "op": "==", "value": 2, "term": "answer#1", "text": "answer#1 == 2"},
        {
            "native": "getLevel",
            "args": [],
            "op": ">=",
            "value": 20,
            "term": "getLevel()",
            "text": "level >= 20",
        },
    ],
]
TICKET_COND = [
    [
        {
            "native": "checkItemCount",
            "args": [1, 5],
            "op": ">=",
            "value": 1,
            "term": "checkItemCount(1, 5)",
            "text": "checkItemCount(1, 5) >= 1",
        }
    ]
]


def _warp(
    wid: str,
    script: str,
    target: str,
    tportal,
    *,
    source=(),
    chapters=(1,),
    conditions=None,
    chapter_end=False,
    used=True,
    npc=None,
) -> dict:
    conds = conditions if conditions is not None else [[]]
    src = [_ref("map", m, role=r, portal=p) for m, r, p in source]
    return {
        "id": wid,
        "kind": "warp",
        "name": _n(f"{script} to {target}", f"{script} to {target}"),
        "used": used,
        "chapters": list(chapters),
        "function": "main",
        "offset": 1,
        "target_map": target,
        "target_portal": tportal,
        "conditions": conds,
        "conditional": conds != [[]],
        "chapter_end": chapter_end,
        "from": [
            {"map": m, "role": r, "portal": p, "npc": npc if r == "npc" else None}
            for m, r, p in source
        ],
        "script": [_ref("script", script)],
        "source": src,
        "target": [_ref("map", target, portal=tportal)],
    }


WARPS = [
    _warp(
        "common-portal280-1",
        "common-portal280",
        "2-0",
        1,
        source=[("2-8", "portal", 0)],
        chapters=(0,),
        conditions=QUEST_COND,
        chapter_end=True,
    ),
    _warp("common-portal200-1", "common-portal200", "2-1", 0, source=[("2-0", "portal", 0)]),
    _warp("common-portal210-1", "common-portal210", "2-0", 0, source=[("2-1", "portal", 0)]),
    _warp(
        "chapter1-portal211-1",
        "chapter1-portal211",
        "3-8",
        3,
        source=[("2-1", "portal", 1)],
        conditions=BOSS_COND,
    ),
    _warp("common-portal213-1", "common-portal213", "2-2", 0, source=[("2-1", "portal", 3)]),
    _warp("common-portal220-1", "common-portal220", "2-1", 3, source=[("2-2", "portal", 0)]),
    _warp(
        "chapter1-Ferry-1",
        "chapter1-Ferry",
        "3-0",
        0,
        source=[("2-0", "npc", None)],
        conditions=ANSWER_COND,
        npc="1",
    ),
    _warp("common-portal300-1", "common-portal300", "2-0", 0, source=[("3-0", "portal", 0)]),
    _warp(
        "common-Juggler-1",
        "common-Juggler",
        "3-6",
        0,
        chapters=(),
        conditions=TICKET_COND,
        used=False,
    ),
]

MAPS = [
    _map(
        "2-8",
        "Training Glade",
        "훈련숲",
        portals=[_portal(0, "portal280", [("common/portal280", "2-0", 1, True)])],
        exits=[_exit("2-0", 0, "common/portal280", 1, True)],
        events=[_ref("event", "common-portal280-chapter_end")],
        no_save="tutorial",
        name_source="default slot",
        name_note="the slot is past the end of the row (synthetic)",
        sound=5,
        resume=False,
    ),
    _map(
        "2-0",
        "Mossy Hollow",
        "이끼골",
        portals=[_portal(0, "portal200", [("common/portal200", "2-1", 0, False)])],
        exits=[_exit("2-1", 0, "common/portal200"), _exit("3-0", None, "chapter1/Ferry", 0, True)],
        entrances=[_exit("2-1", 0, "common/portal210"), _exit("3-0", 0, "common/portal300")],
        npcs=[_ref("npc", "1", script="Ferry", tile=[5, 6])],
        sound=5,
        resume=False,
    ),
    _map(
        "2-1",
        "Dripping Path",
        "물길",
        portals=[
            _portal(0, "portal210", [("common/portal210", "2-0", 0, False)]),
            _portal(1, "portal211", [("chapter1/portal211", "3-8", 3, True)]),
            _portal(3, "portal213", [("common/portal213", "2-2", 0, False)], hidden=True),
        ],
        exits=[
            _exit("2-0", 0, "common/portal210"),
            _exit("3-8", 1, "chapter1/portal211", 3, True),
            _exit("2-2", 3, "common/portal213"),
        ],
        entrances=[_exit("2-0", 0, "common/portal200"), _exit("2-2", 0, "common/portal220", 3)],
        spawns=[_ref("monster", "1", count=4)],
    ),
    _map(
        "2-2",
        "Secret Grotto",
        "비밀동굴",
        portals=[_portal(0, "portal220", [("common/portal220", "2-1", 3, False)])],
        exits=[_exit("2-1", 0, "common/portal220", 3)],
        entrances=[_exit("2-1", 3, "common/portal213")],
    ),
    _map(
        "3-0",
        "Pier Town",
        "부두마을",
        portals=[_portal(0, "portal300", [("common/portal300", "2-0", 0, False)])],
        exits=[_exit("2-0", 0, "common/portal300")],
        entrances=[_exit("2-0", None, "chapter1/Ferry", 0, True)],
        sound=6,
        resume=False,
    ),
    _map(
        "3-8",
        "Gloom Altar",
        "어둠제단",
        portals=[_portal(3, None, hidden=True)],
        entrances=[_exit("2-1", 1, "chapter1/portal211", 3, True)],
        spawns=[_ref("monster", "9", count=1)],
        events=[_ref("event", "chapter1-GloomMap-boss")],
        boss=True,
        no_save="boss",
        sound=34,
    ),
    _map("3-5", "Sealed Room", "봉인방"),
    _map(
        "3-6",
        "Pier Town",
        "부두마을",
        no_save="Monster Carnival",
        name_source="default slot",
        name_note="the slot is past the end of the row (synthetic)",
    ),
]

AREAS = [
    {
        "id": "2",
        "kind": "area",
        "name": _n("Mossy Vale", "이끼골짜기"),
        "maps": [_ref("map", m["id"]) for m in MAPS if m["area_no"] == 2],
        "world_spot": 1,
        "town_bgm": 5,
        "field_bgm": 7,
    },
    {
        "id": "3",
        "kind": "area",
        "name": _n("Pier Coast", "부두해안"),
        "maps": [_ref("map", m["id"]) for m in MAPS if m["area_no"] == 3],
        "world_spot": 2,
        "town_bgm": 6,
        "field_bgm": 7,
    },
]

EVENTS = [
    {
        "id": "chapter1-GloomMap-boss",
        "kind": "event",
        "name": _n("chapter1/GloomMap: boss", "chapter1/GloomMap: boss"),
        "type": "boss",
        "args": [],
        "used": True,
        "chapters": [1],
        "conditions": BOSS_COND,
        "maps": [_ref("map", "3-8")],
        "script": [_ref("script", "chapter1-GloomMap")],
        "trigger": ["map"],
    },
    {
        "id": "common-portal280-chapter_end",
        "kind": "event",
        "name": _n("common/portal280: chapter end", "common/portal280: chapter end"),
        "type": "chapter_end",
        "args": [],
        "used": True,
        "chapters": [0],
        "conditions": QUEST_COND,
        "maps": [_ref("map", "2-8")],
        "script": [_ref("script", "common-portal280")],
        "trigger": ["portal"],
    },
    {
        "id": "common-Juggler-carnival-0",
        "kind": "event",
        "name": _n("common/Juggler: carnival", "common/Juggler: carnival"),
        "type": "carnival",
        "args": [0],
        "used": False,
        "chapters": [],
        "conditions": TICKET_COND,
        "maps": [],
        "script": [_ref("script", "common-Juggler")],
        "trigger": [],
    },
]


def _script(sid: str, used=True, events=(), reason=None) -> dict:
    return {
        "id": sid,
        "kind": "script",
        "name": _n(sid.replace("-", "/", 1), sid.replace("-", "/", 1)),
        "used": used,
        "unused_reason": reason,
        "events": [_ref("event", e) for e in events],
        "warps": [_ref("warp", w["id"]) for w in WARPS if w["script"][0]["id"] == sid],
    }


SCRIPTS = [
    _script(s)
    for s in sorted({w["script"][0]["id"] for w in WARPS} - {"common-Juggler"})
    if s != "common-portal280"
] + [
    _script("common-portal280", events=["common-portal280-chapter_end"]),
    _script("chapter1-GloomMap", events=["chapter1-GloomMap-boss"]),
    _script(
        "common-Juggler",
        used=False,
        events=["common-Juggler-carnival-0"],
        reason="no map object names it",
    ),
]

KB = {
    "area": AREAS,
    "map": MAPS,
    "warp": WARPS,
    "event": EVENTS,
    "script": SCRIPTS,
    "quest": [
        {"id": "0-main-1", "kind": "quest", "name": _n("First Steps", "첫걸음"), "chapter": 0},
        {"id": "1-main-1", "kind": "quest", "name": _n("Gloom Rising", "어둠"), "chapter": 1},
    ],
    "npc": [
        {
            "id": "1",
            "kind": "npc",
            "name": _n("Old Ferryman", "뱃사공"),
            "stands_on": [_ref("map", "2-0", script="Ferry", tile=[5, 6])],
            "opens_shop": [_ref("shop", "1", map="2-0", script="common-Ferry", chapters=[1])],
            "gives_quests": [_ref("quest", "1-main-1", map="2-0", script="chapter1-Ferry")],
        }
    ],
    "shop": [{"id": "1", "kind": "shop", "name": _n("Dockside Goods", "부두상점")}],
    "monster": [
        {"id": "1", "kind": "monster", "name": _n("Puddle Slime", "웅덩이"), "level": 3},
        {"id": "9", "kind": "monster", "name": _n("Gloomcap", "어둠버섯"), "level": 30},
    ],
    "item": [{"id": "etc:5", "kind": "item", "name": _n("Juggling Ticket", "곡예표")}],
}

MANIFEST = {
    "schema": 1,
    "images": {
        "map:2-1": [
            {"path": "map/2-1.png", "w": 40, "h": 20, "alt": "Map 2-1", "role": "render"},
            {"path": "map/2-1.svg", "w": 40, "h": 20, "alt": "Map 2-1 marks", "role": "overlay"},
            {"path": "map/2-1@2x.png", "w": 80, "h": 40, "alt": "Map 2-1", "role": "render2x"},
        ]
    },
}


def world_kb() -> dict:
    return copy.deepcopy(KB)
