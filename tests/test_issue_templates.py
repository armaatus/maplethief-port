"""The issue templates apply the labels triage relies on (#330): a new bug report carries
``needs-triage`` until the maintainer has read it (CONTRIBUTING.md → Triage)."""

from __future__ import annotations

import re

from repo_paths import REPO

TEMPLATES = REPO / ".github" / "ISSUE_TEMPLATE"


def template_labels(name: str) -> list[str]:
    text = (TEMPLATES / name).read_text(encoding="utf-8")
    m = re.search(r"^labels:\s*\[(.*)\]\s*$", text, re.MULTILINE)
    assert m, f"{name} has no one-line labels list"
    return [label.strip().strip("\"'") for label in m.group(1).split(",") if label.strip()]


def test_bug_report_needs_triage() -> None:
    assert "needs-triage" in template_labels("bug.yml")


def test_contributing_explains_triage() -> None:
    text = (REPO / "CONTRIBUTING.md").read_text(encoding="utf-8")
    assert "## Triage" in text
    assert "`needs-triage`" in text
