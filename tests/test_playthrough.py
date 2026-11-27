"""The playthrough runner (mtport.playthrough, #306): CTest's test list and JUnit report read
back, the chain's log split and compared part by part, and whole runs against a stand-in for
ctest (a small Python program that lists a chain, "plays" the parts a regex selects by writing
their checkpoint logs, and writes a JUnit report), so repeats, skips, a failed part, a one-tick
change, baselines and a quick run against a full baseline are tested without the game."""

from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

import pytest

from mtport import playthrough as pt
from mtport.regress import digest
from repo_paths import REPO

# The stand-in: `FAKE --test-dir BUILD --show-only=json-v1` lists a chain; `FAKE --test-dir BUILD
# -R REGEX ... --output-junit FILE` plays the matching parts. FAKE_MODE picks a misbehaviour.
FAKE = r"""
import json, os, re, sys
from xml.sax.saxutils import quoteattr
argv = sys.argv[1:]
build = argv[argv.index("--test-dir") + 1]
exe = os.path.join(build, "tests", "play_real_test")
out = os.path.join(build, "out")
chain = [("play.chapter_1_a", "chapter_1_a"), ("play.chapter_1_b", "chapter_1_b"),
         ("starts.chapter_2", None), ("play.chapter_2_a", "chapter_2_a")]
tests = []
for name, mode in chain:
    if mode:
        tests.append({"name": name, "command": [exe, mode, os.path.join(out, mode)]})
    else:
        tests.append({"name": name, "command": ["/usr/bin/python3", "-m", "mtport.savedata"]})
if "--show-only=json-v1" in argv:
    print(json.dumps({"kind": "ctestInfo", "tests": tests}))
    sys.exit(0)
regex = argv[argv.index("-R") + 1]
junit = argv[argv.index("--output-junit") + 1]
fake = os.environ.get("FAKE_MODE", "")
assert os.environ["MT_CHECKPOINTS"].isdigit()
nudge = os.environ.get("MT_NUDGE_TICK")
cases = []
for name, mode in chain:
    if not re.search(regex, name) and not (name == "starts.chapter_2" and "chapter_2" in regex):
        continue
    if mode is None:
        cases.append(f'<testcase name="{name}" time="0.5" status="run"/>')
        continue
    if fake == "skip":
        cases.append(f'<testcase name="{name}" time="0.1" status="notrun">'
                     '<skipped message="SKIP_RETURN_CODE=77"/></testcase>')
        continue
    os.makedirs(os.path.join(out, mode), exist_ok=True)
    ticks = 1200 + (1 if nudge and mode == "chapter_1_b" else 0)
    text = (f"s60000 step 60000 frame 0123456789abcdef audio fedcba9876543210/66000 gl 30 "
            f"| map=2.8 ticks=1200\n"
            f"end step 99999 frame 9e9fed904d7c4375 audio a3378409895b2259/33 gl 1559 "
            f"| map=2.9 mode={mode} ticks={ticks}\n")
    if fake != "nolog":
        open(os.path.join(out, mode, f"checkpoints_{mode}.txt"), "w", encoding="utf-8").write(text)
    if fake == "fail" and mode == "chapter_1_b":
        cases.append(f'<testcase name="{name}" time="2.0" status="fail">'
                     '<failure message="Failed"/></testcase>')
    else:
        cases.append(f'<testcase name="{name}" time="2.0" status="run"/>')
open(junit, "w", encoding="utf-8").write('<?xml version="1.0"?><testsuite>' + "".join(cases) + "</testsuite>")
sys.exit(0)
"""


@pytest.fixture
def fake(tmp_path: Path) -> list[str]:
    script = tmp_path / "fake_ctest.py"
    script.write_text(FAKE, encoding="utf-8")
    return [sys.executable, str(script)]


def run(fake_ctest: list[str], tmp_path: Path, monkeypatch, mode: str = "", **kw) -> int:
    monkeypatch.setenv("FAKE_MODE", mode)
    regex = kw.pop("regex", pt.FULL)
    return pt.playthrough(fake_ctest, tmp_path / "build", tmp_path / "pt", regex, **kw)


def test_the_chain_regex_matches_the_main_line_only() -> None:
    full = pt.FULL
    for name in (
        "play.chapter_1_a",
        "play.chapter_1_m",
        "play.chapter_2_b_synthetic",
        "play.chapter_2_f_synthetic",
        "play.chapter_3_a",
        "play.chapter_3_e_synthetic",
        "play.chapter_3_e_skipped_synthetic",
        "play.chapter_3_postgame_reload",
    ):
        assert re.search(full, name), name
    for name in ("play.chapter_2_sides_a", "play.chapter_2_shops", "play.chapter_2_crafts_a"):
        assert not re.search(full, name), name
    assert re.search(pt.QUICK, "play.chapter_1_c") and not re.search(pt.QUICK, "play.chapter_1_d")


