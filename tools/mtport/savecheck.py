"""The save checks (#148, ADR 0078): what the game trusts in its save files, as rules.

The port's import, restore and ``--check-saves`` refuse a save file that would make the game
read past a table or load a map that does not exist. The rules run in C, in the runtime
(``mt_saves_check`` in ``runtime/src/foundation/saves.cpp``), because players have no Python.
This module is the same rules written a second way, on :mod:`mtport.savedata`'s parsed
records instead of byte offsets, so that the two can check each other: CTest
``saves.check_parity`` writes :func:`fixtures` (synthetic saves, valid and hostile) as folders
and zips, runs ``MapleThief --check-saves`` on each and compares its verdicts with
:func:`check_set`'s. ``saves.cli_round_trip`` exports and imports them through the program.

Each file gets the first rule it breaks, in byte order (``docs/formats/save.md``, "Checking a
save"):

==============  ==========================================================================
verdict         rule
==============  ==========================================================================
``bad_name``    not a file the game saves
``bad_size``    not the file's size (theif 1,124, quest 4,781, config 6, cupon 5,962,
                CTerms 2), or a receipt list under 2 bytes or over 1 MiB
``bad_job``     theif: branch above 2 or job tier above 4
``bad_level``   theif: level outside 1-100
``bad_item``    an equipment id (inventory, worn, a quest's item) not empty (0xFF) and not
                below 74, or another item's not below 138
``bad_skill``   theif: a skill level above 20
``bad_map``     theif: an area above 10 or a map past the area's shipped maps
``bad_quest``   quest: an active record whose title or description is not UTF-8 text
                without control characters ending in a NUL inside its field, or a
                requirement or reward of kind above 2, or of kind 0 with an item table
                other than 0 and 1
``bad_config``  config: game speed (byte 1) or byte 5 above 2 (sound is bits: any value)
``bad_receipts`` a receipt list with a negative count or an entry past its end
``bad_counters`` the port's ``maplethief-cashshop<n>`` (#156): a byte that is not printable
                ASCII, a tab or a line end, or a line that is not blank, a ``#`` comment or
                ``key = number`` (key letters, digits, ``_``; number up to 4294967295);
                over 1,024 bytes is ``bad_size``
``bad_pair``    a set with ``theif<n>`` and no ``quest<n>``, or the other way round
==============  ==========================================================================

Command line::

    python3 -m mtport.savecheck check PATH...           # each file's verdict (folders too)
    python3 -m mtport.savecheck fixtures OUT            # the synthetic fixture sets
    python3 -m mtport.savecheck parity --program EXE [--work DIR]
    python3 -m mtport.savecheck cli --program EXE [--work DIR]
"""

from __future__ import annotations

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from mtport import savedata as sd

# The limits: facts of the shipped 1.5 data (data-tables.md, map.md), the same numbers as
# runtime/include/maplethief/saves.h (tests/test_savecheck.py compares them, and
# tests/test_savecheck_real.py checks them against the bundle).
EQUIP_IDS = 74  # item_equip.dat rows
ETC_IDS = 138  # item_etc.dat rows
MAX_LEVEL = 100  # hero.dat rows
MAX_BRANCH = 2
MAX_JOB_TIER = 4
MAX_SKILL_LEVEL = 20  # the highest skill.dat max_level
MAPS_IN_AREA = (6, 5, 12, 9, 19, 4, 4, 7, 7, 11, 6)  # map/<area><map>.map, maps 0..n-1
MAX_RECEIPTS_BYTES = 1024 * 1024
MAX_COUNTERS_BYTES = 1024  # the cash shop writes its counters from a 1 KB buffer
COUNTERS_PREFIX = "maplethief-cashshop"  # MT_SAVES_COUNTERS_PREFIX: the port's file, per slot

TMP_SUFFIX = ".mt-tmp"  # MT_SAVES_TMP_SUFFIX: never a save

VERDICTS = (
    "ok",
    "bad_name",
    "bad_size",
    "bad_job",
    "bad_level",
    "bad_item",
    "bad_skill",
    "bad_map",
    "bad_quest",
    "bad_config",
    "bad_receipts",
    "bad_counters",
)
SET_VERDICTS = ("bad_pair",)


@dataclass(frozen=True)
class Verdict:
    code: str  # one of VERDICTS (SET_VERDICTS for a set's problem)
    why: str = ""

    @property
    def ok(self) -> bool:
        return self.code == "ok"


