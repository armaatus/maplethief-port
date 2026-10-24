"""Parser for the language data tables ``<Language>/data/*.dat`` (docs/formats/data-tables.md).

Each of ``English/``, ``Korean/`` and ``Japanese/`` holds seven tables plus ``buildup.dat``.
A table is a 6-byte header followed by fixed-size records::

    u16 columns; u16 rows; u16 stride;     little-endian
    rows x record[stride]

Every column is one of ``u8``, ``i16`` (LE, sign-extended), ``i32`` (LE) or a fixed-width
string (NUL-padded bytes). The game reads columns through four accessors on the data
manager (u8 0x398D5, i16 0x3793D, i32 0x385D1, string 0x37999) that take a byte offset into
the record, so the file stores no per-column types: the layouts below come from the code
(constant offsets passed to those accessors) and are checked here against ``columns`` and
``stride`` from the header. Strings are converted by the game with
``kCFStringEncodingEUC_KR`` in every language; this parser decodes them as CP949 (a superset
of EUC-KR and of ASCII) and fails on bytes that do not decode.

``buildup.dat`` is not a table: it is an ASCII decimal integer (``"1"``), read with ``atoi``.

The input is untrusted data: sizes are checked before anything is read.

Command line::

    python3 -m mtport.datatables dump  FILE.dat     # one line per record
    python3 -m mtport.datatables check DATA_DIR...  # parse every table in each dir
"""

from __future__ import annotations

import argparse
import dataclasses
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from mtport.bytereader import FormatError

HEADER = struct.Struct("<HHH")
ENCODING = "cp949"
LANGUAGES = ("English", "Korean", "Japanese")

_KINDS: dict[str, tuple[str, int]] = {"u8": ("<B", 1), "i16": ("<h", 2), "i32": ("<i", 4)}


class DataTableError(FormatError):
    """The input is not a well-formed data table of the expected kind."""


def _col(kind: str, doc: str, width: int = 0) -> Any:
    return field(metadata={"kind": kind, "width": width, "doc": doc})


def u8(doc: str) -> Any:
    return _col("u8", doc)


def i16(doc: str) -> Any:
    return _col("i16", doc)


def i32(doc: str) -> Any:
    return _col("i32", doc)


def text(width: int, doc: str) -> Any:
    return _col("str", doc, width)


# -- record layouts (field order = byte order in the record) ------------------------------


@dataclass(frozen=True)
class HeroLevel:
    """hero.dat: one row per character level (18 bytes, 13 columns)."""

    level: int = u8("level 1..100 (lookup key)")
    exp: int = i32("EXP needed to finish this level")
    hp_gain: int = i16("max HP gained at this level; max HP = sum over levels 1..L")
    mp_gain: int = i16("max MP gained at this level; max MP = sum over levels 1..L")
    unknown_9: int = u8("unknown, never read through the accessors; 1 in every row")
    strength: int = u8("status-window stat 1 (probably STR)")
    dexterity: int = u8("status-window stat 2 (probably DEX)")
    intelligence: int = u8("status-window stat 3 (probably INT)")
    luck: int = u8("status-window stat 4 (probably LUK)")
    attack: int = u8("base attack, probably (hero+0xA4)")
    defense: int = u8("base defense, probably (hero+0xA8)")
    accuracy: int = u8("base accuracy, probably (hero+0xB0)")
    avoidability: int = u8("base avoidability, probably (hero+0xAC); 50 for levels 1-4")


