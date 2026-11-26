"""Each folder of docs with an index lists every page in it (#333): ``docs/re/``,
``docs/testing/`` and ``docs/build/how-to/``. A new page that its index doesn't link is a page
no one finds."""

from __future__ import annotations

import re

import pytest

from repo_paths import REPO

INDEXED = ("docs/re", "docs/testing", "docs/build/how-to")
LINK = re.compile(r"\]\(\s*<?([^)<>\s#]+)")


@pytest.mark.parametrize("folder", INDEXED)
def test_the_index_links_every_page(folder):
    root = REPO / folder
    index = root / "README.md"
    assert index.is_file(), f"{folder} has no README.md index"
    linked = {(root / m).resolve() for m in LINK.findall(index.read_text(encoding="utf-8"))}
    pages = {p.resolve() for p in root.glob("*.md") if p.name != "README.md"}
    pages |= {p.resolve() for p in root.glob("*/README.md")}
    missing = sorted(str(p.relative_to(REPO)) for p in pages - linked)
    assert not missing, f"add these to {folder}/README.md: {missing}"
