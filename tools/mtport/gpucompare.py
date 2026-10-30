"""The GPU backend against the software rasterizer on the game's own frames (#120, ADR 0060).

The device golden list's "fresh" run (``tests/golden/device_screens.json``: the title, the main
menu, the intro's speech box drawn with GL_LINES, an NPC dialogue, the first map with the HUD and
the key pad, the cash shop's popup) runs with ``MapleThief --fixed-step`` once per backend, and
every screen is shot with ``--shot`` and ``--shot-gl``. The fixed-step run is deterministic and
the game never reads its own pixels, so both runs draw the same frames. Then:

- **GL at 1x**: each GL frame of ``--gfx-backend gpu`` matches the software rasterizer's within
  ADR 0060's tolerance (a channel off by at most 2; at most 0.5% of the pixels beyond that).
- **screen at 1x**: the composited screens (the GL frame under UIKit's views) match the same way.
- **GL at 4x**: the same two runs at ``--render-scale 4``; the GPU's 1920x1280 GL frames match
  the software rasterizer's.
- **GPU time at 4x**: ``MAPLETHIEF_GFX_GPU_TIMING=1`` times every frame's flush with a fence. The
  median frame must take under 2 ms (issue #120's acceptance criterion; the median, as in
  ``gfx_gpu_test``, because a fence wait now and then also waits for the OS to schedule the
  thread or for the window's own work on the queue). The average and the worst frame are
  reported too. All are upper bounds: they include queueing and the completion notice.

The GPU runs need a GPU device, which needs the platform's real video driver (SDL GPU has no
device under SDL's dummy driver), so they open a window for the length of the run. Without a GPU
device the GPU runs say "the GPU backend is not available" and the test is skipped (77), as it
is with MAPLETHIEF_GPU_TESTS=0 (no windows wanted; gfx_gpu_test reads the same variable). Local
only: CI has neither the game nor a GPU.

Everything goes to ``--out`` (CTest: ``build/test-output/<preset>/gpu_compare/``). Exit status:
0 nothing failed, 1 something failed, 77 skipped, 2 bad usage.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import devicegolden as dg
from . import png

SKIP = 77
RUN = "fresh"
CHANNEL = 2  # ADR 0060: a pixel differs when a channel is off by more than this
RATIO = 0.005  # and at most this fraction of the pixels may differ
GPU_MS = 2.0  # issue #120: GPU time per frame at 4x
NOT_AVAILABLE = "gfx: the GPU backend is not available"
DRAWS_ON_GPU = "gfx: mt_gfx draws with gpu ("
TIMING_RE = re.compile(
    r"gfx: GPU time per flush with draws: ([0-9.]+) ms on average, ([0-9.]+) ms at most, over "
    r"(\d+) flushes"
)
MEDIAN_RE = re.compile(r"gfx: GPU time per frame: ([0-9.]+) ms median over (\d+) frames")


@dataclass(frozen=True)
class Diff:
    """How far two images of the same size are apart."""

    pixels: int  # all pixels
    beyond: int  # pixels with a channel off by more than CHANNEL
    max_delta: int  # the largest channel difference

    @property
    def ratio(self) -> float:
        return self.beyond / self.pixels if self.pixels else 0.0

    def within(self) -> bool:
        return self.ratio <= RATIO

    def text(self) -> str:
        return (
            f"{self.beyond} of {self.pixels} pixels beyond +-{CHANNEL} ({100 * self.ratio:.3f}%, "
            f"at most {100 * RATIO:g}%), max channel delta {self.max_delta}"
        )


def diff(a: png.Image, b: png.Image) -> Diff:
    """Compares two images pixel by pixel; a size mismatch is an error."""
    if (a.width, a.height) != (b.width, b.height):
        raise ValueError(f"sizes differ: {a.width}x{a.height} and {b.width}x{b.height}")
    if a.rgba == b.rgba:
        return Diff(a.width * a.height, 0, 0)
    beyond = 0
    max_delta = 0
    ra, rb = a.rgba, b.rgba
    # Rows that are equal (most of them) are skipped as a whole.
    stride = a.width * 4
    for y in range(a.height):
        row_a = ra[y * stride : (y + 1) * stride]
        row_b = rb[y * stride : (y + 1) * stride]
        if row_a == row_b:
            continue
        for x in range(0, stride, 4):
            d = max(
                abs(row_a[x] - row_b[x]),
                abs(row_a[x + 1] - row_b[x + 1]),
                abs(row_a[x + 2] - row_b[x + 2]),
                abs(row_a[x + 3] - row_b[x + 3]),
            )
            if d > max_delta:
                max_delta = d
            if d > CHANNEL:
                beyond += 1
    return Diff(a.width * a.height, beyond, max_delta)


@dataclass(frozen=True)
class Timing:
    average_ms: float  # per flush with draws
    max_ms: float
    flushes: int
    median_ms: float  # per frame
    frames: int


def gpu_timing(log: str) -> Timing | None:
    """The GPU backend's timing lines from a run's log (GfxBackendHost::close), or None."""
    m = TIMING_RE.search(log)
    f = MEDIAN_RE.search(log)
    if m is None or f is None:
        return None
    return Timing(
        float(m.group(1)), float(m.group(2)), int(m.group(3)), float(f.group(1)), int(f.group(2))
    )


@dataclass(frozen=True)
class Variant:
    name: str  # the run directory
    backend: str  # --gfx-backend
    scale: int  # --render-scale


VARIANTS = (
    Variant("software_1x", "software", 1),
    Variant("gpu_1x", "gpu", 1),
    Variant("software_4x", "software", 4),
    Variant("gpu_4x", "gpu", 4),
)


def compare_pair(
    report: dg.Report,
    screens: Sequence[dg.Screen],
    out: Path,
    sw: str,
    gpu: str,
    kinds: Sequence[str],
) -> None:
    for s in screens:
        for kind, suffix in (("GL", ".gl.png"), ("screen", ".png")):
            if kind not in kinds:
                continue
            try:
                a = png.load(out / sw / f"{s.name}{suffix}")
                b = png.load(out / gpu / f"{s.name}{suffix}")
                d = diff(b, a)
            except (OSError, ValueError) as e:
                report.add("FAIL", f"{s.name} {kind} ({gpu} vs {sw}): {e}")
                continue
            report.add(
                "PASS" if d.within() else "FAIL", f"{s.name} {kind} ({gpu} vs {sw}): {d.text()}"
            )


def run_all(
    spec: dg.Spec, command: Sequence[str], out: Path, timeout: float, scale4: bool = True
) -> dg.Report:
    report = dg.Report([])
    run = next((r for r in spec.runs if r.name == RUN), None)
    if run is None or run.data != "empty":
        raise dg.SpecError(f"the screen list has no run {RUN!r} from an empty data directory")
    screens = spec.screens_of(RUN)
    out.mkdir(parents=True, exist_ok=True)
    logs: dict[str, str] = {}
    for v in VARIANTS:
        if v.scale != 1 and not scale4:
            continue
        run_dir = out / v.name
        extra = ("--gfx-backend", v.backend, "--render-scale", str(v.scale), "--shot-gl")
        # The GPU needs the real video driver; the software runs stay headless.
        env = {"MAPLETHIEF_GFX_GPU_TIMING": "1"} if v.backend == "gpu" else {}
        status, what = dg._run_game(
            command,
            spec,
            run,
            run_dir,
            None,
            timeout,
            extra=extra,
            env=env,
            headless=v.backend != "gpu",
        )
        print(f"run {RUN} {v.name}: {what}", flush=True)
        if status == dg.EXIT_NO_GAME:
            report.no_game = True
            report.add("SKIP", f"the game did not start ({what}; log: {run_dir / 'log.txt'})")
            return report
        log = (run_dir / "log.txt").read_text(encoding="utf-8", errors="replace")
        if v.backend == "gpu" and (NOT_AVAILABLE in log or DRAWS_ON_GPU not in log):
            report.no_game = True  # nothing to compare: no GPU here
            report.add(
                "SKIP", f"{v.name}: no GPU backend on this machine (log: {run_dir / 'log.txt'})"
            )
            return report
        if status != 0:
            report.add("FAIL", f"{v.name}: {what} (log: {run_dir / 'log.txt'})")
            return report
        logs[v.name] = log

    compare_pair(report, screens, out, "software_1x", "gpu_1x", ("GL", "screen"))
    if scale4:
        compare_pair(report, screens, out, "software_4x", "gpu_4x", ("GL",))
    for name in ("gpu_1x", "gpu_4x"):
        if name not in logs:
            continue
        t = gpu_timing(logs[name])
        if t is None:
            report.add("FAIL", f"{name}: no GPU timing line in the log")
            continue
        verdict = "PASS" if name != "gpu_4x" or t.median_ms < GPU_MS else "FAIL"
        bound = f" (must be under {GPU_MS:g} ms)" if name == "gpu_4x" else ""
        report.add(
            verdict,
            f"{name}: GPU time per frame {t.median_ms:.3f} ms median over {t.frames} frames"
            f"{bound}; per flush {t.average_ms:.3f} ms on average, {t.max_ms:.3f} ms at most, "
            f"over {t.flushes} (fence-timed, upper bounds)",
        )
    return report


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.gpucompare", description=__doc__.split("\n\n")[0]
    )
    parser.add_argument(
        "--binary", type=Path, required=True, help="MapleThief, built with the generated code"
    )
    parser.add_argument("--screens", type=Path, default=Path("tests/golden/device_screens.json"))
    parser.add_argument("--out", type=Path, default=Path("build/test-output/gpu_compare"))
    parser.add_argument("--no-scale4", action="store_true", help="only the 1x runs")
    parser.add_argument(
        "--timeout", type=float, default=1800.0, help="seconds per run (default 1800)"
    )
    args = parser.parse_args(argv)
    try:
        spec = dg.load_spec(args.screens)
    except dg.SpecError as e:
        print(f"gpucompare: {e}", file=sys.stderr)
        return 2
    if os.environ.get("MAPLETHIEF_GPU_TESTS") == "0":
        print("SKIP   MAPLETHIEF_GPU_TESTS=0: no GPU runs")
        return SKIP
    if not args.binary.is_file():
        print(f"SKIP   {args.binary}: no such program (build MapleThief with work/gen)")
        return SKIP
    try:
        report = run_all(spec, [str(args.binary)], args.out, args.timeout, not args.no_scale4)
    except dg.SpecError as e:
        print(f"gpucompare: {e}", file=sys.stderr)
        return 2
    print(
        f"gpu_compare: {report.passed} passed, {report.failed} failed, {report.skipped} skipped "
        f"(output in {args.out})"
    )
    if report.failed:
        return 1
    return SKIP if report.no_game or report.passed == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
