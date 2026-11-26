"""Unit tests for the save-file parser (synthetic saves built from docs/formats/save.md)."""

import dataclasses
import json
import random
import re
import struct
import subprocess
import sys
from types import SimpleNamespace

import pytest

from mtport import names as nm
from mtport import savedata as sd
from repo_paths import REPO

SPEC = REPO / "docs" / "formats" / "save.md"


def synthetic_character() -> sd.CharacterSave:
    """A character with every field set to a distinct value, as the spec lays them out."""
    equips = tuple(
        sd.EquipItem(i, i % 2, 7 - i % 8, i % 4, i, -i, 2 * i, -2 * i, 300 + i, -300 - i)
        if i % 3
        else sd.EquipItem()
        for i in range(48)
    )
    etcs = tuple(sd.EtcItem(i + 10, 100 * i) if i % 2 else sd.EtcItem() for i in range(48))
    worn = (
        sd.EquipItem(0, 0, 7, 0, 1, 2, 0, 0, 5, 6),
        sd.EquipItem(0x48, 0, 5, 0),
        sd.EquipItem(),
        sd.EquipItem(60, 1, 6, 1, 0, 0, 3, 0, 0, 0),
        sd.EquipItem(48, 0, 7, 0),
    )
    return sd.CharacterSave(
        branch=1,
        job_tier=3,
        level=42,
        exp=-123456,
        sp=-7,
        unwritten_009=b"\xde\xad\xbe\xef",
        hp=1234,
        mp=-567,
        meso=0x01020304,
        equip_inventory=equips,
        unwritten_315=bytes(range(0xA0, 0xB0)),
        etc_inventory=etcs,
        skill_levels=tuple(range(1, 14)),
        equipped=worn,
        shortcuts=(
            sd.Shortcut(1, 3),
            sd.Shortcut(2, 9),
            sd.Shortcut(),
            sd.Shortcut(1, 6),
            sd.Shortcut(3, 255),
        ),
        area=5,
        map=3,
        x=-300,
        y=700,
        flags=0x81,
        action=5,
        caption=sd.fixed("Lith Harbor", 64),
    )


def test_character_layout_tiles_the_record():
    offset = 0
    for off, size, name in sd.CHARACTER_FIELDS:
        assert off == offset, name
        offset += size
    assert offset == sd.CHARACTER_SIZE == 1124
    assert [f.name for f in dataclasses.fields(sd.CharacterSave)] == [
        n for _, _, n in sd.CHARACTER_FIELDS
    ]
    assert sd.CHAR_HEAD.size == 0x15
    assert sd.CHAR_TAIL.size == sd.CHARACTER_SIZE - 0x41C
    assert sd.EQUIP.size == 16 and sd.ETC.size == 3


def test_synthetic_character_bytes_are_where_the_spec_says():
    save = synthetic_character()
    data = sd.encode_character(save)
    assert len(data) == 0x464
    assert data[0:3] == bytes([1, 3, 42])
    assert struct.unpack_from("<i", data, 0x003)[0] == -123456
    assert struct.unpack_from("<h", data, 0x007)[0] == -7
    assert data[0x009:0x00D] == b"\xde\xad\xbe\xef"
    assert struct.unpack_from("<hh", data, 0x00D) == (1234, -567)
    assert data[0x011:0x015] == bytes([4, 3, 2, 1])
    # equip inventory slot 1 at 0x015 + 16: id, rolled, slots_left, scrolls_used, 6 x i16
    assert data[0x025:0x029] == bytes([1, 1, 6, 1])
    assert struct.unpack_from("<6h", data, 0x029) == (1, -1, 2, -2, 301, -301)
    assert data[0x015:0x019] == bytes([0xFF, 0, 0, 0])  # slot 0 empty
    assert data[0x315:0x325] == bytes(range(0xA0, 0xB0))
    assert data[0x325] == 0xFF and data[0x328] == 11  # etc slot 0 empty, slot 1 id 11
    assert struct.unpack_from("<h", data, 0x329)[0] == 100
    assert data[0x3B5:0x3C2] == bytes(range(1, 14))
    assert data[0x3D2:0x3D6] == bytes([0x48, 0, 5, 0])  # worn weapon (slot 1)
    assert data[0x412:0x41C] == bytes([1, 3, 2, 9, 0, 0, 1, 6, 3, 255])
    assert data[0x41C:0x41E] == bytes([5, 3])
    assert struct.unpack_from("<hh", data, 0x41E) == (-300, 700)
    assert data[0x422:0x424] == bytes([0x81, 5])
    assert data[0x424:0x464] == b"Lith Harbor".ljust(64, b"\0")