@dataclass(frozen=True)
class Mob:
    """mob.dat: one row per monster (50 bytes, 17 columns)."""

    id: int = u8("monster id (lookup key)")
    name: str = text(16, "display name")
    hp: int = i32("max HP")
    level: int = i16("level, probably (pulled toward the hero's attack/defense mean in one mode)")
    unknown_23: int = u8("unknown (mob+0x20); 0..5")
    unknown_24: int = u8("unknown (mob+0xAC); 1 normal, 2 boss, 3 Malady Bomb")
    exp: int = i16("EXP given to the hero on kill")
    meso: int = i16("meso drop: 80 % chance of 80..120 % of this")
    equip_drop: int = u8("item_equip id dropped (0 with chance 0 = none)")
    equip_drop_chance: int = i32("equip drop chance, per mille")
    etc_drop_1: int = u8("item_etc id dropped (slot 1)")
    etc_drop_1_chance: int = i32("slot 1 drop chance, per mille")
    etc_drop_2: int = u8("item_etc id dropped (slot 2)")
    etc_drop_2_chance: int = i32("slot 2 drop chance, per mille")
    etc_drop_3: int = u8("item_etc id dropped (slot 3)")
    etc_drop_3_chance: int = i32("slot 3 drop chance, per mille")
    sprite: int = u8("sprite project number: sprite/mob/<n>.sprprj")


@dataclass(frozen=True)
class Npc:
    """npc.dat: one row per NPC (33 bytes, 2 columns)."""

    id: int = u8("NPC id (lookup key)")
    name: str = text(32, "display name; '$' marks a line break")


@dataclass(frozen=True)
class ItemEquip:
    """item_equip.dat: one row per equipment item (73 bytes, 24 columns).

    The six stat columns come in (min, max) pairs; a dropped item rolls each stat in
    [min, max] (0x69BB9), a bought or quest item takes the minimum (0x696A9). The stat order
    ATT, DEF, ACC, AVOID, HP, MP is the one scrolls use (item_etc, 0x69145).
    """

    id: int = u8("item id (lookup key)")
    name: str = text(32, "display name")
    slot: int = u8("equipment slot: 0 hat, 1 weapon, 2 overall, 3 earrings, 4 shoes")
    weapon_type: int = u8("0 not a weapon, 1 claw, 2 dagger")
    job: int = u8("required job; 4 (thief) in every row")
    req_level: int = u8("required level")
    speed: int = u8("unknown (item+0xB0); weapons only: 1..3, probably attack speed")
    attack_min: int = i16("attack, minimum roll")
    attack_max: int = i16("attack, maximum roll")
    defense_min: int = i16("defense, minimum roll")
    defense_max: int = i16("defense, maximum roll")
    accuracy_min: int = i16("accuracy, minimum roll")
    accuracy_max: int = i16("accuracy, maximum roll")
    avoid_min: int = i16("avoidability, minimum roll")
    avoid_max: int = i16("avoidability, maximum roll")
    hp_min: int = i16("max HP bonus, minimum roll")
    hp_max: int = i16("max HP bonus, maximum roll")
    mp_min: int = i16("max MP bonus, minimum roll")
    mp_max: int = i16("max MP bonus, maximum roll")
    bonus_stat: int = u8("dropped items only: stat 1..6 (ATT..MP) that gets bonus_value; 0 none")
    bonus_value: int = i16("amount added to bonus_stat on a dropped item")
    price: int = i32("price in meso")
    icon: int = i16("inventory icon index")
    look: int = i16("avatar sprite index when worn; -1 none (shoes, earrings)")


@dataclass(frozen=True)
class ItemEtc:
    """item_etc.dat: one row per non-equipment item (218 bytes, 14 columns)."""

    id: int = u8("item id (lookup key)")
    name: str = text(64, "display name")
    type: int = u8(
        "0 meso, 1 potion, 2 throwing star, 3 scroll, 4 return scroll, 5 etc/material,"
        " 6 quest item, 7 special (cash) item, 8 100 % scroll"
    )
    hp: int = i16("HP restored (potions) or max HP added (scrolls); percent for Elixirs")
    mp: int = i16("MP restored (potions) or max MP added (scrolls); percent for Elixirs")
    attack: int = i16("attack added (scrolls) or star attack")
    defense: int = i16("defense added (scrolls)")
    accuracy: int = i16("accuracy added (scrolls)")
    avoidability: int = i16("avoidability added (scrolls)")
    max_stack: int = i16("stack size (item+0xFC); 100, stars 200..800")
    price: int = i32("price in meso")
    unknown_84: int = i32("unknown (item+0xA0 and +0xA4); stars only: 2..30")
    icon: int = i16("inventory icon index")
    description: str = text(128, "description; '$' marks a line break")