OK = Verdict("ok")

_SIZES = {
    "character": (sd.CHARACTER_SIZE, "a character file"),
    "quest": (sd.QUEST_SIZE, "a quest file"),
    "config": (sd.CONFIG_SIZE, "config"),
    "cupon": (sd.CUPON_SIZE, "cupon"),
    "terms": (sd.TERMS_SIZE, "CTerms"),
}


def _is_counters(name: str) -> bool:
    return re.fullmatch(re.escape(COUNTERS_PREFIX) + "[0-2]", name) is not None


def is_game_file(name: str) -> bool:
    """A save file export, import and backups copy: the game's, or the port's counters."""
    if _is_counters(name):
        return True
    try:
        sd.kind_of(name)
    except sd.SaveDataError:
        return False
    return True


def _equip_bad(item: sd.EquipItem) -> bool:
    return item.id != sd.EMPTY and item.id >= EQUIP_IDS


def _check_character(name: str, c: sd.CharacterSave) -> Verdict:
    if c.branch > MAX_BRANCH:
        return Verdict("bad_job", f"{name}: job branch {c.branch}; the game has 0-{MAX_BRANCH}")
    if c.job_tier > MAX_JOB_TIER:
        return Verdict(
            "bad_job", f"{name}: job advancement {c.job_tier}; the game has 0-{MAX_JOB_TIER}"
        )
    if not 1 <= c.level <= MAX_LEVEL:
        return Verdict("bad_level", f"{name}: level {c.level}; the game has levels 1-{MAX_LEVEL}")
    for i, item in enumerate(c.equip_inventory):
        if _equip_bad(item):
            return Verdict("bad_item", f"{name}: equipment inventory slot {i} holds {item.id}")
    for i, etc in enumerate(c.etc_inventory):
        if etc.id != sd.EMPTY and etc.id >= ETC_IDS:
            return Verdict("bad_item", f"{name}: item inventory slot {i} holds item {etc.id}")
    for i, level in enumerate(c.skill_levels):
        if level > MAX_SKILL_LEVEL:
            return Verdict("bad_skill", f"{name}: skill {i} is at level {level}")
    for i, item in enumerate(c.equipped):
        if _equip_bad(item):
            return Verdict("bad_item", f"{name}: worn slot {i} holds equipment {item.id}")
    if c.area >= len(MAPS_IN_AREA):
        return Verdict("bad_map", f"{name}: area {c.area}")
    if c.map >= MAPS_IN_AREA[c.area]:
        return Verdict("bad_map", f"{name}: map {c.map} of area {c.area}")
    return OK


def _text_char(raw: bytes, i: int) -> int:
    """The length of the well-formed UTF-8 character at raw[i] that is not a control character
    (below 0x20, or 0x7F), or 0: the same scan as saves.cpp's text_char, so both name the same
    byte. Overlong forms, surrogates and code points past U+10FFFF are not well formed."""
    b = raw[i]
    if b < 0x80:
        return 1 if b >= 0x20 and b != 0x7F else 0
    lo, hi = 0x80, 0xBF
    if 0xC2 <= b <= 0xDF:
        n = 2
    elif 0xE0 <= b <= 0xEF:
        n = 3
        lo = 0xA0 if b == 0xE0 else 0x80
        hi = 0x9F if b == 0xED else 0xBF
    elif 0xF0 <= b <= 0xF4:
        n = 4
        lo = 0x90 if b == 0xF0 else 0x80
        hi = 0x8F if b == 0xF4 else 0xBF
    else:
        return 0
    if i + n > len(raw) or not lo <= raw[i + 1] <= hi:
        return 0
    if any(not 0x80 <= c <= 0xBF for c in raw[i + 2 : i + n]):
        return 0
    return n


def _key_bad(raw: bytes) -> str | None:
    """Why a quest record's text field is not a key (UTF-8 text with no control characters, up to
    a NUL inside the field), or None."""
    i = 0
    while i < len(raw):
        if raw[i] == 0:
            return None
        n = _text_char(raw, i)
        if n == 0:
            return f"is not a text key (byte 0x{raw[i]:02X} at {i})"
        i += n
    return f"has no end within its {len(raw)} bytes"


