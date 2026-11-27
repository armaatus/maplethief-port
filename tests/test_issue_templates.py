"""The issue templates (#327, #330): a player's bug report asks for what triage needs (version,
system, log), every template warns against attaching game files, and a new bug report carries
``needs-triage`` until the maintainer has read it (CONTRIBUTING.md → Triage). ``scripts/lint``
checks the forms' syntax against GitHub's schema; these tests check their content."""

from __future__ import annotations

import re

import pytest

from repo_paths import REPO

TEMPLATES = REPO / ".github" / "ISSUE_TEMPLATE"
FORMS = sorted(p.name for p in TEMPLATES.glob("*.yml") if p.name != "config.yml")


def template_labels(name: str) -> list[str]:
    text = (TEMPLATES / name).read_text(encoding="utf-8")
    m = re.search(r"^labels:\s*\[(.*)\]\s*$", text, re.MULTILINE)
    assert m, f"{name} has no one-line labels list"
    return [label.strip().strip("\"'") for label in m.group(1).split(",") if label.strip()]


def fields(name: str) -> dict[str, str]:
    """The form's body items with an id, as {id: the item's YAML text}."""
    text = (TEMPLATES / name).read_text(encoding="utf-8")
    items = re.split(r"^  - type: ", text.split("\nbody:\n", 1)[1], flags=re.MULTILINE)
    out: dict[str, str] = {}
    for item in items:
        m = re.search(r"^    id: (\S+)$", item, re.MULTILINE)
        if m:
            assert m.group(1) not in out, f"{name}: id {m.group(1)} twice"
            out[m.group(1)] = item
    return out


def required(item: str) -> bool:
    return re.search(r"^      required: true$", item, re.MULTILINE) is not None


def test_forms_found() -> None:
    assert {"bug.yml", "feature.yml"} <= set(FORMS)


def test_bug_report_needs_triage() -> None:
    assert "needs-triage" in template_labels("bug.yml")


def test_bug_report_asks_for_version_system_and_log() -> None:
    f = fields("bug.yml")
    for key in ("version", "source", "system", "preset", "what", "steps"):
        assert key in f and required(f[key]), f"bug.yml: {key} missing or optional"
    assert "--version" in f["version"]
    for system in ("Windows", "macOS", "Linux", "Steam Deck", "iPhone", "Nintendo Switch"):
        assert system in f["system"], f"bug.yml: no {system} among the systems"
    # A place for the log and the crash report: a textarea without `render`, so files can be
    # dragged in.
    assert f["logs"].startswith("textarea\n")
    assert "data/logs/" in f["logs"] and "data/crashes/" in f["logs"]
    assert "render:" not in f["logs"]


@pytest.mark.parametrize("name", FORMS)
def test_template_warns_against_game_files(name: str) -> None:
    text = (TEMPLATES / name).read_text(encoding="utf-8")
    assert "Don't attach game files" in text


def test_feature_request_needs_triage() -> None:
    assert template_labels("feature.yml") == ["type:feature", "needs-triage"]


def test_questions_go_to_discussions() -> None:
    text = (TEMPLATES / "config.yml").read_text(encoding="utf-8")
    assert "blank_issues_enabled: false" in text
    assert "/discussions/categories/q-a" in text


def test_contributing_explains_triage() -> None:
    text = (REPO / "CONTRIBUTING.md").read_text(encoding="utf-8")
    assert "## Triage" in text
    assert "`needs-triage`" in text
