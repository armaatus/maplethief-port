"""The logged performance session's judge (mtport.perfsession, #244, ADR 0145).

Synthetic logs: lines in the game's format (platform/src/perf_session.h), made up so that every
number of the verdict is known. KLINE is the exact text tests/cpp/perf_session_test.cpp checks
the game writes, so the game and the judge agree on the format.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

import pytest

from mtport import perfsession as ps
from repo_paths import REPO

KLINE = (
    "perf: t=1.050 s=1.000 display=handheld scene=map:2.0 ff=1 scale=1 backend=gpu frames=20 "
    "ticks=20 held=20 held3=20 over=0 tick_max=50.00 guest=40.00 gfx=20.00 take=10.00 "
    "compose=10.00 draw=5.00 present=80.00 wait=830.00 other=5.00"
)


def line(
    t: float,
    display: str = "handheld",
    scene: str = "map:2.0",
    ff: int = 1,
    ticks: int = 20,
    held3: int | None = None,
    s: float = 1.0,
    guest: float = 40.0,
) -> str:
    held3 = ticks if held3 is None else held3
    return (
        f"2026-11-11 12:00:00.000 MapleThief[1:2] perf: t={t:.3f} s={s:.3f} display={display} "
        f"scene={scene} ff={ff} scale=1 backend=gpu frames={ticks} ticks={ticks} held={ticks} "
        f"held3={held3} over=0 tick_max=50.00 guest={guest:.2f} gfx=20.00 take=10.00 "
        f"compose=10.00 draw=5.00 present=80.00 wait=830.00 other=5.00"
    )


def session(
    handheld_s: int = 300,
    docked_s: int = 300,
    late: int = 0,
    ff: bool = True,
    busy_at: float | None = None,
    ended: bool = True,
) -> str:
    out = ["perf: session start: render_scale = 1, gfx_backend = gpu (gpu (deko3d)), mode handheld"]
    t = 0.0
    for _ in range(5):  # the title and a load: not judged
        out.append(line(t := t + 1, scene="menu"))
    out.append(line(t := t + 1, scene="load"))
    for i in range(handheld_s):
        held3 = 20 - (1 if i < late else 0)
        out.append(line(t := t + 1, held3=held3))
    for _ in range(docked_s):
        out.append(line(t := t + 1, display="docked", scene="map:7.4"))
    if ff:
        for _ in range(10):
            out.append(line(t := t + 1, display="docked", ff=2, ticks=40, held3=0))
    if busy_at is not None:
        out.append(line(busy_at, display="docked", scene="map:9.9", guest=400.0))
    if ended:
        out.append(
            f"perf: session ended: {len(out)} lines (python3 -m mtport.perfsession judge LOG)"
        )
    return "\n".join(out) + "\n"


def judge(text: str, **kw) -> tuple[bool, list[str]]:
    return ps.judge(ps.parse_log(text), ["handheld", "docked"], **kw)


def test_the_games_line_parses():
    w = ps.parse_line(KLINE)
    assert w is not None
    assert (w.t, w.s, w.display, w.scene, w.ff, w.scale, w.backend) == (
        1.05,
        1.0,
        "handheld",
        "map:2.0",
        1,
        1,
        "gpu",
    )
    assert (w.frames, w.ticks, w.held, w.held3, w.over, w.tick_max) == (20, 20, 20, 20, 0, 50.0)
    assert (w.guest, w.gfx, w.take, w.compose, w.draw, w.present, w.wait, w.other) == (
        40.0,
        20.0,
        10.0,
        10.0,
        5.0,
        80.0,
        830.0,
        5.0,
    )
    assert w.on_map
    assert w.busy_ms == 90.0  # the split without present and wait
    # Anything before "perf:" (a timestamp, the program's name) is ignored; other lines are not
    # session lines.
    assert ps.parse_line("2026-11-11 MapleThief[1] " + KLINE) == w
    assert ps.parse_line("perf: display 60.00 Hz, vsync on") is None
    assert ps.parse_line("perf: session ended: 3 lines") is None
    with pytest.raises(ValueError, match="without held3"):
        ps.parse_line(KLINE.replace(" held3=20", ""))


def test_a_full_session_passes():
    ok, out = judge(session())
    assert ok, out
    assert out[0].startswith("perfsession: settings: render_scale = 1")
    hand = next(x for x in out if x.startswith("perfsession: handheld on a map:"))
    assert "PASS: 300.0 s (map:2.0 300 s); 20.00 ticks a second (6000 ticks); " in hand
    assert "100.00 % held 3 vblanks (6000 of 6000)" in hand
    # The split per tick: 40 ms of guest over 20 ticks.
    assert (
        "per tick guest 2.00, mt_gfx 1.00, take 0.50, compositor 0.50, draw 0.25, "
        "present 4.00, other 0.25 ms; CPU busy 9.0 %" in hand
    )
    assert any(
        x.startswith("perfsession: docked on a map: PASS: 300.0 s (map:7.4 300 s)") for x in out
    )
    ff = next(x for x in out if "fast-forward" in x)
    assert ff.startswith(
        "perfsession: docked fast-forward x2: target 40.00 ticks a second, 100.0 % of it; "
        "10.0 s (map:2.0 10 s); 40.00 ticks a second (400 ticks)"
    )
    assert out[-1] == "perfsession: verdict: PASS"


def test_too_short_a_session_fails():
    ok, out = judge(session(handheld_s=299))
    assert not ok
    assert any("handheld on a map: FAIL (299.0 s on a map, not 300)" in x for x in out)


def test_late_frames_fail_under_99_percent():
    # 61 of 6000 frames not held 3 vblanks: 98.98 %.
    ok, out = judge(session(late=61))
    assert not ok
    assert any("FAIL (98.98 % held 3 vblanks, under 99 %)" in x for x in out)
    ok, _ = judge(session(late=60))  # exactly 99.00 %
    assert ok


def test_the_tick_rate_must_read_20_00():
    text = session()
    # One second on the handheld map with 19 ticks: 5999 ticks in 300 s = 19.9967, reads 20.00.
    first_map = text.index("scene=map:2.0")
    one_short = text[:first_map] + text[first_map:].replace(
        "ticks=20 held=20 held3=20", "ticks=19 held=20 held3=20", 1
    )
    ok, out = judge(one_short)
    assert ok, out
    assert any("20.00 ticks a second (5999 ticks)" in x for x in out)
    # Ten such seconds: 19.97.
    slow = "\n".join(
        x.replace("ticks=20 held=20 held3=20", "ticks=19 held=20 held3=20")
        if (" display=handheld scene=map:2.0" in x and i % 30 == 0)
        else x
        for i, x in enumerate(text.splitlines())
    )
    ok, out = judge(slow)
    assert not ok
    assert any("ticks a second, not 20.00" in x for x in out)
    assert ps.rate_ok(19.995, 20.0) and not ps.rate_ok(19.994, 20.0)


def test_fast_forward_must_be_logged_unless_waived():
    ok, out = judge(session(ff=False))
    assert not ok
    assert "perfsession: fast-forward: not logged (FAIL: the criterion asks for it)" in out
    ok, out = judge(session(ff=False), need_fast_forward=False)
    assert ok
    assert "perfsession: fast-forward: not logged" in out


def test_the_busiest_second_is_named():
    _, out = judge(session(busy_at=700.5))
    busy = next(x for x in out if x.startswith("perfsession: busiest second:"))
    assert busy.startswith(
        "perfsession: busiest second: docked map:9.9 at 700.5 s: CPU busy 45.0 %"
    )


def test_a_run_that_did_not_end_is_judged_with_a_note():
    ok, out = judge(session(ended=False))
    assert ok
    assert any("no 'session ended' line" in x for x in out)


def test_cli(tmp_path, capsys):
    log = tmp_path / "MapleThief-perf.log"
    log.write_text(session(), encoding="utf-8")
    assert ps.main(["judge", str(log)]) == 0
    assert capsys.readouterr().out.splitlines()[-1] == "perfsession: verdict: PASS"
    log.write_text(session(late=100), encoding="utf-8")
    assert ps.main(["judge", str(log)]) == 1
    capsys.readouterr()
    # A desktop run: its own screen.
    desk = (
        session()
        .replace("display=handheld", "display=desktop")
        .replace("display=docked", "display=desktop")
    )
    log.write_text(desk, encoding="utf-8")
    assert ps.main(["judge", str(log), "--displays", "desktop", "--min-seconds", "600"]) == 0
    capsys.readouterr()
    log.write_text("no session here\n", encoding="utf-8")
    assert ps.main(["judge", str(log)]) == 2
    assert "no perf: lines" in capsys.readouterr().err


@pytest.mark.skipif(sys.platform == "win32" or shutil.which("bash") is None, reason="a bash script")
def test_switch_perf_judges_a_kept_log(tmp_path):
    log = tmp_path / "kept.log"
    log.write_text(session(), encoding="utf-8")
    env = dict(os.environ)
    run = subprocess.run(
        ["bash", str(REPO / "scripts" / "switch-perf"), "--judge", str(log)],
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    assert run.returncode == 0, run.stderr
    assert run.stdout.splitlines()[-1] == "perfsession: verdict: PASS"
