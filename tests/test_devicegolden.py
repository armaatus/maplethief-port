"""The device golden harness (mtport.devicegolden, #60): the committed screen list, the command
lines it builds, and a whole harness run against a stand-in for MapleThief (a small Python
program that writes synthetic shots), so the flow (data carried between runs, skips, recording
our renders, device comparisons, the determinism check) is tested without the game."""

from __future__ import annotations

import copy
import json
import sys

import pytest

from mtport import devicegolden as dg
from mtport import goldencmp as gc
from mtport import png
from repo_paths import REPO

SCREENS = REPO / "tests" / "golden" / "device_screens.json"

# The stand-in: writes <capture>/<name>.png for each --shot, a colour made of the shot's name
# and the number of taps before it, and counts its launches in the data directory.
FAKE = r"""
import os, sys, zlib
sys.path.insert(0, sys.argv[1])
from mtport import png
args = sys.argv[2:]
mode = os.environ.get("FAKE_MODE", "")
if mode == "nogame":
    sys.exit(4)
if mode == "crash":
    sys.exit(5)
data = os.environ["MAPLETHIEF_DATA_DIR"]
count = os.path.join(data, "launches")
n = int(open(count).read()) if os.path.exists(count) else 0
open(count, "w").write(str(n + 1))
cap = args[args.index("--capture") + 1]
taps = [float(args[i + 1].split(",")[0]) for i, a in enumerate(args) if a == "--tap"]
assert args[args.index("--fixed-step") + 1] == "1"
for i, a in enumerate(args):
    if a == "--shot":
        t, name = args[i + 1].split(",")
        k = sum(1 for x in taps if x < float(t))
        c = zlib.crc32(name.encode()) & 0xFF
        noise = os.urandom(1)[0] if mode == "random" else 0
        img = png.Image(480, 320, bytes((c, k * 10 % 256, n * 50 % 256, 255)) * (480 * 320))
        if noise:
            b = bytearray(img.rgba); b[0] = noise; img = png.Image(480, 320, bytes(b))
        png.save(os.path.join(cap, name + ".png"), img)
"""


@pytest.fixture
def spec() -> dg.Spec:
    return dg.load_spec(SCREENS)


def doc() -> dict:
    return json.loads(SCREENS.read_text(encoding="utf-8"))


def test_committed_list_has_the_issues_screens(spec):
    names = [s.name for s in spec.screens]
    assert names == [
        "title",
        "main_menu",
        "intro_dialog",
        "npc_dialog",
        "first_map",
        "popup",
        "character_select",
    ]
    assert [r.name for r in spec.runs] == ["fresh", "saved"]
    assert next(r for r in spec.runs if r.name == "saved").data == "fresh"
    assert spec.tolerance["ours"] == gc.Tolerance(0, 0.0)
    # Every screen's shot comes after the taps that lead to it and before the run's later taps
    # change it: the shots of the fresh run are in the order of the screens.
    fresh = [s.at for s in spec.screens_of("fresh")]
    assert fresh == sorted(fresh)
    for s in spec.screens:
        assert s.device.strip() and s.what.strip()


def test_game_args(spec, tmp_path):
    run = spec.runs[1]
    assert dg.game_args(spec, run, tmp_path) == [
        "--fixed-step",
        "1",
        "--capture",
        str(tmp_path),
        "--tap",
        "3,106,165",
        "--shot",
        "5,character_select",
    ]
    fresh = dg.game_args(spec, spec.runs[0], tmp_path)
    assert fresh.count("--tap") == 12 and fresh.count("--shot") == 6
    assert "2.5,title" in fresh and "4.5,106,165" in fresh


@pytest.mark.parametrize(
    "change, message",
    [
        (lambda d: d.update(version=2), "version"),
        (lambda d: d["screens"][1].update(name="title"), "twice"),
        (lambda d: d["screens"][0].update(name="a/b"), "name"),
        (lambda d: d["screens"][0].update(run="nope"), "no run"),
        (lambda d: d["runs"][1].update(data="later"), "earlier run"),
        (lambda d: d["runs"][0]["taps"][0].update(x=480), "off the 480x320"),
        (lambda d: d["runs"][0]["taps"][1].update(at=1.0), "time order"),
        (lambda d: d["screens"][2]["masks"][0].update(rect=[400, 0, 100, 10]), "off the screen"),
        (lambda d: d["screens"][2]["masks"][0].update(rect=[0, 0, 0, 10]), "positive size"),
        (lambda d: d["screens"][0].update(device=""), "device"),
        (lambda d: d["tolerance"].pop("device_2x"), "tolerance"),
        (lambda d: d["tolerance"]["ours"].update(channel=300), "0..255"),
        (lambda d: d.update(fixed_step_ms=0), "fixed_step_ms"),
    ],
)
def test_bad_lists_are_rejected(change, message):
    d = copy.deepcopy(doc())
    change(d)
    with pytest.raises(dg.SpecError, match=message):
        dg.parse_spec(d)


# --- a harness run with the stand-in ----------------------------------------------------------


@pytest.fixture
def fake(tmp_path) -> list[str]:
    script = tmp_path / "fake_maplethief.py"
    script.write_text(FAKE, encoding="utf-8")
    return [sys.executable, str(script), str(REPO / "tools")]


