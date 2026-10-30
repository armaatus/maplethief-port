"""The menus before the game, flow by flow (#75): main menu, terms, help, about, the online
buttons, and character creation, selection and deletion.

Each flow drives ``MapleThief --fixed-step 1`` headless (SDL's dummy drivers) through one or
more launches that share a data directory, with clicks and swipes through the frontend's
Input (``--tap``, ``--swipe``: the mouse's path, ADR 0047), composited shots (``--shot``)
and the ``uikit`` trace. It checks three kinds of evidence:

- **the trace**: the button actions that ran (``uikit: action -[Class selector:]``), in order,
  ``openURL:`` refused, the leaderboard presented and dismissed, a paging scroll view's page;
- **the data directory**: the save files each launch leaves (``Documents/theif<n>``,
  ``quest<n>``, ``CTerms``) and their sizes (docs/formats/save.md);
- **the shots**, by hash, never stored anywhere but the output directory: a screen that was
  opened and closed again leaves the same picture as before; a page, a scroll, a new or a
  deleted slot leaves another one. The loading screen is the one picture that is nearly all
  dark (at least :data:`LOADING_DARK` of its pixels).

Every launch must also end cleanly at its time limit (no trap, no guest crash, exit 0), with
``unimplemented report: none`` and no GL misuse.

Clicks wait until the main menu has slid in (3 s): an earlier tap can raise an exception
(#122). Coordinates are screen points of the 480 x 320 landscape screen, origin top left.

    python3 -m mtport.menuflows --binary MapleThief --out DIR [--flow NAME]...

Exit status: 0 passed, 1 failed, 77 skipped (no program, or it cannot run the game: no
generated code or no ``work/ipa``), 2 bad usage.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from . import png

SKIP = 77
EXIT_NO_GAME = 4  # MapleThief cannot run the game (game.h: kExitNoGame)
FIXED_STEP_MS = "1"
REPORT_NONE = "unimplemented report: none"
# A pixel is dark when no channel reaches DARK_LEVEL; the loading screen (black, grey skulls)
# is about 99 % dark, the first map 32 % (its black bars), the menus under 5 %.
DARK_LEVEL = 48
LOADING_DARK = 0.8
THEIF_BYTES = 1124  # theif<n>: one character (docs/formats/save.md)
QUEST_BYTES = 4781  # quest<n>: its quests
CTERMS_BYTES = 2

# The buttons, as screen points (docs/re/objc-shell.md, docs/controls/keypad.md).
BANNER = (240, 215)  # MainMenuAD.png, (0, 190, 480, 50)
BANNER_CLOSE = (462, 227)  # TouchCloseButton.png at (445, 210, 35, 35)
PLAY = (106, 165)  # (46, 148, 120, h)
HELP = (106, 215)  # (46, 198, 120, h)
ABOUT = (106, 265)  # (46, 248, 120, h)
GAME_CENTER = (404, 290)  # (376, 262, 57, h)
MORE_GAMES = (452, 290)  # (436, 262, 57, h)
CLOSE = (460, 15)  # Help's and About's close button, top right
TERMS_AGREE = (247, 262)  # TermsView (108, 245, 278, 35)
ABOUT_LINK1 = (240, 252)  # clickLinkURL1: before scrolling
ABOUT_SCROLL = (240, 290, 240, 60)  # a swipe up: the About text to its end
ABOUT_LINKS_2_TO_4 = ((200, 62), (200, 128), (200, 153))  # after the scroll
HELP_SWIPE = (400, 160, 80, 160)  # a swipe left: the next help page
SLOTS = ((100, 220), (190, 220), (280, 220))  # ChooseCharacterView's three portraits
START = (420, 70)
NEW = (420, 122)
DELETE = (420, 172)
BACK = (440, 20)
ALERT_YES = (209, 203)
ALERT_NO = (268, 203)
ALERT_OK = (240, 203)
# Shots after a click that starts the game: the loading screen shows for a few turns about
# 0.02 s after the touch's up (0.1 s after its down).
LOADING_SHOTS = tuple(round(0.105 + 0.01 * i, 3) for i in range(10))

URLS = (
    "http://itunes.apple.com/us/app/maplestory-live/id479812410?l=ko&ls=1&mt=8",
    "http://global.nexonmobile.com/App/?app=MapleThief",
    "http://maplestory.nexon.net/FunStuff/MobileGames.aspx",
    "http://global.nexonmobile.com",
    "http://www.nexon.net",
    "http://maplestory.nexon.net",
)

ACTION_LINE = re.compile(r"uikit: action -\[(\w+) ([\w:]+)\]")
OPEN_URL_LINE = re.compile(r"uikit: -\[UIApplication openURL:\] (\S+): (.*)")
PAGE_LINE = re.compile(r"uikit: UIScrollView 0x[0-9A-F]+: drag ended, pages to page (\d+)")
SHOT_LINE = re.compile(r"game: shot (?P<name>\S+) at (?P<at>[0-9.]+) s")
VIRTUAL_LINE = re.compile(r"game: virtual time at the end: (?P<t>[0-9.]+) s")
STOP_LINE = re.compile(r"game: stop reason: (?P<reason>.*)")
DRAWS_LINE = re.compile(
    r"game: GL draws: \d+ drawn, \d+ skipped, (?P<misuses>\d+) misuses; "
    r"mt_gfx: .*?(?P<unsupported>\d+) unsupported, (?P<invalid>\d+) invalid"
)
ERROR_LINES = (
    re.compile(r"=== guest crash"),
    re.compile(r"game: trap kind"),
    re.compile(r"game: exit status"),
    re.compile(r"uncaught NSException"),  # Foundation's raise (an exception stops the run)
)


# --- a launch -------------------------------------------------------------------------------


@dataclass
class Launch:
    """One start of MapleThief: its clicks, swipes and shots, in virtual seconds."""

    name: str
    seconds: float
    language: str = "en"
    events: list[tuple[str, str]] = field(default_factory=list)  # (option, value)
    flags: tuple[str, ...] = ()  # settings on the command line (``--hide-online-ui on``)

    def tap(self, at: float, point: tuple[int, int]) -> Launch:
        self.events.append(("--tap", f"{at:g},{point[0]},{point[1]}"))
        return self

    def swipe(self, at: float, line: tuple[int, int, int, int]) -> Launch:
        self.events.append(("--swipe", f"{at:g}," + ",".join(str(v) for v in line)))
        return self

    def shot(self, at: float, name: str) -> Launch:
        self.events.append(("--shot", f"{at:g},{name}"))
        return self

    def loading_shots(self, tap_at: float) -> Launch:
        """Shots ``load_0``... just after a tap at ``tap_at`` that starts the game."""
        for i, dt in enumerate(LOADING_SHOTS):
            self.shot(round(tap_at + dt, 3), f"load_{i}")
        return self

    def argv(self, command: Sequence[str], out: Path) -> list[str]:
        args = [*command, "--fixed-step", FIXED_STEP_MS, "--seconds", f"{self.seconds:g}"]
        args += ["--capture", str(out), *self.flags]
        for option, value in self.events:
            args += [option, value]
        return args


@dataclass
class Result:
    """What a launch left: its log, the shots' hashes and dark ratios, the data files."""

    name: str
    returncode: int
    log: str
    shots: dict[str, str] = field(default_factory=dict)  # name -> sha256 of the RGBA pixels
    dark: dict[str, float] = field(default_factory=dict)  # name -> dark pixel ratio
    files: dict[str, int] = field(default_factory=dict)  # Documents/<name> -> bytes

    @property
    def actions(self) -> list[str]:
        return [f"-[{m[1]} {m[2]}]" for m in ACTION_LINE.finditer(self.log)]

    @property
    def opened_urls(self) -> list[tuple[str, str]]:
        return [(m[1], m[2]) for m in OPEN_URL_LINE.finditer(self.log)]

    @property
    def pages(self) -> list[int]:
        return [int(m[1]) for m in PAGE_LINE.finditer(self.log)]

    def loading_shown(self) -> bool:
        return any(ratio >= LOADING_DARK for n, ratio in self.dark.items() if n.startswith("load_"))


