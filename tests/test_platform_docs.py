"""The platform list (#309): docs/platforms.json names the systems MapleThief plays on, and
README.md, docs/play/README.md and docs/BUILDING.md each show exactly those, one row each in
their 'System' table."""

from __future__ import annotations

import json
import re

import pytest

from repo_paths import REPO

DATA = REPO / "docs" / "platforms.json"
DOCS = ["README.md", "docs/play/README.md", "docs/BUILDING.md"]


def platforms() -> list[dict]:
    return json.loads(DATA.read_text(encoding="utf-8"))["platforms"]


def system_rows(text: str) -> list[str]:
    """The first cells of the first table whose header starts with 'System' (indented tables,
    as inside a list item, count too)."""
    rows: list[str] = []
    in_table = False
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not line.lstrip().startswith("|"):
            if in_table:
                break
            continue
        if not in_table:
            in_table = cells[0] == "System"
            continue
        if re.fullmatch(r":?-+:?", cells[0]):
            continue
        rows.append(cells[0])
    return rows


def test_system_rows_reads_an_indented_table():
    text = "x\n\n   | System | Minimum |\n   |---|---|\n   | A | 1 |\n   | B | 2 |\n\nafter\n"
    assert system_rows(text) == ["A", "B"]


def test_every_platform_names_a_row_in_every_doc():
    ids = [p["id"] for p in platforms()]
    assert len(ids) == len(set(ids))
    for p in platforms():
        assert p["status"] in ("supported", "preview"), p["id"]
        assert sorted(p["rows"]) == sorted(DOCS), p["id"]


@pytest.mark.parametrize("doc", DOCS)
def test_doc_shows_the_same_platforms(doc):
    shown = system_rows((REPO / doc).read_text(encoding="utf-8"))
    expected = [p["rows"][doc] for p in platforms()]
    assert sorted(shown) == sorted(expected), (
        f"{doc}'s System table and docs/platforms.json disagree: "
        f"only in the doc {sorted(set(shown) - set(expected))}, "
        f"only in the data {sorted(set(expected) - set(shown))}"
    )
