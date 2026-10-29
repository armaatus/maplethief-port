"""The generated CP949 table of the runtime's CFString encodings (#48, ADR 0032)."""

from __future__ import annotations

import pytest

from mtport import cp949


def test_committed_table_is_current():
    assert cp949.TABLE.read_text(encoding="utf-8") == cp949.generate()


def test_ksx1001_region():
    table = cp949.ksx1001()
    assert len(table) == 94 * 94
    at = lambda lead, trail: table[(lead - 0xA1) * 94 + (trail - 0xA1)]  # noqa: E731
    assert at(0xB0, 0xA1) == 0xAC00  # 가
    assert at(0xBC, 0xD2) == ord("소")  # docs/ANALYSIS.md: bc d2 c8 af ... = 소환
    assert at(0xC8, 0xAF) == ord("환")
    assert at(0xA1, 0xA1) == 0x3000
    assert sum(1 for v in table if v) == 8226


def test_extension_rule_matches_codec():
    table = cp949.ksx1001()
    ext = cp949.extension(table)
    assert len(ext) == 8822
    assert ext[(0x81, 0x41)] == 0xAC02
    assert ext[(0xC6, 0x52)] == 0xD7A3
    cp949.check_extension(table)


def test_extension_rule_failure_is_reported():
    table = cp949.ksx1001()
    table[table.index(0xAC00)] = 0  # pretend 가 is missing: the rule shifts every code
    with pytest.raises(ValueError):
        cp949.check_extension(table)
