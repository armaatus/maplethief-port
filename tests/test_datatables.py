"""Unit tests for the language data table parser (synthetic fixtures only)."""

import dataclasses
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

from mtport import datatables as dt
from repo_paths import REPO

SPEC = REPO / "docs" / "formats" / "data-tables.md"

# (columns, stride) of each table, as in the shipped headers (docs/formats/data-tables.md).
SHAPES = {
    "hero": (13, 18),
    "mob": (17, 50),
    "npc": (2, 33),
    "item_equip": (24, 73),
    "item_etc": (14, 218),
    "skill": (19, 297),
    "market": (4, 7),
}


def sample(record: type, seed: int):
    """A record with every field set to a distinct synthetic value."""
    values = {}
    for i, c in enumerate(dt.layout(record)):
        if c.kind == "str":
            values[c.name] = f"T{seed}-{c.name}"[: c.size]
        elif c.kind == "u8":
            values[c.name] = (seed * 7 + i) % 256
        elif c.kind == "i16":
            values[c.name] = -(seed + i) if i % 2 else seed * 100 + i
        else:
            values[c.name] = -(seed * 100000 + i) if i % 3 == 0 else seed * 100000 + i
    return record(**values)


def test_layouts_match_the_shipped_headers():
    assert set(SHAPES) == set(dt.TABLES)
    for name, (columns, stride) in SHAPES.items():
        record = dt.TABLES[name].record
        assert len(dt.layout(record)) == columns, name
        assert dt.record_size(record) == stride, name


def test_layout_offsets_are_contiguous():
    for spec in dt.TABLES.values():
        offset = 0
        for c in dt.layout(spec.record):
            assert c.offset == offset
            offset += c.size


def test_npc_from_hand_written_bytes():
    data = struct.pack("<HHH", 2, 2, 33)
    data += b"\x00" + b"Tomb".ljust(32, b"\0")
    data += b"\x01" + b"Mr. $X".ljust(32, b"\0")
    table = dt.parse("npc", data)
    assert (table.columns, table.stride) == (2, 33)
    assert table.records == (dt.Npc(0, "Tomb"), dt.Npc(1, "Mr. $X"))


def test_market_from_hand_written_bytes():
    data = struct.pack("<HHH", 4, 2, 7) + bytes([3, 1, 24]) + struct.pack("<i", 100)
    data += bytes(7)
    table = dt.parse("market", data)
    assert table.records[0] == dt.MarketItem(shop=3, table=1, item=24, price=100)
    assert table.records[1] == dt.MarketItem(0, 0, 0, 0)


def test_mob_from_hand_written_bytes():
    rec = bytes([5]) + b"Snail".ljust(16, b"\0")
    rec += struct.pack("<ihBBhhBi", 20, 8, 2, 1, 7, 15, 9, 600)
    rec += struct.pack("<BiBiBi", 32, 930, 6, 40, 0, 0) + bytes([60])
    assert len(rec) == 50
    mob = dt.parse("mob", struct.pack("<HHH", 17, 1, 50) + rec).records[0]
    assert (mob.id, mob.name, mob.hp, mob.level, mob.exp, mob.meso) == (5, "Snail", 20, 8, 7, 15)
    assert (mob.equip_drop, mob.equip_drop_chance) == (9, 600)
    assert (mob.etc_drop_1, mob.etc_drop_1_chance, mob.etc_drop_2) == (32, 930, 6)
    assert mob.sprite == 60


def test_signed_columns():
    rec = bytes(17) + struct.pack("<ihBBhhBi", -1, -2, 0, 0, -3, 0, 0, 0) + bytes(16)
    mob = dt.parse("mob", struct.pack("<HHH", 17, 1, 50) + rec).records[0]
    assert (mob.hp, mob.level, mob.exp) == (-1, -2, -3)


@pytest.mark.parametrize("name", sorted(dt.TABLES))
def test_encode_parse_round_trip(name):
    record = dt.TABLES[name].record
    records = [sample(record, s) for s in range(1, 4)]
    data = dt.encode(name, records)
    columns, stride = SHAPES[name]
    assert data[:6] == struct.pack("<HHH", columns, 3, stride)
    assert len(data) == 6 + 3 * stride
    assert list(dt.parse(name, data).records) == records


