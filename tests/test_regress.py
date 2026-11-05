"""The regression runner (mtport.regress, #91, ADR 0104): the digest (FNV-1a 64, the same as
replay.h's), the first difference between two logs, the settings list, the committed replay's
shape, and whole runs against a stand-in for play_real_test (a small Python program that writes a
checkpoint log), so repeats, skips, failures, a run that differs, a lying digest and baselines are
tested without the game."""

from __future__ import annotations

import os
import stat
import sys
from pathlib import Path

import pytest

from mtport import regress as rg
from repo_paths import REPO

REPLAY = REPO / "tests" / "regress" / "title_to_combat.replay"

# The stand-in: `FAKE regress OUT` writes OUT/checkpoints.txt and prints its digest as
# play_real_test does. FAKE_MODE picks a misbehaviour; the settings go into the state.
FAKE = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
from mtport import regress as rg
mode_arg, out = sys.argv[2], sys.argv[3]
assert mode_arg == "regress"
mode = os.environ.get("FAKE_MODE", "")
if mode == "skip":
    print("skipped: built without generated code")
    sys.exit(77)
assert os.environ["SDL_VIDEO_DRIVER"] == "dummy"
assert os.path.isfile(os.environ["MT_REPLAY"])
os.makedirs(out, exist_ok=True)
count = os.path.join(os.path.dirname(out), "count")
n = int(open(count).read()) if os.path.exists(count) else 0
open(count, "w").write(str(n + 1))
settings = os.environ.get("MAPLETHIEF_SETTINGS", "none").replace(",", ";").replace("=", ":")
frame = "0123456789abcdef"
if mode == "differs" and n == 2:
    frame = "0123456789abcdee"
lines = [
    f"title step 1501 frame {frame} audio fedcba9876543210/66000 gl 30 | map=-1.-1 s={settings}",
    "combat step 76424 frame 9e9fed904d7c4375 audio a3378409895b2259/3370298 gl 1559 "
    "| map=2.9 kills=1",
]
if mode == "short" and n == 1:
    lines = lines[:1]
text = "".join(l + "\n" for l in lines)
if mode != "nolog":
    open(os.path.join(out, "checkpoints.txt"), "w").write(text)
if mode == "fail":
    print("FAIL: combat shows kills>=1 (kills=0)")
    sys.exit(1)
