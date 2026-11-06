"""mtport.compendium.kb (#213) on a synthetic bundle: relations stored both ways, skill levels,
unknown ids reported, deterministic output, the check command, map names.

The bundle is built here from the parsers' own encoders: two areas, three maps, a handful of
table rows and portal scripts assembled with tests/test_scriptout.py's assembler.
"""

from __future__ import annotations

import dataclasses
import json
import plistlib
from pathlib import Path

import pytest

from mtport import datatables as dt
from mtport import mapfile, mapprj, objfile
from mtport.compendium import kb as kbm
from mtport.compendium import tables, world
from test_scriptout import chunk


def _row(record: type, **values):
    """A record with every field 0 or "" except ``values``."""
    base = {}
    for f in dataclasses.fields(record):
        base[f.name] = "" if f.metadata["kind"] == "str" else 0
    return record(**{**base, **values})


def _tables(lang: str) -> dict[str, list]:
    ko = lang == "Korean"
    mob = [
        _row(
            dt.Mob,
            id=1,
            name="달팽이" if ko else "Snail",
            hp=20,
            level=8,
            unknown_24=1,
            exp=7,
            meso=150,
            etc_drop_1=1,
            etc_drop_1_chance=930,
            equip_drop=0,
            equip_drop_chance=600,
            sprite=60,
        ),
        _row(
            dt.Mob,
            id=2,
            name="버섯" if ko else "Mushroom",
            hp=80,
            unknown_24=2,
            meso=0,
            etc_drop_2=0,
            etc_drop_2_chance=5,
            sprite=61,
        ),
    ]
    return {
        "hero": [
            _row(dt.HeroLevel, level=n, exp=10 * n, hp_gain=n + 1, mp_gain=2 * n) for n in (1, 2, 3)
        ],
        "mob": mob,
        "npc": [
            _row(dt.Npc, id=0, name="상인" if ko else "Mr. $Lee"),
            _row(dt.Npc, id=1, name="경비" if ko else "Guard"),
        ],
        "item_equip": [
            _row(
                dt.ItemEquip,
                id=0,
                name="모자" if ko else "Hat",
                attack_min=1,
                attack_max=3,
                bonus_stat=4,
                bonus_value=2,
                price=100,
            )
        ],
        "item_etc": [
            _row(dt.ItemEtc, id=0, name="메소" if ko else "Meso (bronze)"),
            _row(
                dt.ItemEtc,
                id=1,
                name="빨간 포션" if ko else "Red Potion",
                type=1,
                hp=50,
                description="회복" if ko else "Recover$HP+50",
            ),
        ],
        "skill": [
            _row(dt.Skill, id=0, name="공격" if ko else "Attack"),
            _row(
                dt.Skill,
                id=1,
                name="럭키 세븐" if ko else "Lucky Seven",
                max_level=3,
                damage_base=30,
                damage_per_level=11,
                mp_base=6,
                mp_per_level=2,
                time_base=5,
                time_per_level=-1,
                detail="MP @mp소비, 데미지 @dmg%" if ko else "Spend MP @mp, @dmg% Damage @time",
            ),
        ],
        "market": [
            _row(dt.MarketItem, shop=0, table=0, item=0, price=170),
            _row(dt.MarketItem, shop=0, table=1, item=1, price=50),
            _row(dt.MarketItem, shop=3, table=1, item=1, price=45),
        ],
    }


def _strings(lang: str) -> bytes:
    ko = lang == "ko"
    table = {}
    for n in range(1, 68):
        table[f"CDataMgr.m_strMiddleSpotName.{n}"] = f"{'장소' if ko else 'Spot'} {n}"
    for n in range(1, 8):
        table[f"CDataMgr.m_strWorldSpotName.{n}"] = f"{'세계' if ko else 'World'} {n}"
    return plistlib.dumps(table, fmt=plistlib.FMT_BINARY)


def _change_map(a: int, m: int, p: int) -> bytes:
    push = [("PUSHBYTE", v) if v > 2 else (f"PUSH{v}",) for v in (a, m, p)]
    return chunk([("PUSHGLOBAL", "changeMap"), *push, ("CALLFUNC", 3, 0), ("RETCODE0",)])


def _gated_change_map(a: int, m: int, p: int) -> bytes:
    push = [("PUSHBYTE", v) if v > 2 else (f"PUSH{v}",) for v in (a, m, p)]
    return chunk(
        [
            ("PUSHGLOBAL", "getQuestState"),
            ("PUSH0",),
            ("PUSH1",),
            ("CALLFUNC", 2, 1),
            ("IFFJMP", 10),
            ("PUSHGLOBAL", "changeMap"),
            *push,
            ("CALLFUNC", 3, 0),
            ("RETCODE0",),
        ]
    )