def _dark_ratio(image: png.Image) -> float:
    rgba = image.rgba
    n = len(rgba) // 4
    dark = sum(
        1
        for i in range(0, len(rgba), 4)
        if rgba[i] < DARK_LEVEL and rgba[i + 1] < DARK_LEVEL and rgba[i + 2] < DARK_LEVEL
    )
    return dark / n if n else 0.0


def _data_files(data: Path) -> dict[str, int]:
    docs = data / "Documents"
    if not docs.is_dir():
        return {}
    return {p.name: p.stat().st_size for p in sorted(docs.iterdir()) if p.is_file()}


def launch_checks(r: Result, seconds: float) -> list[str]:
    """The checks every launch passes: a clean end at its time limit, nothing unimplemented."""
    fails = []
    if r.returncode != 0:
        fails.append(f"MapleThief exited with status {r.returncode}")
    for line in r.log.splitlines():
        if any(p.search(line) for p in ERROR_LINES):
            fails.append(f"error in the log: {line.strip()}")
    stop = STOP_LINE.search(r.log)
    if stop is None or "the host ended the run" not in stop["reason"]:
        fails.append(f"the run did not end by the host ({stop['reason'] if stop else 'no reason'})")
    end = VIRTUAL_LINE.search(r.log)
    last_shot = max((float(m["at"]) for m in SHOT_LINE.finditer(r.log)), default=0.0)
    # A run ends at --seconds or after its last shot, whichever comes first.
    want = min(seconds, last_shot) if last_shot else seconds
    if end is None or float(end["t"]) < want - 0.05:
        fails.append(f"the run ended at {end['t'] if end else '?'} s, before {want:g} s")
    if REPORT_NONE not in r.log:
        fails.append("the unimplemented report is not 'none'")
    draws = DRAWS_LINE.search(r.log)
    if draws is None:
        fails.append("no GL draw counters in the log")
    else:
        for key in ("misuses", "unsupported", "invalid"):
            if int(draws[key]) != 0:
                fails.append(f"GL draws: {draws[key]} {key}")
    return fails


