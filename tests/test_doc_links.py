"""Every relative Markdown link in README.md, CONTRIBUTING.md and docs/ resolves (#270): the
target exists, and a ``#fragment`` on a Markdown target names one of its headings.

External links (``scheme:`` and ``//``) aren't fetched. Fenced code blocks and code spans are
skipped. ``docs/compendium/pages/`` are the compendium site's own pages, which link to the
built site's ``.html`` paths, so they are left out.
"""

from __future__ import annotations

import re
import subprocess
import unicodedata
from pathlib import Path

import pytest

from repo_paths import REPO

LINK = re.compile(r"!?\[(?:[^\]\\]|\\.)*\]\(\s*<?([^)<>\s]+)>?(?:\s+\"[^\"]*\")?\s*\)")
FENCE = re.compile(r"^\s*(```|~~~)")
CODE_SPAN = re.compile(r"(`+).+?\1")
HEADING = re.compile(r"^#{1,6}\s+(.*?)\s*#*\s*$")


def tracked_markdown() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "-z", "--", "README.md", "CONTRIBUTING.md", "docs"],
        cwd=REPO,
        capture_output=True,
        check=True,
    ).stdout
    names = [n for n in out.decode("utf-8").split("\0") if n.endswith(".md")]
    return [REPO / n for n in names if not n.startswith("docs/compendium/pages/")]


def lines_outside_fences(path: Path) -> list[str]:
    lines = []
    fence = ""
    for line in path.read_text(encoding="utf-8").splitlines():
        m = FENCE.match(line)
        if m:
            if not fence:
                fence = m.group(1)
            elif fence == m.group(1):
                fence = ""
            lines.append("")
            continue
        lines.append("" if fence else line)
    return lines


def links(path: Path) -> list[str]:
    return [
        m.group(1)
        for line in lines_outside_fences(path)
        for m in LINK.finditer(CODE_SPAN.sub("", line))
    ]


def anchor_char(c: str) -> bool:
    """What GitHub keeps from a heading: letters, marks, decimal digits and connectors such as
    ``_``. A superscript digit (category No) is dropped, unlike Python's ``\\w``."""
    category = unicodedata.category(c)
    return category[0] in "LM" or category in ("Nd", "Pc")


def anchor(heading: str) -> str:
    """GitHub's anchor for a heading: its text, without markup and punctuation, in lower case,
    with each space a dash."""
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading).replace("`", "").replace("*", "")
    kept = "".join(c for c in text if c in "- " or anchor_char(c))
    return kept.strip().lower().replace(" ", "-")


def anchors(path: Path) -> set[str]:
    """Every heading's anchor in ``path``, with GitHub's -1, -2 for repeated headings, plus the
    ids of inline HTML anchors."""
    seen: dict[str, int] = {}
    found: set[str] = set()
    for line in lines_outside_fences(path):
        found.update(re.findall(r"<a\s+(?:name|id)=\"([^\"]+)\"", line))
        m = HEADING.match(line)
        if m:
            base = anchor(m.group(1))
            n = seen.get(base, 0)
            seen[base] = n + 1
            found.add(f"{base}-{n}" if n else base)
    return found


def problem(source: Path, target: str) -> str | None:
    """Why ``target``, a link in ``source``, doesn't resolve, or None if it does."""
    if target.startswith("//") or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
        return None  # external
    path_part, _, fragment = target.partition("#")
    dest = (source.parent / path_part) if path_part else source
    # A link starting with "/" is relative to the repository root on GitHub; none of ours are.
    if path_part.startswith("/"):
        return "starts with '/', which only works on GitHub"
    if not dest.exists():
        return "no such file"
    if dest.is_dir():
        return None
    if fragment and dest.suffix == ".md" and fragment not in anchors(dest):
        return f"no heading gives the anchor #{fragment}"
    return None


def check(paths: list[Path]) -> list[str]:
    errors = []
    for path in paths:
        for target in links(path):
            why = problem(path, target)
            if why:
                rel = path.relative_to(REPO) if path.is_relative_to(REPO) else path
                errors.append(f"{rel}: {target}: {why}")
    return errors


def test_every_relative_link_resolves():
    errors = check(tracked_markdown())
    assert not errors, "\n".join(errors)


def test_the_build_docs_are_tracked():
    out = subprocess.run(
        ["git", "ls-files", "docs/build"],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    assert {f"docs/build/{p}.md" for p in ("README", "running", "testing", "releases")} <= set(out)


@pytest.fixture
def page(tmp_path):
    """A page beside a target page with a heading and a folder."""
    (tmp_path / "target.md").write_text("# Playing\n", encoding="utf-8")
    (tmp_path / "folder").mkdir()
    return tmp_path / "page.md"


@pytest.mark.parametrize(
    "text",
    [
        "[a](does-not-exist.md)",
        "[a](target.md#no-such-heading)",
        "![a](missing.png)",
    ],
)
def test_a_dead_link_is_found(page, text):
    page.write_text(text + "\n", encoding="utf-8")
    assert check([page]), f"{text} should be reported"


@pytest.mark.parametrize(
    "text",
    [
        "[a](https://example.com/x#y)",
        "[a](mailto:x@example.com)",
        "`[a](does-not-exist.md)`",
        "```\n[a](does-not-exist.md)\n```",
        "[a](target.md#playing)",
        "[a](folder/)",
    ],
)
def test_a_good_link_or_code_is_not_reported(page, text):
    page.write_text(text + "\n", encoding="utf-8")
    assert not check([page]), f"{text} should not be reported"
