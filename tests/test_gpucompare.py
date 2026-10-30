"""The GPU backend's comparison with the software rasterizer (mtport.gpucompare, #120, ADR 0060):
the tolerance on synthetic images, the timing line, and a whole run against a stand-in for
MapleThief (a small Python program that writes synthetic shots and GL frames per backend and
logs what the real frontend logs), so the flow is tested without the game or a GPU."""

from __future__ import annotations

import sys

import pytest

from mtport import devicegolden as dg
from mtport import gpucompare as gc
from mtport import png
from repo_paths import REPO

SCREENS = REPO / "tests" / "golden" / "device_screens.json"


def image(
    w: int = 40, h: int = 20, fill: tuple[int, int, int, int] = (10, 20, 30, 255)
) -> png.Image:
    return png.Image(w, h, bytes(fill) * (w * h))


def poke(img: png.Image, pixels: list[tuple[int, int]], delta: int) -> png.Image:
    b = bytearray(img.rgba)
    for x, y in pixels:
        o = 4 * (y * img.width + x) + 1  # the green channel
        b[o] = (b[o] + delta) % 256
    return png.Image(img.width, img.height, bytes(b))


def test_diff_counts_pixels_beyond_the_channel_tolerance() -> None:
    a = image()
    assert gc.diff(a, a) == gc.Diff(800, 0, 0)
    # Off by 2: within the channel tolerance, so not counted.
    d = gc.diff(a, poke(a, [(0, 0), (5, 5)], 2))
    assert d == gc.Diff(800, 0, 2) and d.within()
    # Off by 3: counted. 4 of 800 is exactly 0.5%, still within; 5 is not.
    four = poke(a, [(0, 0), (1, 0), (2, 0), (3, 0)], 3)
    assert gc.diff(a, four) == gc.Diff(800, 4, 3)
    assert gc.diff(a, four).within()
    five = poke(four, [(4, 0)], 200)
    d = gc.diff(a, five)
    assert d == gc.Diff(800, 5, 200) and not d.within()
    assert "5 of 800 pixels beyond +-2 (0.625%" in d.text()


def test_diff_rejects_different_sizes() -> None:
    with pytest.raises(ValueError, match="sizes differ"):
        gc.diff(image(40, 20), image(20, 40))


def test_gpu_timing_reads_the_frontends_lines() -> None:
    log = (
        "gfx: GPU backend: 300 flushes, ...\n"
        "gfx: GPU time per flush with draws: 0.655 ms on average, 7.200 ms at most, over 300 "
        "flushes (fence-timed, upper bounds)\n"
        "gfx: GPU time per frame: 0.512 ms median over 299 frames (fence-timed, an upper bound)\n"
    )
    assert gc.gpu_timing(log) == gc.Timing(0.655, 7.2, 300, 0.512, 299)
    assert gc.gpu_timing(log.split("gfx: GPU time per frame")[0]) is None
    assert gc.gpu_timing("nothing here") is None


# The stand-in: for each --shot, a screen and a GL frame at the render scale. The GPU runs log
# the frontend's lines and differ from the software runs in a few pixels by 1 (within the
# tolerance), or by more (FAKE_MODE=off), or have no GPU (nogpu), or are slow (slow). It checks
# that only the GPU runs keep the real video driver.
FAKE = r"""
import os, sys
sys.path.insert(0, sys.argv[1])
from mtport import png
args = sys.argv[2:]
backend = args[args.index("--gfx-backend") + 1]
scale = int(args[args.index("--render-scale") + 1])
mode = os.environ.get("FAKE_MODE", "")
assert "--shot-gl" in args
assert (os.environ.get("SDL_VIDEO_DRIVER") == "dummy") == (backend == "software")
if mode == "nogame":
    sys.exit(4)
gpu = backend == "gpu"
if gpu and mode == "nogpu":
    print("gfx: the GPU backend is not available: no GPU device (none); mt_gfx stays on the "
          "software rasterizer")
    gpu = False
if gpu:
    assert os.environ.get("MAPLETHIEF_GFX_GPU_TIMING") == "1"
print("gfx: mt_gfx draws with %s; the window's renderer is gpu"
      % ("gpu (metal)" if gpu else "the software rasterizer"))
cap = args[args.index("--capture") + 1]
def frame(w, h, delta):
    b = bytearray(bytes((10, 20, 30, 255)) * (w * h))
    if gpu:
        for x in range(10):
            b[4 * (5 * w + x) + 2] += delta
    return png.Image(w, h, bytes(b))
for i, a in enumerate(args):
    if a == "--shot":
        name = args[i + 1].split(",")[1]
        delta = 1
        if mode == "off" and name == "first_map":
            delta = 9 if scale == 1 else 1
        png.save(os.path.join(cap, name + ".png"), frame(480 * scale, 320 * scale, delta))
        gl = frame(480 * scale, 320 * scale, delta)
        if gpu and mode == "off" and name == "first_map" and scale == 1:
            b = bytearray(gl.rgba)
            for o in range(0, 4 * 2000, 4):
                b[o] = 99
            gl = png.Image(gl.width, gl.height, bytes(b))
        png.save(os.path.join(cap, name + ".gl.png"), gl)
print("game: frame time: %.2f ms per window frame in the frontend (...)"
      % ((2.5 if gpu else 30.0) * scale))
if gpu:
    if mode != "readback":
        print("present: the GL frame stays on the GPU: UIKit's views are composited as an "
              "overlay over it (read-backs only for shots, captures and checks)")
    print("gfx: GPU backend: 360 flushes, 900 batches, 5000 vertices, 3 pipelines, 10 texture "
          "uploads, 1 target uploads, %d read-backs (0.0 MB)" % (360 if mode == "readback" else 0))
    ms = 3.5 if mode == "slow" else 0.8
    print("gfx: GPU time per flush with draws: 2.500 ms on average, 40.000 ms at most, over 360 "
          "flushes (fence-timed, upper bounds)")
    print("gfx: GPU time per frame: %.3f ms median over 359 frames (fence-timed, an upper "
          "bound)" % ms)
"""


