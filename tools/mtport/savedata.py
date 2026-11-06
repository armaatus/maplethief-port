"""Parser and encoder for the game's save files (docs/formats/save.md).

The game keeps its state in small raw binary files in the app's ``Documents/`` directory,
written with ``NSFileHandle`` (no ``NSUserDefaults``, no ``NSKeyedArchiver``, no checksum or
version field). Every multi-byte value is little-endian. The files:

==================  ======  ===============================================================
file                bytes   contents
==================  ======  ===============================================================
``theif0..2``       1,124   one character per slot (sic: "theif"), ``CDataMgr::SaveSaveData``
``quest0..2``       4,781   that character's quest state, saved together with ``theif%d``
``config``          6       options (sound, game speed, ...)
``cupon``           5,962   three best scores and 50 fixed-width string records
``CTerms``          2       present once the Korean terms of service were accepted
``crestore``,       varies  in-app purchase receipt lists: ``i16 count`` then
``riprestore1/2``,          ``count x (u8 flag, i16 len, len bytes)``
``totalrestore``
==================  ======  ===============================================================

The game allocates each buffer with ``operator new[]`` and never clears it, so a few bytes of
``theif%d`` and ``quest%d`` are never written and hold whatever the heap had. They are kept
as opaque ``unwritten_*`` bytes so that parse -> encode is byte-exact. One byte of each quest
record is shared with the next record (the game's record stride is one byte too short); see
:class:`QuestSave`.

The input is untrusted data: every size is checked before it is read.

Command line::

    python3 -m mtport.savedata dump FILE...           # print each file as JSON
    python3 -m mtport.savedata new-character OUT_DIR --hp N --mp N [--slot S]
    python3 -m mtport.savedata names [OUT]            # work/re/names/save.json
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import re
import struct
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

from mtport import ghidra_common as gc
from mtport.bytereader import FormatError

SLOTS = 3
EMPTY = 0xFF  # item id / record kind of an empty slot

CHARACTER_SIZE = 0x464
QUEST_SIZE = 0x12AD
CONFIG_SIZE = 6
CUPON_SIZE = 0x174A
TERMS_SIZE = 2

RECEIPT_FILES = ("crestore", "riprestore1", "riprestore2", "totalrestore")

# Default caption written by CDataMgr::LoadSaveData (ScriptExtra "CDataMgr.LoadSaveData.1").
DEFAULT_CAPTION = "How it All Began"


class SaveDataError(FormatError):
    """The input is not a well-formed save file of the expected kind."""


def _need(data: bytes, size: int, what: str) -> None:
    if len(data) != size:
        raise SaveDataError(f"{what}: expected {size} bytes, got {len(data)}")


def cstr(raw: bytes) -> str:
    """A fixed-width text field as text: up to the first NUL, UTF-8, bad bytes escaped."""
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="backslashreplace")


def fixed(text: str | bytes, width: int) -> bytes:
    """``text`` as the game stores it (0x1BE31): truncated to ``width`` bytes, NUL-padded."""
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    return raw[:width].ljust(width, b"\0")


# -- theif%d: one character -----------------------------------------------------------------

EQUIP = struct.Struct("<BBBB6h")  # 16 bytes
ETC = struct.Struct("<Bh")  # 3 bytes
CHAR_HEAD = struct.Struct("<BBBih4shhi")  # 0x000..0x014
CHAR_TAIL = struct.Struct("<BBhhBB64s")  # 0x41C..0x463

EQUIP_INVENTORY = (0x015, 48)  # (offset, count)
GAP_315 = (0x315, 16)
ETC_INVENTORY = (0x325, 48)
SKILL_LEVELS = (0x3B5, 13)
EQUIPPED = (0x3C2, 5)
SHORTCUTS = (0x412, 5)


@dataclass(frozen=True)
class EquipItem:
    """A 16-byte equipment record (inventory slot or worn item); ``id == 0xFF`` is empty."""

    id: int = EMPTY  # +0 item_equip id (item+0x90)
    rolled: int = 0  # +1 item+0xD4: 1 = dropped item, its stats are all in the bonus fields
    slots_left: int = 0  # +2 item+0xCC: upgrade (scroll) slots left
    scrolls_used: int = 0  # +3 item+0xD0: successful scrolls
    attack: int = 0  # +4.. item+0xD8..+0xEC: added to the table stats
    defense: int = 0
    accuracy: int = 0
    avoid: int = 0
    hp: int = 0
    mp: int = 0

    @property
    def empty(self) -> bool:
        return self.id == EMPTY

    def pack(self) -> bytes:
        return EQUIP.pack(*dataclasses.astuple(self))

    @classmethod
    def unpack(cls, data: bytes, offset: int) -> EquipItem:
        return cls(*EQUIP.unpack_from(data, offset))


@dataclass(frozen=True)
class EtcItem:
    """A 3-byte non-equipment inventory slot; ``id == 0xFF`` is empty."""

    id: int = EMPTY  # item_etc id (item+0x90)
    count: int = 0  # stack size (item+0x100)

    @property
    def empty(self) -> bool:
        return self.id == EMPTY


@dataclass(frozen=True)
class Shortcut:
    """One of five 2-byte shortcut slots (hero+0xE0+8i, +0xE4+8i); ``kind == 0`` is empty."""

    kind: int = 0
    id: int = 0


def _empty_equips(n: int) -> tuple[EquipItem, ...]:
    return tuple(EquipItem() for _ in range(n))


def _empty_etcs(n: int) -> tuple[EtcItem, ...]:
    return tuple(EtcItem() for _ in range(n))


@dataclass(frozen=True)
class CharacterSave:
    """``theif%d``: the 0x464-byte character record (CDataMgr::SaveSaveData, 0x38711)."""

    branch: int = 0  # 0x000 hero+0x88: 0 Rogue, 1 Assassin, 2 Bandit
    job_tier: int = 0  # 0x001 hero+0x8C: job advancement, 0 = beginner
    level: int = 1  # 0x002 hero+0x80
    exp: int = 0  # 0x003 hero+0x90
    sp: int = 0  # 0x007 hero+0xBC: unspent skill points
    # 0x009: max HP / MP of a new character (i16 x 2). Written only by LoadSaveData; never read;
    # SaveSaveData leaves these 4 bytes unwritten (heap garbage in a saved file).
    unwritten_009: bytes = bytes(4)
    hp: int = 0  # 0x00D hero+0x94
    mp: int = 0  # 0x00F hero+0x98
    meso: int = 0  # 0x011 hero+0xD0
    equip_inventory: tuple[EquipItem, ...] = field(default_factory=lambda: _empty_equips(48))
    unwritten_315: bytes = bytes(16)  # 0x315: never written by the game
    etc_inventory: tuple[EtcItem, ...] = field(default_factory=lambda: _empty_etcs(48))
    skill_levels: tuple[int, ...] = (0,) * 13  # 0x3B5, order: see skill_slots()
    equipped: tuple[EquipItem, ...] = field(default_factory=lambda: _empty_equips(5))
    shortcuts: tuple[Shortcut, ...] = tuple(Shortcut() for _ in range(5))
    area: int = 0  # 0x41C world+0x84: map area (selects <area>.mapprj, "%d%d.map")
    map: int = 0  # 0x41D world+0x88: map within the area
    x: int = -1  # 0x41E hero+4: position; -1 = the map's default spawn
    y: int = -1  # 0x420 hero+8
    flags: int = 0  # 0x422 hero+0xC (low byte)
    action: int = 0  # 0x423 hero+0x10: the hero's state (0x60551)
    caption: bytes = bytes(64)  # 0x424: UTF-8 progress caption shown on the slot

    @property
    def unwritten_009_as_hp_mp(self) -> tuple[int, int]:
        hp, mp = struct.unpack("<hh", self.unwritten_009)
        return hp, mp

    @property
    def caption_text(self) -> str:
        return cstr(self.caption)

    def __post_init__(self) -> None:
        for name, n in (
            ("equip_inventory", 48),
            ("etc_inventory", 48),
            ("skill_levels", 13),
            ("equipped", 5),
            ("shortcuts", 5),
        ):
            if len(getattr(self, name)) != n:
                raise SaveDataError(f"CharacterSave.{name} needs {n} entries")
        for name, n in (("unwritten_009", 4), ("unwritten_315", 16), ("caption", 64)):
            if len(getattr(self, name)) != n:
                raise SaveDataError(f"CharacterSave.{name} needs {n} bytes")


def parse_character(data: bytes) -> CharacterSave:
    _need(data, CHARACTER_SIZE, "theif%d")
    branch, tier, level, exp, sp, unwritten_009, hp, mp, meso = CHAR_HEAD.unpack_from(data, 0)
    off, n = EQUIP_INVENTORY
    equips = tuple(EquipItem.unpack(data, off + 16 * i) for i in range(n))
    off, n = ETC_INVENTORY
    etcs = tuple(EtcItem(*ETC.unpack_from(data, off + 3 * i)) for i in range(n))
    off, n = SKILL_LEVELS
    skills = tuple(data[off : off + n])
    off, n = EQUIPPED
    equipped = tuple(EquipItem.unpack(data, off + 16 * i) for i in range(n))
    off, n = SHORTCUTS
    shortcuts = tuple(Shortcut(data[off + 2 * i], data[off + 2 * i + 1]) for i in range(n))
    area, map_, x, y, flags, action, caption = CHAR_TAIL.unpack_from(data, 0x41C)
    return CharacterSave(
        branch=branch,
        job_tier=tier,
        level=level,
        exp=exp,
        sp=sp,
        unwritten_009=unwritten_009,
        hp=hp,
        mp=mp,
        meso=meso,
        equip_inventory=equips,
        unwritten_315=data[GAP_315[0] : GAP_315[0] + GAP_315[1]],
        etc_inventory=etcs,
        skill_levels=skills,
        equipped=equipped,
        shortcuts=shortcuts,
        area=area,
        map=map_,
        x=x,
        y=y,
        flags=flags,
        action=action,
        caption=caption,
    )


def encode_character(save: CharacterSave) -> bytes:
    out = bytearray(CHARACTER_SIZE)
    CHAR_HEAD.pack_into(
        out,
        0,
        save.branch,
        save.job_tier,
        save.level,
        save.exp,
        save.sp,
        save.unwritten_009,
        save.hp,
        save.mp,
        save.meso,
    )
    for i, item in enumerate(save.equip_inventory):
        EQUIP.pack_into(out, EQUIP_INVENTORY[0] + 16 * i, *dataclasses.astuple(item))
    out[GAP_315[0] : GAP_315[0] + GAP_315[1]] = save.unwritten_315
    for i, etc in enumerate(save.etc_inventory):
        ETC.pack_into(out, ETC_INVENTORY[0] + 3 * i, etc.id, etc.count)
    out[SKILL_LEVELS[0] : SKILL_LEVELS[0] + SKILL_LEVELS[1]] = bytes(save.skill_levels)
    for i, item in enumerate(save.equipped):
        EQUIP.pack_into(out, EQUIPPED[0] + 16 * i, *dataclasses.astuple(item))
    for i, s in enumerate(save.shortcuts):
        out[SHORTCUTS[0] + 2 * i] = s.kind
        out[SHORTCUTS[0] + 2 * i + 1] = s.id
    CHAR_TAIL.pack_into(
        out, 0x41C, save.area, save.map, save.x, save.y, save.flags, save.action, save.caption
    )
    return bytes(out)


# Top-level layout of theif%d: (offset, size, field). Checked against docs/formats/save.md.
CHARACTER_FIELDS: tuple[tuple[int, int, str], ...] = (
    (0x000, 1, "branch"),
    (0x001, 1, "job_tier"),
    (0x002, 1, "level"),
    (0x003, 4, "exp"),
    (0x007, 2, "sp"),
    (0x009, 4, "unwritten_009"),
    (0x00D, 2, "hp"),
    (0x00F, 2, "mp"),
    (0x011, 4, "meso"),
    (0x015, 48 * 16, "equip_inventory"),
    (0x315, 16, "unwritten_315"),
    (0x325, 48 * 3, "etc_inventory"),
    (0x3B5, 13, "skill_levels"),
    (0x3C2, 5 * 16, "equipped"),
    (0x412, 5 * 2, "shortcuts"),
    (0x41C, 1, "area"),
    (0x41D, 1, "map"),
    (0x41E, 2, "x"),
    (0x420, 2, "y"),
    (0x422, 1, "flags"),
    (0x423, 1, "action"),
    (0x424, 64, "caption"),
)


def new_character(hp: int, mp: int, caption: str | bytes = DEFAULT_CAPTION) -> CharacterSave:
    """The record CDataMgr::LoadSaveData (0x39989) creates for an empty slot.

    ``hp`` and ``mp`` are hero.dat's ``hp_gain`` / ``mp_gain`` for level 1. The game leaves
    0x315..0x324 unwritten; this returns zeros there.
    """
    equipped = list(_empty_equips(5))
    equipped[1] = EquipItem(id=0x48, rolled=0, slots_left=5)  # Wooden Club, weapon slot
    etcs = list(_empty_etcs(48))
    etcs[0] = EtcItem(3, 5)  # 5 Red Potions
    etcs[1] = EtcItem(6, 5)  # 5 Blue Potions
    shortcuts = [Shortcut(), Shortcut(1, 3), Shortcut(1, 6), Shortcut(), Shortcut()]
    return CharacterSave(
        branch=0,
        job_tier=0,
        level=1,
        exp=0,
        sp=0,
        unwritten_009=struct.pack("<hh", hp, mp),
        hp=hp,
        mp=mp,
        meso=100,
        etc_inventory=tuple(etcs),
        equipped=tuple(equipped),
        shortcuts=tuple(shortcuts),
        area=2,
        map=8,
        x=-1,
        y=-1,
        flags=0,
        action=2,
        caption=fixed(caption, 64),
    )


class SkillRow(Protocol):
    id: int
    branch: int
    tier: int
    type: int


def skill_slots(branch: int, job_tier: int, skills: Iterable[SkillRow]) -> list[int]:
    """Skill id of each of the 13 ``skill_levels`` slots (-1 = unused), as 0x7FD69 orders them.

    For each tier from ``job_tier`` down to 1, every skill.dat row (in table order) with
    ``type != 0`` whose ``tier`` is that tier and whose ``branch`` is 0 or the hero's branch.
    """
    rows = list(skills)
    out: list[int] = []
    for tier in range(job_tier, 0, -1):
        for row in rows:
            if row.type != 0 and row.tier == tier and row.branch in (0, branch):
                out.append(row.id)
    if len(out) > 13:
        raise SaveDataError(f"{len(out)} skills for branch {branch} tier {job_tier}; 13 slots")
    return out + [-1] * (13 - len(out))


# -- quest%d: quest state -------------------------------------------------------------------

QUEST_HEAD = 0xC9  # u8 chapter, u8[100], u8[100]
QUEST_RECORDS = 10
QUEST_STRIDE = 0x1C9
QUEST_RECORD_HEAD = struct.Struct("<BBB64s256s")  # 0x143 bytes
REQUIREMENT = struct.Struct("<BBBhBhhi")  # 14 bytes
REWARD = struct.Struct("<BBBhii")  # 13 bytes
REQUIREMENTS_AT = 0x143
REWARDS_AT = 0x189
QUEST_TAIL = QUEST_HEAD + QUEST_RECORDS * QUEST_STRIDE + 1  # 0x12A4: first never-written byte


@dataclass(frozen=True)
class Requirement:
    """14 bytes; ``kind`` 0 = item (table, id, count), 1 = monsters, 2 = meso, 0xFF = none."""

    kind: int = EMPTY
    item_table: int = 0  # +1 kind 0: 0 item_equip, 1 item_etc
    item_id: int = 0  # +2
    item_count: int = 0  # +3 i16
    target: int = 0  # +5 u8, kind 1 (defeat monsters): the monster
    needed: int = 0  # +6 i16, kind 1
    progress: int = 0  # +8 i16, kind 1
    value: int = 0  # +A i32, kind 2: meso


@dataclass(frozen=True)
class Reward:
    """13 bytes; ``kind`` 0 = item (table, id, count), 1 = meso, 2 = EXP, 0xFF = none."""

    kind: int = EMPTY
    item_table: int = 0
    item_id: int = 0
    item_count: int = 0  # +3 i16
    value_1: int = 0  # +5 i32, kind 1: meso
    value_2: int = 0  # +9 i32, kind 2: EXP (its top byte is the next record's first byte)


@dataclass(frozen=True)
class QuestRecord:
    """One of 10 active-quest records (stride 0x1C9); ``key_a == 0xFF`` is empty."""

    key_a: int = EMPTY  # +0 quest+4
    key_b: int = 0  # +1 quest+8
    extra: int = 0  # +2 quest+0xC
    title: bytes = bytes(64)  # +3 Scripts.strings key
    description: bytes = bytes(256)  # +0x43 Scripts.strings key
    requirements: tuple[Requirement, ...] = tuple(Requirement() for _ in range(5))
    rewards: tuple[Reward, ...] = tuple(Reward() for _ in range(5))

    @property
    def empty(self) -> bool:
        return self.key_a == EMPTY


@dataclass(frozen=True)
class QuestSave:
    """``quest%d``: 0x12AD bytes (CDataMgr::SaveQuestData 0x38F55, loaded by 0x83631).

    The records are 0x1C9 bytes apart but each writes 0x1CA bytes: the top byte of
    ``rewards[4].value_2`` is the next record's ``key_a``. :func:`encode_quest` writes the
    records in order, as the game does, so the next record's ``key_a`` wins;
    :func:`link_quest_records` gives the value the game reads back.
    """

    chapter: int = 0  # 0x000 questmgr+4
    flags_a: bytes = bytes(100)  # 0x001 questmgr+8+4i (one byte each)
    flags_b: bytes = bytes(100)  # 0x065 questmgr+0x19C+4i
    records: tuple[QuestRecord, ...] = tuple(QuestRecord() for _ in range(QUEST_RECORDS))
    unwritten_tail: bytes = bytes(QUEST_SIZE - QUEST_TAIL)  # 0x12A4..0x12AC

    def __post_init__(self) -> None:
        if len(self.flags_a) != 100 or len(self.flags_b) != 100:
            raise SaveDataError("QuestSave flags need 100 bytes each")
        if len(self.records) != QUEST_RECORDS:
            raise SaveDataError(f"QuestSave needs {QUEST_RECORDS} records")
        if len(self.unwritten_tail) != QUEST_SIZE - QUEST_TAIL:
            raise SaveDataError(f"QuestSave.unwritten_tail needs {QUEST_SIZE - QUEST_TAIL} bytes")
        for r in self.records:
            if len(r.requirements) != 5 or len(r.rewards) != 5:
                raise SaveDataError("QuestRecord needs 5 requirements and 5 rewards")
            if len(r.title) != 64 or len(r.description) != 256:
                raise SaveDataError("QuestRecord title/description need 64/256 bytes")


def parse_quest(data: bytes) -> QuestSave:
    _need(data, QUEST_SIZE, "quest%d")
    records = []
    for i in range(QUEST_RECORDS):
        base = QUEST_HEAD + i * QUEST_STRIDE
        key_a, key_b, extra, title, desc = QUEST_RECORD_HEAD.unpack_from(data, base)
        reqs = tuple(
            Requirement(*REQUIREMENT.unpack_from(data, base + REQUIREMENTS_AT + 14 * k))
            for k in range(5)
        )
        rewards = tuple(
            Reward(*REWARD.unpack_from(data, base + REWARDS_AT + 13 * k)) for k in range(5)
        )
        records.append(QuestRecord(key_a, key_b, extra, title, desc, reqs, rewards))
    return QuestSave(
        chapter=data[0],
        flags_a=data[1:101],
        flags_b=data[101:201],
        records=tuple(records),
        unwritten_tail=data[QUEST_TAIL:],
    )


def link_quest_records(save: QuestSave) -> QuestSave:
    """``save`` as the game reads it back: the top byte of each record's
    ``rewards[4].value_2`` replaced by the next record's ``key_a`` (records 0..8)."""
    records = list(save.records)
    for i in range(QUEST_RECORDS - 1):
        rewards = list(records[i].rewards)
        low = rewards[4].value_2 & 0x00FFFFFF
        value = low | (records[i + 1].key_a << 24)
        value -= (1 << 32) if value & 0x80000000 else 0
        rewards[4] = dataclasses.replace(rewards[4], value_2=value)
        records[i] = dataclasses.replace(records[i], rewards=tuple(rewards))
    return dataclasses.replace(save, records=tuple(records))