def _item_bad(kind: int, table: int, item_id: int) -> str | None:
    """The code a requirement's or reward's (kind, table, id) breaks, or None."""
    if kind > 2:
        return "bad_quest"
    if kind != 0:
        return None
    if table > 1:
        return "bad_quest"
    return "bad_item" if item_id >= (EQUIP_IDS if table == 0 else ETC_IDS) else None


def _check_quest(name: str, q: sd.QuestSave) -> Verdict:
    for r, rec in enumerate(q.records):
        if rec.empty:
            break  # QuestSave_Apply 0x83631 stops at the first empty record
        for what, raw in (("title", rec.title), ("description", rec.description)):
            why = _key_bad(raw)
            if why is not None:
                return Verdict("bad_quest", f"{name}: quest {r}'s {what} {why}")
        lists = (("requirement", rec.requirements), ("reward", rec.rewards))
        for what, entries in lists:
            for k, e in enumerate(entries):
                if e.kind == sd.EMPTY:
                    break
                code = _item_bad(e.kind, e.item_table, e.item_id)
                if code is not None:
                    return Verdict(code, f"{name}: quest {r}'s {what} {k}")
    return OK


def _check_config(c: sd.Config) -> Verdict:
    # Only the two table indexes; sound (byte 0) is bits, any value works.
    if c.speed > 2:
        return Verdict("bad_config", f"config: game speed is {c.speed}")
    if c.unknown_5 > 2:
        return Verdict("bad_config", f"config: byte 5 is {c.unknown_5}")
    return OK


_COUNTERS_LINE = re.compile(rb"[ \t]*(#.*|[A-Za-z0-9_]+[ \t]*=[ \t]*([0-9]{1,10})[ \t]*)?")


def _check_counters(name: str, data: bytes) -> Verdict:
    """The same scan as saves.cpp's check_counters: the bytes first, then each line."""
    if len(data) > MAX_COUNTERS_BYTES:
        return Verdict(
            "bad_size",
            f"{name} has {len(data)} bytes; the cash shop's counters fit in {MAX_COUNTERS_BYTES}",
        )
    for i, b in enumerate(data):
        if not (0x20 <= b <= 0x7E or b in b"\t\r\n"):
            return Verdict("bad_counters", f"{name} is not text (byte 0x{b:02X} at {i})")
    lines = data.split(b"\n")
    if data.endswith(b"\n"):
        lines.pop()  # the end of the last line, not an empty line after it
    for n, line in enumerate(lines, 1):
        if line.endswith(b"\r"):
            line = line[:-1]
        m = _COUNTERS_LINE.fullmatch(line)
        if m is None or (m.group(2) is not None and int(m.group(2)) > 0xFFFFFFFF):
            return Verdict("bad_counters", f'{name}: line {n} is not "key = number"')
    return OK


def check_file(name: str, data: bytes) -> Verdict:
    """The first rule the save file ``name`` (a name in Documents/) breaks, or :data:`OK`."""
    if _is_counters(name):
        return _check_counters(name, data)
    try:
        kind = sd.kind_of(name)
    except sd.SaveDataError:
        return Verdict("bad_name", f"{name} is not a file the game saves")
    if kind in _SIZES:
        size, what = _SIZES[kind]
        if len(data) != size:
            return Verdict("bad_size", f"{name} has {len(data)} bytes; {what} has exactly {size}")
    if kind == "character":
        return _check_character(name, sd.parse_character(data))
    if kind == "quest":
        return _check_quest(name, sd.parse_quest(data))
    if kind == "config":
        return _check_config(sd.parse_config(data))
    if kind == "receipts":
        if len(data) > MAX_RECEIPTS_BYTES or len(data) < 2:
            return Verdict("bad_size", f"{name} has {len(data)} bytes")
        try:
            sd.parse_receipts(data, name)
        except sd.SaveDataError as e:
            return Verdict("bad_receipts", str(e))
    return OK


def check_set(files: Mapping[str, bytes]) -> dict[str, Verdict]:
    """Each file's verdict, plus ``"set"`` for a theif/quest pair that is not complete."""
    out = {name: check_file(name, data) for name, data in sorted(files.items())}
    for slot in range(sd.SLOTS):
        have = [f"{k}{slot}" in files for k in ("theif", "quest")]
        if have[0] != have[1]:
            there, missing = (f"theif{slot}", f"quest{slot}")[:: 1 if have[0] else -1]
            out["set"] = Verdict("bad_pair", f"{there} comes without {missing}")
            break
    return out


