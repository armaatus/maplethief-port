"""scripts/ci-scope (ADR 0006, addendum of 2026-11-06): a pull request that only touches
documentation or compendium work skips CI's build matrix; anything else, or an empty list,
runs it."""

from __future__ import annotations

import os
import shutil
import subprocess

import pytest

from repo_paths import REPO

pytestmark = pytest.mark.skipif(os.name == "nt" or not shutil.which("bash"), reason="needs bash")


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
            "tests/fixtures/compendium/kb/monster.json",
            "scripts/compendium",
            ".github/workflows/compendium.yml",
        )
        == "native=false"
    )


@pytest.mark.parametrize(
    "path",
    [
        "src/runtime/main.cpp",
        "CMakeLists.txt",
        "tools/mtport/datatables.py",
        "tests/test_datatables.py",
        "scripts/lint",
        "scripts/test",
        ".github/workflows/ci.yml",
        "uv.lock",
        "tools/mtport/compendium_extra.py",
    ],
)
def test_any_other_path_runs_the_build(path):
    assert scope("docs/compendium/style.md", path) == "native=true"


def test_an_empty_list_runs_the_build():
    assert scope() == "native=true"


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
    assert out.stdout.strip() == "native=true"