def encode_quest(save: QuestSave) -> bytes:
    out = bytearray(QUEST_SIZE)
    out[0] = save.chapter
    out[1:101] = save.flags_a
    out[101:201] = save.flags_b
    for i, r in enumerate(save.records):
        base = QUEST_HEAD + i * QUEST_STRIDE
        QUEST_RECORD_HEAD.pack_into(out, base, r.key_a, r.key_b, r.extra, r.title, r.description)
        for k, q in enumerate(r.requirements):
            REQUIREMENT.pack_into(out, base + REQUIREMENTS_AT + 14 * k, *dataclasses.astuple(q))
        for k, w in enumerate(r.rewards):
            REWARD.pack_into(out, base + REWARDS_AT + 13 * k, *dataclasses.astuple(w))
    out[QUEST_TAIL:] = save.unwritten_tail
    return bytes(out)


# -- config, cupon, CTerms, receipt lists ---------------------------------------------------


@dataclass(frozen=True)
class Config:
    """``config``: 6 option bytes, kept in memory at ``*0x11A98C`` (loader 0x38611)."""

    sound: int = 3  # 0x37C79: bit 0 music, bit 1 effects (probable)
    speed: int = 1  # 0 / 1 / 2 -> 65 / 50 / 40 (table 0x108C58), probably the tick in ms
    unknown_2: int = 0
    unknown_3: int = 1  # 0x37BD1: 0 = off, else passed on as a level
    unknown_4: int = 1  # cleared by 0x3EAA1 in state 0x68
    unknown_5: int = 1  # index into {0, 1, 3} (0x37B85)