def read_set(path: Path) -> tuple[dict[str, bytes], list[str]]:
    """The game's files in a folder (or its Documents/ folder) and the other names in it."""
    if not any(is_game_file(p.name) for p in path.iterdir()) and (path / "Documents").is_dir():
        path = path / "Documents"
    files: dict[str, bytes] = {}
    ignored: list[str] = []
    for p in sorted(path.iterdir()):
        if p.is_file() and is_game_file(p.name):
            files[p.name] = p.read_bytes()
        elif not p.name.endswith(TMP_SUFFIX):
            ignored.append(p.name)
    return files, ignored


# -- fixtures --------------------------------------------------------------------------------


def _key(text: str, width: int) -> bytes:
    return sd.fixed(text, width)


def _full_character(slot: int) -> sd.CharacterSave:
    rng = random.Random(1000 + slot)
    equips = tuple(
        sd.EquipItem(
            rng.randrange(EQUIP_IDS),
            rng.randrange(2),
            rng.randrange(8),
            rng.randrange(5),
            *(rng.randrange(-50, 50) for _ in range(6)),
        )
        if i % 3 == 0
        else sd.EquipItem()
        for i in range(48)
    )
    etcs = tuple(
        sd.EtcItem(rng.randrange(ETC_IDS), rng.randrange(1, 100)) if i % 4 == 0 else sd.EtcItem()
        for i in range(48)
    )
    worn = (
        sd.EquipItem(0, 0, 7, 0, 1, 2, 0, 0, 5, 6),
        sd.EquipItem(0x48, 0, 5, 0),
        sd.EquipItem(),
        sd.EquipItem(60, 1, 6, 1, 0, 0, 3, 0, 0, 0),
        sd.EquipItem(EQUIP_IDS - 1, 0, 7, 0),
    )
    return sd.CharacterSave(
        branch=1 + slot % 2,
        job_tier=slot + 2,
        level=30 + 30 * slot,
        exp=12345 * (slot + 1),
        sp=slot,
        unwritten_009=bytes(rng.randrange(256) for _ in range(4)),  # heap bytes
        hp=900,
        mp=400,
        meso=1_000_000 + slot,
        equip_inventory=equips,
        unwritten_315=bytes(rng.randrange(256) for _ in range(16)),
        etc_inventory=etcs,
        skill_levels=tuple(min(MAX_SKILL_LEVEL, i + slot * 5) for i in range(13)),
        equipped=worn,
        shortcuts=(
            sd.Shortcut(1, 3),
            sd.Shortcut(2, 9),
            sd.Shortcut(),
            sd.Shortcut(1, 6),
            sd.Shortcut(),
        ),
        area=(2, 4, 10)[slot],
        map=(11, 18, 5)[slot],  # each area's last map
        x=120,
        y=-1,
        flags=8,
        action=2,
        caption=sd.fixed("Chapter 1 - The Thief", 64),
    )


def _full_quest(slot: int) -> sd.QuestSave:
    rng = random.Random(2000 + slot)
    records = []
    for r in range(10):
        if r >= 3 + slot:
            records.append(sd.QuestRecord())
            continue
        reqs = [
            sd.Requirement(0, 1, ETC_IDS - 1, 5, 0, 0, 0, 0),
            sd.Requirement(1, 0, 0, 0, 12, 30, 7, 0),
            sd.Requirement(2, 0, 0, 0, 0, 0, 0, 500),
            sd.Requirement(),
            sd.Requirement(),
        ]
        rewards = [
            sd.Reward(0, 0, EQUIP_IDS - 1, 1, 0, 0),
            sd.Reward(1, 0, 0, 0, 1500, 0),
            sd.Reward(2, 0, 0, 0, 0, 2000),
            sd.Reward(),
            sd.Reward(),
        ]
        records.append(
            sd.QuestRecord(
                key_a=r,
                key_b=rng.randrange(256),
                extra=rng.randrange(4),
                title=_key(f"Chapter{slot + 1}_Quest_Bruce.mainQuest1{r}.1", 64),
                description=_key(f"Chapter{slot + 1}_Quest_Bruce.mainQuest1{r}.2", 256),
                requirements=tuple(reqs),
                rewards=tuple(rewards),
            )
        )
    return sd.QuestSave(
        chapter=2 + slot,
        flags_a=bytes(rng.randrange(3) for _ in range(100)),
        flags_b=bytes(rng.randrange(3) for _ in range(100)),
        records=tuple(records),
        unwritten_tail=bytes(rng.randrange(256) for _ in range(sd.QUEST_SIZE - sd.QUEST_TAIL)),
    )


