"""The menu flows (mtport.menuflows, #75): the launch's arguments, the log's evidence, the
per-launch checks, each flow's checks on synthetic results, and a whole flow against a
stand-in for MapleThief (a small Python program that answers taps with action lines, shots and
save files), so the driver is tested without the game."""

from __future__ import annotations

import sys

import pytest

from mtport import menuflows as mf
from repo_paths import REPO

CLEAN_END = (
    "game: virtual time at the end: {t:.3f} s\n"
    "game: GL draws: 10 drawn, 0 skipped, 0 misuses; mt_gfx: 20 triangles, 0 lines, "
    "0 unsupported, 0 invalid\n"
    "game: stop reason: UIApplicationMain: the host ended the run loop\n"
    "unimplemented report: none\n"
)

# The stand-in: a tap on Play (106, 165) prints clickPlay and writes slot 0; any other tap
# prints a numbered action. A shot's colour counts the taps before it, so a shot after a tap
# differs from one before; a shot named load_* after Play is dark (the loading screen).
# FAKE_MODE changes one thing.
FAKE = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
from mtport import png
args = sys.argv[2:]
mode = os.environ.get("FAKE_MODE", "")
if mode == "nogame":
    sys.exit(4)
assert os.environ["MAPLETHIEF_TRACE"] == "uikit" and os.environ["SDL_VIDEO_DRIVER"] == "dummy"
cap = args[args.index("--capture") + 1]
seconds = float(args[args.index("--seconds") + 1])
data = os.environ["MAPLETHIEF_DATA_DIR"]
taps = [args[i + 1].split(",") for i, a in enumerate(args) if a == "--tap"]
shots = [args[i + 1].split(",") for i, a in enumerate(args) if a == "--shot"]
docs = os.path.join(data, "saves")
os.makedirs(docs, exist_ok=True)
played = None
for n, (t, x, y) in enumerate(taps):
    if (x, y) == ("106", "165"):
        print("uikit: action -[MainMenuView clickPlay:] from 0x01011050 (events 0x40)")
        played = float(t)
        if mode != "nosave":
            open(os.path.join(docs, "theif0"), "wb").write(bytes(1124))
            open(os.path.join(docs, "quest0"), "wb").write(bytes(4781))
    else:
        print(f"uikit: action -[Fake tap{n}:] from 0x01011050 (events 0x40)")
for t, name in shots:
    t = float(t)
    before = sum(1 for tap in taps if float(tap[0]) < t)
    dark = name.startswith("load_") and played is not None and mode != "noload"
    c = (5, 5, 5) if dark else (40 + 30 * before, 200, 30)
    png.save(os.path.join(cap, name + ".png"), png.Image(48, 32, bytes((*c, 255)) * (48 * 32)))
    print(f"game: shot {name} at {t:.3f} s (turn {round(t * 1000)}, GL frame 1) written to x")
if mode == "crash":
    print("=== guest crash: memory fault ===")
end = max(float(t) for t, _ in shots) if shots else seconds
print(f"game: virtual time at the end: {end:.3f} s")
print("game: GL draws: 10 drawn, 0 skipped, 0 misuses; mt_gfx: 20 triangles, 0 lines, "
      "0 unsupported, 0 invalid")
print("game: stop reason: UIApplicationMain: the host ended the run loop")
with open(os.environ["MAPLETHIEF_REPORT"], "w", encoding="utf-8") as f:
    f.write("unimplemented report: none\n")