def parse_config(data: bytes) -> Config:
    _need(data, CONFIG_SIZE, "config")
    return Config(*data)


def encode_config(cfg: Config) -> bytes:
    return bytes(dataclasses.astuple(cfg))


CUPON_ENTRY = struct.Struct("<64s35s10s10s")  # 0x77 bytes


@dataclass(frozen=True)
class CuponEntry:
    """One of 50 string records of ``cupon`` (data manager +0x118/+0x1E0/+0x2A8/+0x370)."""

    text_a: bytes = bytes(64)
    text_b: bytes = bytes(35)
    text_c: bytes = bytes(10)
    text_d: bytes = bytes(10)


@dataclass(frozen=True)
class Cupon:
    """``cupon``: best scores of three game modes, then 50 string records (0x38171)."""

    best_scores: tuple[int, int, int] = (0, 0, 0)  # data manager +0x438, +0x43C, +0x440
    entries: tuple[CuponEntry, ...] = tuple(CuponEntry() for _ in range(50))


def parse_cupon(data: bytes) -> Cupon:
    _need(data, CUPON_SIZE, "cupon")
    scores = struct.unpack_from("<3i", data, 0)
    entries = tuple(CuponEntry(*CUPON_ENTRY.unpack_from(data, 12 + 0x77 * i)) for i in range(50))
    return Cupon(scores, entries)


