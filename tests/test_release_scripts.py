"""scripts/release and scripts/release.ps1 (ADR 0024): argument handling, without touching
GitHub or building anything. The end-to-end flow is checked with --dry-run on a local tag
(docs/release-checklist.md)."""

from __future__ import annotations

import os
import shutil
import subprocess

import pytest

from repo_paths import REPO

needs_bash = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")
needs_pwsh = pytest.mark.skipif(not shutil.which("pwsh"), reason="needs pwsh (PowerShell 7+)")


def run(*cmd: str) -> subprocess.CompletedProcess:
    return subprocess.run(list(cmd), cwd=REPO, capture_output=True, text=True, timeout=60)


@needs_bash
def test_release_help():
    out = run("bash", "scripts/release", "--help")
    assert out.returncode == 0
    for command in ("start TAG", "build TAG", "publish TAG", "--dry-run", "--replace"):
        assert command in out.stderr


@needs_bash
@pytest.mark.parametrize(
    "args, status, message",
    [
        ((), 2, "Usage: scripts/release"),
        (("tag", "v1.0.0"), 2, "unknown command: tag"),
        (("start",), 2, "Usage: scripts/release"),
        (("start", "v1.0", "--dry-run"), 1, "tag 'v1.0' is not vX.Y.Z or vX.Y.Z-rc.N"),
        (("build", "v1.0.0", "--bogus"), 2, "unknown option: --bogus"),
        (("publish", "v0.0.0-rc.0", "a", "b"), 2, "Usage: scripts/release"),
    ],
)
def test_release_rejects(args, status, message):
    out = run("bash", "scripts/release", *args)
    assert out.returncode == status, out.stdout + out.stderr
    assert message in out.stderr


@needs_pwsh
def test_release_ps1_parses():
    script = (
        "$errors = $null; "
        "[void][System.Management.Automation.Language.Parser]::ParseFile("
        "'scripts/release.ps1', [ref]$null, [ref]$errors); "
        "$errors | ForEach-Object { $_.ToString() }; exit $errors.Count"
    )
    out = run("pwsh", "-NoProfile", "-NonInteractive", "-Command", script)
    assert out.returncode == 0, out.stdout + out.stderr


@needs_pwsh
@pytest.mark.parametrize("command", ["start", "publish"])
def test_release_ps1_sends_start_and_publish_to_the_mac(command):
    out = run(
        "pwsh", "-NoProfile", "-NonInteractive", "-File", "scripts/release.ps1", command, "v0.0.1"
    )
    assert out.returncode == 1
    assert f"runs on the Mac: scripts/release {command} v0.0.1" in out.stderr