WIDTH = 10


def make_app(root: Path, *, mutate=None) -> Path:
    app = root / "MapleThief.app"
    for lang in ("English", "Korean"):
        data = app / lang / "data"
        data.mkdir(parents=True)
        rows = _tables(lang)
        if mutate:
            mutate(lang, rows)
        for name, records in rows.items():
            (data / f"{name}.dat").write_bytes(dt.encode(name, records))
    for lang in ("en", "ko"):
        (app / f"{lang}.lproj").mkdir()
        (app / f"{lang}.lproj" / "ScriptExtra.strings").write_bytes(_strings(lang))
        empty = plistlib.dumps({}, fmt=plistlib.FMT_BINARY)
        (app / f"{lang}.lproj" / "Scripts.strings").write_bytes(empty)
    maps = app / "map"
    maps.mkdir()
    for area in (0, 1):
        project = mapprj.MapProject(16, 16, (f"Sheet{area}.png",))
        (maps / f"{area}.mapprj").write_bytes(mapprj.encode(project))
    R = objfile.Record
    objects = {
        "00": [
            R(5, 0, "portal000"),
            R(12, 4 + 0, "Lee"),
            R(13, 4 + 0, "Lee2"),
            R(14, 60, ""),
            R(15, 60, ""),
            R(16, 61, ""),
        ],
        "01": [R(-1, 0xFF, "TestMap"), R(3, 0, "portal010"), R(4, 3, "portal013"), R(21, 60, "")],
        # map 6 of area 0: past the end of area 0's name row (default slot)
        "06": [R(2, 4 + 1, "Guard")],
        "10": [R(1, 0, "portal100")],
    }
    for stem, records in objects.items():
        grid = mapfile.MapFile(WIDTH, 3, ((0,) * (WIDTH * 3),))
        (maps / f"{stem}.map").write_bytes(mapfile.encode(grid))
        (maps / f"{stem}.obj").write_bytes(objfile.encode(objfile.ObjFile(tuple(records))))
    common = app / "script" / "common"
    common.mkdir(parents=True)
    (app / "script" / "chapter1").mkdir()
    (common / "portal000.out").write_bytes(_change_map(0, 1, 0))
    (common / "portal010.out").write_bytes(_change_map(0, 0, 0))
    (common / "portal013.out").write_bytes(_change_map(1, 0, 0))
    (common / "portal100.out").write_bytes(_change_map(0, 1, 3))
    (app / "script" / "chapter1" / "portal100.out").write_bytes(_gated_change_map(0, 6, 0))
    for name in ("Lee", "Lee2", "Guard", "TestMap"):  # the quests stage (#217) reads them
        (common / f"{name}.out").write_bytes(chunk([("RETCODE0",)]))
    return app


@pytest.fixture()
def app(tmp_path: Path) -> Path:
    return make_app(tmp_path)


@pytest.fixture()
def built(app: Path) -> kbm.KB:
    kb = kbm.build(app)
    assert kb.problems == []
    return kb


def test_counts_and_ids(built: kbm.KB) -> None:
    e = built.entities
    assert {k: len(v) for k, v in e.items()} == {
        "area": 2,
        "item": 3,
        "level": 3,
        "listing": 3,
        "map": 4,
        "monster": 2,
        "npc": 2,
        "shop": 2,
        "skill": 2,
        # the quests stage (#217): no intro map in this bundle, so every script is unused
        "script": 9,
        "warp": 5,
    }
    assert sorted(e["item"]) == ["equip:0", "etc:0", "etc:1"]
    assert sorted(e["map"]) == ["0-0", "0-1", "0-6", "1-0"]
    assert e["npc"]["0"]["name"] == {"en": "Mr. Lee", "ko": "상인"}
    assert e["item"]["etc:1"]["description"] == {"en": "Recover\nHP+50", "ko": "회복"}