def encode_cupon(c: Cupon) -> bytes:
    if len(c.entries) != 50:
        raise SaveDataError("cupon needs 50 entries")
    out = bytearray(struct.pack("<3i", *c.best_scores))
    for e in c.entries:
        out += CUPON_ENTRY.pack(*dataclasses.astuple(e))
    return bytes(out)


@dataclass(frozen=True)
class Receipt:
    flag: int  # u8; 0 when appended
    data: bytes  # the App Store receipt bytes (i16 length prefix)


@dataclass(frozen=True)
class ReceiptList:
    """``crestore``, ``riprestore1/2``, ``totalrestore``: ``i16 count`` + entries.

    The game opens files for update without truncating, so a shorter rewrite would leave
    stale bytes after the last entry; they are kept in ``trailing``.
    """

    receipts: tuple[Receipt, ...] = ()
    trailing: bytes = b""


def parse_receipts(data: bytes, name: str = "receipt list") -> ReceiptList:
    if len(data) < 2:
        raise SaveDataError(f"{name}: {len(data)} bytes is too short for the count")
    (count,) = struct.unpack_from("<h", data, 0)
    if count < 0:
        raise SaveDataError(f"{name}: negative count {count}")
    pos = 2
    out = []
    for i in range(count):
        if pos + 3 > len(data):
            raise SaveDataError(f"{name}: entry {i} header runs past the end")
        flag = data[pos]
        (size,) = struct.unpack_from("<h", data, pos + 1)
        if size < 0 or pos + 3 + size > len(data):
            raise SaveDataError(f"{name}: entry {i} of {size} bytes runs past the end")
        out.append(Receipt(flag, data[pos + 3 : pos + 3 + size]))
        pos += 3 + size
    return ReceiptList(tuple(out), data[pos:])