@dataclass(frozen=True)
class Skill:
    """skill.dat: one row per skill (297 bytes, 19 columns).

    The four (base, per_level) pairs give ``base + (level - 1) * per_level`` and fill the
    ``@dmg``, ``@time``, ``@etc`` and ``@mp`` placeholders of ``detail``.
    """

    id: int = u8("skill id (lookup key)")
    name: str = text(16, "display name")
    icon: int = i16("icon index (36-byte entries in the skill icon sheet)")
    branch: int = u8("job branch: 0 Rogue, 1 Assassin (claw), 2 Bandit (dagger)")
    tier: int = u8("skill-window tier 1..4")
    type: int = u8("0 basic attack, 1 passive, 2 buff, 3 active attack")
    max_level: int = u8("maximum skill level (0 = not learnable)")
    unknown_23: int = u8("unknown, never read through the accessors; 0 in every row")
    unknown_24: int = u8("unknown, never read through the accessors; 0 in every row")
    damage_base: int = i16("@dmg: damage percent at level 1")
    damage_per_level: int = i16("@dmg: added per level")
    time_base: int = i16("@time: duration at level 1")
    time_per_level: int = i16("@time: added per level")
    etc_base: int = i16("@etc: skill-specific value at level 1")
    etc_per_level: int = i16("@etc: added per level")
    mp_base: int = i16("@mp: MP cost at level 1")
    mp_per_level: int = i16("@mp: added per level")
    description: str = text(128, "description")
    detail: str = text(128, "effect text with @mp/@dmg/@time/@etc placeholders")


@dataclass(frozen=True)
class MarketItem:
    """market.dat: one row per shop listing (7 bytes, 4 columns); all-zero rows are unused."""

    shop: int = u8("shop id the listing belongs to")
    table: int = u8("0 item_equip, 1 item_etc")
    item: int = u8("item id in that table")
    price: int = i32("buying price in meso")


@dataclass(frozen=True)
class TableSpec:
    name: str
    record: type
    loader: int  # Thumb function pointer of the loader
    pointer: int  # VM address of the global that holds the loaded file


TABLES: dict[str, TableSpec] = {
    t.name: t
    for t in (
        TableSpec("hero", HeroLevel, 0x67181, 0x11A9A8),
        TableSpec("mob", Mob, 0x77225, 0x11A9E8),
        TableSpec("npc", Npc, 0x39895, 0x11A988),
        TableSpec("item_equip", ItemEquip, 0x86215, 0x11A9AC),
        TableSpec("item_etc", ItemEtc, 0x86215, 0x11A9B0),
        TableSpec("skill", Skill, 0x800A9, 0x11A9F8),
        TableSpec("market", MarketItem, 0x861D1, 0x11A994),
    )
}


@dataclass(frozen=True)
class Column:
    name: str
    kind: str  # "u8", "i16", "i32" or "str"
    offset: int
    size: int
    doc: str


def layout(record: type) -> tuple[Column, ...]:
    """The byte layout of a record class: one Column per field, in file order."""
    out = []
    offset = 0
    for f in dataclasses.fields(record):
        kind = f.metadata["kind"]
        size = f.metadata["width"] if kind == "str" else _KINDS[kind][1]
        out.append(Column(f.name, kind, offset, size, f.metadata["doc"]))
        offset += size
    return tuple(out)


def record_size(record: type) -> int:
    cols = layout(record)
    return cols[-1].offset + cols[-1].size


@dataclass(frozen=True)
class Table:
    name: str
    columns: int
    stride: int
    records: tuple[Any, ...]