def _receipts(n: int, trailing: bytes = b"") -> bytes:
    rng = random.Random(3000 + n)
    lst = sd.ReceiptList(
        tuple(
            sd.Receipt(0, bytes(rng.randrange(256) for _ in range(40 + 9 * i))) for i in range(n)
        ),
        trailing,
    )
    return sd.encode_receipts(lst)


# The cash shop's counters as mt_cash_counters_format writes them (runtime/src/cashshop).
COUNTERS = (
    b"# MapleThief: the cash shop's counters for this save slot (ADR 0072). The port's file, "
    b"not the game's.\nlevel = 30\ndeaths = 2\npaid_revives = 1\nInitializeSP = 0\n"
    b"mesopack3 = 4\nexppack1 = 0\nComebackPack = 1\n"
)


def new_character_set() -> dict[str, bytes]:
    """A new character in slot 0 and the files the game makes at start."""
    return {
        "theif0": sd.encode_character(sd.new_character(10, 10)),
        "quest0": sd.encode_quest(sd.QuestSave()),
        "config": sd.encode_config(sd.Config()),
        "cupon": sd.encode_cupon(sd.Cupon()),
    }


def full_set() -> dict[str, bytes]:
    """Three characters with items, skills and quests, and every other file the game keeps."""
    files: dict[str, bytes] = {}
    for slot in range(sd.SLOTS):
        files[f"theif{slot}"] = sd.encode_character(_full_character(slot))
        files[f"quest{slot}"] = sd.encode_quest(_full_quest(slot))
    files["config"] = sd.encode_config(sd.Config(sound=1, speed=2, unknown_5=2))
    entries = tuple(
        sd.CuponEntry(_key(f"code{i}", 64), _key("b", 35), _key("c", 10), _key("d", 10))
        for i in range(50)
    )
    files["cupon"] = sd.encode_cupon(sd.Cupon((10, 2000, 30000), entries))
    files["CTerms"] = bytes(2)
    files["crestore"] = _receipts(2, trailing=b"\x07\x07")  # a shorter rewrite's old tail
    files["riprestore1"] = _receipts(0)
    files["riprestore2"] = _receipts(1)
    files["totalrestore"] = _receipts(3)
    files["maplethief-cashshop0"] = COUNTERS
    files["maplethief-cashshop2"] = COUNTERS.replace(b"level = 30", b"level = 90")
    return files


def _poke(data: bytes, offset: int, value: bytes) -> bytes:
    out = bytearray(data)
    out[offset : offset + len(value)] = value
    return bytes(out)


def _record(r: int) -> int:
    return sd.QUEST_HEAD + r * sd.QUEST_STRIDE


@dataclass(frozen=True)
class Fixture:
    name: str
    files: dict[str, bytes]
    expect: str  # the verdict the set must get: "ok" or the code of the rule it breaks
    file: str = ""  # the file that breaks it ("set" for a set rule)