def encode_receipts(lst: ReceiptList) -> bytes:
    out = bytearray(struct.pack("<h", len(lst.receipts)))
    for r in lst.receipts:
        out += struct.pack("<Bh", r.flag, len(r.data)) + r.data
    return bytes(out + lst.trailing)


# -- files by name --------------------------------------------------------------------------

_SLOT_FILE = re.compile(r"(theif|quest)([0-2])")


def kind_of(name: str) -> str:
    """The kind of save file called ``name`` (a file name in ``Documents/``)."""
    m = _SLOT_FILE.fullmatch(name)
    if m:
        return "character" if m.group(1) == "theif" else "quest"
    if name in RECEIPT_FILES:
        return "receipts"
    if name in ("config", "cupon"):
        return name
    if name == "CTerms":
        return "terms"
    raise SaveDataError(f"{name!r} is not a save file name the game uses")


def parse_file(name: str, data: bytes) -> Any:
    kind = kind_of(name)
    if kind == "character":
        return parse_character(data)
    if kind == "quest":
        return parse_quest(data)
    if kind == "config":
        return parse_config(data)
    if kind == "cupon":
        return parse_cupon(data)
    if kind == "receipts":
        return parse_receipts(data, name)
    _need(data, TERMS_SIZE, name)
    return bytes(data)