def test_every_relation_resolves_both_ways(built: kbm.KB, tmp_path: Path) -> None:
    out = tmp_path / "kb"
    kbm.write(built, out)
    loaded = kbm.load_kb(out)
    assert kbm.check_kb(loaded, kbm.load_relations(out)) == []
    rels = kbm.load_relations(out)
    for r in rels:
        for ident, entity in loaded.get(r.kind, {}).items():
            for rf in entity[r.field]:
                back = {**rf, "kind": r.kind, "id": ident}
                assert back in loaded[r.target][rf["id"]][r.inverse], (r, ident, rf)
    # one of each relation, by hand
    mob = loaded["monster"]["1"]
    assert {
        "kind": "item",
        "id": "etc:1",
        "slot": 1,
        "chance": 930,
        "per": 1000,
        "percent": 93.1641,  # 30528 of the 32768 values of the 15-bit Rand_Next
        "nominal_percent": 93.0,
    } in mob["drops"]
    assert {
        "kind": "item",
        "id": "equip:0",
        "slot": 0,
        "chance": 600,
        "per": 100000,
        "percent": 1.8311,  # 600 in 32768, not in 100000
        "nominal_percent": 0.6,
    } in mob["drops"]
    assert loaded["item"]["etc:1"]["dropped_by"][0]["id"] == "1"
    assert mob["meso"] == {"amount": 150, "min": 120, "max": 180, "percent": 80}
    assert [r["id"] for r in mob["meso_drops"]] == ["etc:1"]  # 120..180 -> item_etc 1
    assert loaded["monster"]["2"]["drops"] == [
        {
            "kind": "item",
            "id": "etc:0",
            "slot": 2,
            "chance": 5,
            "per": 1000,
            "percent": 0.5035,
            "nominal_percent": 0.5,
        }
    ]
    assert mob["spawns_on"] == [
        {"kind": "map", "id": "0-0", "count": 2},
        {"kind": "map", "id": "0-1", "count": 1},
    ]
    assert loaded["map"]["0-0"]["spawns"] == [
        {"kind": "monster", "id": "1", "count": 2},
        {"kind": "monster", "id": "2", "count": 1},
    ]
    assert [(r["id"], r["script"]) for r in loaded["map"]["0-0"]["npcs"]] == [
        ("0", "Lee"),
        ("0", "Lee2"),
    ]
    assert loaded["npc"]["0"]["stands_on"][0] == {
        "kind": "map",
        "id": "0-0",
        "script": "Lee",
        "tile": [2, 1],
    }
    assert loaded["shop"]["0"]["sells"] == [
        {"kind": "item", "id": "equip:0", "price": 170, "listing": "0"},
        {"kind": "item", "id": "etc:1", "price": 50, "listing": "1"},
    ]
    assert [r["id"] for r in loaded["item"]["etc:1"]["sold_by"]] == ["0", "3"]
    assert loaded["listing"]["2"]["shop"] == [{"kind": "shop", "id": "3", "price": 45}]
    assert loaded["area"]["0"]["maps"] == [{"kind": "map", "id": i} for i in ("0-0", "0-1", "0-6")]


def test_portal_exits(built: kbm.KB) -> None:
    maps = built.entities["map"]
    assert maps["0-0"]["exits"] == [
        {
            "kind": "map",
            "id": "0-1",
            "portal": 0,
            "target_portal": 0,
            "script": "common/portal000",
            "gated": False,
        }
    ]
    assert {
        "kind": "map",
        "id": "0-0",
        "portal": 0,
        "target_portal": 0,
        "script": "common/portal000",
        "gated": False,
    } in maps["0-1"]["entrances"]
    exits = maps["1-0"]["exits"]
    assert [(x["id"], x["script"], x["gated"]) for x in exits] == [
        ("0-1", "common/portal100", False),
        ("0-6", "chapter1/portal100", True),
    ]
    hidden = [p for p in maps["0-1"]["portals"] if p["hidden"]]
    assert hidden == [
        {
            "number": 3,
            "hidden": True,
            "script": "portal013",
            "tile": [4, 0],
            "targets": [{"script": "common/portal013", "map": "1-0", "portal": 0, "gated": False}],
        }
    ]


def test_skill_levels_are_base_plus_per_level(built: kbm.KB) -> None:
    skill = built.entities["skill"]["1"]
    assert [lv["level"] for lv in skill["levels"]] == [1, 2, 3]
    for lv in skill["levels"]:
        n = lv["level"]
        assert lv["damage"] == 30 + (n - 1) * 11
        assert lv["mp"] == 6 + (n - 1) * 2
        assert lv["time"] == 5 - (n - 1)
        assert lv["detail"]["en"] == f"Spend MP {lv['mp']}, {lv['damage']}% Damage {lv['time']}"
        assert lv["detail"]["ko"] == f"MP {lv['mp']}소비, 데미지 {lv['damage']}%"
    assert built.entities["skill"]["0"]["levels"] == []
    assert built.entities["skill"]["0"]["learnable"] is False