def test_the_chain_in_cmake_is_what_the_regex_names() -> None:
    cmake = (REPO / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "set(_chapter_1_parts a b c d e f g h i j k l m)" in cmake
    assert "set(_chapter_2_parts a b c d e f)" in cmake
    assert "set(_chapter_3_parts a b c d e)" in cmake


def test_ctest_json_and_junit_are_read() -> None:
    parts = pt.parts_from_ctest_json(
        json.dumps(
            {
                "tests": [
                    {"name": "play.x", "command": ["/b/tests/play_real_test", "x", "/o/x"]},
                    {"name": "starts.y", "command": ["/usr/bin/python3", "-m", "m"]},
                ]
            }
        )
    )
    assert parts["play.x"].log_path == Path("/o/x/checkpoints_x.txt")
    assert parts["starts.y"].log_path is None
    cases = pt.parse_junit(
        '<testsuite><testcase name="a" time="1.5" status="run"/>'
        '<testcase name="b" time="2" status="fail"><failure/></testcase>'
        '<testcase name="c" time="0" status="notrun">'
        '<skipped message="Fixture dependency failed"/></testcase>'
        '<testcase name="d" time="0.1" status="notrun">'
        '<skipped message="SKIP_RETURN_CODE=77"/></testcase></testsuite>'
    )
    assert cases == [
        ("a", "PASS", 1.5),
        ("b", "FAIL", 2.0),
        ("c", "NOT RUN", 0.0),
        ("d", "SKIP", 0.1),
    ]


def test_compare_names_the_part_and_the_checkpoint() -> None:
    a = pt.split_log("p.a s1 x\np.a end y\np.b end z\n")
    b = pt.split_log("p.a s1 x\np.a end y\np.b end Z\n")
    assert pt.compare(a, a, ["p.a", "p.b"]) is None
    d = pt.compare(a, b, ["p.a", "p.b"])
    assert d is not None and (d.test, d.index) == ("p.b", 0)
    assert "first difference in p.b, at checkpoint 1 (end)" in d.describe("a", "b")
    assert pt.compare(a, b, ["p.a"]) is None


def test_a_run_prints_each_part_and_the_chains_digest(fake, tmp_path, monkeypatch, capsys) -> None:
    status = run(fake, tmp_path, monkeypatch, repeat=2)
    text = capsys.readouterr().out
    assert status == 0, text
    assert "PASS   deterministic: 2 runs" in text
    for name in ("play.chapter_1_a", "play.chapter_1_b", "play.chapter_2_a"):
        assert f"PASS    {name}" in text
    assert "starts.chapter_2" not in text  # a start state has no checkpoints
    chain = (tmp_path / "pt" / "run_01" / "chain.txt").read_text(encoding="utf-8")
    assert chain.splitlines()[0].startswith("play.chapter_1_a s60000 step 60000")
    assert text.strip().splitlines()[-1] == f"playthrough: digest {digest(chain)}"


def test_a_one_tick_change_is_caught_by_the_baseline(fake, tmp_path, monkeypatch, capsys) -> None:
    base = tmp_path / "base.txt"
    assert run(fake, tmp_path, monkeypatch, save_baseline=base) == 0
    assert run(fake, tmp_path, monkeypatch, baseline=base) == 0
    capsys.readouterr()
    assert run(fake, tmp_path, monkeypatch, baseline=base, nudge=1000) == 1
    text = capsys.readouterr().out
    assert "first difference in play.chapter_1_b, at checkpoint 2 (end)" in text
    assert "ticks=1201" in text


def test_a_quick_run_compares_with_a_full_baseline(fake, tmp_path, monkeypatch, capsys) -> None:
    base = tmp_path / "base.txt"
    assert run(fake, tmp_path, monkeypatch, save_baseline=base) == 0
    capsys.readouterr()
    assert run(fake, tmp_path, monkeypatch, regex=r"^play\.chapter_1_", baseline=base) == 0
    assert "2 parts identical (1 more in it not played)" in capsys.readouterr().out
    small = tmp_path / "small.txt"
    assert run(fake, tmp_path, monkeypatch, regex=r"^play\.chapter_1_a$", save_baseline=small) == 0
    capsys.readouterr()
    assert run(fake, tmp_path, monkeypatch, baseline=small) == 1
    assert "has no play.chapter_1_b, play.chapter_2_a" in capsys.readouterr().out


def test_failures_and_skips(fake, tmp_path, monkeypatch, capsys) -> None:
    assert run(fake, tmp_path, monkeypatch, "fail", save_baseline=tmp_path / "b.txt") == 1
    text = capsys.readouterr().out
    assert "FAIL    play.chapter_1_b" in text and "SAVED" not in text
    for stale in (tmp_path / "build" / "out").rglob("checkpoints_*.txt"):
        os.utime(stale, (1, 1))  # from an earlier run: not this run's logs
    assert run(fake, tmp_path, monkeypatch, "nolog") == 1
    assert run(fake, tmp_path, monkeypatch, "skip") == pt.SKIP
    assert run(fake, tmp_path, monkeypatch, regex=r"^nothing$") == 2
    assert run(fake, tmp_path, monkeypatch, baseline=tmp_path / "none.txt") == 1


def test_main_rejects_bad_usage(tmp_path) -> None:
    assert pt.main(["--build", str(tmp_path), "--repeat", "0"]) == 2


def test_the_scripts_name_the_module() -> None:
    sh = (REPO / "scripts" / "playthrough").read_text(encoding="utf-8")
    ps1 = (REPO / "scripts" / "playthrough.ps1").read_text(encoding="utf-8")
    assert "-m mtport.playthrough" in sh and "-m mtport.playthrough" in ps1
    assert "mac-release" in sh and "win-release" in ps1
    presets = json.loads((REPO / "CMakePresets.json").read_text(encoding="utf-8"))
    names = {p["name"] for p in presets["configurePresets"]}
    assert {"mac-release", "linux-release", "win-release"} <= names
