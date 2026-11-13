"""Judge a logged performance session (#244, ADR 0145): the game's ``--perf-session`` lines.

With ``--perf-session`` the game logs one ``perf:`` line per second of play with the counts and
sums of ADR 0071's telemetry over that second (platform/src/perf_session.h has the format)::

    perf: t=1.050 s=1.000 display=handheld scene=map:2.0 ff=1 scale=1 backend=gpu frames=20
          ticks=20 held=20 held3=20 over=0 tick_max=50.00 guest=40.00 gfx=20.00 take=10.00
          compose=10.00 draw=5.00 present=80.00 wait=830.00 other=5.00

(on one line). Because they are counts and sums, any set of lines adds up exactly. This module
reads a log (nxlink's output kept by ``scripts/switch-perf``, the player log on the SD card, or a
desktop run's stderr; anything before ``perf:`` on a line is ignored), adds the lines up per
screen and scene, and states pass or fail against #244's criterion: on a map, without
fast-forward, for at least 300 s on each screen, 20.00 ticks a second (to two decimals) and at
least 99 % of the window frames held 3 vblanks. It also reports fast-forward's rate against its
target and the busiest second on a map, both of which the criterion asks to be logged.

CLI::

    python3 -m mtport.perfsession judge LOG [--displays handheld,docked] [--min-seconds 300]
                                            [--held3 99] [--no-fast-forward]

Prints one ``perfsession:`` line per screen (with PASS or FAIL), one for fast-forward, one for the
busiest second, and a last ``perfsession: verdict: PASS|FAIL`` line; exits 0 on PASS, 1 on FAIL,
2 when the log has no session lines. The log is untrusted data: it is parsed, never run.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The split's keys, in the line's order, with the names the summary prints.
SPLIT = (
    ("guest", "guest"),
    ("gfx", "mt_gfx"),
    ("take", "take"),
    ("compose", "compositor"),
    ("draw", "draw"),
    ("present", "present"),
    ("other", "other"),
)
INT_KEYS = ("ff", "scale", "frames", "ticks", "held", "held3", "over")
FLOAT_KEYS = ("t", "s", "tick_max") + tuple(k for k, _ in SPLIT) + ("wait",)
TEXT_KEYS = ("display", "scene", "backend")
KEYS = FLOAT_KEYS + INT_KEYS + TEXT_KEYS

LINE = re.compile(r"perf: (t=\S+(?: [a-z_0-9]+=\S+)+)\s*$")
START = re.compile(r"perf: session start: (.*?)\s*$")
END = re.compile(r"perf: session ended: (\d+) lines")

TICK_RATE = 20.0  # the game's ticks a second at its default speed (a 50 ms tick)


@dataclass
class Window:
    """One ``perf:`` line."""

    t: float
    s: float
    display: str
    scene: str
    ff: int
    scale: int
    backend: str
    frames: int
    ticks: int
    held: int
    held3: int
    over: int
    tick_max: float
    guest: float
    gfx: float
    take: float
    compose: float
    draw: float
    present: float
    wait: float
    other: float

    @property
    def on_map(self) -> bool:
        return self.scene.startswith("map:")

    @property
    def busy_ms(self) -> float:
        """The CPU's work in the window: the split without the waits (SDL_RenderPresent's
        wait for the display and the turn hook's)."""
        return self.guest + self.gfx + self.take + self.compose + self.draw + self.other


def parse_line(text: str) -> Window | None:
    """The window of one log line, or None if it is not a session line. A session line with a
    missing or malformed field raises ValueError."""
    m = LINE.search(text)
    if not m:
        return None
    pairs = dict(item.split("=", 1) for item in m.group(1).split(" "))
    missing = [k for k in KEYS if k not in pairs]
    if missing:
        raise ValueError(f"a perf: line without {', '.join(missing)}: {text.strip()}")
    values: dict[str, object] = {}
    for k in FLOAT_KEYS:
        values[k] = float(pairs[k])
    for k in INT_KEYS:
        values[k] = int(pairs[k])
    for k in TEXT_KEYS:
        values[k] = pairs[k]
    return Window(**values)  # type: ignore[arg-type]


