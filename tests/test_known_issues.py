"""The known-issues pre-flight (#309): docs/KNOWN_ISSUES.md cites no closed issue. Synthetic
files and a given list of closed issues stand in for docs/KNOWN_ISSUES.md and GitHub."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

import pytest

from mtport import release
from repo_paths import REPO

needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")

URL = "https://github.com/armaatus/maplethief-port/issues"
SYNTHETIC = f"""# Known issues

| What | Source |
|---|---|
| Something open | [#11]({URL}/11) |
| Something fixed | [#12]({URL}/12), [ADR 0100](adr/0100-x.md) |
| A bare reference | #13, and a [section link](play/saves.md#the-user-folder-instead) |
| Another repository | https://github.com/libsdl-org/SDL/issues/14 |
"""


def test_cited_issues_reads_links_and_bare_references():
    assert release.cited_issues(SYNTHETIC) == [11, 12, 13]


def test_cited_issues_ignores_anchors_and_colours():
    assert release.cited_issues("[x](a.md#2-steps) and &#39; and color #fff") == []


def test_closed_citations():
    assert release.closed_citations(SYNTHETIC, {12, 13, 99}) == [12, 13]
    assert release.closed_citations(SYNTHETIC, {14, 99}) == []


def _cli(tmp_path, closed: list[int]) -> subprocess.CompletedProcess:
    doc = tmp_path / "KNOWN_ISSUES.md"
    doc.write_text(SYNTHETIC, encoding="utf-8")
    closed_file = tmp_path / "closed.json"
    closed_file.write_text(json.dumps(closed), encoding="utf-8")
    env = dict(os.environ, PYTHONPATH=str(REPO / "tools"))
    return subprocess.run(
        [
            sys.executable,
            "-B",
            "-m",
            "mtport.release",
            "known-issues",
            str(doc),
            "--closed",
            str(closed_file),
        ],
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )


def test_cli_fails_on_a_closed_issue(tmp_path):
    out = _cli(tmp_path, [12, 40])
    assert out.returncode == 1
    assert "cites 1 closed issue(s): #12" in out.stderr


def test_cli_passes_when_all_are_open(tmp_path):
    out = _cli(tmp_path, [40])
    assert out.returncode == 0, out.stderr
    assert "3 cited issues, none closed" in out.stdout


TAG = "v0.0.0-rc.309"


@pytest.fixture
def release_env(tmp_path):
    doc = tmp_path / "KNOWN_ISSUES.md"
    doc.write_text(SYNTHETIC, encoding="utf-8")
    env = dict(os.environ, PYTHON=sys.executable, MAPLETHIEF_KNOWN_ISSUES=str(doc))
    yield tmp_path, env
    shutil.rmtree(REPO / "build" / "release" / TAG, ignore_errors=True)


def _start(env: dict, tmp_path, closed: list[int]) -> subprocess.CompletedProcess:
    closed_file = tmp_path / "closed.json"
    closed_file.write_text(json.dumps(closed), encoding="utf-8")
    env = dict(env, MAPLETHIEF_CLOSED_ISSUES=str(closed_file))
    return subprocess.run(
        ["bash", "scripts/release", "start", TAG, "--dry-run"],
        cwd=REPO,
        capture_output=True,
        text=True,
        env=env,
        timeout=60,
    )


@needs_bash
def test_release_start_stops_on_a_closed_issue(release_env):
    tmp_path, env = release_env
    out = _start(env, tmp_path, [13])
    assert out.returncode == 1
    assert "cites 1 closed issue(s): #13" in out.stderr
    assert "docs/KNOWN_ISSUES.md cites a closed issue" in out.stderr
    assert "== Checks" not in out.stdout  # stops before the tag checks


@needs_bash
def test_release_start_goes_on_when_all_are_open(release_env):
    tmp_path, env = release_env
    out = _start(env, tmp_path, [40])
    assert "known issues: OK" in out.stdout, out.stderr
    assert "== Checks" in out.stdout  # then the tag checks, which fail on the missing tag
    assert f"no local tag {TAG}" in out.stderr