def test_levels_are_cumulative(built: kbm.KB) -> None:
    lv = built.entities["level"]
    assert [lv[str(n)]["exp_total"] for n in (1, 2, 3)] == [0, 10, 30]
    assert [lv[str(n)]["max_hp"] for n in (1, 2, 3)] == [2, 5, 9]
    assert [lv[str(n)]["max_mp"] for n in (1, 2, 3)] == [2, 6, 12]


def test_map_names(built: kbm.KB) -> None:
    maps = built.entities["map"]
    # area 0 -> world spot 0 -> group 0; row 0 = (0, 1, 2, 14, 13): map 1 -> index 1 -> key 2
    assert maps["0-1"]["name"] == {"en": "Spot 2", "ko": "장소 2"}
    assert maps["0-1"]["name_source"] == "spot"
    assert maps["0-1"]["confidence"]["name"] == "code"
    assert maps["0-1"]["map_script"] == "TestMap"
    assert maps["1-0"]["name"]["en"] == "Spot 6"  # row 1 starts with 5
    default = maps["0-6"]
    assert default["name"]["en"] == "Spot 1"
    assert default["name_source"] == "default slot"
    assert default["confidence"]["name"] == "probable"
    assert default["name_note"]
    assert built.entities["area"]["1"]["name"] == {"en": "World 2", "ko": "세계 2"}
    report = world.name_report(built.entities)
    assert len(report) == 1 and report[0].startswith("map 0-6 (06)")


def test_every_field_has_a_confidence(built: kbm.KB) -> None:
    for table in built.entities.values():
        for e in table.values():
            keys = set(e) - {"id", "kind", "confidence"}
            assert keys == set(e["confidence"]), e["id"]
            assert set(e["confidence"].values()) <= set(kbm.CONFIDENCE)


def test_unknown_ids_are_reported(tmp_path: Path) -> None:
    def mutate(lang, rows):
        rows["mob"][0] = dataclasses.replace(rows["mob"][0], etc_drop_3=77, etc_drop_3_chance=10)
        rows["market"].append(_row(dt.MarketItem, shop=1, table=0, item=9, price=5))

    app = make_app(tmp_path, mutate=mutate)
    (app / "map" / "01.obj").write_bytes(
        objfile.encode(
            objfile.ObjFile((objfile.Record(3, 0, "portal010"), objfile.Record(4, 60 + 40, "")))
        )
    )
    (app / "script" / "common" / "portal010.out").write_bytes(_change_map(5, 5, 0))
    kb = kbm.build(app)
    assert sorted(kb.problems) == sorted(
        [
            "monster 1.drops -> item etc:77: no item etc:77",
            "listing 3.item -> item equip:9: no item equip:9",
            "shop 1.sells -> item equip:9: no item equip:9",
            "monster 41.spawns_on -> map 0-1: no monster 41",
            "map 0-1.exits -> map 5-5: no map 5-5",
            "warp common-portal010-1.target -> map 5-5: no map 5-5",
        ]
    )
    out = tmp_path / "kb"
    assert kbm.main(["build", "--app", str(app), "--out", str(out)]) == 1


def test_check_kb_finds_broken_references(built: kbm.KB, tmp_path: Path) -> None:
    out = tmp_path / "kb"
    kbm.write(built, out)
    loaded = kbm.load_kb(out)
    rels = kbm.load_relations(out)
    loaded["item"]["etc:1"]["dropped_by"] = []  # drop the inverse
    loaded["map"]["0-0"]["npcs"].append({"kind": "npc", "id": "99"})
    del loaded["skill"]["1"]["confidence"]["levels"]
    loaded["npc"]["1"]["name"] = {"en": "Guard"}
    problems = kbm.check_kb(loaded, rels)
    assert "monster 1.drops -> item etc:1: no inverse in dropped_by" in problems
    assert "map 0-0.npcs: dangling npc 99" in problems
    assert "skill 1: field levels has confidence None" in problems
    assert any(p.startswith("npc 1: name is not {en, ko}") for p in problems)