def test_synthetic_character_round_trips():
    save = synthetic_character()
    data = sd.encode_character(save)
    assert sd.parse_character(data) == save
    assert sd.encode_character(sd.parse_character(data)) == data
    assert sd.parse_character(data).caption_text == "Lith Harbor"


def test_new_character_matches_load_save_data_defaults():
    data = sd.encode_character(sd.new_character(10, 10))
    assert data[0:3] == bytes([0, 0, 1])
    assert struct.unpack_from("<ih", data, 3) == (0, 0)
    assert struct.unpack_from("<4h", data, 9) == (10, 10, 10, 10)
    assert struct.unpack_from("<i", data, 0x11)[0] == 100
    for i in range(48):
        assert data[0x15 + 16 * i : 0x15 + 16 * i + 16] == b"\xff" + bytes(15)
    assert data[0x325:0x32B] == bytes([3, 5, 0, 6, 5, 0])
    assert all(data[0x325 + 3 * i] == 0xFF for i in range(2, 48))
    assert data[0x3B5:0x3C2] == bytes(13)
    assert data[0x3C2:0x3C6] == b"\xff\0\0\0"
    assert data[0x3D2:0x3E2] == bytes([0x48, 0, 5]) + bytes(13)
    assert data[0x412:0x41C] == bytes([0, 0, 1, 3, 1, 6, 0, 0, 0, 0])
    assert data[0x41C:0x424] == bytes([2, 8, 0xFF, 0xFF, 0xFF, 0xFF, 0, 2])
    assert sd.parse_character(data).caption_text == "How it All Began"


@pytest.mark.parametrize(
    ("size", "parse", "encode"),
    [
        (sd.CHARACTER_SIZE, sd.parse_character, sd.encode_character),
        (sd.QUEST_SIZE, sd.parse_quest, sd.encode_quest),
        (sd.CONFIG_SIZE, sd.parse_config, sd.encode_config),
        (sd.CUPON_SIZE, sd.parse_cupon, sd.encode_cupon),
    ],
)
def test_any_bytes_round_trip_exactly(size, parse, encode):
    rng = random.Random(size)
    for _ in range(20):
        data = bytes(rng.randrange(256) for _ in range(size))
        assert encode(parse(data)) == data


@pytest.mark.parametrize(
    ("parse", "size"),
    [
        (sd.parse_character, sd.CHARACTER_SIZE),
        (sd.parse_quest, sd.QUEST_SIZE),
        (sd.parse_config, sd.CONFIG_SIZE),
        (sd.parse_cupon, sd.CUPON_SIZE),
    ],
)
def test_wrong_sizes_are_rejected(parse, size):
    for n in (0, size - 1, size + 1):
        with pytest.raises(sd.SaveDataError):
            parse(bytes(n))