def run_fake(tmp_path, monkeypatch, mode: str = "", scale4: bool = True) -> dg.Report:
    fake = tmp_path / "fake.py"
    fake.write_text(FAKE, encoding="utf-8")
    monkeypatch.setenv("SDL_VIDEO_DRIVER", "cocoa")  # the GPU runs must drop it
    if mode:
        monkeypatch.setenv("FAKE_MODE", mode)
    spec = dg.load_spec(SCREENS)
    return gc.run_all(
        spec, [sys.executable, str(fake), str(REPO / "tools")], tmp_path / "out", 120, scale4
    )


def test_a_whole_run_against_a_stand_in(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch)
    text = "\n".join(report.lines)
    assert report.failed == 0, text
    screens = dg.load_spec(SCREENS).screens_of(gc.RUN)
    # GL and screen at 1x and 4x, and per GPU run the read-back and the timing lines.
    assert report.passed == 4 * len(screens) + 4, text
    assert "first_map GL (gpu_4x vs software_4x): 0 of 2457600 pixels beyond +-2" in text
    assert "first_map screen (gpu_4x vs software_4x): 0 of 2457600 pixels beyond +-2" in text
    assert "gpu_4x: GPU time per frame 0.800 ms median over 359 frames (must be under 2 ms)" in text
    assert (
        "PASS   gpu_4x: presented on the GPU, 0 read-backs of mt_gfx's targets (must be 0); "
        "frame time 10.00 ms per window frame (the software run's 120.00 ms)"
    ) in text
    assert report.status() == 0


def test_gpu_runs_that_read_back_fail(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "readback", scale4=False)
    text = "\n".join(report.lines)
    assert report.failed == 1, text
    assert "FAIL   gpu_1x: not presented on the GPU (360 read-backs); frame time 2.50 ms" in text


def test_readbacks_and_frame_time_read_the_frontends_lines() -> None:
    log = (
        "gfx: GPU backend: 1202 flushes, 17831 batches, 720142 vertices, 3 pipelines, 164 texture "
        "uploads, 1 target uploads, 0 read-backs (0.0 MB)\n"
        "game: frame time: 12.34 ms per window frame in the frontend (...)\n"
    )
    assert gc.readbacks(log) == 0
    assert gc.readbacks(log.replace(" 0 read-backs", " 1202 read-backs")) == 1202
    assert gc.readbacks("nothing") is None
    assert gc.frame_time(log) == 12.34
    assert gc.frame_time("nothing") is None


def test_only_the_1x_runs(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, scale4=False)
    assert report.failed == 0
    assert not (tmp_path / "out" / "gpu_4x").exists()
    assert "gpu_1x: GPU time per frame" in "\n".join(report.lines)


def test_pixels_beyond_the_tolerance_fail(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "off", scale4=False)
    text = "\n".join(report.lines)
    assert report.failed == 1, text
    assert "FAIL   first_map GL (gpu_1x vs software_1x): 2010 of 153600" in text


def test_slow_gpu_time_at_4x_fails(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "slow")
    text = "\n".join(report.lines)
    assert report.failed == 1, text
    assert "FAIL   gpu_4x: GPU time per frame 3.500 ms" in text


def test_no_gpu_skips(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "nogpu")
    assert report.no_game and report.failed == 0
    assert report.status() == gc.SKIP


def test_no_game_skips(tmp_path, monkeypatch) -> None:
    report = run_fake(tmp_path, monkeypatch, "nogame")
    assert report.no_game
    assert report.status() == gc.SKIP