def test_first_run_skips_then_records_then_passes(spec, fake, tmp_path, capsys):
    golden, out = tmp_path / "golden", tmp_path / "out"
    r = dg.run_all(spec, fake, golden, out)
    assert (r.failed, r.passed, r.skipped) == (0, 0, 14) and r.status() == dg.SKIP
    text = capsys.readouterr().out
    assert (
        "title device: not captured yet" in text and "MAPLETHIEF_UPDATE_GOLDEN=1 records it" in text
    )
    # The saved run started from the fresh run's data directory (one launch already counted).
    assert (out / "runs" / "saved" / "data" / "launches").read_text(encoding="utf-8") == "2"
    assert (out / "runs" / "fresh" / "data" / "launches").read_text(encoding="utf-8") == "1"
    assert (out / "character_select.actual.png").is_file()

    r = dg.run_all(spec, fake, golden, out, update=True)
    assert r.failed == 0 and sorted(p.name for p in (golden / "ours").iterdir()) == sorted(
        f"{s.name}.png" for s in spec.screens
    )
    r = dg.run_all(spec, fake, golden, out)
    assert (r.failed, r.passed, r.skipped) == (0, 7, 7) and r.status() == 0


def test_a_changed_render_fails_and_writes_a_diff(spec, fake, tmp_path):
    golden, out = tmp_path / "golden", tmp_path / "out"
    dg.run_all(spec, fake, golden, out, update=True)
    img = png.load(golden / "ours" / "popup.png")
    b = bytearray(img.rgba)
    b[0] ^= 0xFF
    png.save(golden / "ours" / "popup.png", png.Image(480, 320, bytes(b)))
    r = dg.run_all(spec, fake, golden, out)
    assert r.failed == 1 and r.status() == 1
    assert png.load(out / "popup.ours.diff.png").pixel(0, 0) == gc.DIFF_DIFFERING


def test_device_screenshots_are_normalised_masked_and_compared(spec, fake, tmp_path, capsys):
    golden, out = tmp_path / "golden", tmp_path / "out"
    dg.run_all(spec, fake, golden, out, only=["npc_dialog", "title"])
    ours = png.load(out / "npc_dialog.actual.png")
    # A portrait 2x screenshot whose text area (a mask) differs completely.
    b = bytearray(ours.rgba)
    for y in range(90, 100):
        for x in range(190, 300):
            b[(y * 480 + x) * 4] ^= 0xFF
    shot = png.Image(480, 320, bytes(b))
    up = bytearray()
    for y in range(640):
        row = shot.rgba[(y // 2) * 1920 : (y // 2 + 1) * 1920]
        up += b"".join(row[x * 4 : x * 4 + 4] * 2 for x in range(480))
    gc.save(
        golden / "device" / "npc_dialog.png", gc.screen_to_portrait(png.Image(960, 640, bytes(up)))
    )
    # The title's screenshot is of the wrong size.
    gc.save(golden / "device" / "title.png", png.Image(100, 100, b"\0\0\0\xff" * 10000))
    r = dg.run_all(spec, fake, golden, out, only=["npc_dialog", "title"])
    text = capsys.readouterr().out
    assert "PASS   npc_dialog device (640x960)" in text and "Retina 2x" in text
    assert "FAIL   title device: " in text and "no screenshot of the 480x320 game" in text
    assert r.failed == 1
    assert png.load(out / "npc_dialog.device.png") == shot
    assert png.load(out / "npc_dialog.device.diff.png").pixel(200, 95) == gc.DIFF_MASKED


def test_repeat_checks_determinism(spec, fake, tmp_path, monkeypatch, capsys):
    r = dg.run_all(
        spec, fake, tmp_path / "g", tmp_path / "out", repeat=2, only=["character_select"]
    )
    assert (
        r.failed == 0
        and "character_select: deterministic: identical over 2 runs" in capsys.readouterr().out
    )
    monkeypatch.setenv("FAKE_MODE", "random")
    r = dg.run_all(spec, fake, tmp_path / "g", tmp_path / "out", repeat=3, only=["title"])
    assert r.failed >= 1 and "NOT identical over 3 runs" in capsys.readouterr().out


def test_no_game_skips_and_a_crash_fails(spec, fake, tmp_path, monkeypatch):
    monkeypatch.setenv("FAKE_MODE", "nogame")
    r = dg.run_all(spec, fake, tmp_path / "g", tmp_path / "out")
    assert r.no_game and r.status() == dg.SKIP
    monkeypatch.setenv("FAKE_MODE", "crash")
    r = dg.run_all(spec, fake, tmp_path / "g", tmp_path / "out")
    assert r.status() == 1 and r.failed == 7  # both runs broken: every screen fails


def test_cli(tmp_path, capsys):
    assert dg.main(["--binary", str(tmp_path / "missing"), "--screens", str(SCREENS)]) == dg.SKIP
    assert "no such program" in capsys.readouterr().out
    bad = tmp_path / "bad.json"
    bad.write_text("{}", encoding="utf-8")
    assert dg.main(["--binary", sys.executable, "--screens", str(bad)]) == 2
    with pytest.raises(dg.SpecError, match="no screen nope"):
        dg.run_all(dg.load_spec(SCREENS), [sys.executable], tmp_path, tmp_path, only=["nope"])
