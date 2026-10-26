"""The LSDA parser of recomp.exceptions (#37, docs/formats/lsda.md) on synthetic tables.

The shapes are the ones the binary uses (cleanup-only tables without a type table, the
game's 0x9B type encoding with an empty exception specification, Chartboost's catch-all),
plus the encodings and errors the format allows. The real tables: test_recomp_exceptions_real.py.
"""

from __future__ import annotations

import pytest

from recomp import exceptions as ex

BASE = 0xE9490


def reader(data: bytes, base: int = BASE):
    def read(addr: int, n: int) -> bytes:
        off = addr - base
        return data[off : off + n] if off >= 0 else b""

    return read


def parse(data: bytes) -> ex.Lsda:
    return ex.parse_lsda(reader(data), BASE)


def test_cleanup_only_table_with_a_padded_length() -> None:
    # LLVM-GCC's: no LPStart, no type table, call-site encoding udata4 (ignored: the SjLj
    # records are ULEB128), the length 2 as a three-byte ULEB128 (82 80 00).
    lsda = parse(bytes.fromhex("ffff0382800000 00"))
    assert lsda.lpstart is None and lsda.ttype_base is None
    assert lsda.call_site_encoding == 3
    assert lsda.call_sites == (ex.CallSite(1, 0, 0),)
    assert lsda.kind(lsda.call_sites[0]) == "cleanup"
    assert lsda.size == 8


def test_empty_exception_specification() -> None:
    # The game's throw() functions: type encoding 0x9B, call sites 3 and 4 point at the
    # action (-1, end); the spec list at the type-table base is just its 0 terminator.
    data = bytes.fromhex("ff9b8c000308 0000010002010301 7f00 00 000000")
    lsda = parse(data)
    assert lsda.ttype_encoding == 0x9B
    assert lsda.ttype_base == BASE + 16
    assert [(s.index, s.landing_pad, s.action) for s in lsda.call_sites] == [
        (1, 0, 0), (2, 1, 0), (3, 2, 1), (4, 3, 1),
    ]  # fmt: skip
    assert lsda.actions == {0: ex.Action(0, -1, None)}
    assert lsda.specs == {-1: ()}
    assert lsda.types == {}
    assert [lsda.kind(s) for s in lsda.call_sites] == ["cleanup", "cleanup", "spec", "spec"]
    assert lsda.end == BASE + 17  # the 3 zero bytes after it are alignment padding


def test_catch_all_and_a_chain_with_a_cleanup() -> None:
    # Call site 1: (filter 1 = catch-all, next +1) -> (0: cleanup, end); call site 2:
    # cleanup only. Type [1] is a 0 entry: catch (...), not relocated by pcrel.
    data = bytes.fromhex("ff9b0e01 04 0001 0100 01010000 00000000")
    lsda = parse(data)
    s1, s2 = lsda.call_sites
    assert [a.filter for a in lsda.chain(s1)] == [1, 0]
    assert lsda.chain(s2) == ()
    assert lsda.types == {1: ex.TypeRef(0)}
    assert lsda.kind(s1) == "catch-all+cleanup"
    assert lsda.end == lsda.ttype_base == BASE + 17


def test_indirect_pc_relative_types_and_a_spec_with_types() -> None:
    # Type table 0x9B: entry = its own address + sdata4, the address of a pointer to the
    # type. Filter 2 catches type [2]; filter -1 is the spec list [1] at base + 0.
    entries = (0x1000).to_bytes(4, "little", signed=True) + (-0x20).to_bytes(
        4, "little", signed=True
    )
    head = bytes.fromhex("ff9b100102000102017f00")  # (2, next +1) -> (-1, end)
    data = head + b"\0" * (19 - len(head) - 8) + entries + bytes.fromhex("0100")
    lsda = parse(data)
    base = BASE + 19
    assert lsda.ttype_base == base
    assert lsda.chain(lsda.call_sites[0]) == (ex.Action(0, 2, 2), ex.Action(2, -1, None))
    assert lsda.types[2] == ex.TypeRef(base - 8 + 0x1000, indirect=True)
    assert lsda.types[1] == ex.TypeRef(base - 4 - 0x20, indirect=True)
    assert lsda.specs == {-1: (1,)}
    assert lsda.kind(lsda.call_sites[0]) == "catch+spec"
    assert lsda.end == base + 2


def test_lpstart_and_absolute_types() -> None:
    data = bytes.fromhex("00 0010000000 0a01 02 0001010044330000")
    lsda = parse(data)
    assert lsda.lpstart == 0x1000
    assert lsda.types == {1: ex.TypeRef(0x3344)}


@pytest.mark.parametrize(
    "hexdata, why",
    [
        ("ffff01", "truncated"),
        ("3001000000", "unsupported pointer application"),
        ("ffff0103000100 00", "overruns its length"),
        ("ffff01020001 0100", "filters but no type table"),
        ("ff0c0201020001 0100 0000", "no fixed size"),
        ("ffff01 ffffffffff7f", "longer than 5 bytes"),
    ],
)
def test_malformed_tables_are_errors(hexdata: str, why: str) -> None:
    with pytest.raises(ex.LsdaError, match=why):
        parse(bytes.fromhex(hexdata))


def test_sleb128_negative_and_multi_byte() -> None:
    r = ex._Reader(reader(bytes.fromhex("7f 807f 3f")), BASE, BASE)
    assert (r.sleb(), r.sleb(), r.sleb()) == (-1, -128, 63)