def synthetic_quest() -> sd.QuestSave:
    reqs = (
        sd.Requirement(0, 1, 30, 10),
        sd.Requirement(1, target=7, needed=20, progress=3),
        sd.Requirement(2, value=-5000),
        sd.Requirement(),
        sd.Requirement(),
    )
    rewards = (
        sd.Reward(0, 0, 12, 1),
        sd.Reward(1, value_1=900),
        sd.Reward(2, value_2=150),
        sd.Reward(),
        sd.Reward(),
    )
    records = [
        sd.QuestRecord(
            3, 4, 5, sd.fixed("Quest.3.4.title", 64), sd.fixed("Quest.3.4.desc", 256), reqs, rewards
        ),
        sd.QuestRecord(3, 9, 0, sd.fixed("Quest.3.9.title", 64), sd.fixed("", 256)),
    ] + [sd.QuestRecord() for _ in range(8)]
    save = sd.QuestSave(
        chapter=3,
        flags_a=bytes(i % 3 for i in range(100)),
        flags_b=bytes(i % 5 for i in range(100)),
        records=tuple(records),
        unwritten_tail=b"\x11" * 9,
    )
    # the shared byte of each record (see QuestSave) as the game reads it back
    return sd.link_quest_records(save)


def test_quest_layout():
    assert sd.QUEST_RECORD_HEAD.size == 0x143
    assert sd.REQUIREMENT.size == 14 and sd.REWARD.size == 13
    assert sd.REQUIREMENTS_AT + 5 * 14 == sd.REWARDS_AT
    # each record writes one byte more than its stride
    assert sd.REWARDS_AT + 5 * 13 == sd.QUEST_STRIDE + 1
    assert sd.QUEST_HEAD + 10 * sd.QUEST_STRIDE + 1 == sd.QUEST_TAIL == 0x12A4
    assert sd.QUEST_SIZE == 4781


def test_synthetic_quest_bytes_and_round_trip():
    save = synthetic_quest()
    data = sd.encode_quest(save)
    assert len(data) == 0x12AD
    assert data[0] == 3 and data[1:4] == bytes([0, 1, 2]) and data[0x65:0x68] == bytes([0, 1, 2])
    r0 = 0xC9
    assert data[r0 : r0 + 3] == bytes([3, 4, 5])
    assert data[r0 + 3 : r0 + 3 + 16] == b"Quest.3.4.title\0"
    assert data[r0 + 0x43 : r0 + 0x43 + 15] == b"Quest.3.4.desc\0"
    assert data[r0 + 0x143 : r0 + 0x143 + 14] == bytes([0, 1, 30, 10, 0]) + bytes(9)
    assert data[r0 + 0x151 : r0 + 0x151 + 9] == bytes([1, 0, 0, 0, 0, 7, 20, 0, 3])
    assert struct.unpack_from("<i", data, r0 + 0x15F + 10)[0] == -5000
    assert data[r0 + 0x16D] == 0xFF  # 4th requirement: none
    assert data[r0 + 0x189 : r0 + 0x189 + 5] == bytes([0, 0, 12, 1, 0])
    assert struct.unpack_from("<i", data, r0 + 0x196 + 5)[0] == 900
    assert struct.unpack_from("<i", data, r0 + 0x1A3 + 9)[0] == 150
    assert data[r0 + 0x1C9] == 3  # record 1 key_a
    assert data[0xC9 + 2 * 0x1C9] == 0xFF  # record 2 empty
    assert data[0x12A4:] == b"\x11" * 9
    assert sd.parse_quest(data) == save
    assert sd.encode_quest(sd.parse_quest(data)) == data


def test_quest_record_overlap_follows_the_game_write_order():
    # rewards[4].value_2 of record 0 is written first; record 1's key_a then overwrites its
    # top byte, so the value read back has record 1's key_a as its top byte.
    rewards = tuple(sd.Reward() for _ in range(4)) + (sd.Reward(2, value_2=0x7F000064),)
    r0 = dataclasses.replace(sd.QuestRecord(), key_a=1, rewards=rewards)
    r1 = dataclasses.replace(sd.QuestRecord(), key_a=2)
    save = dataclasses.replace(sd.QuestSave(), records=(r0, r1) + sd.QuestSave().records[2:])
    back = sd.parse_quest(sd.encode_quest(save))
    assert back.records[1].key_a == 2
    assert back.records[0].rewards[4].value_2 == 0x02000064
    # An empty record 0 has value_2 == 0, so its top byte reads back as the next key_a.
    assert sd.parse_quest(sd.encode_quest(sd.QuestSave())).records[0].rewards[4].value_2 == (
        0xFF << 24
    ) - (1 << 32)
    assert sd.parse_quest(sd.encode_quest(save)) == sd.link_quest_records(save)