def fixtures() -> list[Fixture]:
    """Valid sets and hostile ones: each hostile set is the full set with one file broken."""
    full = full_set()

    def broken(name: str, file: str, data: bytes, expect: str) -> Fixture:
        files = dict(full)
        files[file] = data
        return Fixture(name, files, expect, file)

    t0, q0, cfg = full["theif0"], full["quest0"], full["config"]
    rec1 = _record(1)
    out = [
        Fixture("new_character", new_character_set(), "ok"),
        Fixture("full", full, "ok"),
        Fixture("config_only", {"config": cfg}, "ok"),
        broken("truncated_theif", "theif0", t0[:1000], "bad_size"),
        broken("truncated_quest", "quest0", q0[:-1], "bad_size"),
        broken("empty_theif", "theif1", b"", "bad_size"),
        broken("oversized_theif", "theif0", t0 + b"\0", "bad_size"),
        broken("oversized_cupon", "cupon", full["cupon"] + bytes(100), "bad_size"),
        broken("oversized_receipts", "crestore", bytes(MAX_RECEIPTS_BYTES + 1), "bad_size"),
        broken("short_receipts", "totalrestore", b"\x01", "bad_size"),
        broken("terms_size", "CTerms", bytes(3), "bad_size"),
        broken("config_size", "config", cfg + b"\0", "bad_size"),
        broken("branch", "theif0", _poke(t0, 0x000, b"\x03"), "bad_job"),
        broken("job_tier", "theif0", _poke(t0, 0x001, b"\x05"), "bad_job"),
        broken("level_0", "theif0", _poke(t0, 0x002, b"\x00"), "bad_level"),
        broken("level_101", "theif0", _poke(t0, 0x002, b"\x65"), "bad_level"),
        broken("equip_id", "theif0", _poke(t0, 0x015 + 16 * 1, bytes([EQUIP_IDS])), "bad_item"),
        broken(
            "etc_id", "theif2", _poke(full["theif2"], 0x325 + 3 * 5, bytes([ETC_IDS])), "bad_item"
        ),
        broken("worn_id", "theif1", _poke(full["theif1"], 0x3C2 + 16 * 2, b"\xc8"), "bad_item"),
        broken(
            "skill_level",
            "theif0",
            _poke(t0, 0x3B5 + 12, bytes([MAX_SKILL_LEVEL + 1])),
            "bad_skill",
        ),
        broken("area", "theif0", _poke(t0, 0x41C, b"\x0b"), "bad_map"),
        broken("map", "theif0", _poke(t0, 0x41C, b"\x02\x0c"), "bad_map"),
        broken("quest_key", "quest0", _poke(q0, rec1 + 0x003 + 7, b"\x9c"), "bad_quest"),
        broken("quest_key_no_end", "quest0", _poke(q0, rec1 + 0x043, b"A" * 256), "bad_quest"),
        broken(
            "quest_key_hangul",
            "quest0",
            _poke(q0, rec1 + 0x003, "ㅊㄹㅍ.dialog.1".encode() + b"\0"),
            "ok",
        ),
        broken("quest_key_control", "quest0", _poke(q0, rec1 + 0x003 + 2, b"\x01"), "bad_quest"),
        broken("quest_key_overlong", "quest0", _poke(q0, rec1 + 0x003, b"\xc0\x80"), "bad_quest"),
        broken(
            "quest_key_surrogate", "quest0", _poke(q0, rec1 + 0x003, b"\xed\xa0\x80"), "bad_quest"
        ),
        broken("quest_key_cut", "quest0", _poke(q0, rec1 + 0x003 + 4, b"\xe3\x85\0"), "bad_quest"),
        broken(
            "quest_key_cut_at_end",
            "quest0",
            _poke(q0, rec1 + 0x043, b"A" * 254 + b"\xe3\x85"),
            "bad_quest",
        ),
        broken("quest_kind", "quest0", _poke(q0, rec1 + 0x143 + 14 * 3, b"\x03"), "bad_quest"),
        broken("quest_item_table", "quest0", _poke(q0, rec1 + 0x143 + 1, b"\x02"), "bad_quest"),
        broken(
            "quest_item",
            "quest1",
            _poke(full["quest1"], rec1 + 0x189 + 2, bytes([EQUIP_IDS])),
            "bad_item",
        ),
        broken("config_speed_3", "config", _poke(cfg, 1, b"\x03"), "bad_config"),
        broken("config_byte_5", "config", _poke(cfg, 5, b"\xff"), "bad_config"),
        broken("config_sound_bits", "config", _poke(cfg, 0, b"\xff"), "ok"),
        broken("receipts_count", "crestore", b"\xff\xff", "bad_receipts"),
        broken("counters_crlf", "maplethief-cashshop0", COUNTERS.replace(b"\n", b"\r\n"), "ok"),
        broken("counters_empty", "maplethief-cashshop1", b"", "ok"),
        broken("counters_no_end", "maplethief-cashshop0", b"  level=7  ", "ok"),
        broken("counters_binary", "maplethief-cashshop0", COUNTERS + b"\x00\x01", "bad_counters"),
        broken("counters_line", "maplethief-cashshop2", COUNTERS + b"level: 3\n", "bad_counters"),
        broken("counters_negative", "maplethief-cashshop0", b"# x\ndeaths = -1\n", "bad_counters"),
        broken(
            "counters_too_big", "maplethief-cashshop0", b"deaths = 4294967296\n", "bad_counters"
        ),
        broken(
            "counters_oversized",
            "maplethief-cashshop0",
            COUNTERS + b"#" * MAX_COUNTERS_BYTES,
            "bad_size",
        ),
        broken(
            "receipts_entry",
            "riprestore2",
            _poke(full["riprestore2"], 3, b"\xff\x7f"),
            "bad_receipts",
        ),
    ]
    # Rules a valid file must not break: an empty record ends the list, so garbage after it is
    # never read; the same for the bytes after a list's 0xFF kind.
    after_empty = _poke(full["quest0"], _record(3) + 0x003, b"\x9c" * 64)
    out.append(broken("quest_after_empty", "quest0", after_empty, "ok"))
    after_end = _poke(q0, rec1 + 0x143 + 14 * 4, b"\x09")
    out.append(broken("quest_after_end", "quest0", after_end, "ok"))
    pair = dict(full)
    del pair["quest1"]
    out.append(Fixture("missing_quest", pair, "bad_pair", "set"))
    pair = {k: v for k, v in full.items() if k != "theif2"}
    out.append(Fixture("missing_theif", pair, "bad_pair", "set"))
    return out