@dataclass
class Session:
    windows: list[Window] = field(default_factory=list)
    start: str | None = None  # the settings the game logged at the start
    ended: int | None = None  # the line count of "session ended", None if the run did not end


def parse_log(text: str) -> Session:
    session = Session()
    for line in text.splitlines():
        w = parse_line(line)
        if w is not None:
            session.windows.append(w)
            continue
        m = START.search(line)
        if m and session.start is None:
            session.start = m.group(1)
            continue
        m = END.search(line)
        if m:
            session.ended = int(m.group(1))
    return session


@dataclass
class Totals:
    """Windows added up."""

    seconds: float = 0.0
    frames: int = 0
    ticks: int = 0
    held: int = 0
    held3: int = 0
    over: int = 0
    tick_max: float = 0.0
    split: dict[str, float] = field(default_factory=lambda: {k: 0.0 for k, _ in SPLIT})
    busy_ms: float = 0.0
    scenes: dict[str, float] = field(default_factory=dict)
    backends: set[str] = field(default_factory=set)
    scales: set[int] = field(default_factory=set)

    def add(self, w: Window) -> None:
        self.seconds += w.s
        self.frames += w.frames
        self.ticks += w.ticks
        self.held += w.held
        self.held3 += w.held3
        self.over += w.over
        self.tick_max = max(self.tick_max, w.tick_max)
        for k, _ in SPLIT:
            self.split[k] += getattr(w, k)
        self.busy_ms += w.busy_ms
        self.scenes[w.scene] = self.scenes.get(w.scene, 0.0) + w.s
        self.backends.add(w.backend)
        self.scales.add(w.scale)

    @property
    def rate(self) -> float:
        return self.ticks / self.seconds if self.seconds > 0 else 0.0

    @property
    def held3_pct(self) -> float:
        return 100.0 * self.held3 / self.held if self.held else 0.0

    @property
    def busy_pct(self) -> float:
        return 100.0 * self.busy_ms / (1000.0 * self.seconds) if self.seconds > 0 else 0.0


def total(windows: list[Window]) -> Totals:
    t = Totals()
    for w in windows:
        t.add(w)
    return t


def rate_ok(rate: float, want: float) -> bool:
    """``rate`` reads ``want`` to two decimals (20.00)."""
    return f"{rate:.2f}" == f"{want:.2f}"


def describe(t: Totals) -> str:
    scenes = ", ".join(
        f"{scene} {secs:.0f} s" for scene, secs in sorted(t.scenes.items(), key=lambda kv: -kv[1])
    )
    per_tick = (
        ", ".join(f"{name} {t.split[k] / t.ticks:.2f}" for k, name in SPLIT) if t.ticks else "-"
    )
    backends = "/".join(sorted(t.backends))
    scales = "/".join(str(s) for s in sorted(t.scales))
    return (
        f"{t.seconds:.1f} s ({scenes}); {t.rate:.2f} ticks a second ({t.ticks} ticks); "
        f"{t.held3_pct:.2f} % held 3 vblanks ({t.held3} of {t.held}); "
        f"tick max {t.tick_max:.2f} ms, {t.over} over 55 ms; per tick {per_tick} ms; "
        f"CPU busy {t.busy_pct:.1f} %; backend {backends}, scale {scales}"
    )