"""


@pytest.fixture
def fake(tmp_path):
    script = tmp_path / "fake_maplethief.py"
    script.write_text(FAKE, encoding="utf-8")
    return [sys.executable, str(script), str(REPO / "tools")]


def _new_game_flow() -> mf.Flow:
    return next(f for f in mf.FLOWS if f.name == "new_game")


def test_launch_argv(tmp_path):
    launch = mf.Launch("x", 5.0, "ko").tap(3.0, mf.PLAY).swipe(4, mf.HELP_SWIPE).shot(4.5, "after")
    assert launch.argv(["MT"], tmp_path) == [
        "MT",
        "--fixed-step",
        "1",
        "--seconds",
        "5",
        "--capture",
        str(tmp_path),
        "--tap",
        "3,106,165",
        "--swipe",
        "4,400,160,80,160",
        "--shot",
        "4.5,after",
    ]
    shots = [v for o, v in mf.Launch("y", 9).loading_shots(7.5).events if o == "--shot"]
    assert shots[0] == "7.605,load_0" and shots[-1] == "7.695,load_9" and len(shots) == 10


def test_every_flow_is_well_formed():
    names = [f.name for f in mf.FLOWS]
    assert len(set(names)) == len(names)
    for flow in mf.FLOWS:
        launches = flow.launches()
        assert launches and len({launch.name for launch in launches}) == len(launches)
        for launch in launches:
            # The menu has slid in by 3 s; an earlier tap can raise an exception (#122).
            taps = [float(v.split(",")[0]) for o, v in launch.events if o in ("--tap", "--swipe")]
            assert all(t >= 3.0 for t in taps), (flow.name, launch.name)
            shot_names = [v.split(",")[1] for o, v in launch.events if o == "--shot"]
            assert len(set(shot_names)) == len(shot_names)
            assert all(n.replace("_", "").isalnum() for n in shot_names)
            last = max(float(v.split(",")[0]) for o, v in launch.events) if launch.events else 0
            assert last < launch.seconds, (flow.name, launch.name)


def test_result_evidence():
    r = mf.Result(
        "r",
        0,
        "uikit: action -[MainMenuView clickPlay:] from 0x01011050 (events 0x40)\n"
        "uikit: -[UIApplication openURL:] http://www.nexon.net: not opened (no handler)\n"
        "uikit: UIScrollView 0x010113E0: drag ended, pages to page 1 (offset 480, 0)\n",
        dark={"load_0": 0.2, "load_1": 0.95, "game": 1.0},
    )
    assert r.actions == ["-[MainMenuView clickPlay:]"]
    assert r.opened_urls == [("http://www.nexon.net", "not opened (no handler)")]
    assert r.pages == [1]
    assert r.loading_shown()
    assert not mf.Result("r", 0, "", dark={"game": 1.0, "load_0": 0.5}).loading_shown()


def test_launch_checks():
    assert mf.launch_checks(mf.Result("r", 0, CLEAN_END.format(t=5.0)), 5.0) == []
    shot = "game: shot a at 4.000 s (turn 4001, GL frame 3) written to x\n"
    assert mf.launch_checks(mf.Result("r", 0, shot + CLEAN_END.format(t=4.0)), 5.0) == []
    fails = mf.launch_checks(mf.Result("r", 5, CLEAN_END.format(t=2.0)), 5.0)
    assert "MapleThief exited with status 5" in fails
    assert any("before 5 s" in f for f in fails)
    bad = (
        CLEAN_END.format(t=5.0)
        .replace("0 misuses", "2 misuses")
        .replace("report: none", "report: 1 import")
    )
    bad += "foundation: -[NSArray objectAtIndex:]: index beyond bounds: stopping, as the "
    bad += "device's uncaught NSException would\n"
    fails = mf.launch_checks(mf.Result("r", 0, bad), 5.0)
    assert "GL draws: 2 misuses" in fails
    assert "the unimplemented report is not 'none'" in fails
    assert any("uncaught NSException" in f for f in fails)


def test_checks():
    a = mf.Result("a", 0, "", shots={"x": "1", "y": "2"}, files={"theif0": 1124, "CTerms": 2})
    c = mf.Checks()
    c.same(a, "x", a, "x", "")
    c.differ(a, "x", a, "y", "")
    c.files(a, ["theif0", "CTerms"], ["quest0"])
    assert c.fails == [] and len(c.passed) == 5
    c.same(a, "x", a, "y", "")
    c.differ(a, "x", a, "missing", "")
    c.files(a, ["quest0"], ["theif0"])
    assert len(c.fails) == 4


def test_actions_in_order():
    r = mf.Result(
        "r", 0, "".join(f"uikit: action -[V {s}:] from 0x1 (events 0x40)\n" for s in "abc")
    )
    c = mf.Checks()
    c.actions(r, "-[V a:]", "-[V c:]")
    assert c.fails == []
    c.actions(r, "-[V c:]", "-[V a:]")
    assert len(c.fails) == 1


def test_delete_check_on_synthetic_results():
    flow = next(f for f in mf.FLOWS if f.name == "delete")
    log = "".join(
        f"uikit: action {a} from 0x1 (events 0x40)\n"
        for a in (
            "-[MainMenuView clickPlay:]",
            "-[ChooseCharacterView chooseCharacterTwo:]",
            "-[ChooseCharacterView clickDelete:]",
            "-[ChooseCharacterView okDeleteAlert:]",
            "-[ChooseCharacterView clickBack:]",
        )
    )
    both = {"theif0": 1124, "quest0": 4781, "theif1": 1124, "quest1": 4781}
    one = {"theif0": 1124, "quest0": 4781}
    rs = {
        "seed": mf.Result("seed", 0, "", files=one),
        "seed2": mf.Result("seed2", 0, "", files=both),
        "delete": mf.Result(
            "delete", 0, log, shots={"two": "2", "alert": "A", "after": "1", "menu": "M"}, files=one
        ),
        "relaunch": mf.Result("relaunch", 0, "", shots={"title": "M", "one": "1"}, files=one),
    }
    c = mf.Checks()
    flow.check(rs, c)
    assert c.fails == []
    # The files stay: the delete did not happen.
    rs["delete"].files = both
    c = mf.Checks()
    flow.check(rs, c)
    assert c.fails == ["delete: no saves/theif1", "delete: no saves/quest1"]


def test_flow_passes(fake, tmp_path, capsys):
    assert mf.run_flow(fake, _new_game_flow(), tmp_path / "out") == 0
    out = capsys.readouterr().out
    assert "PASS   new_game:" in out
    assert "the loading screen shows (100% of a shot dark)" in out
    root = tmp_path / "out" / "new_game"
    assert (root / "data" / "saves" / "theif0").stat().st_size == 1124
    assert (root / "fresh" / "game.png").is_file()
    assert (root / "fresh" / "log.txt").read_text(encoding="utf-8").startswith(sys.executable)


@pytest.mark.parametrize(
    "mode, message",
    [
        ("nosave", "fresh: saves/theif0 is 1124 bytes (got None)"),
        ("noload", "the loading screen shows (0% of a shot dark)"),
        ("crash", "error in the log: === guest crash"),
    ],
)
def test_flow_fails(fake, tmp_path, capsys, monkeypatch, mode, message):
    monkeypatch.setenv("FAKE_MODE", mode)
    assert mf.run_flow(fake, _new_game_flow(), tmp_path / "out") == 1
    out = capsys.readouterr().out
    assert f"FAIL   new_game: {message}" in out or message in out


def test_skips_without_the_game(fake, tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("FAKE_MODE", "nogame")
    assert mf.run_flow(fake, _new_game_flow(), tmp_path / "out") == mf.SKIP
    assert mf.run_flow([str(tmp_path / "missing")], _new_game_flow(), tmp_path / "o2") == mf.SKIP
    assert mf.main(["--binary", str(tmp_path / "missing")]) == mf.SKIP
    assert "SKIP" in capsys.readouterr().out
