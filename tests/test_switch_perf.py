"""scripts/switch-perf's build and NRO choice (#284, ADR 0145's addendum), on a synthetic repo.

The script is copied into tmp_path next to stub scripts/build-switch and scripts/switch-run that
record their arguments, with a synthetic work/gen (gen.cmake and a dispatch.c of eight entries)
and synthetic NROs: the game's (the dispatch table as data, as a stripped build keeps it) and the
hello NRO (no recompiled code). Nothing here needs Docker, a console or the IPA; the real build
is the issue's local acceptance step.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from mtport import appicon, nro
from repo_paths import REPO

pytestmark = pytest.mark.skipif(
    sys.platform == "win32" or shutil.which("bash") is None, reason="a bash script"
)

TABLE = [0x27E0, 0x280D, 0x2879, 0x28AD, 0x28D1, 0x2905, 0x2929, 0x29A5]
GAME_CODE = b"\0" * 64 + b"".join(a.to_bytes(4, "little") + b"\0" * 12 for a in TABLE)
HELLO_CODE = b"\0 MapleThief 0.0.1: press + to return to the Homebrew Menu \0"

# The stubs: build-switch writes the NRO named by $FAKE_NRO into build/<preset>/, as the real
# one copies the container's; switch-run prints a session the judge can read.
BUILD_SWITCH = """#!/usr/bin/env bash
set -euo pipefail
echo "$*" >> "$(dirname "$0")/../build-switch.args"
preset=switch-release
while [ "$#" -gt 0 ]; do
    case "$1" in --preset) preset="$2"; shift 2 ;; *) shift ;; esac
done
mkdir -p "build/$preset"
cp "$FAKE_NRO" "build/$preset/MapleThief.nro"
"""
SWITCH_RUN = """#!/usr/bin/env bash
echo "$*" >> "$(dirname "$0")/../switch-run.args"
"""


def make_nro(code: bytes) -> bytes:
    return nro.build(
        code, icon=appicon.switch_icon(), nacp=nro.build_nacp("MapleThief", "armaatus", "0.0.1")
    )


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    (root / "scripts").mkdir(parents=True)
    shutil.copy2(REPO / "scripts" / "switch-perf", root / "scripts" / "switch-perf")
    for name, text in (("build-switch", BUILD_SWITCH), ("switch-run", SWITCH_RUN)):
        path = root / "scripts" / name
        path.write_text(text, encoding="utf-8")
        path.chmod(0o755)
    (root / "tools").symlink_to(REPO / "tools", target_is_directory=True)
    gen = root / "work" / "gen"
    gen.mkdir(parents=True)
    (gen / "gen.cmake").write_text("# synthetic\n", encoding="utf-8")
    rows = "\n".join(f"    {{0x{a:08X}u, f_{a:08X}}}," for a in TABLE)
    (gen / "dispatch.c").write_text(
        f"const mt_fn_entry mt_fn_table[] = {{\n{rows}\n}};\n", encoding="utf-8"
    )
    (tmp_path / "game.nro").write_bytes(make_nro(GAME_CODE))
    (tmp_path / "hello.nro").write_bytes(make_nro(HELLO_CODE))
    # python3 is the one running these tests (3.10+), whatever the PATH's is.
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    (bin_dir / "python3").symlink_to(sys.executable)
    return root


def switch_perf(repo: Path, *args: str, nro_file: str = "game.nro") -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["PATH"] = f"{repo.parent / 'bin'}{os.pathsep}{env['PATH']}"
    env["FAKE_NRO"] = str(repo.parent / nro_file)
    env.pop("MAPLETHIEF_SWITCH_IP", None)
    return subprocess.run(
        ["bash", str(repo / "scripts" / "switch-perf"), *args],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )


def recorded(repo: Path, name: str) -> list[str]:
    path = repo / f"{name}.args"
    return path.read_text(encoding="utf-8").splitlines() if path.is_file() else []


def test_build_selects_the_generated_code_build(repo):
    run = switch_perf(repo, "--build", "--build-only")
    assert run.returncode == 0, run.stderr
    assert recorded(repo, "build-switch") == ["HEAD --preset switch-player --gen work/gen"]
    assert "build/switch-player/MapleThief.nro holds the game" in run.stdout
    assert recorded(repo, "switch-run") == []  # --build-only sends nothing


def test_the_game_nro_is_sent_with_the_session(repo):
    run = switch_perf(repo, "--build", "--ip", "10.0.0.2", "--", "--gfx-backend", "software")
    assert recorded(repo, "switch-run") == [
        "--ip 10.0.0.2 build/switch-player/MapleThief.nro -- --perf-session --fast-forward x2 "
        "--gfx-gpu-timing --gfx-backend software"
    ]
    # The stub prints no session, so the judge fails; what matters is what was sent.
    assert "perfsession:" in run.stdout + run.stderr


def test_a_hello_nro_is_refused(repo):
    run = switch_perf(repo, "--build", nro_file="hello.nro")
    assert run.returncode == 1
    assert "not the game's NRO" in run.stderr
    assert "not a player build" in run.stderr  # the guard's own finding
    assert recorded(repo, "switch-run") == []


def test_an_existing_hello_nro_is_refused_without_build(repo):
    nro_path = repo / "build" / "switch-player" / "MapleThief.nro"
    nro_path.parent.mkdir(parents=True)
    nro_path.write_bytes(make_nro(HELLO_CODE))
    run = switch_perf(repo)
    assert run.returncode == 1
    assert "refusing to measure it" in run.stderr
    assert recorded(repo, "build-switch") == []
    assert recorded(repo, "switch-run") == []


@pytest.mark.parametrize("preset", ["switch-release", "switch-debug"])
def test_the_hello_presets_are_refused(repo, preset):
    run = switch_perf(repo, "--build", "--preset", preset)
    assert run.returncode == 2
    assert "hello NRO" in run.stderr
    assert recorded(repo, "build-switch") == []


def test_no_generated_code_says_how_to_make_it(repo):
    shutil.rmtree(repo / "work")
    run = switch_perf(repo, "--build")
    assert run.returncode == 2
    assert "python3 -m recomp all" in run.stderr
    assert recorded(repo, "build-switch") == []


def test_no_nro_without_build_says_how_to_make_it(repo):
    run = switch_perf(repo)
    assert run.returncode == 2
    assert "scripts/build-switch --preset switch-player --gen work/gen" in run.stderr
