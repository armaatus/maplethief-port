"""The title soak and its coverage report (mtport.titlerun, #74): the log, report and coverage
parsers, the checks, and a whole soak against a stand-in for MapleThief (a small Python program
that writes a synthetic log, report and shots), so the flow is tested without the game."""

from __future__ import annotations

import sys

import pytest

from mtport import png
from mtport import titlerun as tr
from repo_paths import REPO

# The stand-in: prints MapleThief's log lines for --shot, --seconds and the end of the run,
# writes the unimplemented report and the coverage file, and a shot per --shot: one colour per
# slide-in shot, one for every title shot. FAKE_MODE changes one thing.
FAKE = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
from mtport import png
args = sys.argv[2:]
mode = os.environ.get("FAKE_MODE", "")
if mode == "nogame":
    sys.exit(4)
cap = args[args.index("--capture") + 1]
seconds = float(args[args.index("--seconds") + 1])
assert args[args.index("--fixed-step") + 1] == "16.667"
shots = [args[i + 1].split(",") for i, a in enumerate(args) if a == "--shot"]
for n, (t, name) in enumerate(shots):
    t = float(t)
    if mode == "stall" and t > 30:
        gl = 600
    else:
        gl = round(t * 20)
    print(f"2026-10-30 MapleThief[1:2] game: shot {name} at {t:.3f} s (turn {round(t * 60)}, "
          f"GL frame {gl}) written to {cap}/{name}.png")
    if name.startswith("slide"):
        c = (40 + n * 20, 0, 0)
    elif mode == "moved" and t > 30:
        c = (0, 0, 99)
    elif mode == "black":
        c = (0, 0, 0)
    else:
        c = (10, 200, 30)
    if mode == "frozen" and name.startswith("slide"):
        c = (10, 200, 30)
    png.save(os.path.join(cap, name + ".png"), png.Image(48, 32, bytes((*c, 255)) * (48 * 32)))
if mode == "trap":
    print("=== guest crash: memory fault ===")
    print("game: trapped after 1.0 s: 60 turns, 21 window frames (first not black 1, first with "
          "a GL frame not black 0), 20 GL frames (first with draws 1, first not black 0)")
    print("game: trap kind 1 at pc 0x0000A0D2, detail 0x00000000")
    sys.exit(5)
print(f"game: stopped after 9.0 s: 3601 turns, 1253 window frames (first not black 1, first "
      f"with a GL frame not black 0), {round(seconds * 20)} GL frames (first with draws 1, "
      f"first not black 0)")
print(f"game: virtual time at the end: {seconds:.3f} s")
misuses = 3 if mode == "misuse" else 0
print(f"game: GL draws: 1230 drawn, 0 skipped, {misuses} misuses; mt_gfx: 2460 triangles, "
      f"0 lines, 0 unsupported, 0 invalid")
print("game: stop reason: UIApplicationMain: the host ended the run loop")
report = os.environ["MAPLETHIEF_REPORT"]
with open(report, "w", encoding="utf-8") as f:
    if mode == "unimplemented":
        f.write("unimplemented report: 1 import (2 calls), 0 selectors (0 messages)\n"
                "import _foo (libSystem.B.dylib): 2 calls, first from 0x0000A123\n")
    else:
        f.write("unimplemented report: none\n")
cov = os.environ.get("MAPLETHIEF_COVERAGE")
if cov:
    with open(cov, "w", encoding="utf-8") as f:
        f.write("# maplethief coverage 1: 2 guest functions entered (pointer, entries, name)\n"
                "0x0000280D 1 main\n0x00021F51 9000 CMTimer::OnRunLoopTimer\n")