def test_config_and_cupon():
    assert sd.encode_config(sd.Config()) == bytes([3, 1, 0, 1, 1, 1])
    assert sd.parse_config(bytes([0, 2, 0, 0, 1, 2])).speed == 2
    entries = list(sd.Cupon().entries)
    entries[3] = sd.CuponEntry(
        sd.fixed("a" * 70, 64), sd.fixed("b", 35), sd.fixed("c", 10), sd.fixed("d", 10)
    )
    c = sd.Cupon((1500, -1, 3001), tuple(entries))
    data = sd.encode_cupon(c)
    assert len(data) == 5962
    assert struct.unpack_from("<3i", data) == (1500, -1, 3001)
    assert data[12 + 3 * 0x77 : 12 + 3 * 0x77 + 64] == b"a" * 64  # truncated, no NUL
    assert data[12 + 3 * 0x77 + 64] == ord("b")
    assert sd.parse_cupon(data) == c


def test_receipt_lists():
    lst = sd.ReceiptList((sd.Receipt(0, b"receipt-one"), sd.Receipt(1, b"")))
    data = sd.encode_receipts(lst)
    assert data == b"\x02\x00" + b"\x00\x0b\x00receipt-one" + b"\x01\x00\x00"
    assert sd.parse_receipts(data) == lst
    stale = data + b"old"
    assert sd.parse_receipts(stale).trailing == b"old"
    assert sd.encode_receipts(sd.parse_receipts(stale)) == stale
    for bad in (b"", b"\x01", b"\xff\xff", b"\x01\x00\x00\x05\x00abc", b"\x02\x00\x00\x00\x00"):
        with pytest.raises(sd.SaveDataError):
            sd.parse_receipts(bad)


def test_file_names():
    assert sd.kind_of("theif0") == "character" and sd.kind_of("theif2") == "character"
    assert sd.kind_of("quest1") == "quest"
    assert sd.kind_of("totalrestore") == "receipts"
    assert sd.kind_of("CTerms") == "terms"
    for bad in ("thief0", "theif3", "quest", "NoCharge1", "../theif0"):
        with pytest.raises(sd.SaveDataError):
            sd.kind_of(bad)
    assert sd.parse_file("CTerms", b"\0\0") == b"\0\0"
    data = sd.encode_character(synthetic_character())
    assert sd.encode_file("theif1", sd.parse_file("theif1", data)) == data


def test_skill_slots_order():
    rows = [
        SimpleNamespace(id=0, branch=0, tier=1, type=0),
        SimpleNamespace(id=2, branch=0, tier=1, type=3),
        SimpleNamespace(id=4, branch=0, tier=1, type=1),
        SimpleNamespace(id=6, branch=1, tier=2, type=1),
        SimpleNamespace(id=11, branch=1, tier=3, type=1),
        SimpleNamespace(id=15, branch=2, tier=2, type=1),
    ]
    assert sd.skill_slots(0, 0, rows) == [-1] * 13
    assert sd.skill_slots(1, 1, rows) == [2, 4] + [-1] * 11
    assert sd.skill_slots(1, 3, rows) == [11, 6, 2, 4] + [-1] * 9
    assert sd.skill_slots(2, 2, rows) == [15, 2, 4] + [-1] * 10


def spec_rows(heading: str) -> list[tuple[int, str]]:
    """``(offset, field)`` of each ``| 0xNNN | type | `field` |`` row under ``heading``."""
    text = SPEC.read_text(encoding="utf-8")
    start = text.index(heading)
    end = text.find("\n## ", start + len(heading))
    body = text[start : end if end > 0 else None]
    rows = re.findall(r"^\| (0x[0-9A-F]{3,4}) \| [^|]+ \| `(\w+)`", body, re.M)
    return [(int(o, 16), n) for o, n in rows]