def _decode_text(raw: bytes, where: str) -> str:
    value = raw.split(b"\0", 1)[0]
    try:
        return value.decode(ENCODING)
    except UnicodeDecodeError as e:
        raise DataTableError(f"{where}: string does not decode as {ENCODING}: {e}") from e


def parse(name: str, data: bytes) -> Table:
    """Parse table ``name`` (a key of TABLES) from the whole file's bytes."""
    if name not in TABLES:
        raise DataTableError(f"unknown table {name!r}")
    record = TABLES[name].record
    cols = layout(record)
    if len(data) < HEADER.size:
        raise DataTableError(f"{name}: {len(data)} bytes is too short for the header")
    columns, rows, stride = HEADER.unpack_from(data, 0)
    if columns != len(cols) or stride != record_size(record):
        raise DataTableError(
            f"{name}: header says {columns} columns x {stride} bytes, "
            f"layout has {len(cols)} x {record_size(record)}"
        )
    expected = HEADER.size + rows * stride
    if len(data) != expected:
        raise DataTableError(f"{name}: {rows} rows need {expected} bytes, file has {len(data)}")
    records = []
    for row in range(rows):
        base = HEADER.size + row * stride
        values = {}
        for c in cols:
            if c.kind == "str":
                where = f"{name} row {row} {c.name}"
                values[c.name] = _decode_text(
                    data[base + c.offset : base + c.offset + c.size], where
                )
            else:
                values[c.name] = struct.unpack_from(_KINDS[c.kind][0], data, base + c.offset)[0]
        records.append(record(**values))
    return Table(name, columns, stride, tuple(records))


def encode(name: str, records: list[Any] | tuple[Any, ...]) -> bytes:
    """Build a table file from records (the inverse of parse, for synthetic fixtures)."""
    record = TABLES[name].record
    cols = layout(record)
    out = bytearray(HEADER.pack(len(cols), len(records), record_size(record)))
    for r in records:
        for c in cols:
            value = getattr(r, c.name)
            if c.kind == "str":
                raw = value.encode(ENCODING)
                if len(raw) > c.size:
                    raise DataTableError(f"{name}.{c.name}: {len(raw)} bytes > {c.size}")
                out += raw.ljust(c.size, b"\0")
            else:
                out += struct.pack(_KINDS[c.kind][0], value)
    return bytes(out)


def parse_buildup(data: bytes) -> int:
    """buildup.dat: the decimal integer the game reads with atoi (0x980B5)."""
    raw = data.strip()
    if not raw or not raw.isdigit() or len(raw) > 9:
        raise DataTableError(f"buildup.dat: expected a decimal integer, got {data[:16]!r}")
    return int(raw)


def table_name(path: str | Path) -> str:
    return Path(path).stem


def load_dir(data_dir: str | Path) -> dict[str, Table]:
    """Parse every table in one ``<Language>/data`` directory."""
    data_dir = Path(data_dir)
    return {n: parse(n, (data_dir / f"{n}.dat").read_bytes()) for n in TABLES}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.datatables", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("dump", help="print every record of one table").add_argument("file")
    sub.add_parser("check", help="parse every table in each data dir").add_argument(
        "dirs", nargs="+"
    )
    args = parser.parse_args(argv)
    try:
        if args.cmd == "dump":
            path = Path(args.file)
            if table_name(path) == "buildup":
                print(parse_buildup(path.read_bytes()))
                return 0
            table = parse(table_name(path), path.read_bytes())
            print(f"{table.name}: {len(table.records)} rows, {table.columns} columns")
            for r in table.records:
                print(r)
        else:
            for d in args.dirs:
                tables = load_dir(d)
                buildup = parse_buildup((Path(d) / "buildup.dat").read_bytes())
                rows = ", ".join(f"{n} {len(t.records)}" for n, t in tables.items())
                print(f"ok  {d}: {rows}, buildup {buildup}")
    except (OSError, DataTableError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