def encode_file(name: str, value: Any) -> bytes:
    kind = kind_of(name)
    encoders = {
        "character": encode_character,
        "quest": encode_quest,
        "config": encode_config,
        "cupon": encode_cupon,
        "receipts": encode_receipts,
    }
    if kind in encoders:
        return encoders[kind](value)
    return bytes(value)


def to_jsonable(value: Any) -> Any:
    """``value`` (a parsed save) as plain JSON data; text fields decoded, other bytes hex."""
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        out = {}
        for f in dataclasses.fields(value):
            v = getattr(value, f.name)
            if isinstance(v, bytes) and not f.name.startswith("unwritten") and f.name != "data":
                out[f.name] = cstr(v)
            else:
                out[f.name] = to_jsonable(v)
        return out
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, (tuple, list)):
        return [to_jsonable(v) for v in value]
    return value


# -- Ghidra names (work/re/names/save.json) -------------------------------------------------

NAMES_OUT = "work/re/names/save.json"

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x6BF5", "function", "SaveIO_Close", "[handle closeFile]"),
    ("0x6C41", "function", "SaveIO_Write", "NSData initWithBytes:length: + writeData: + sync"),
    ("0x6CED", "function", "SaveIO_Read", "readDataOfLength: + getBytes:"),
    ("0x6EF1", "function", "SaveIO_DocumentsPath", "NSHomeDirectory()/Documents/<name>"),
    ("0x6F65", "function", "SaveIO_Open", "mode 1 read, 2 write, 4 write+truncate, 8 update"),
    ("0x7105", "function", "SaveIO_Exists", "fileExistsAtPath: under Documents/"),
    ("0x71E9", "function", "SaveIO_Size", "length of availableData"),
    ("0xB6A9", "function", "SaveFile_Exists", "bool (const char *name)"),
    ("0xB6E9", "function", "SaveFile_ThiefSlotExists", 'probes "thief%d"; files are "theif%d"'),
    ("0xB729", "function", "SaveFile_WriteAt", "(buf, name, offset, size)"),
    ("0xB785", "function", "SaveFile_Write", "(buf, name, size): create/update, no truncate"),
    ("0xB7C9", "function", "SaveFile_Read", "new[] buffer with the whole file, or NULL"),
    ("0x3757D", "function", "CDataMgr::FreeSlot", "drops the cached theif/quest buffers"),
    ("0x37095", "function", "CDataMgr::QuestCaption", "caption for theif%d +0x424"),
    ("0x379E5", "function", "CDataMgr::WriteFile", "-> SaveFile_Write"),
    ("0x37A01", "function", "CDataMgr::LoadSlots", "reads theif0..2 and quest0..2"),
    ("0x37D7D", "function", "CDataMgr::LoadQuestData", "cached quest%d, or a new one (written)"),
    ("0x38171", "function", "CDataMgr::SaveCupon", "cupon: 3 scores + 50 records"),
    ("0x38311", "function", "CDataMgr::LoadCupon", "cupon, created with defaults"),
    ("0x38611", "function", "CDataMgr::LoadConfig", "config (6 bytes), created with defaults"),
    ("0x38711", "function", "CDataMgr::SaveSaveData", "writes theif%d (0x464 bytes)"),
    ("0x38F55", "function", "CDataMgr::SaveQuestData", "writes quest%d (0x12AD bytes)"),
    ("0x39989", "function", "CDataMgr::LoadSaveData", "cached theif%d, or a new character"),
    ("0x672A9", "function", "CHero::LoadSavePreview", "theif%d -> hero, worn items only"),
    ("0x677E9", "function", "CHero::LoadSave", "theif%d -> hero"),
    ("0x83631", "function", "QuestSave_Apply", "quest%d -> quest manager"),
    ("0x11A98C", "data", "g_pConfig", "the 6-byte config buffer"),
)


