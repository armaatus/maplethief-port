"""Compendium prose lint (#212, tools/mtport/prose_lint.py): each rule fires on synthetic
prose, game text marked verbatim is exempt in Markdown and HTML, a clean file passes, and the
style guide itself passes."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from mtport import prose_lint
from mtport.prose_lint import Finding, lint_text

ROOT = Path(__file__).resolve().parents[1]
STYLE = ROOT / "docs" / "compendium" / "style.md"

CLEAN_MD = """\
# Blue snail

Blue Snail is a level 1 monster found on {{map:2-8}}. It gives 3 EXP and drops 2 to 4 meso.

## Drops and locations

| Item | Chance | Levels |
|---|---|---|
| {{item:etc:3}} | 2.5% (25/1000) | 1-5 |

Talk to {{npc:10}}, then collect 12 Leaves from Dark Stumps.

## Notes about Henesys

The column is probable ([spec](../formats/data-tables.md#mobdat--x), field `level`).

```text
a -- b — c
```

---
"""

CLEAN_HTML = """\
<!doctype html>
<html><head><title>Blue snail</title><style>p::before { content: "—"; }</style></head>
<body>
<h1><span data-verbatim>Blue Snail</span></h1>
<h2><span data-verbatim>Blue Snail</span> drops</h2>
<p>A level 1 monster.<br>It drops 2 to 4 meso.</p>
<p data-verbatim>Game text -- with — dashes, Title Case And All. Delve!</p>
<div data-verbatim="name"><p>Nested – game text</p><img src="a.png"></div>
<pre>code -- here</pre><code>x – y</code>
<script>var a = "—"; // --</script>
<!-- a comment -- here -->
</body></html>
"""


def rules(findings: list[Finding]) -> list[str]:
    return [f.rule for f in findings]


def test_clean_markdown_passes() -> None:
    assert lint_text(CLEAN_MD, "md") == []


def test_clean_html_passes() -> None:
    assert lint_text(CLEAN_HTML, "html") == []


@pytest.mark.parametrize(
    ("line", "rule"),
    [
        ("Blue Snail — a monster.", "em-dash"),
        ("Levels 10–15.", "en-dash"),
        ("A bar ― here.", "other-dash"),
        ("A snail -- slow.", "double-hyphen"),
        ("A snail--slow.", "double-hyphen"),
        ("A snail --- slow.", "double-hyphen"),
        ("It's worth noting that snails are slow.", "banned-phrase"),
        ("It’s worth noting that snails are slow.", "banned-phrase"),
        ("Let us delve into the drops.", "banned-phrase"),
        ("In conclusion, snails are slow.", "banned-phrase"),
        ("We recommend a claw.", "first-person"),
        ("Let's go.", "first-person"),
        ("It costs 1,500 mesos.", "term"),
        ("A snail \U0001f40c.", "emoji"),
        ("Done ✅.", "emoji"),
        ("## Drops And Locations", "title-case-heading"),
        ("# Where To Train", "title-case-heading"),
        ("### Blue snail Strategy", "title-case-heading"),
    ],
)
def test_each_rule_fires_in_markdown(line: str, rule: str) -> None:
    findings = lint_text(line + "\n", "md")
    assert rule in rules(findings), findings


@pytest.mark.parametrize(
    ("html", "rule"),
    [
        ("<p>Blue Snail — a monster.</p>", "em-dash"),
        ("<p>Levels 10&ndash;15.</p>", "en-dash"),
        ("<p>Blue Snail &mdash; a monster.</p>", "em-dash"),
        ("<p>A snail -- slow.</p>", "double-hyphen"),
        ("<p>In conclusion, slow.</p>", "banned-phrase"),
        ("<p>Our pick.</p>", "first-person"),
        ("<td>12 mesos</td>", "term"),
        ("<p>\U0001f40c</p>", "emoji"),
        ("<h2>Drops And Locations</h2>", "title-case-heading"),
        ("<h2><span data-verbatim>Blue Snail</span> Drops</h2>", "title-case-heading"),
        ("<p title='x'><span data-verbatim>ok</span> but — not this</p>", "em-dash"),
    ],
)
def test_each_rule_fires_in_html(html: str, rule: str) -> None:
    assert rule in rules(lint_text(html, "html"))


def test_positions_are_one_based_line_and_column() -> None:
    md = lint_text("Fine.\nA snail — slow.\n", "md", path="p.md")
    assert md == [Finding("p.md", 2, 9, "em-dash", prose_lint.DASHES["—"][1])]
    assert str(md[0]).startswith("p.md:2:9: em-dash: ")
    html = lint_text("<html>\n<body>\n<p>ok — no</p>\n", "html")
    assert [(f.line, f.col) for f in html] == [(3, 7)]


def test_markdown_exemptions() -> None:
    exempt = [
        "Use `a -- b` and `—` in code spans.",
        "``a ` -- ` b`` is one span.",
        "References {{item:etc:3 -- x}} and {% if a -- b %} tags.",
        "A [link](https://example.com/a--b—c) target.",
        'Inline <span data-verbatim="1">Title Case — game text</span> is exempt.',
        "<!-- a comment -->",
        "| Level | Chance |",
        "|---|:---:|",
        "---",
        "~~~",
        "— inside a tilde fence",
        "~~~",
    ]
    assert lint_text("\n".join(exempt) + "\n", "md") == []


def test_text_after_a_closed_fence_is_checked() -> None:
    assert lint_text("```\na — b\n```\nc — d\n", "md")[0].line == 4


def test_heading_rule_allows_names_acronyms_and_new_sentences() -> None:
    ok = [
        "# Henesys",
        "## Shops in Kerning City and El Nath",
        "## Mr. Lee and Mr. Wang",
        "## HP, MP and EXP of NPCs",
        "## Training: Where to go",
        "## Lv. 10 monsters",
        "## {{monster:1}} drops",
        "## MacArthur's quests",
        "## The A and B buttons",
        "## Robin's Double Step",
    ]
    for h in ok:
        assert lint_text(h + "\n", "md") == [], h
    assert lint_text("## Ligator drops\n", "md") == []  # first word
    assert rules(lint_text("## Drops of Ligator\n", "md")) == ["title-case-heading"]
    assert lint_text("## Drops of Ligator\n", "md", names={"Ligator"}) == []


def test_first_person_does_not_match_inside_words_or_names() -> None:
    assert lint_text("Abandoned Mine, a status, the user, Dumbo.\n", "md") == []


def test_kind_must_be_md_or_html() -> None:
    with pytest.raises(ValueError):
        lint_text("x", "txt")


def test_glossary_names_reads_the_terms_table() -> None:
    md = (
        "# Guide\n\n## Terms\n\n| English | Korean |\n|---|---|\n| Snow Queen | x |\n"
        "\n## Other\n\n| Not Read | y |\n"
    )
    assert prose_lint.glossary_names(md) == {"English", "Snow", "Queen"}


def test_style_guide_passes_and_covers_the_issue() -> None:
    text = STYLE.read_text(encoding="utf-8")
    assert lint_text(text, "md", path=str(STYLE)) == []
    headings = [h.lower() for h in re.findall(r"^#+\s+(.*)$", text, re.MULTILINE)]
    for topic in (
        "voice",
        "punctuation",
        "headings",
        "numbers and units",
        "terms",
        "spoilers",
        "evidence",
    ):
        assert topic in headings, topic
    for page in ("monster", "item", "npc", "shop", "map", "skill", "quest", "chapter", "guide"):
        assert f"{page} page" in headings, page
    # One worked example (a fenced Markdown block) per page template.
    sections = re.split(r"^### ", text, flags=re.MULTILINE)[1:]
    pages = [s for s in sections if s.split("\n", 1)[0].lower().endswith(" page")]
    assert len(pages) == 9
    for s in pages:
        assert "Infobox" in s and "Sections, in order:" in s and "```markdown" in s, s[:40]


def test_style_guide_lists_exactly_the_banned_phrases() -> None:
    text = STYLE.read_text(encoding="utf-8")
    line = next(ln for ln in text.splitlines() if ln.strip().startswith("`it's worth noting`"))
    assert tuple(re.findall(r"`([^`]+)`", line)) == prose_lint.BANNED_PHRASES


def test_glossary_feeds_known_names() -> None:
    names = prose_lint.known_names()
    for n in ("Henesys", "Kerning", "MacArthur", "Balrog", "Meso"):
        assert n in names


def test_cli_reports_and_exits(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    good = tmp_path / "site" / "good.html"
    good.parent.mkdir()
    good.write_text(CLEAN_HTML, encoding="utf-8")
    assert prose_lint.main([str(good.parent)]) == 0
    bad = tmp_path / "site" / "pages" / "bad.md"
    bad.parent.mkdir()
    bad.write_text("# Ok\n\nA – b.\n", encoding="utf-8")
    (tmp_path / "site" / "skip.txt").write_text("—", encoding="utf-8")
    assert prose_lint.main([str(tmp_path / "site")]) == 1
    out = capsys.readouterr().out.splitlines()
    assert out == [f"{bad}:3:3: en-dash: {prose_lint.DASHES[chr(0x2013)][1]}"]


def test_default_paths_include_the_style_guide() -> None:
    assert STYLE in prose_lint.default_paths()
    assert prose_lint.main([]) == 0