def test_spec_documents_every_character_field():
    assert spec_rows("## `theif0`") == [(o, n) for o, _, n in sd.CHARACTER_FIELDS]


def test_spec_documents_the_quest_header():
    assert spec_rows("## `quest0`") == [
        (0x000, "chapter"),
        (0x001, "flags_a"),
        (0x065, "flags_b"),
        (0x0C9, "records"),
        (0x12A4, "unwritten_tail"),
    ]


def test_names_file_is_valid():
    doc = json.loads(sd.names_json())
    source, entries = nm.parse(doc, "save.json")
    assert source == "save"
    assert len(entries) == len(sd.NAMES)
    functions = [e for e in entries if e.kind == "function"]
    assert all(e.thumb for e in functions)  # every named save function is Thumb code


def test_cli(tmp_path):
    env_cmd = [sys.executable, "-m", "mtport.savedata"]
    env = {"PYTHONPATH": str(REPO / "tools")}
    out = subprocess.run(
        [*env_cmd, "new-character", str(tmp_path), "--slot", "2", "--hp", "10", "--mp", "12"],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    assert "theif2" in out.stdout
    assert (tmp_path / "theif2").stat().st_size == sd.CHARACTER_SIZE
    assert (tmp_path / "quest2").stat().st_size == sd.QUEST_SIZE
    dump = subprocess.run(
        [*env_cmd, "dump", str(tmp_path / "theif2"), str(tmp_path / "quest2")],
        check=True,
        capture_output=True,
        text=True,
        env=env,
    )
    assert '"caption": "How it All Began"' in dump.stdout
    names = tmp_path / "names" / "save.json"
    subprocess.run([*env_cmd, "names", str(names)], check=True, env=env, capture_output=True)
    assert json.loads(names.read_text(encoding="utf-8"))["source"] == "save"
    bad = tmp_path / "theif0"
    bad.write_bytes(b"short")
    res = subprocess.run([*env_cmd, "dump", str(bad)], capture_output=True, text=True, env=env)
    assert res.returncode == 1 and "expected 1124 bytes" in res.stderr


# -- building a save from a description (#297) ----------------------------------------------

SKILL_ROWS = [
    SimpleNamespace(id=2, branch=0, tier=1, type=3),
    SimpleNamespace(id=4, branch=0, tier=1, type=1),
    SimpleNamespace(id=15, branch=2, tier=2, type=1),
]


def valid_pair() -> tuple[sd.CharacterSave, sd.QuestSave]:
    """The synthetic pair with quest states the game can hold (0 to 2)."""
    quest = dataclasses.replace(synthetic_quest(), flags_b=bytes(i % 3 for i in range(100)))
    return synthetic_character(), quest


def test_build_defaults_are_the_new_character():
    character, quest = sd.build({"hp": 10, "mp": 12})
    assert character == sd.new_character(10, 12)
    assert quest == sd.QuestSave()


def test_describe_then_build_gives_every_written_byte():
    character, quest = valid_pair()
    desc = json.loads(json.dumps(sd.describe(character, quest)))  # through JSON, as on disk
    built_c, built_q = sd.build(desc)
    assert (
        sd.written_diff("theif0", sd.encode_character(built_c), sd.encode_character(character))
        == []
    )
    assert sd.written_diff("quest0", sd.encode_quest(built_q), sd.encode_quest(quest)) == []
    assert desc["map"] == [5, 3] and desc["position"] == [-300, 700]
    assert desc["caption"] == "Lith Harbor"
    assert desc["records"][0]["requirements"][1] == {"mob": 7, "needed": 20, "progress": 3}
    assert desc["records"][0]["rewards"][2] == {"exp": 150}


def test_build_places_skills_by_id_and_captions_by_key():
    strings = {"Other.key": "Crisis", "CDataMgr.m_strQuestTitle.7": "Crisis"}
    desc = {
        "hp": 50,
        "mp": 20,
        "branch": 2,
        "job_tier": 2,
        "skills": {"15": 3, "4": 1},
        "caption": {"key": "CDataMgr.m_strQuestTitle.7"},
        "chapter": 2,
        "main": {"40": 1},
        "side": {"7": 2},
        "records": [{"kind": 0, "number": 40, "npc": 8, "title": "T", "rewards": [{"exp": 350}]}],
    }
    character, quest = sd.build(desc, SKILL_ROWS, strings)
    assert character.skill_levels[:3] == (3, 0, 1)
    assert character.caption_text == "Crisis"
    assert quest.chapter == 2 and quest.flags_a[40] == 1 and quest.flags_b[7] == 2
    assert quest.records[0].extra == 8 and quest.records[0].rewards[0] == sd.Reward(2, value_2=350)
    back = sd.describe(character, quest, SKILL_ROWS, strings)
    assert back["skills"] == {"15": 3, "4": 1}
    assert back["caption"] == {"key": "CDataMgr.m_strQuestTitle.7"}
    assert sd.build(back, SKILL_ROWS, strings) == sd.build(desc, SKILL_ROWS, strings)


@pytest.mark.parametrize(
    ("desc", "message"),
    [
        ({"hp": 1, "mp": 1, "levle": 3}, "unknown keys"),
        ({"hp": 1}, "hp and mp are required"),
        ({"hp": 1, "mp": 1, "level": 0}, "level 0"),
        ({"hp": 1, "mp": 1, "main": {"9": 3}}, "main quest 9"),
        ({"hp": 1, "mp": 1, "side": {"100": 1}}, "side quest 100"),
        ({"hp": 1, "mp": 1, "skills": {"15": 1}}, "need skill.dat"),
        ({"hp": 1, "mp": 1, "skills": {}, "skill_levels": []}, "both set"),
        ({"hp": 1, "mp": 1, "etc_inventory": [None] * 49}, "at most 48"),
        ({"hp": 1, "mp": 1, "equipped": [{"id": 1, "colour": 2}]}, "is not an item"),
        ({"hp": 1, "mp": 1, "map": [2]}, "not a pair"),
        ({"hp": 1, "mp": 1, "caption": {"key": "A.b"}}, "needs the game's strings"),
        ({"hp": 1, "mp": 1, "records": [{"requirements": [{"gold": 1}]}]}, "no item, mob or meso"),
        ({"hp": 1, "mp": 1, "records": [{}] * 11}, "at most 10"),
    ],
)
def test_build_refuses_a_malformed_description(desc, message):
    with pytest.raises(sd.SaveDataError, match=message):
        sd.build(desc)


def test_build_refuses_unknown_skills_and_caption_keys():
    with pytest.raises(sd.SaveDataError, match="skill 15 has no slot"):
        sd.build({"hp": 1, "mp": 1, "skills": {"15": 1}}, SKILL_ROWS)
    with pytest.raises(sd.SaveDataError, match="not in the strings"):
        sd.build({"hp": 1, "mp": 1, "caption": {"key": "A.b"}}, strings={})


@pytest.mark.parametrize(
    ("change", "code"),
    [
        ({"equipped": [None, {"id": 74}]}, "bad_item"),
        ({"etc_inventory": [[138, 1]]}, "bad_item"),
        ({"level": 101}, "bad_level"),
        ({"branch": 3}, "bad_job"),
        ({"skill_levels": [21]}, "bad_skill"),
        ({"map": [11, 0]}, "bad_map"),
        ({"records": [{"title": "Bad\nkey"}]}, "bad_quest"),
    ],
)
def test_build_files_runs_the_ports_checks(change, code):
    with pytest.raises(sd.SaveDataError, match=code):
        sd.build_files({"hp": 10, "mp": 10, **change})


def test_build_files_writes_the_slot():
    files = sd.build_files({"hp": 10, "mp": 10, "level": 5, "map": [0, 0]}, slot=2)
    assert sorted(files) == ["quest2", "theif2"]
    assert sd.parse_character(files["theif2"]).level == 5


def test_written_diff_ignores_unwritten_bytes():
    character, quest = valid_pair()
    theif, qbytes = sd.encode_character(character), sd.encode_quest(quest)
    garbage = dataclasses.replace(character, unwritten_009=b"zzzz", unwritten_315=b"y" * 16)
    assert sd.written_diff("theif0", sd.encode_character(garbage), theif) == []
    tail = dataclasses.replace(quest, unwritten_tail=b"\x99" * 9)
    assert sd.written_diff("quest0", sd.encode_quest(tail), qbytes) == []
    richer = sd.encode_character(dataclasses.replace(character, meso=7))
    assert sd.written_diff("theif0", richer, theif) == [
        f"theif0.meso: built 7, other {character.meso}"
    ]
    states = bytearray(quest.flags_a)
    states[7] = (states[7] + 1) % 3
    moved = sd.encode_quest(dataclasses.replace(quest, flags_a=bytes(states)))
    assert [line.split(" (")[1] for line in sd.written_diff("quest0", moved, qbytes)] == [
        f"main quest 7): built {states[7]}, other {quest.flags_a[7]}"
    ]
    assert sd.written_diff("quest0", qbytes, qbytes[:-1]) != []


def test_cli_build_and_describe(tmp_path, capsys):
    character, quest = valid_pair()
    character = dataclasses.replace(character, area=0, map=0)  # a map the checks know
    desc = tmp_path / "start.json"
    (tmp_path / "theif").write_bytes(sd.encode_character(character))
    (tmp_path / "quest").write_bytes(sd.encode_quest(quest))
    assert sd.main(["describe", str(tmp_path / "theif"), str(tmp_path / "quest")]) == 0
    desc.write_text(capsys.readouterr().out, encoding="utf-8")
    assert sd.main(["build", str(desc), str(tmp_path / "out"), "--slot", "1"]) == 0
    built = (tmp_path / "out" / "theif1").read_bytes()
    assert sd.written_diff("theif1", built, sd.encode_character(character)) == []
    desc.write_text('{"hp": 1, "mp": 1, "level": 200}', encoding="utf-8")
    assert sd.main(["build", str(desc), str(tmp_path / "bad")]) == 1
    assert "bad_level" in capsys.readouterr().err
    assert not (tmp_path / "bad").exists()
    desc.write_text("[1]", encoding="utf-8")
    assert sd.main(["build", str(desc), str(tmp_path / "bad")]) == 1


def test_cli_build_skips_without_the_bundle(tmp_path, capsys):
    # A start state's CTest fixture (#302) is skipped, not failed, without your own copy.
    desc = tmp_path / "start.json"
    desc.write_text('{"hp": 1, "mp": 1}', encoding="utf-8")
    missing = tmp_path / "MapleThief.app"
    assert sd.main(["build", str(desc), str(tmp_path / "out"), "--app", str(missing)]) == 77
    assert "skipped" in capsys.readouterr().out
    assert not (tmp_path / "out").exists()


def test_build_turns_wrong_types_into_save_data_errors():
    for desc in ({"hp": 1, "mp": 1, "skills": {"x": 1}}, {"hp": 1, "mp": 1, "main": [1]}):
        with pytest.raises(sd.SaveDataError, match="description"):
            sd.build(desc, SKILL_ROWS)


def test_start_descriptions_are_well_formed():
    starts = sorted((REPO / "tests" / "starts").glob("*.json"))
    assert starts
    for path in starts:
        desc = json.loads(path.read_text(encoding="utf-8"))
        assert isinstance(desc, dict) and set(desc) <= sd.DESCRIPTION_KEYS, path
        assert "hp" in desc and "mp" in desc, path
        # No game text in the repository: a caption is named by its strings key.
        assert not isinstance(desc.get("caption"), str), path
