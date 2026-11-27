"""The pull request template (#328): a one-commit pull request fills the short part at the top
(what, why, how it was tested, which real-asset tests ran) without deleting anything; the epic
part sits below it in a collapsed, optional ``<details>`` block."""

from __future__ import annotations

import re

from repo_paths import REPO

TEMPLATE = REPO / ".github" / "pull_request_template.md"
COMMENT = re.compile(r"<!--.*?-->", re.DOTALL)


def parts() -> tuple[str, str]:
    text = TEMPLATE.read_text(encoding="utf-8")
    short, sep, epic = text.partition("<details>")
    assert sep, "the epic part is a <details> block"
    return short, epic


def headings(text: str) -> list[str]:
    return [m.group(1) for m in re.finditer(r"^#{2,6}\s+(.+?)\s*$", text, re.MULTILINE)]


def test_short_part_asks_what_why_and_how_tested() -> None:
    short, _ = parts()
    assert headings(short) == ["What", "Why", "How it was tested", "Checklist"]
    assert short.startswith("Closes #")
    assert "Real-asset tests" in short


def test_short_part_has_no_epic_placeholders() -> None:
    # A one-commit pull request deletes nothing: outside comments, the short part never asks
    # for an epic, a child or per-child criteria.
    short, _ = parts()
    words = COMMENT.sub("", short).lower()
    for word in ("epic", "child", "<epic>", "exit criteria"):
        assert word not in words, word


def test_epic_part_is_optional_and_complete() -> None:
    _, epic = parts()
    summary = re.search(r"<summary>(.*?)</summary>", epic)
    assert summary and "optional" in summary.group(1).lower()
    assert "</details>" in epic
    assert headings(epic) == [
        "Epic exit criteria",
        "Acceptance criteria per child",
        "#N Title",
        "Checks",
        "Epic checklist",
        "Notes for review",
    ]
    assert "Closes #<child>" in epic