def names_json() -> str:
    return gc.names_json("save", NAMES)


# -- command line ---------------------------------------------------------------------------


def _write_new_character(out_dir: Path, slot: int, hp: int, mp: int) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    files = {
        f"theif{slot}": encode_character(new_character(hp, mp)),
        f"quest{slot}": encode_quest(QuestSave()),
    }
    paths = []
    for name, data in files.items():
        path = out_dir / name
        path.write_bytes(data)
        paths.append(path)
    return paths


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.savedata", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    dump = sub.add_parser("dump", help="print save files as JSON (name picks the kind)")
    dump.add_argument("files", nargs="+")
    new = sub.add_parser("new-character", help="write theif<S> + quest<S> of a new character")
    new.add_argument("out_dir")
    new.add_argument("--slot", type=int, default=0, choices=range(SLOTS))
    new.add_argument("--hp", type=int, required=True, help="hero.dat level 1 hp_gain")
    new.add_argument("--mp", type=int, required=True, help="hero.dat level 1 mp_gain")
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "dump":
            for f in args.files:
                path = Path(f)
                value = parse_file(path.name, path.read_bytes())
                print(json.dumps({path.name: to_jsonable(value)}, ensure_ascii=False, indent=1))
        elif args.cmd == "new-character":
            for p in _write_new_character(Path(args.out_dir), args.slot, args.hp, args.mp):
                print(f"wrote {p}")
        else:
            out = Path(args.out)
            gc.write_text(out, names_json())
            print(f"wrote {out} ({len(NAMES)} names)")
    except (OSError, SaveDataError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