"""

COVERAGE = """\
# maplethief coverage 1: 6 guest functions entered (pointer, entries, name)
0x000027E0 1 start
0x0000280D 1 main
0x00002879 3 -[MapleThiefAppDelegate setWindow:]
0x00005C2D 1 Clet_Start
0x00021F51 61234 CMTimer::OnRunLoopTimer
0x0009DBB1 2 f_0009DBB1
"""


@pytest.fixture
def fake(tmp_path):
    script = tmp_path / "fake_maplethief.py"
    script.write_text(FAKE, encoding="utf-8")
    return [sys.executable, str(script), str(REPO / "tools")]


def test_game_args_cover_the_slide_in_and_the_minute(tmp_path):
    args = tr.game_args(tmp_path)
    assert args[:6] == ["--fixed-step", "16.667", "--seconds", "60", "--capture", str(tmp_path)]
    shots = [args[i + 1] for i, a in enumerate(args) if a == "--shot"]
    assert shots[0] == "0.5,slide_500ms" and shots[-1] == "60,title_60s"
    assert len(shots) == len(tr.SLIDE_SHOTS) + len(tr.STILL_SHOTS)
    # Every name is one MapleThief's --shot accepts.
    assert all(n.split(",")[1].replace("_", "").replace("-", "").isalnum() for n in shots)
    # A shorter run leaves out the later shots.
    assert tr.game_args(tmp_path, 10).count("--shot") == len(tr.SLIDE_SHOTS) + 2


def test_parse_log():
    log = tr.parse_log(
        "2026-10-30 06:49 MapleThief[41384:2] game: shot title_10s at 10.001 s (turn 601, GL frame "
        "200) written to /x/title_10s.png\n"
        "game: stopped after 21.0 s: 3601 turns, 1253 window frames (first not black 1, first "
        "with a GL frame not black 0), 1200 GL frames (first with draws 1, first not black 0)\n"
        "game: virtual time at the end: 60.001 s\n"
        "game: GL draws: 1230 drawn, 0 skipped, 2 misuses; mt_gfx: 2460 triangles, 0 lines, "
        "4 unsupported, 5 invalid\n"
        "game: stop reason: UIApplicationMain: the host ended the run loop\n"
        "=== guest crash: memory fault ===\n"
    )
    assert log.shots == {"title_10s": tr.Shot("title_10s", 10.001, 601, 200)}
    assert log.outcome == "stopped" and log.gl_frames == 1200
    assert log.virtual_end == pytest.approx(60.001)
    assert log.draws == {"drawn": 1230, "skipped": 0, "misuses": 2, "unsupported": 4, "invalid": 5}
    assert log.stop_reason == "UIApplicationMain: the host ended the run loop"
    assert log.errors == ["=== guest crash: memory fault ==="]


def test_soak_passes(fake, tmp_path, capsys):
    assert tr.run_soak(fake, tmp_path / "out") == 0
    out = capsys.readouterr().out
    assert out.startswith("PASS   title: 60 s of virtual time without errors: 1200 GL frames")
    assert "unimplemented report: none" in out
    assert (tmp_path / "out" / "title_60s.png").is_file()
    assert (tmp_path / "out" / "log.txt").read_text(encoding="utf-8").startswith(sys.executable)


@pytest.mark.parametrize(
    "mode, message",
    [
        ("trap", "error in the log: === guest crash"),
        ("unimplemented", "the unimplemented report is not empty"),
        ("misuse", "GL draws: 3 misuses"),
        ("stall", "below 14"),
        ("moved", "the still title moved"),
        ("black", "title_2500ms is black"),
        ("frozen", "the slide-in does not animate"),
    ],
)
def test_soak_fails(fake, tmp_path, capsys, monkeypatch, mode, message):
    monkeypatch.setenv("FAKE_MODE", mode)
    assert tr.run_soak(fake, tmp_path / "out") == 1
    assert message in capsys.readouterr().out


def test_soak_skips_without_the_game(fake, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("FAKE_MODE", "nogame")
    assert tr.run_soak(fake, tmp_path / "out") == tr.SKIP
    assert tr.main(["soak", "--binary", str(tmp_path / "missing")]) == tr.SKIP
    assert tr.main(["soak", "--binary", sys.executable, "--seconds", "1"]) == 2


def test_soak_writes_coverage(fake, tmp_path, monkeypatch):
    monkeypatch.setenv("MAPLETHIEF_COVERAGE", "/should/not/leak")
    cov = tmp_path / "title.coverage"
    assert tr.run_soak(fake, tmp_path / "out", coverage=cov) == 0
    assert [c.name for c in tr.parse_coverage(cov.read_text(encoding="utf-8"))] == [
        "main",
        "CMTimer::OnRunLoopTimer",
    ]


def test_check_reports_missing_pieces():
    fails = tr.check(tr.RunLog(), 3, None, {})
    assert "MapleThief exited with status 3" in fails
    assert "no unimplemented report was written" in fails
    assert "no GL draw counters in the log" in fails
    assert any(f.startswith("shots missing: slide_500ms") for f in fails)
    assert any(f.startswith("the run ended at -1.000 s") for f in fails)


def test_parse_coverage():
    covered = tr.parse_coverage(COVERAGE)
    assert len(covered) == 6
    assert covered[4] == tr.Covered(0x21F51, 61234, "CMTimer::OnRunLoopTimer")
    with pytest.raises(ValueError, match="header"):
        tr.parse_coverage("0x000027E0 1 start\n")
    with pytest.raises(ValueError, match="line 2"):
        tr.parse_coverage("# maplethief coverage 1: 1 guest functions entered\nnope\n")
    with pytest.raises(ValueError, match="says 2"):
        tr.parse_coverage("# maplethief coverage 1: 2 guest functions entered\n0x00000001 1 a\n")


@pytest.mark.parametrize(
    "name, group",
    [
        ("CMTimer::OnRunLoopTimer", "CMTimer"),
        ("std::vector<int>::push_back(int)", "std::vector<int>"),
        ("-[MapleThiefAppDelegate setWindow:]", "MapleThiefAppDelegate"),
        ("+[EAGLView layerClass]", "EAGLView"),
        ("-[UIView(Extras) foo]", "UIView"),
        ("Clet_Start", "(functions)"),
        ("f_0009DBB1", "(unnamed)"),
        ("FUN_0009dbb0", "(unnamed)"),
    ],
)
def test_group_of(name, group):
    assert tr.group_of(name) == group


def test_summarise():
    text = tr.summarise(tr.parse_coverage(COVERAGE), 2655, top=2)
    assert "- **6 guest functions entered** of 2,655 discovered (0.2%), 5 of them named." in text
    assert "- 61,242 entries in all." in text
    assert "| `(functions)` | 3 | 3 |" in text
    assert "| `CMTimer` | 1 | 61,234 |" in text
    assert "## The 2 most entered" in text
    assert "| `CMTimer::OnRunLoopTimer` | `0x00021F51` | 61,234 |" in text
    assert "| `-[MapleThiefAppDelegate setWindow:]` | `0x00002879` | 3 |" in text
    assert "`start`" not in text.split("## The 2 most entered")[1]
    assert "of 2,655" not in tr.summarise(tr.parse_coverage(COVERAGE), None)


def test_coverage_skips_without_generated_code(tmp_path, capsys):
    assert tr.main(["coverage", "--repo", str(tmp_path)]) == tr.SKIP
    assert "no work/gen" in capsys.readouterr().out


def test_find_program(tmp_path):
    assert tr._find_program(tmp_path) is None
    exe = tmp_path / "platform" / "MapleThief"
    exe.parent.mkdir()
    exe.write_bytes(b"")
    assert tr._find_program(tmp_path) == exe


def test_is_black():
    assert tr._is_black(png.Image(2, 1, bytes(8)))
    assert not tr._is_black(png.Image(2, 1, bytes((0, 0, 0, 255, 0, 1, 0, 255))))
