"""The compendium guard (issue #226, ADR 0121): ``scripts/compendium guard`` fails when a
tracked file looks like compendium output, on a fixture checkout with one file planted at a
time, and passes on a clean checkout and on this repository."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import pytest

from mtport.compendium import bundle, site
from repo_paths import REPO

pytestmark = pytest.mark.skipif(
    os.name == "nt" or not shutil.which("bash") or not shutil.which("git"),
    reason="needs bash and git",
)

CLEAN = {
    "README.md": b"# A checkout\n",
    "tools/mtport/compendium/site.py": b"# the generator\n",
    "docs/compendium/pages/index.md": b"# MapleThief Compendium\n",
    # the disclaimer in Markdown is documentation, not an exported page
    "docs/legal.md": site.DISCLAIMER.encode() + b"\n",
    "tests/golden/data/test_pattern.png": b"\x89PNG\r\n\x1a\nsynthetic",
    "docs/controls/keypad.svg": b"<svg/>",
    "tests/fixtures/viewer.html": b"<!doctype html><p>a synthetic viewer page</p>",
}

PLANTED = {
    "build/compendium/site/index.html": "git-ignored output directory",
    "build/compendium/kb/monster.json": "git-ignored output directory",
    "work/ipa/Payload/x.dat": "git-ignored output directory",
    "assets-out/a.png": "git-ignored output directory",
    "site/.mtport-compendium-site": "compendium site file",
    "docs/search-index.js": "compendium site file",
    "docs/img/manifest.json": "image manifest",
    "dist/compendium-site.tar.gz": "site bundle",
    "docs/site/img/monster/1/move.png": "images layout",
    "docs/pages/img/map/1-1.svg": "images layout",
    "tests/golden/data/img/item/etc/3.png": "images layout",
    "docs/sprite.png": "an image outside",
    "notes/hero.webp": "an image outside",
    "pages/monster/1.html": "exported compendium page",
}


def git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@example.invalid",
             "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@example.invalid"},
    )  # fmt: skip


def checkout(tmp_path: Path, extra: dict[str, bytes] | None = None) -> Path:
    repo = tmp_path / "checkout"
    repo.mkdir()
    git(repo, "init", "-q")
    for rel, data in {**CLEAN, **(extra or {})}.items():
        f = repo / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)
    # -f: the planted files sit where .gitignore would keep them out in the real repository
    git(repo, "add", "-f", "--", ".")
    git(repo, "commit", "-q", "-m", "fixture")
    return repo


def run_guard(repo: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["bash", str(REPO / "scripts/compendium"), "guard", "--repo-dir", str(repo)],
        capture_output=True,
        text=True,
        timeout=60,
    )


def test_clean_checkout_passes(tmp_path):
    res = run_guard(checkout(tmp_path))
    assert res.returncode == 0, res.stderr
    assert "OK" in res.stdout


@pytest.mark.parametrize("rel", sorted(PLANTED))
def test_planted_compendium_file_fails(tmp_path, rel):
    data = (
        f'<!doctype html><footer class="site-foot"><p>{site.DISCLAIMER}</p></footer>'.encode()
        if rel.endswith(".html")
        else b"derived"
    )
    res = run_guard(checkout(tmp_path, {rel: data}))
    assert res.returncode == 1
    assert f"compendium guard: {rel}: " in res.stderr
    assert PLANTED[rel] in res.stderr
    assert "1 tracked file(s)" in res.stderr


def test_untracked_output_is_not_the_guards_business(tmp_path):
    """build/compendium/ on disk is the normal state after a build; only tracked files count."""
    repo = checkout(tmp_path)
    (repo / "build/compendium/site").mkdir(parents=True)
    (repo / "build/compendium/site/index.html").write_text(site.DISCLAIMER)
    assert run_guard(repo).returncode == 0


def test_this_repository_is_clean():
    assert bundle.guard(REPO) == []


def test_guard_problem_rules():
    assert bundle.guard_problem("tests/golden/data/cg_clip.png") is None
    assert bundle.guard_problem("docs/compendium/pages/monsters/index.md") is None
    assert bundle.guard_problem("build/x") is not None
    page = site.DISCLAIMER.encode()
    assert bundle.guard_problem("a/b.html", lambda rel: page) is not None
    assert bundle.guard_problem("a/b.html", lambda rel: b"<p>ours</p>") is None
