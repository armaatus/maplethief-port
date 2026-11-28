"""The release notes pre-flight (#313): no merged pull request since the last release lands in
"Other changes" of GitHub's generated notes. Synthetic notes, in the shape GitHub's
generate-notes endpoint returns, stand in for GitHub."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys

import pytest

from mtport import release
from repo_paths import REPO

needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")

PULL = "https://github.com/armaatus/maplethief-port/pull"
HEADER = "<!-- Release notes generated using configuration in .github/release.yml at dev -->\n\n"
FOOTER = (
    "\n\n**Full Changelog**: https://github.com/armaatus/maplethief-port/compare/v9.0.0...v9.1.0\n"
)
LABELLED = f"""## What's Changed
### Milestones (epics)
* Epic #1: a synthetic epic by @someone in {PULL}/101
### Nintendo Switch
* A Switch fix by @someone in {PULL}/102"""
UNLABELLED = f"""
### Other changes
* An unlabelled change (#7) by @someone in {PULL}/103
* No author line in {PULL}/104"""
NEW_CONTRIBUTORS = f"""

## New Contributors
* @newcomer made their first contribution in {PULL}/105"""


def test_other_changes_lists_the_catch_all_only():
    notes = HEADER + LABELLED + UNLABELLED + NEW_CONTRIBUTORS + FOOTER
    assert release.other_changes(notes) == [
        release.NotesEntry(103, "An unlabelled change (#7)"),
        release.NotesEntry(104, "No author line"),
    ]


def test_other_changes_is_empty_when_every_pull_request_has_a_category():
    assert release.other_changes(HEADER + LABELLED + NEW_CONTRIBUTORS + FOOTER) == []
    assert release.other_changes("") == []


def test_catch_all_title_matches_release_yml():
    """The parser looks for the title of release.yml's "*" category; the epics' areas have one."""
    text = (REPO / ".github" / "release.yml").read_text(encoding="utf-8")
    titles = re.findall(r"^\s*- title: (.+)$", text, re.M)
    assert titles[-1] == release.OTHER_CHANGES
    assert re.search(r'labels: \["\*"\]\s*$', text)
    for label in ("area:platform-switch", "area:compendium", "area:platform-android"):
        assert label in text
    assert re.search(r"exclude:\s*\n\s*labels: \[type:release\]", text)


def _cli(tmp_path, notes: str) -> subprocess.CompletedProcess:
    f = tmp_path / "notes.md"
    f.write_text(notes, encoding="utf-8")
    env = dict(os.environ, PYTHONPATH=str(REPO / "tools"))
    return subprocess.run(
        [sys.executable, "-B", "-m", "mtport.release", "notes-check", "v9.1.0", "--notes", str(f)],
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )


def test_cli_fails_on_an_unlabelled_pull_request(tmp_path):
    out = _cli(tmp_path, HEADER + LABELLED + UNLABELLED + FOOTER)
    assert out.returncode == 1
    assert 'list 2 pull request(s) under "Other changes"' in out.stderr
    assert "#103 An unlabelled change (#7)" in out.stderr
    assert "#104 No author line" in out.stderr


def test_cli_passes_when_every_pull_request_has_a_category(tmp_path):
    out = _cli(tmp_path, HEADER + LABELLED + FOOTER)
    assert out.returncode == 0, out.stderr
    assert 'no pull request under "Other changes"' in out.stdout


def test_cli_rejects_a_bad_tag(tmp_path):
    f = tmp_path / "notes.md"
    f.write_text(LABELLED, encoding="utf-8")
    assert release.main(["notes-check", "v9", "--notes", str(f)]) == 1


TAG = "v0.0.0-rc.313"


@pytest.fixture
def notes_env(tmp_path):
    yield tmp_path, dict(os.environ, PYTHON=sys.executable)
    shutil.rmtree(REPO / "build" / "release" / TAG, ignore_errors=True)


def _notes(env: dict, tmp_path, notes: str) -> subprocess.CompletedProcess:
    f = tmp_path / "notes.md"
    f.write_text(notes, encoding="utf-8")
    return subprocess.run(
        ["bash", "scripts/release", "notes", TAG],
        cwd=REPO,
        capture_output=True,
        text=True,
        env=dict(env, MAPLETHIEF_RELEASE_NOTES=str(f)),
        timeout=60,
    )


@needs_bash
def test_scripts_release_notes_fails_on_an_unlabelled_pull_request(notes_env):
    tmp_path, env = notes_env
    out = _notes(env, tmp_path, HEADER + LABELLED + UNLABELLED + FOOTER)
    assert out.returncode == 1
    assert "#103 An unlabelled change" in out.stderr
    assert "a merged pull request has no category label" in out.stderr


@needs_bash
def test_scripts_release_notes_passes(notes_env):
    tmp_path, env = notes_env
    out = _notes(env, tmp_path, HEADER + LABELLED + FOOTER)
    assert out.returncode == 0, out.stderr
    assert "notes: OK" in out.stdout