d = rg.digest(text) if mode != "lie" else "0000000000000000"
print(f"regress: 2 checkpoints (2 named), digest {d}")
print("all ok")
"""


@pytest.fixture
def fake(tmp_path: Path) -> Path:
    script = tmp_path / "fake.py"
    script.write_text(FAKE, encoding="utf-8")
    exe = tmp_path / "play_real_test"
    exe.write_text(
        f'#!/bin/sh\nexec "{sys.executable}" "{script}" "{REPO / "tools"}" "$@"\n', encoding="utf-8"
    )
    exe.chmod(exe.stat().st_mode | stat.S_IEXEC)
    return exe


pytestmark = pytest.mark.skipif(os.name == "nt", reason="the stand-in is a shell script")


def run(fake: Path, tmp_path: Path, mode: str = "", **kw) -> int:
    env = dict(os.environ)
    env["FAKE_MODE"] = mode
    return rg.regress(
        fake,
        REPLAY,
        tmp_path / "out",
        kw.pop("repeat", 1),
        kw.pop("settings", ""),
        60,
        kw.pop("baseline", None),
        kw.pop("save_baseline", None),
        environ=env,
    )


def test_digest_is_fnv1a64() -> None:
    assert rg.fnv1a64(b"") == 0xCBF29CE484222325
    assert rg.fnv1a64(b"a") == 0xAF63DC4C8601EC8C  # replay_test.cpp's reference values
    assert rg.digest("a") == "af63dc4c8601ec8c"
    assert len(rg.digest("x" * 1000)) == 16


def test_first_difference() -> None:
    a = "title step 1 | map=1\ncombat step 2 | map=2\n"
    assert rg.first_difference(a, a) is None
    d = rg.first_difference(a, "title step 1 | map=1\ncombat step 2 | map=3\n")
    assert d is not None and d.index == 1
    assert "checkpoint 2 (combat)" in d.describe("a", "b")
    d = rg.first_difference(a, "title step 1 | map=1\n")
    assert d is not None and d.index == 1 and d.b == ""
    assert "(none: the log ends)" in d.describe("a", "b")


def test_settings_list() -> None:
    assert rg.settings_list("", "") == ""
    assert rg.settings_list("", "original") == "preset=original"
    assert rg.settings_list("turbo_attack=on", "original") == "preset=original,turbo_attack=on"


def test_the_committed_replay() -> None:
    text = REPLAY.read_text(encoding="utf-8")
    lines = [ln for ln in text.splitlines() if ln and not ln.startswith("#")]
    assert lines[0] == "maplethief-replay 1"
    assert "step_us 1000" in lines and "data empty" in lines
    checks = [ln.split()[2] for ln in lines if ln.split()[1:2] == ["check"]]
    assert checks == ["title", "play", "intro", "first_map", "map_2_9", "combat", "end"]
    assert lines[-1].startswith("expect end ")
    assert any(ln.startswith("expect combat") and "kills>=1" in ln for ln in lines)
    steps = [int(ln.split()[0]) for ln in lines if ln.split()[0].isdigit()]
    assert steps == sorted(steps)
    assert lines[[i for i, ln in enumerate(lines) if ln.split()[0].isdigit()][-1]].endswith(" end")


def test_repeat_one_digest(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    assert run(fake, tmp_path, repeat=3) == 0
    out = capsys.readouterr().out
    assert out.count(": PASS ") == 3
    assert "deterministic: 3 runs, one digest" in out
    assert "3 of 3 runs passed" in out
    log = (tmp_path / "out" / "run_01" / "checkpoints.txt").read_text(encoding="utf-8")
    assert out.rstrip().endswith(f"regress: digest {rg.digest(log)}")
    assert (tmp_path / "out" / "run_03" / "stdout.txt").is_file()


def test_a_run_that_differs(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    assert run(fake, tmp_path, "differs", repeat=3) == 1
    out = capsys.readouterr().out
    assert "run 3 differs from run 1: first difference at checkpoint 1 (title)" in out
    assert "frame 0123456789abcdee" in out
    assert "deterministic" not in out


def test_a_shorter_log(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    assert run(fake, tmp_path, "short", repeat=2) == 1
    assert "checkpoint 2 (combat)" in capsys.readouterr().out


def test_failures(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    assert run(fake, tmp_path, "fail") == 1
    assert "FAIL: combat shows kills>=1" in capsys.readouterr().out
    assert run(fake, tmp_path, "nolog") == 1
    assert "no checkpoints.txt" in capsys.readouterr().out
    assert run(fake, tmp_path, "lie") == 1
    assert "the program's digest 0000000000000000 is not its log's" in capsys.readouterr().out


def test_skip(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    assert run(fake, tmp_path, "skip", repeat=3) == rg.SKIP
    assert "SKIP   skipped: built without generated code" in capsys.readouterr().out
    assert rg.main(["--binary", str(tmp_path / "none"), "--replay", str(REPLAY)]) == rg.SKIP
    assert rg.main(["--binary", str(fake), "--replay", str(tmp_path / "none")]) == 2
    assert rg.main(["--binary", str(fake), "--replay", str(REPLAY), "--repeat", "0"]) == 2


def test_settings_reach_the_run(fake: Path, tmp_path: Path) -> None:
    assert run(fake, tmp_path, settings="preset=original") == 0
    log = (tmp_path / "out" / "run_01" / "checkpoints.txt").read_text(encoding="utf-8")
    assert "s=preset:original" in log
    assert run(fake, tmp_path) == 0
    log2 = (tmp_path / "out" / "run_01" / "checkpoints.txt").read_text(encoding="utf-8")
    assert "s=none" in log2 and rg.digest(log2) != rg.digest(log)


def test_baselines(fake: Path, tmp_path: Path, capsys: pytest.CaptureFixture) -> None:
    base = tmp_path / "work" / "regress" / "base.txt"
    assert run(fake, tmp_path, baseline=base) == 1
    assert "no baseline at" in capsys.readouterr().out
    assert run(fake, tmp_path, save_baseline=base) == 0
    assert "SAVED  the baseline" in capsys.readouterr().out
    assert run(fake, tmp_path, baseline=base) == 0
    assert "identical" in capsys.readouterr().out
    # Another preset's run against it: the first checkpoint differs, and says how.
    assert run(fake, tmp_path, settings="preset=enhanced", baseline=base) == 1
    out = capsys.readouterr().out
    assert "differs: first difference at checkpoint 1 (title)" in out
    assert "s=preset:enhanced" in out