def test_output_is_byte_identical_across_runs(app: Path, tmp_path: Path) -> None:
    a, b = tmp_path / "a", tmp_path / "b"
    assert kbm.main(["build", "--app", str(app), "--out", str(a)]) == 0
    assert kbm.main(["build", "--app", str(app), "--out", str(b)]) == 0
    files = sorted(p.name for p in a.iterdir())
    assert files == sorted(p.name for p in b.iterdir())
    assert files == [
        "area.json",
        "index.json",
        "item.json",
        "level.json",
        "listing.json",
        "map.json",
        "monster.json",
        "npc.json",
        "script.json",
        "shop.json",
        "skill.json",
        "warp.json",
    ]
    for name in files:
        assert (a / name).read_bytes() == (b / name).read_bytes(), name
    text = (a / "monster.json").read_text(encoding="utf-8")
    assert text.endswith("}\n") and "달팽이" in text  # ensure_ascii=False
    doc = json.loads(text)
    assert doc["schema"] == 1 and doc["kind"] == "monster"
    assert [e["id"] for e in doc["entities"]] == ["1", "2"]
    index = json.loads((a / "index.json").read_text(encoding="utf-8"))
    assert index["kinds"]["map"] == 4


def test_check_command(app: Path, tmp_path: Path, capsys) -> None:
    out = tmp_path / "kb"
    assert kbm.main(["build", "--app", str(app), "--out", str(out)]) == 0
    assert kbm.main(["check", "--app", str(app), "--out", str(out)]) == 0
    (out / "stale.json").write_text("{}\n", encoding="utf-8")
    path = out / "npc.json"
    path.write_text(path.read_text(encoding="utf-8").replace("Guard", "Gaurd"), encoding="utf-8")
    capsys.readouterr()
    assert kbm.main(["check", "--app", str(app), "--out", str(out)]) == 1
    err = capsys.readouterr().err
    assert "npc.json: differs from a rebuild" in err
    assert "stale.json: not part of the KB" in err
    # a rebuild removes the stray file and restores the page
    assert kbm.main(["build", "--app", str(app), "--out", str(out)]) == 0
    assert not (out / "stale.json").exists()


def test_builder_api_for_later_stages(built: kbm.KB) -> None:
    """A later stage (#217) registers its relation and links to the kinds already there."""
    kbm.relation("questtest", "given_by", "npc", "gives_questtest")
    try:
        built.add("questtest", "1", {"en": "Q", "ko": "Q"}, {"chapter": (1, "code")})
        assert built.link("questtest", "1", "given_by", "npc", "0", script="Lee")
        assert not built.link("questtest", "1", "given_by", "npc", "42")
        built.finish()
        assert built.entities["npc"]["1"]["gives_questtest"] == []
        assert built.entities["npc"]["0"]["gives_questtest"] == [
            {"kind": "questtest", "id": "1", "script": "Lee"}
        ]
        assert kbm.check_kb(built.entities) == []
        with pytest.raises(kbm.KBError):
            built.link("questtest", "1", "nope", "npc", "0")
        with pytest.raises(kbm.KBError):
            built.add("questtest", "1", {"en": "Q", "ko": "Q"}, {})
        with pytest.raises(kbm.KBError):
            built.add("questtest", "2", {"en": "Q", "ko": "Q"}, {"x": (1, "maybe")})
    finally:
        for key in [k for k in kbm.RELATIONS if "questtest" in k[0] or "questtest" in k[1]]:
            del kbm.RELATIONS[key]


def test_helpers() -> None:
    assert sorted(["10", "2", "etc:12", "etc:3", "1-10", "1-2"], key=kbm.natural_key) == [
        "1-2",
        "1-10",
        "2",
        "10",
        "etc:3",
        "etc:12",
    ]
    assert kbm.ref_text("item", "etc:3") == "{{item:etc:3}}"
    assert tables.meso_items(16, 25) == [0]
    assert tables.meso_items(80, 120) == [0, 1]
    assert tables.meso_items(160, 240) == [1, 2]
    assert (
        tables.fill_detail("@mp/@dmg/@time/@etc", {"mp": 1, "dmg": 2, "time": 3, "etc": 4})
        == "1/2/3/4"
    )
    assert [tables.etc_value(v) for v in (500, 35, 9, 0, -10)] == [50, 3, 0, 0, -10]
    assert world.spot(4, 18) == (1, 0, False)
    assert world.spot(10, 5) == (1, 31, True)
    assert world.spot(6, 0) == (2, 0, True)
    assert world.spot_key(2, 10) == 67


def test_names_file_is_valid() -> None:
    from mtport import names as nm

    source, entries = nm.parse(json.loads(world.names_json()), "mapnames.json")
    assert source == "mapnames"
    assert len(entries) == len(world.NAMES)
    assert all(e.thumb for e in entries if e.kind == "function")
