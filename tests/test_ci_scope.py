"""scripts/ci-scope (ADR 0006, addendum of 2026-11-06): a pull request that only touches
documentation or compendium work skips CI's build matrix; anything else, or an empty list,
runs it. The Nintendo Switch job (ADR 0138) runs for every native change except one that only
touches another platform's own files."""

from __future__ import annotations

import os
import shutil
import subprocess

import pytest

from repo_paths import REPO

pytestmark = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")


NONE = "native=false\nswitch=false"
BOTH = "native=true\nswitch=true"


def scope(*paths: str) -> str:
    out = subprocess.run(
        ["bash", "scripts/ci-scope"],
        cwd=REPO,
        input="".join(p + "\n" for p in paths),
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    return out.stdout.strip()


def test_compendium_and_docs_only_skip_the_build():
    assert (
        scope(
            "docs/compendium/style.md",
            "docs/adr/0121-public-compendium-site.md",
            "README.md",
            "tools/mtport/compendium/site.py",
            "tools/mtport/compendium/sections/monsters.py",
            "tools/mtport/prose_lint.py",
            "tests/test_compendium_site.py",
            "tests/test_prose_lint.py",
            "tests/test_doc_links.py",
            "tools/mtport/handoffs.py",
            "tests/test_handoff_checklists.py",
            "tests/fixtures/compendium/kb/monster.json",
            "scripts/compendium",
            "scripts/local-ci",
            "scripts/lint",
            ".github/workflows/compendium.yml",
        )
        == NONE
    )


@pytest.mark.parametrize(
    "path",
    [
        "src/runtime/main.cpp",
        "CMakeLists.txt",
        "tools/mtport/datatables.py",
        "tests/test_datatables.py",
        "scripts/test",
        ".github/workflows/ci.yml",
        "uv.lock",
        "tools/mtport/compendium_extra.py",
        "platform/switch/switch.cmake",
        "tools/containers/switch/Dockerfile",
        "scripts/build-switch",
    ],
)
def test_any_other_path_runs_the_build_and_the_switch_job(path):
    assert scope("docs/compendium/style.md", path) == BOTH


@pytest.mark.parametrize(
    "path",
    [
        "platform/macos/Info.plist.in",
        "platform/ios/audio_session_ios.mm",
        "platform/windows/MapleThief.rc.in",
        "platform/linux/MapleThief.sh",
        "platform/icon/icon.png",
        "platform/src/update_fetch_apple.mm",
        "cmake/MacApp.cmake",
        "cmake/IosApp.cmake",
        "tools/containers/linux/Dockerfile",
        "scripts/test.ps1",
        "scripts/test-linux",
        "tests/fuzz/zip_fuzz.cpp",
        "tests/fuzz/seeds/zip.py",
        "scripts/fuzz",
    ],
)
def test_another_platforms_own_files_skip_the_switch_job(path):
    assert scope("docs/a.md", path) == "native=true\nswitch=false"
    assert scope(path, "platform/src/main.cpp") == BOTH


def test_an_empty_list_runs_the_build():
    assert scope() == BOTH


def test_a_last_line_without_newline_counts():
    out = subprocess.run(
        ["bash", "scripts/ci-scope"],
        cwd=REPO,
        input="docs/a.md\nsrc/x.cpp",
        capture_output=True,
        text=True,
        timeout=30,
        check=True,
    )
    assert out.stdout.strip() == BOTH


def test_local_ci_offers_the_scope_options():
    out = subprocess.run(
        ["bash", "scripts/local-ci", "--help"], cwd=REPO, capture_output=True, text=True, timeout=30
    )
    assert out.returncode == 0
    for option in ("--since REF", "--full", "scripts/ci-scope"):
        assert option in out.stderr
