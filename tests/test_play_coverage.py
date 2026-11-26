"""The player guide's coverage table (#332): docs/play/README.md has a table whose header starts
with 'Platform', one row per system of docs/platforms.json, and every cell names at least one
page by a link that resolves (the file, and the heading a ``#fragment`` names)."""

from __future__ import annotations

import re

import pytest
import test_doc_links as doc_links
import test_platform_docs as platform_docs

from repo_paths import REPO

README = REPO / "docs" / "play" / "README.md"
COLUMNS = [
    "Platform",
    "Install",
    "Controls",
    "Troubleshooting",
    "Reset and uninstall",
    "Save transfer",
]


def table(text: str, first: str = "Platform") -> list[list[str]]:
    """The rows (header first, the separator left out) of the first table whose header's first
    cell is ``first``."""
    rows: list[list[str]] = []
    for line in text.splitlines():
        if not line.lstrip().startswith("|"):
            if rows:
                break
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not rows and cells[0] != first:
            continue
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows


def cell_links(cell: str) -> list[str]:
    return [m.group(1) for m in doc_links.LINK.finditer(cell)]


def test_table_reads_rows_and_skips_other_tables():
    text = "| System | x |\n|---|---|\n| A | 1 |\n\n| Platform | y |\n|---|---|\n| B | [b](b.md) |\n"
    assert table(text) == [["Platform", "y"], ["B", "[b](b.md)"]]


def test_the_table_has_the_columns():
    rows = table(README.read_text(encoding="utf-8"))
    assert rows, "docs/play/README.md has no table starting with 'Platform'"
    assert rows[0] == COLUMNS


def test_the_table_has_a_row_per_platform():
    rows = table(README.read_text(encoding="utf-8"))[1:]
    expected = [p["rows"]["docs/play/README.md"] for p in platform_docs.platforms()]
    assert sorted(r[0] for r in rows) == sorted(expected)


@pytest.mark.parametrize("row", table(README.read_text(encoding="utf-8"))[1:], ids=lambda r: r[0])
def test_no_empty_cell_and_every_link_resolves(row):
    assert len(row) == len(COLUMNS), row
    for column, cell in zip(COLUMNS[1:], row[1:], strict=True):
        targets = cell_links(cell)
        assert targets, f"{row[0]} / {column}: the cell has no link: {cell!r}"
        for target in targets:
            assert not target.startswith(("http:", "https:")), (
                f"{row[0]} / {column}: {target} should be a page of this guide"
            )
            why = doc_links.problem(README, target)
            assert why is None, f"{row[0]} / {column}: {target}: {why}"


def test_a_dead_link_in_a_cell_is_found(tmp_path):
    page = tmp_path / "README.md"
    page.write_text("# Top\n", encoding="utf-8")
    assert doc_links.problem(page, "missing.md") == "no such file"
    assert doc_links.problem(page, "#nope") is not None
    assert doc_links.problem(page, "#top") is None
