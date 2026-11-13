"""The "About the game" section (#225) on a synthetic knowledge base, and its prose files.

Synthetic scripts only: every name and line of text here is made up. The prose checks read
the committed Markdown under docs/compendium/pages/about/ and docs/re/original-bugs.md.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.compendium import site
from mtport.compendium.sections import about
from tests.fixtures.compendium import site_kb as fx

ROOT = Path(__file__).resolve().parents[1]
ABOUT = ROOT / "docs" / "compendium" / "pages" / "about"
BUGS = ROOT / "docs" / "re" / "original-bugs.md"


def _script(sid: str, used: bool, reason: str | None = None, text=None, **more) -> dict:
    d, name = sid.split("-", 1)
    e = {
        "id": sid,
        "kind": "script",
        "name": {"en": f"{d}/{name}", "ko": f"{d}/{name}"},
        "dir": d,
        "base_name": name,
        "used": used,
        "unused_reason": reason,
        "chapters": [1] if used else [],
        "roles": ["npc"] if used else [],
        "functions": ["main", "talk"],
        "unused_quests": [],
        "notes": [],
        "text": text or [],
        "maps": [{"kind": "map", "id": "1-1"}] if used else [],
        "confidence": {},
    }
    e.update(more)
    return e


SCRIPTS = [
    _script("chapter1-Keeper", True),
    _script(
        "common-Keeper",
        False,
        "in chapter 1, where a map that names it is reachable, the game runs chapter1/Keeper"
        " instead",
        [{"key": "Common_Keeper.talk.1", "en": "Made-up line.", "ko": "지어낸 말."}],
    ),
    _script(
        "common-Juggler_old",
        False,
        "no map object names it",
        [{"key": "가짜 문장.", "en": "가짜 문장.", "ko": "가짜 문장."}],
        unused_quests=[{"kind": 1, "number": 7, "title_key": "가짜"}],
        notes=["common/Juggler_old: talk 0x10 startQuest(): a made-up slip"],
    ),
    _script("common-portal900", False, "no map that names it is reachable in any chapter"),
]

PROSE = {
    "about/index.md": "# About the game\n\nA synthetic about section.\n",
    "about/history.md": "# History\n\nMade up.\n",
}


@pytest.fixture
def built(tmp_path: Path) -> site.Site:
    kb = dict(fx.KB)
    kb["script"] = SCRIPTS
    kb_dir = fx.write_kb(tmp_path, kb)
    pages = fx.write_prose(tmp_path, {**fx.PROSE, **PROSE})
    return site.build_site(kb_dir, pages, None, plugins=[about.SECTION])


def text(s: site.Site, path: str) -> str:
    return s.files[path].decode("utf-8")


def test_every_page_of_the_section_is_built(built):
    for stem, _, _ in about.PAGES:
        assert f"about/{stem}.html" in built.files
    index = text(built, "about/index.html")
    for stem, title, _ in about.PAGES:
        assert f'href="{stem}.html"' in index and title in index


def test_every_script_has_an_anchor_on_the_scripts_page(built):
    missing, counts = site.completeness(built)
    assert missing == []
    assert counts["script"] == (len(SCRIPTS), len(SCRIPTS))
    page = text(built, "about/scripts.html")
    for e in SCRIPTS:
        assert f'id="{about.script_anchor(e["id"])}"' in page
    assert built.registry[("script", "common-Keeper")][0] == "about/scripts.html"
    # an unused script's row links to its entry on the unused page
    assert 'href="unused.html#script-common-Keeper"' in page
    assert "chapter 1" in page


def test_unused_scripts_have_entries_with_evidence_and_text(built):
    page = text(built, "about/unused.html")
    for e in SCRIPTS[1:]:
        assert f'<h3 id="{about.script_anchor(e["id"])}">' in page
    assert "3 of the 4 script files never run" in page
    # the override names the file that runs instead, linked to its row
    assert 'href="scripts.html#script-chapter1-Keeper"' in page
    # keyed text: English and Korean, as game text
    assert '<span data-verbatim lang="en" class="en">Made-up line.</span>' in page
    assert '<span data-verbatim lang="ko" class="ko">지어낸 말.</span>' in page
    # a literal Korean line is marked Korean only, and stays game text
    assert "This text is Korean only" in page
    assert '<li><span data-verbatim lang="ko">가짜 문장.</span></li>' in page
    assert "sub quest 7" in page and '<span data-verbatim lang="ko">가짜</span>' in page
    assert "a made-up slip" in page
    assert "Lines" in page


def test_korean_only_is_the_literal_case():
    assert about.korean_only({"key": "가나.", "en": "가나.", "ko": "가나."})
    assert not about.korean_only({"key": "A.b.1", "en": "Hi.", "ko": "안녕."})
    assert not about.korean_only({"key": "A.b.1", "en": "가나.", "ko": "가나."})


def test_unused_groups():
    assert [about.unused_group(e) for e in SCRIPTS[1:]] == [
        "overridden",
        "unnamed",
        "unreachable",
    ]


def test_glossary_reads_the_style_guide_terms():
    rows = about.glossary_rows(about.STYLE_GUIDE.read_text(encoding="utf-8"))
    style = about.STYLE_GUIDE.read_text(encoding="utf-8")
    terms = style.split("## Terms", 1)[1].split("\n## ", 1)[0]
    table = [ln for ln in terms.splitlines() if ln.startswith("|")]
    assert len(rows) == len(table) - 2  # header and separator
    assert all(len(r) == 4 for r in rows)
    assert rows[0][2] == "world"


def test_glossary_page_lists_every_term(built):
    page = text(built, "about/glossary.html")
    rows = about.glossary_rows(about.STYLE_GUIDE.read_text(encoding="utf-8"))
    assert page.count('<tr data-i="') == len(rows)


def test_glossary_rows_synthetic():
    md = "# X\n\n## Terms\n\n| English | Korean | Kind | Source |\n|---|---|---|---|\n"
    md += "| Aa | 가 | town | `t` 1 |\n| Bb | | job | scripts |\n\nAfter.\n\n## Next\n| no |\n"
    assert about.glossary_rows(md) == [["Aa", "가", "town", "`t` 1"], ["Bb", "", "job", "scripts"]]
    assert about.glossary_rows("no terms here") == []


# --- the committed prose -------------------------------------------------------------------


def test_every_original_bug_has_an_entry():
    """The bugs page has one entry per bug of docs/re/original-bugs.md, in the same order."""
    re_doc = BUGS.read_text(encoding="utf-8")
    ours = (ABOUT / "bugs.md").read_text(encoding="utf-8")
    listed = [int(n) for n in re.findall(r"^### (\d+)\. ", re_doc, re.M)]
    table = [int(n) for n in re.findall(r"^\| (\d+) \| \[", re_doc, re.M)]
    entries = [int(n) for n in re.findall(r"^## Bug (\d+): ", ours, re.M)]
    assert listed == table
    assert entries == listed
    assert len(entries) == 16


UNUSED_ENTRIES = {
    "monster-carnival": "Spiegelman",
    "orphan-collision-grid": "106.att",
    "empty-shop-rows": "202 to 219",
    "shops-no-npc-opens": "{{shop:16}}",
    "cash-items-no-product-grants": "{{item:etc:131}}",
    "the-build-up-value": "buildup.dat",
    "intro-movie-and-logo-timer": "MoviePlayerController",
    "special-map-object-kinds": "kind 42",
    "unused-root-images": "MainMenuCompanyLogo.png",
    "version-badges": "v1.0.5.png",
}


def _sections(md: str) -> dict[str, str]:
    out = {}
    for part in re.split(r"^## ", md, flags=re.M)[1:]:
        head, _, body = part.partition("\n")
        out[re.sub(r"[^a-z0-9]+", "-", head.lower()).strip("-")] = body
    return out


def test_every_unused_item_has_an_entry_with_evidence():
    secs = _sections((ABOUT / "unused.md").read_text(encoding="utf-8"))
    for anchor, needle in UNUSED_ENTRIES.items():
        assert anchor in secs, anchor
        assert needle in secs[anchor], anchor
        assert "**Evidence:**" in secs[anchor], anchor
        assert "https://github.com/armaatus/maplethief-port/blob/main/docs/" in secs[anchor]


def test_outside_claims_cite_a_source():
    """Every row of the release table names a source: a link, or the game's own files."""
    md = (ABOUT / "history.md").read_text(encoding="utf-8")
    table = md.split("## Releases", 1)[1].split("\n## ", 1)[0]
    rows = [ln for ln in table.splitlines() if ln.startswith("| ") and "---" not in ln][1:]
    assert rows
    for row in rows:
        source = row.strip("|").split("|")[-1]
        assert "](https://" in source or "the game's files" in source, row
    # the developer and the release come from cited pages
    assert "pocketgamer.com" in md and "toucharcade.com" in md


def test_technical_page_links_the_format_specs():
    md = (ABOUT / "technical.md").read_text(encoding="utf-8")
    linked = set(re.findall(r"docs/formats/([A-Za-z-]+\.md)", md))
    assert {"README.md", "script-out.md", "map.md", "att.md", "obj.md", "sprprj.md"} <= linked
    for name in linked:
        assert (ROOT / "docs" / "formats" / name).is_file(), name


def test_prose_lint_is_green_on_the_section():
    files = sorted(ABOUT.glob("*.md"))
    assert len(files) >= 9
    for f in files:
        assert prose_lint.lint_text(f.read_text(encoding="utf-8"), kind="md") == [], f.name


def test_built_pages_pass_the_prose_lint(built):
    for path, data in built.files.items():
        if path.startswith("about/"):
            assert prose_lint.lint_text(data.decode("utf-8"), kind="html") == [], path