def set_verdict(verdicts: Mapping[str, Verdict]) -> str:
    """The set's verdict: ok, or the code of the first broken file in name order ("set" last)."""
    for name in sorted(verdicts, key=lambda n: (n == "set", n)):
        if not verdicts[name].ok:
            return verdicts[name].code
    return "ok"


def write_fixtures(out: Path) -> list[Fixture]:
    """Each fixture as ``out/<name>/`` (the files) and ``out/<name>.zip`` (deflated)."""
    out.mkdir(parents=True, exist_ok=True)
    all_fixtures = fixtures()
    for fx in all_fixtures:
        folder = out / fx.name
        if folder.exists():
            shutil.rmtree(folder)
        folder.mkdir()
        for name, data in fx.files.items():
            (folder / name).write_bytes(data)
        with zipfile.ZipFile(out / f"{fx.name}.zip", "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in sorted(fx.files.items()):
                z.writestr(f"Documents/{name}", data)
    return all_fixtures


# -- the program's side ----------------------------------------------------------------------

_LINE = re.compile(r"^([A-Za-z0-9-]+|set|ignored): ([a-z_]+)(?:: (.*))?$")


def program_verdicts(program: Sequence[str], path: Path, env: Mapping[str, str]) -> dict[str, str]:
    """``program --check-saves path``'s verdict codes: {file or "set": code}."""
    res = subprocess.run(
        [*program, "--check-saves", str(path)], capture_output=True, text=True, env=dict(env)
    )
    out: dict[str, str] = {}
    for line in res.stdout.splitlines():
        m = _LINE.match(line)
        if m is None or m.group(1) == "ignored":
            continue
        out[m.group(1)] = m.group(2)
    want_status = 0 if all(c == "ok" for c in out.values()) else 3
    if res.returncode != want_status:
        raise RuntimeError(
            f"{path}: exit status {res.returncode}, expected {want_status}\n{res.stderr}"
        )
    return out


def _env(work: Path) -> dict[str, str]:
    env = dict(os.environ)
    env["MAPLETHIEF_DATA_DIR"] = str(work / "data")  # never the player's own
    env.setdefault("SDL_VIDEO_DRIVER", "dummy")
    return env


def parity(program: Sequence[str], work: Path) -> list[str]:
    """Every fixture through both checks; the differences (empty: in sync)."""
    problems = []
    for fx in write_fixtures(work):
        want = {k: v.code for k, v in check_set(fx.files).items()}
        if set_verdict(check_set(fx.files)) != fx.expect:
            problems.append(
                f"{fx.name}: Python says {set_verdict(check_set(fx.files))}, "
                f"the fixture expects {fx.expect}"
            )
        for path in (work / fx.name, work / f"{fx.name}.zip"):
            got = program_verdicts(program, path, _env(work))
            if got != want:
                diff = {
                    k: (want.get(k), got.get(k))
                    for k in set(want) | set(got)
                    if want.get(k) != got.get(k)
                }
                problems.append(f"{path.name}: (Python, program) differ: {diff}")
    return problems


def cli_round_trip(program: Sequence[str], work: Path) -> list[str]:
    """Import, export and refusal through the program's command line, on the fixtures."""
    problems = []
    all_fixtures = write_fixtures(work / "fixtures")
    for fx in all_fixtures:
        for source in (work / "fixtures" / fx.name, work / "fixtures" / f"{fx.name}.zip"):
            data = work / "data"
            if data.exists():
                shutil.rmtree(data)
            docs = data / "saves"  # the game's files (ADR 0127)
            docs.mkdir(parents=True)
            before = {"theif0": b"old character", "config": b"\x03\x01\x00\x01\x01\x01"}
            for name, b in before.items():
                (docs / name).write_bytes(b)
            env = _env(work)
            res = subprocess.run(
                [*program, "--import-saves", str(source)], capture_output=True, text=True, env=env
            )
            after = {p.name: p.read_bytes() for p in docs.iterdir() if p.is_file()}
            if fx.expect != "ok":
                if res.returncode != 3 or after != before or fx.expect not in res.stdout:
                    problems.append(
                        f"{source.name}: a rejected import changed saves/ or did "
                        f"not say why (exit {res.returncode}): {res.stdout!r}"
                    )
                continue
            if res.returncode != 0 or any(after.get(k) != v for k, v in fx.files.items()):
                problems.append(
                    f"{source.name}: import failed (exit {res.returncode}): "
                    f"{res.stdout!r} {res.stderr[-400:]!r}"
                )
                continue
            imports = data / "backups" / "import"
            backups = sorted(imports.iterdir()) if imports.is_dir() else []
            replaced = {n for n in before if n in fx.files}
            got_backup = {p.name: p.read_bytes() for p in backups[-1].iterdir()} if backups else {}
            if got_backup != {n: before[n] for n in replaced}:
                problems.append(
                    f"{source.name}: the import's backup holds {sorted(got_backup)}, "
                    f"not the replaced {sorted(replaced)}"
                )
            for target in (work / "export", work / "export.zip"):
                if target.is_dir():
                    shutil.rmtree(target)
                elif target.exists():
                    target.unlink()
                res = subprocess.run(
                    [*program, "--export-saves", str(target)],
                    capture_output=True,
                    text=True,
                    env=env,
                )
                if res.returncode != 0:
                    problems.append(
                        f"{source.name} -> {target.name}: export failed: {res.stdout!r}"
                    )
                    continue
                if target.suffix == ".zip":
                    with zipfile.ZipFile(target) as z:
                        exported = {i.filename: z.read(i) for i in z.infolist()}
                else:
                    exported = {p.name: p.read_bytes() for p in target.iterdir()}
                if exported != after:
                    problems.append(
                        f"{source.name} -> {target.name}: the export is not saves/ byte for byte"
                    )
    return problems


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.savecheck", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    check = sub.add_parser("check", help="each save file's verdict (a folder: its files)")
    check.add_argument("paths", nargs="+", type=Path)
    fx = sub.add_parser("fixtures", help="write the synthetic fixture sets")
    fx.add_argument("out", type=Path)
    for cmd in ("parity", "cli"):
        p = sub.add_parser(cmd, help=f"{cmd}: the program against these checks")
        p.add_argument("--program", required=True, nargs="+", help="the MapleThief executable")
        p.add_argument("--work", type=Path, help="scratch folder (default: a temporary one)")
    args = parser.parse_args(argv)
    if args.cmd == "check":
        bad = False
        for path in args.paths:
            files = read_set(path)[0] if path.is_dir() else {path.name: path.read_bytes()}
            for name, v in check_set(files).items():
                print(f"{name}: {v.code}" + (f": {v.why}" if v.why else ""))
                bad = bad or not v.ok
        return 1 if bad else 0
    if args.cmd == "fixtures":
        for f in write_fixtures(args.out):
            print(f"{f.name}: {f.expect}")
        return 0
    with tempfile.TemporaryDirectory(prefix="maplethief-savecheck-") as tmp:
        work = args.work or Path(tmp)
        run = parity if args.cmd == "parity" else cli_round_trip
        problems = run(args.program, work)
    for p in problems:
        print(p, file=sys.stderr)
    print(f"{args.cmd}: {len(fixtures())} fixtures, {len(problems)} problems")
    return 1 if problems else 0


__all__ = [
    "Fixture",
    "Verdict",
    "check_file",
    "check_set",
    "fixtures",
    "full_set",
    "is_game_file",
    "new_character_set",
    "parity",
    "cli_round_trip",
    "read_set",
    "set_verdict",
    "write_fixtures",
]

if __name__ == "__main__":
    sys.exit(main())