def run_launch(
    command: Sequence[str], launch: Launch, out: Path, data: Path, timeout: float
) -> Result:
    shot_dir = out / launch.name
    if shot_dir.exists():
        shutil.rmtree(shot_dir)
    shot_dir.mkdir(parents=True)
    report = shot_dir / "unimplemented-report.txt"
    env = dict(
        os.environ,
        SDL_VIDEO_DRIVER="dummy",
        SDL_AUDIO_DRIVER="dummy",
        MAPLETHIEF_DATA_DIR=str(data),
        MAPLETHIEF_REPORT=str(report),
        MAPLETHIEF_TRACE="uikit",
        MAPLETHIEF_LANGUAGE=launch.language,
    )
    argv = launch.argv(command, shot_dir)
    log_path = shot_dir / "log.txt"
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(" ".join(argv) + "\n")
        f.flush()
        try:
            p = subprocess.run(argv, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
            code = p.returncode
        except subprocess.TimeoutExpired:
            f.write(f"menuflows: timed out after {timeout:g} s\n")
            code = -1
        except OSError as e:
            f.write(f"menuflows: cannot start {argv[0]}: {e}\n")
            code = EXIT_NO_GAME
    log = log_path.read_text(encoding="utf-8", errors="replace")
    if report.is_file():
        log += "\n" + report.read_text(encoding="utf-8")
    r = Result(launch.name, code, log, files=_data_files(data))
    for m in SHOT_LINE.finditer(log):
        try:
            image = png.load(shot_dir / f"{m['name']}.png")
        except (OSError, ValueError):
            continue
        r.shots[m["name"]] = hashlib.sha256(image.rgba).hexdigest()
        r.dark[m["name"]] = _dark_ratio(image)
    return r


# --- the flows ------------------------------------------------------------------------------


class Checks:
    """Collects a flow's failures with the evidence that passed."""

    def __init__(self) -> None:
        self.fails: list[str] = []
        self.passed: list[str] = []

    def that(self, ok: bool, what: str) -> bool:
        (self.passed if ok else self.fails).append(what)
        return ok

    def actions(self, r: Result, *want: str) -> None:
        """``want`` happened in this order (other actions may come between)."""
        got = r.actions
        i = 0
        for a in got:
            if i < len(want) and a == want[i]:
                i += 1
        self.that(i == len(want), f"{r.name}: actions {', '.join(want)} (got {', '.join(got)})")

    def same(self, a: Result, sa: str, b: Result, sb: str, why: str) -> None:
        ha, hb = a.shots.get(sa), b.shots.get(sb)
        self.that(ha is not None and ha == hb, f"{a.name}/{sa} = {b.name}/{sb}: {why}")

    def differ(self, a: Result, sa: str, b: Result, sb: str, why: str) -> None:
        ha, hb = a.shots.get(sa), b.shots.get(sb)
        ok = ha is not None and hb is not None and ha != hb
        self.that(ok, f"{a.name}/{sa} != {b.name}/{sb}: {why}")

    def files(self, r: Result, present: Sequence[str], absent: Sequence[str] = ()) -> None:
        sizes = {"CTerms": CTERMS_BYTES}
        for name in present:
            want = sizes.get(name, THEIF_BYTES if name.startswith("theif") else QUEST_BYTES)
            got = r.files.get(name)
            self.that(got == want, f"{r.name}: Documents/{name} is {want} bytes (got {got})")
        for name in absent:
            self.that(name not in r.files, f"{r.name}: no Documents/{name}")

    def loading(self, r: Result) -> None:
        dark = max((v for n, v in r.dark.items() if n.startswith("load_")), default=0.0)
        self.that(
            r.loading_shown(), f"{r.name}: the loading screen shows ({dark:.0%} of a shot dark)"
        )


@dataclass(frozen=True)
class Flow:
    name: str
    what: str
    launches: Callable[[], list[Launch]]
    check: Callable[[dict[str, Result], Checks], None]


def _new_game_seed(name: str = "seed") -> Launch:
    """A fresh install's Play: slot 0 is written at once (docs/formats/save.md)."""
    return Launch(name, 5.0).tap(3.0, BANNER_CLOSE).tap(4.0, PLAY).shot(4.9, "game")


def _second_slot_seed() -> Launch:
    """The character select's New + Yes: slot 1."""
    return Launch("seed2", 5.5).tap(3.0, PLAY).tap(4.0, NEW).tap(4.5, ALERT_YES).shot(5.4, "game")


def _help_about_launches() -> list[Launch]:
    return [
        Launch("menus", 9.4)
        .tap(3.0, BANNER_CLOSE)
        .shot(3.8, "menu")
        .tap(4.0, HELP)
        .shot(4.8, "help")
        .tap(5.0, CLOSE)
        .shot(5.8, "after_help")
        .tap(6.0, ABOUT)
        .shot(6.8, "about")
        .swipe(7.0, ABOUT_SCROLL)
        .shot(8.3, "about_scrolled")
        .tap(8.5, CLOSE)
        .shot(9.3, "after_about")
    ]


def _help_about_check(rs: dict[str, Result], c: Checks) -> None:
    r = rs["menus"]
    c.actions(
        r,
        "-[MainMenuView clickADClose:]",
        "-[MainMenuView clickHelp:]",
        "-[MainMenuView closeHelp:]",
        "-[MainMenuView clickAbout:]",
        "-[MainMenuView closeAbout:]",
    )
    c.differ(r, "help", r, "menu", "Help opens over the menu")
    c.same(r, "after_help", r, "menu", "Help closes and leaves the menu as it was")
    c.differ(r, "about", r, "menu", "About opens over the menu")
    c.differ(r, "about_scrolled", r, "about", "About's text scrolls")
    c.same(r, "after_about", r, "menu", "About closes and leaves the menu as it was")


def _terms_launches() -> list[Launch]:
    return [
        Launch("first", 8.9, "ko")
        .shot(3.0, "terms")
        .tap(3.5, TERMS_AGREE)
        .shot(4.3, "title")
        .tap(4.5, BANNER_CLOSE)
        .shot(5.3, "menu")
        .tap(5.5, HELP)
        .shot(6.3, "help_page1")
        .swipe(6.5, HELP_SWIPE)
        .shot(7.5, "help_page2")
        .tap(8.0, CLOSE)
        .shot(8.8, "after_help"),
        Launch("second", 4.4, "ko").shot(4.3, "title"),
    ]


def _terms_check(rs: dict[str, Result], c: Checks) -> None:
    first, second = rs["first"], rs["second"]
    c.actions(
        first,
        "-[TermsView pressAgreementButton:]",
        "-[TermsView releaseAgreementButton:]",
        "-[MainMenuView clickADClose:]",
        "-[MainMenuView clickHelp:]",
        "-[MainMenuView closeHelp:]",
    )
    c.files(first, ["CTerms"])
    c.differ(first, "terms", first, "title", "the terms screen shows, and agreeing closes it")
    c.differ(first, "help_page2", first, "help_page1", "the Korean help swipes to its 2nd page")
    c.that(first.pages == [1], f"first: the help's scroll view pages to page 1 (got {first.pages})")
    c.same(first, "after_help", first, "menu", "Help closes and leaves the menu as it was")
    c.that(not second.actions, f"second: no button pressed (got {second.actions})")
    c.same(second, "title", first, "title", "a second launch shows no terms: the plain title")


def _online_launches() -> list[Launch]:
    link2, link3, link4 = ABOUT_LINKS_2_TO_4
    return [
        Launch("online", 13.0)
        .shot(2.9, "title")
        .tap(3.0, BANNER)
        .shot(3.8, "after_banner")
        .tap(4.0, BANNER_CLOSE)
        .shot(4.8, "menu")
        .tap(5.0, GAME_CENTER)
        .shot(6.0, "after_game_center")
        .tap(6.5, MORE_GAMES)
        .shot(7.3, "after_more_games")
        .tap(7.5, ABOUT)
        .tap(8.5, ABOUT_LINK1)
        .swipe(9.0, ABOUT_SCROLL)
        .tap(10.5, link2)
        .tap(11.0, link3)
        .tap(11.5, link4)
        .tap(12.0, CLOSE)
        .shot(12.9, "after_about")
    ]


def _online_check(rs: dict[str, Result], c: Checks) -> None:
    r = rs["online"]
    c.actions(
        r,
        "-[MainMenuView clickADButton:]",
        "-[MainMenuView clickADClose:]",
        "-[MainMenuView clickGameCenter:]",
        "-[MainMenuView clickMoreGame:]",
        "-[MainMenuView clickAbout:]",
        "-[MainMenuView clickLinkURL1:]",
        "-[MainMenuView clickLinkURL2:]",
        "-[MainMenuView clickLinkURL3:]",
        "-[MainMenuView clickLinkURL4:]",
        "-[MainMenuView closeAbout:]",
    )
    opened = r.opened_urls
    c.that(
        [u for u, _ in opened] == list(URLS),
        f"online: openURL: for the banner, More Games and the 4 About links, in order "
        f"(got {[u for u, _ in opened]})",
    )
    c.that(
        all(answer.startswith("not opened") for _, answer in opened),
        "online: every openURL: is refused (not opened, the game goes on)",
    )
    c.that(
        "presentModalViewController:GKLeaderboardViewController" in r.log
        and "dismissModalViewControllerAnimated: GKLeaderboardViewController" in r.log,
        "online: Game Center's leaderboard is presented and dismissed at once",
    )
    c.same(r, "after_banner", r, "title", "the banner opens nothing: the title as it was")
    c.same(r, "after_game_center", r, "menu", "Game Center leaves the menu as it was")
    c.same(r, "after_more_games", r, "menu", "More Games leaves the menu as it was")
    c.same(r, "after_about", r, "menu", "the About links open nothing; About closes")


# The dead online buttons hidden (#154, ADR 0084): the Enhanced preset (hide_online_ui and
# local_game_center on), and hide_online_ui alone, which hides the Game Center button too.
ENHANCED = ("--preset", "enhanced")
HIDE_ONLINE_ONLY = ("--hide-online-ui", "on")
DEAD_ACTIONS = (
    "-[MainMenuView clickADButton:]",
    "-[MainMenuView clickADClose:]",
    "-[MainMenuView clickMoreGame:]",
    "-[MainMenuView clickLinkURL1:]",
    "-[MainMenuView clickLinkURL2:]",
    "-[MainMenuView clickLinkURL3:]",
    "-[MainMenuView clickLinkURL4:]",
)
AWAY = (300, 100)  # nothing of the menu: the leaderboard's closing tap


def _hidden_launch(name: str, flags: tuple[str, ...]) -> Launch:
    """The online flow's taps, with a tap that closes a leaderboard after Game Center's."""
    link2, link3, link4 = ABOUT_LINKS_2_TO_4
    return (
        Launch(name, 13.0, flags=flags)
        .shot(2.9, "title")
        .tap(3.0, BANNER)
        .shot(3.8, "after_banner")
        .tap(4.0, BANNER_CLOSE)
        .tap(4.5, MORE_GAMES)
        .shot(4.8, "menu")
        .tap(5.0, GAME_CENTER)
        .tap(5.6, AWAY)
        .shot(6.3, "after_game_center")
        .tap(6.5, ABOUT)
        .tap(8.5, ABOUT_LINK1)
        .swipe(9.0, ABOUT_SCROLL)
        .tap(10.5, link2)
        .tap(11.0, link3)
        .tap(11.5, link4)
        .tap(12.0, CLOSE)
        .shot(12.9, "after_about")
    )


def _offline_ui_launches() -> list[Launch]:
    return [
        Launch("original", 3.0).shot(2.9, "title"),
        _hidden_launch("enhanced", ENHANCED),
        _hidden_launch("no_game_center", HIDE_ONLINE_ONLY),
    ]


def _offline_ui_check(rs: dict[str, Result], c: Checks) -> None:
    original, enhanced, no_gc = rs["original"], rs["enhanced"], rs["no_game_center"]
    for r in (enhanced, no_gc):
        dead = [a for a in r.actions if a in DEAD_ACTIONS]
        c.that(not dead, f"{r.name}: no dead button can be pressed (got {dead})")
        c.that(not r.opened_urls, f"{r.name}: no openURL: at all (got {r.opened_urls})")
        c.that(
            "offline: 2 MainMenuView overrides hide" in r.log,
            f"{r.name}: the two MainMenuView overrides are installed",
        )
        c.differ(r, "title", original, "title", "the banner and More Games are gone")
        c.same(r, "after_banner", r, "title", "a tap where the banner was changes nothing")
        c.same(r, "menu", r, "title", "nor do taps where its close button and More Games were")
        c.same(r, "after_about", r, "menu", "About opens and closes, its links do nothing")
    c.differ(no_gc, "title", enhanced, "title", "without the local leaderboard Game Center goes")
    c.actions(
        enhanced,
        "-[MainMenuView clickGameCenter:]",
        "-[MainMenuView clickAbout:]",
        "-[MainMenuView closeAbout:]",
    )
    c.that(
        "the local leaderboard of com.nexonmobile.maplestory.thief.totalranking is open"
        in enhanced.log
        and "the local leaderboard is closed" in enhanced.log
        and "dismissModalViewControllerAnimated: GKLeaderboardViewController" in enhanced.log,
        "enhanced: Game Center opens the local leaderboard, a tap closes it, the game dismisses it",
    )
    c.same(
        enhanced,
        "after_game_center",
        enhanced,
        "menu",
        "the leaderboard is drawn in the window only; the game behind it saw no tap",
    )
    c.that(
        "-[MainMenuView clickGameCenter:]" not in no_gc.actions,
        "no_game_center: the Game Center button cannot be pressed",
    )


def _new_game_launches() -> list[Launch]:
    return [
        Launch("fresh", 6.0)
        .tap(3.0, BANNER_CLOSE)
        .shot(3.9, "menu")
        .tap(4.5, PLAY)
        .loading_shots(4.5)
        .shot(5.9, "game")
    ]


def _new_game_check(rs: dict[str, Result], c: Checks) -> None:
    r = rs["fresh"]
    c.actions(r, "-[MainMenuView clickPlay:]")
    c.that(
        not any("ChooseCharacterView" in a for a in r.actions),
        "fresh: no character select on a fresh install",
    )
    c.files(r, ["theif0", "quest0"], ["theif1", "theif2"])
    c.loading(r)
    c.differ(r, "game", r, "menu", "the game runs after the loading screen")


def _create_launches() -> list[Launch]:
    return [
        _new_game_seed(),
        Launch("create", 7.0)
        .tap(3.0, PLAY)
        .shot(3.9, "select")
        .tap(4.5, NEW)
        .shot(5.3, "alert")
        .tap(5.5, ALERT_YES)
        .loading_shots(5.5)
        .shot(6.9, "game"),
        Launch("relaunch", 4.0).tap(3.0, PLAY).shot(3.9, "select"),
    ]


def _create_check(rs: dict[str, Result], c: Checks) -> None:
    seed, create, relaunch = rs["seed"], rs["create"], rs["relaunch"]
    c.files(seed, ["theif0", "quest0"], ["theif1", "quest1"])
    c.actions(
        create,
        "-[MainMenuView clickPlay:]",
        "-[ChooseCharacterView clickNew:]",
        "-[ChooseCharacterView okNewAlert:]",
    )
    c.differ(create, "alert", create, "select", "New asks 'create and play new game?'")
    c.files(create, ["theif0", "quest0", "theif1", "quest1"], ["theif2", "quest2"])
    c.loading(create)
    c.differ(relaunch, "select", create, "select", "the new character fills the 2nd slot")


def _select_launches() -> list[Launch]:
    return [
        _new_game_seed(),
        Launch("select", 9.1)
        .tap(3.0, PLAY)
        .shot(3.9, "select")
        .tap(4.0, DELETE)
        .shot(4.8, "no_selection")
        .tap(5.0, ALERT_OK)
        .shot(5.8, "closed")
        .tap(6.0, SLOTS[0])
        .shot(7.3, "chosen")  # the status panel slides in after about 1 s
        .tap(7.5, START)
        .loading_shots(7.5)
        .shot(9.0, "game"),
    ]


def _select_check(rs: dict[str, Result], c: Checks) -> None:
    r = rs["select"]
    c.actions(
        r,
        "-[MainMenuView clickPlay:]",
        "-[ChooseCharacterView clickDelete:]",
        "-[ChooseCharacterView confirmAlert:]",
        "-[ChooseCharacterView chooseCharacterOne:]",
        "-[ChooseCharacterView clickStart:]",
    )
    c.differ(r, "no_selection", r, "select", "Delete with nothing chosen shows an alert")
    c.same(r, "closed", r, "select", "its OK closes it")
    c.differ(r, "chosen", r, "select", "choosing the slot shows its status")
    c.loading(r)
    c.files(r, ["theif0", "quest0"], ["theif1", "quest1"])


def _delete_launches() -> list[Launch]:
    return [
        _new_game_seed(),
        _second_slot_seed(),
        Launch("delete", 9.1)
        .tap(3.0, PLAY)
        .shot(3.9, "two")
        .tap(4.5, SLOTS[1])
        .tap(5.5, DELETE)
        .shot(6.3, "alert")
        .tap(6.5, ALERT_YES)
        .shot(7.5, "after")
        .tap(8.0, BACK)
        .shot(9.0, "menu"),
        Launch("relaunch", 4.0).shot(2.9, "title").tap(3.0, PLAY).shot(3.9, "one"),
    ]


def _delete_check(rs: dict[str, Result], c: Checks) -> None:
    seed2, delete, relaunch = rs["seed2"], rs["delete"], rs["relaunch"]
    c.files(seed2, ["theif0", "quest0", "theif1", "quest1"])
    c.actions(
        delete,
        "-[MainMenuView clickPlay:]",
        "-[ChooseCharacterView chooseCharacterTwo:]",
        "-[ChooseCharacterView clickDelete:]",
        "-[ChooseCharacterView okDeleteAlert:]",
        "-[ChooseCharacterView clickBack:]",
    )
    c.differ(delete, "alert", delete, "two", "Delete asks 'really delete the 2nd character?'")
    c.files(delete, ["theif0", "quest0"], ["theif1", "quest1"])
    c.differ(delete, "after", delete, "two", "the 2nd slot is empty after the delete")
    c.same(delete, "after", relaunch, "one", "the slots show one character, as on a relaunch")
    c.same(delete, "menu", relaunch, "title", "Back returns to the main menu")


FLOWS = (
    Flow("help_about", "Help and About open and close", _help_about_launches, _help_about_check),
    Flow("terms", "the Korean terms screen, and the help swipe", _terms_launches, _terms_check),
    Flow("online", "the online buttons do nothing harmful", _online_launches, _online_check),
    Flow(
        "offline_ui",
        "hide_online_ui hides the dead buttons; the local leaderboard",
        _offline_ui_launches,
        _offline_ui_check,
    ),
    Flow(
        "new_game", "a fresh install's Play: slot 0, loading", _new_game_launches, _new_game_check
    ),
    Flow("create", "New in an empty slot, loading", _create_launches, _create_check),
    Flow("select", "choose a saved character and Start, loading", _select_launches, _select_check),
    Flow(
        "delete", "delete a character: its files and portrait go", _delete_launches, _delete_check
    ),
)


def run_flow(command: Sequence[str], flow: Flow, out: Path, timeout: float = 600.0) -> int:
    """Runs one flow into ``out/<flow>``; prints its verdict and returns the exit status."""
    if shutil.which(command[0]) is None:
        print(f"SKIP   {flow.name}: {command[0]}: no such program")
        return SKIP
    root = out / flow.name
    if root.exists():
        shutil.rmtree(root)
    data = root / "data"
    data.mkdir(parents=True)
    results: dict[str, Result] = {}
    fails: list[str] = []
    for launch in flow.launches():
        r = run_launch(command, launch, root, data, timeout)
        if r.returncode == EXIT_NO_GAME:
            print(f"SKIP   {flow.name}: MapleThief cannot run the game (log: {root / launch.name})")
            return SKIP
        results[launch.name] = r
        fails += [f"{launch.name}: {f}" for f in launch_checks(r, launch.seconds)]
    checks = Checks()
    flow.check(results, checks)
    fails += checks.fails
    for line in checks.passed:
        print(f"ok     {flow.name}: {line}")
    for fail in fails:
        print(f"FAIL   {flow.name}: {fail}")
    if fails:
        print(f"{flow.name}: {len(fails)} checks failed (logs and shots in {root})")
        return 1
    print(
        f"PASS   {flow.name}: {flow.what}: {len(checks.passed)} checks over "
        f"{len(results)} launches (logs and shots in {root})"
    )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.menuflows",
        description="The menus before the game, flow by flow (#75).",
    )
    parser.add_argument(
        "--binary", type=Path, required=True, help="MapleThief, with generated code"
    )
    parser.add_argument("--out", type=Path, default=Path("build/test-output/menus"))
    names = [f.name for f in FLOWS]
    parser.add_argument("--flow", action="append", choices=names, help="default: every flow")
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds a launch (600)")
    args = parser.parse_args(argv)
    if not args.binary.is_file():
        print(f"SKIP   menus: {args.binary}: no such program (build MapleThief with work/gen)")
        return SKIP
    status = 0
    for flow in FLOWS:
        if args.flow and flow.name not in args.flow:
            continue
        code = run_flow([str(args.binary)], flow, args.out, args.timeout)
        if code == SKIP:
            return SKIP
        status = max(status, code)
    return status


if __name__ == "__main__":
    sys.exit(main())