def judge(
    session: Session,
    displays: list[str],
    min_seconds: float = 300.0,
    held3_pct: float = 99.0,
    need_fast_forward: bool = True,
) -> tuple[bool, list[str]]:
    """The verdict and the report's lines."""
    out: list[str] = []
    ok = True
    if session.start:
        out.append(f"perfsession: settings: {session.start}")
    if session.ended is None:
        out.append(
            "perfsession: note: the log has no 'session ended' line (the run did not end "
            "normally, or the log was cut); judging the lines it has"
        )
    for display in displays:
        mine = [w for w in session.windows if w.display == display and w.on_map and w.ff == 1]
        t = total(mine)
        problems = []
        if t.seconds < min_seconds:
            problems.append(f"{t.seconds:.1f} s on a map, not {min_seconds:.0f}")
        if not rate_ok(t.rate, TICK_RATE):
            problems.append(f"{t.rate:.2f} ticks a second, not {TICK_RATE:.2f}")
        if t.held3_pct < held3_pct:
            problems.append(f"{t.held3_pct:.2f} % held 3 vblanks, under {held3_pct:g} %")
        verdict = "PASS" if not problems else "FAIL (" + "; ".join(problems) + ")"
        ok = ok and not problems
        out.append(
            f"perfsession: {display} on a map: {verdict}: {describe(t) if mine else 'no lines'}"
        )
    # Fast-forward: logged, and its rate against the game's at that rate (reported, not judged).
    ff = [w for w in session.windows if w.ff > 1 and w.display in displays and w.on_map]
    if ff:
        for display in displays:
            for rate in sorted({w.ff for w in ff if w.display == display}):
                t = total([w for w in ff if w.display == display and w.ff == rate])
                want = TICK_RATE * rate
                out.append(
                    f"perfsession: {display} fast-forward x{rate}: target {want:.2f} ticks a "
                    f"second, {100.0 * t.rate / want:.1f} % of it; {describe(t)}"
                )
    else:
        out.append(
            "perfsession: fast-forward: not logged"
            + (" (FAIL: the criterion asks for it)" if need_fast_forward else "")
        )
        ok = ok and not need_fast_forward
    # The busiest second on a map (without fast-forward): the most CPU work for its length.
    busy = [
        w
        for w in session.windows
        if w.on_map and w.ff == 1 and w.s >= 0.5 and w.display in displays
    ]
    if busy:
        b = max(busy, key=lambda w: w.busy_ms / w.s)
        out.append(
            f"perfsession: busiest second: {b.display} {b.scene} at {b.t:.1f} s: CPU busy "
            f"{100.0 * b.busy_ms / (1000.0 * b.s):.1f} %, {b.ticks / b.s:.2f} ticks a second, "
            f"{b.held3} of {b.held} held 3 vblanks, tick max {b.tick_max:.2f} ms; "
            + ", ".join(f"{name} {getattr(b, k) / max(b.ticks, 1):.2f}" for k, name in SPLIT)
            + " ms per tick"
        )
    else:
        out.append("perfsession: busiest second: none on a map")
    out.append(f"perfsession: verdict: {'PASS' if ok else 'FAIL'}")
    return ok, out


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.perfsession", description=__doc__.split("\n")[0]
    )
    sub = parser.add_subparsers(dest="command", required=True)
    j = sub.add_parser("judge", help="add up a log's perf: lines and judge the pace")
    j.add_argument("log", type=Path)
    j.add_argument(
        "--displays",
        default="handheld,docked",
        help="the screens that must each pass (default handheld,docked; desktop for a desktop run)",
    )
    j.add_argument("--min-seconds", type=float, default=300.0)
    j.add_argument("--held3", type=float, default=99.0, help="the share held 3 vblanks, percent")
    j.add_argument(
        "--no-fast-forward", action="store_true", help="do not require fast-forward in the log"
    )
    args = parser.parse_args(argv)
    text = args.log.read_text(encoding="utf-8", errors="replace")
    try:
        session = parse_log(text)
    except ValueError as e:
        print(f"perfsession: {e}", file=sys.stderr)
        return 2
    if not session.windows:
        print(
            f"perfsession: {args.log}: no perf: lines (was the game started with --perf-session?)",
            file=sys.stderr,
        )
        return 2
    ok, lines = judge(
        session,
        [d for d in args.displays.split(",") if d],
        min_seconds=args.min_seconds,
        held3_pct=args.held3,
        need_fast_forward=not args.no_fast_forward,
    )
    for line in lines:
        print(line)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