def test_korean_text_round_trips_as_cp949():
    npc = dt.Npc(7, "테스트 상인")
    data = dt.encode("npc", [npc])
    assert "테스트".encode("cp949") in data
    assert dt.parse("npc", data).records == (npc,)


def test_string_filling_the_whole_field_needs_no_nul():
    data = struct.pack("<HHH", 2, 1, 33) + b"\x01" + b"A" * 32
    assert dt.parse("npc", data).records[0].name == "A" * 32


def test_string_stops_at_first_nul():
    data = struct.pack("<HHH", 2, 1, 33) + b"\x01" + b"Ab\0junk".ljust(32, b"\0")
    assert dt.parse("npc", data).records[0].name == "Ab"


@pytest.mark.parametrize(
    "data, message",
    [
        (b"\x02\x00\x01", "too short"),
        (struct.pack("<HHH", 3, 1, 33) + bytes(33), "columns"),
        (struct.pack("<HHH", 2, 1, 34) + bytes(34), "columns"),
        (struct.pack("<HHH", 2, 2, 33) + bytes(33), "need 72 bytes"),
        (struct.pack("<HHH", 2, 1, 33) + bytes(34), "need 39 bytes"),
        (struct.pack("<HHH", 2, 1, 33) + b"\x01\xff\xff" + bytes(30), "cp949"),
    ],
)
def test_rejects_malformed_tables(data, message):
    with pytest.raises(dt.DataTableError, match=message):
        dt.parse("npc", data)


def test_rejects_unknown_table_and_oversized_strings():
    with pytest.raises(dt.DataTableError, match="unknown table"):
        dt.parse("buildup", b"1")
    with pytest.raises(dt.DataTableError, match="33 bytes > 32"):
        dt.encode("npc", [dt.Npc(0, "x" * 33)])


def test_buildup():
    assert dt.parse_buildup(b"1") == 1
    assert dt.parse_buildup(b" 12\n") == 12
    for bad in (b"", b"x", b"-1", b"1" * 10):
        with pytest.raises(dt.DataTableError):
            dt.parse_buildup(bad)


def test_loader_addresses_are_thumb_pointers():
    for spec in dt.TABLES.values():
        assert spec.loader & 1
        assert spec.pointer & 3 == 0


def spec_fields(text: str, heading: str) -> list[str]:
    """Field names (first backticked word of each table row) in the spec section ``heading``."""
    section = text.split(f"## {heading}\n", 1)[1].split("\n## ", 1)[0]
    return re.findall(r"^\|[^|]+\|[^|]+\|\s*`(\w+)`", section, re.M)


def test_spec_documents_every_field_in_order():
    text = SPEC.read_text(encoding="utf-8")
    for name, spec in dt.TABLES.items():
        names = [f.name for f in dataclasses.fields(spec.record)]
        assert spec_fields(text, f"`{name}.dat`") == names, name


def make_dir(tmp_path: Path) -> Path:
    for name, spec in dt.TABLES.items():
        (tmp_path / f"{name}.dat").write_bytes(dt.encode(name, [sample(spec.record, 1)]))
    (tmp_path / "buildup.dat").write_bytes(b"1")
    return tmp_path


def run_cli(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", "-m", "mtport.datatables", *args],
        capture_output=True,
        text=True,
        cwd=REPO / "tools",
        check=False,
    )


def test_load_dir_and_cli(tmp_path):
    data_dir = make_dir(tmp_path)
    assert {n: len(t.records) for n, t in dt.load_dir(data_dir).items()} == dict.fromkeys(
        dt.TABLES, 1
    )
    check = run_cli("check", str(data_dir))
    assert check.returncode == 0, check.stderr
    assert "buildup 1" in check.stdout
    dump = run_cli("dump", str(data_dir / "npc.dat"))
    assert dump.returncode == 0 and "Npc(id=" in dump.stdout
    assert run_cli("dump", str(data_dir / "buildup.dat")).stdout.strip() == "1"
    (data_dir / "mob.dat").write_bytes(b"\0")
    bad = run_cli("check", str(data_dir))
    assert bad.returncode == 1 and "too short" in bad.stderr
