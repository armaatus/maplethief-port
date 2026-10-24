"""Timing and threading of the armv7 slice (#21): names, and a coverage check of the doc.

The analysis is written up in ``docs/re/timing-threading.md``. This module keeps the parts of
it that a machine can check:

* :data:`TIMING_IMPORTS` and :data:`TIMING_SELECTORS`: the imports and ObjC messages that
  read a clock, arm a timer, schedule work on a run loop or a GCD queue, make a block, take a
  lock, or ask which thread is running.
* :func:`collect`: every code site of those (from #11's ``work/re/imports.json``), each with
  its #10 tag. :func:`check_doc` applies the rule of :mod:`mtport.sitecheck` (shared with
  #23's ``mtport.network``): a site is named by its address in the doc, or it sits in a
  ``lib:chartboost`` function whose address the doc names.
* :data:`ABSENT_SELECTORS`: thread and timer messages the doc says are never sent. The
  real-asset test checks that the binary has no selector reference for any of them.
* :data:`ATOMIC_MNEMONICS` and :func:`atomics`: exclusive loads/stores, barriers and swaps,
  found with :func:`mtport.armscan.find_instructions`. Each one must also be covered.
* :data:`NAMES`: Ghidra names for the clock, game-speed and random-number functions, written
  to ``work/re/names/timing.json``.

Command line::

    python3 -m mtport.timing names [OUT]      # work/re/names/timing.json
    python3 -m mtport.timing sites [--repo D] # every timing/threading site, as Markdown rows
    python3 -m mtport.timing check [--repo D] # exit 1 if the doc misses a site
"""

from __future__ import annotations

import argparse
import json
import sys
from collections.abc import Iterable, Sequence
from pathlib import Path

from mtport import sitecheck

NAMES_OUT = "work/re/names/timing.json"
IMPORTS_JSON = sitecheck.IMPORTS_JSON
TAGS_JSON = sitecheck.TAGS_JSON
OBJC_JSON = "work/re/objc.json"
SLICE = "work/slices/MapleThief.armv7"
DOC = "docs/re/timing-threading.md"

Site = sitecheck.Site

TIMING_IMPORTS: tuple[str, ...] = (
    # clocks
    "_gettimeofday",
    "_CFAbsoluteTimeGetCurrent",
    "_OBJC_CLASS_$_NSDate",
    # run-loop timers and sources
    "_CFRunLoopTimerCreate",
    "_CFRunLoopTimerSetNextFireDate",
    "_CFRunLoopAddTimer",
    "_CFRunLoopGetMain",
    "_CFRunLoopGetCurrent",
    "_kCFRunLoopCommonModes",
    "_kCFRunLoopDefaultMode",
    "_CFRunLoopAddSource",
    "_CFSocketCreateRunLoopSource",
    "_SCNetworkReachabilityScheduleWithRunLoop",
    "_SCNetworkReachabilityUnscheduleFromRunLoop",
    "_AudioSessionInitialize",  # registers the interruption listener (main run loop)
    # Foundation threads and timers
    "_OBJC_CLASS_$_NSThread",
    "_OBJC_CLASS_$_NSTimer",
    # GCD
    "__dispatch_main_q",
    "_dispatch_async",
    "_dispatch_sync",
    "_dispatch_after",
    "_dispatch_time",
    "_dispatch_once",
    "_dispatch_get_global_queue",
    "_dispatch_group_create",
    "_dispatch_group_async",
    "_dispatch_group_wait",
    "_dispatch_release",
    # blocks (completion handlers and GCD work items)
    "__NSConcreteStackBlock",
    "__NSConcreteGlobalBlock",
    # locks
    "_objc_sync_enter",
    "_objc_sync_exit",
)

TIMING_SELECTORS: tuple[str, ...] = (
    "isMainThread",
    "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
    "invalidate",
    "isValid",
    "performSelector:",
    "performSelector:withObject:",
    "performSelector:withObject:withObject:",
    "callDelegateOnMainThread:withArg:error:",
    "date",
    "timeIntervalSince1970",
    "timeIntervalSinceNow",
    "beginAnimations:context:",
    # framework calls that answer later through a completion block or an observer
    "authenticateWithCompletionHandler:",
    "loadAchievementsWithCompletionHandler:",
    "loadPlayersForIdentifiers:withCompletionHandler:",
    "loadScoresWithCompletionHandler:",
    "reportAchievementWithCompletionHandler:",
    "reportScoreWithCompletionHandler:",
    "resetAchievementsWithCompletionHandler:",
    "addTransactionObserver:",
)

# Never sent: the binary has no selector reference for any of these.
ABSENT_SELECTORS: tuple[str, ...] = (
    "detachNewThreadSelector:toTarget:withObject:",
    "initWithTarget:selector:object:",
    "currentThread",
    "mainThread",
    "performSelectorOnMainThread:withObject:waitUntilDone:",
    "performSelectorInBackground:withObject:",
    "performSelector:withObject:afterDelay:",
    "cancelPreviousPerformRequestsWithTarget:",
    "sleepForTimeInterval:",
    "sleepUntilDate:",
    "timerWithTimeInterval:target:selector:userInfo:repeats:",
    "addTimer:forMode:",
    "currentRunLoop",
    "mainRunLoop",
    "runUntilDate:",
    "runMode:beforeDate:",
    "fire",
    "displayLinkWithTarget:selector:",
    "addOperation:",
    "addOperationWithBlock:",
    "lock",
    "unlock",
)

# Exclusive access, barriers and swaps (Capstone mnemonics without condition suffixes).
ATOMIC_MNEMONICS: tuple[str, ...] = (
    "ldrex",
    "ldrexb",
    "ldrexh",
    "ldrexd",
    "strex",
    "strexb",
    "strexh",
    "strexd",
    "clrex",
    "dmb",
    "dsb",
    "isb",
    "swp",
    "swpb",
)

# -- Ghidra names (work/re/names/timing.json) -----------------------------------------------

# (address, kind, name, comment). Function pointers carry the Thumb bit.
NAMES: tuple[tuple[str, str, str, str], ...] = (
    ("0x37C41", "function", "CDataMgr::ApplyGameSpeed", "config[1] -> {65,50,40} ms interval"),
    ("0x386E1", "function", "ResetSubTimers01", "clears 0x11A990/0x11A991, sub-timers 0, 1"),
    ("0x24EA1", "function", "CMUtil::CMUtil", "seeds the RNG from the clock once; CRC32 table"),
    ("0x24DE9", "function", "Rand_SeedFromTime", "g_randSeed = (u32)GetTimeMs()"),
    ("0x225E5", "function", "Rand_Next", "MSVC rand(): s*214013+2531011, (s>>16)&0x7FFF"),
    ("0x22641", "function", "Rand_Range", "(lo, hi): lo + Rand_Next() % (hi-lo+1)"),
    ("0x22685", "function", "Rand_Chance", "(n, d): Rand_Next() % d + 1 <= n"),
    ("0x226B9", "function", "Rand_Percent", "(p): Rand_Next() % 100 + 1 <= p"),
    ("0xA17AD", "function", "TouchWidget_RepeatDue", "(w, ms): 1 if ms < elapsed <= 5*ms"),
    ("0xA18B1", "function", "TouchWidget_StartRepeat", "+0xB0 = 1, +0xA8 = GetTimeMs()"),
    ("0xA18E5", "function", "TouchWidget_HandleTouch", "(w, type, x, y); 300 ms double tap"),
    ("0x11A510", "data", "g_randSeed", "Rand_Next state; 0 until CMUtil::CMUtil seeds it"),
    ("0x108C58", "data", "g_gameSpeedIntervals", "i32 {65, 50, 40, -1}: ms per tick"),
)


def names_json() -> str:
    doc = {
        "source": "timing",
        "names": [{"address": a, "name": n, "kind": k, "comment": c} for a, k, n, c in NAMES],
    }
    return json.dumps(doc, indent=1, ensure_ascii=False) + "\n"


# -- sites ----------------------------------------------------------------------------------


def collect(imports_doc: dict, tags_doc: dict) -> list[Site]:
    """Every site of :data:`TIMING_IMPORTS` and :data:`TIMING_SELECTORS`, sorted."""
    return sitecheck.collect(imports_doc, tags_doc, TIMING_IMPORTS, TIMING_SELECTORS)


def atomic_sites(
    insns: Iterable, tags_doc: dict, names: dict[int, str] | None = None
) -> list[Site]:
    """:class:`Site` records for instructions from :func:`mtport.armscan.find_instructions`."""
    tags = {int(f["address"], 16): f["tag"] for f in tags_doc["functions"]}
    names = names or {}
    return [
        Site(
            i.mnemonic,
            i.site,
            i.function,
            names.get(i.function, ""),
            tags.get(i.function, "unknown"),
        )
        for i in insns
    ]


def atomics(repo: Path) -> list[Site]:
    """Every :data:`ATOMIC_MNEMONICS` instruction of the armv7 slice (needs Capstone)."""
    from mtport import armscan, macho

    image = macho.load(repo / SLICE)
    imports_doc = sitecheck.load_json(repo, IMPORTS_JSON)
    names = {int(f["function"], 16): f["name"] for f in imports_doc.get("functions", [])}
    found = armscan.find_instructions(image, ATOMIC_MNEMONICS)
    return atomic_sites(found, sitecheck.load_json(repo, TAGS_JSON), names)


check_doc = sitecheck.check_doc
site_rows = sitecheck.site_rows


def absent_selectors(objc_doc: dict) -> list[str]:
    """The :data:`ABSENT_SELECTORS` that do have a selector reference (should be empty)."""
    present = {r["selector"] for r in objc_doc["selrefs"]}
    return [s for s in ABSENT_SELECTORS if s in present]


# -- command line ---------------------------------------------------------------------------


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.timing", description=__doc__.splitlines()[0]
    )
    sub = parser.add_subparsers(dest="cmd", required=True)
    names = sub.add_parser("names", help=f"write the Ghidra names file (default {NAMES_OUT})")
    names.add_argument("out", nargs="?", default=NAMES_OUT)
    default_repo = Path(__file__).resolve().parents[2]
    for cmd, text in (("sites", "print every timing site"), ("check", f"check {DOC}")):
        p = sub.add_parser(cmd, help=text)
        p.add_argument("--repo", type=Path, default=default_repo)
        p.add_argument("--no-atomics", action="store_true", help="skip the Capstone sweep")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "names":
            out = Path(args.out)
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(names_json(), encoding="utf-8")
            print(f"wrote {out} ({len(NAMES)} names)")
            return 0
        sites = sitecheck.load_sites(args.repo, TIMING_IMPORTS, TIMING_SELECTORS)
        if not args.no_atomics:
            sites += atomics(args.repo)
        if args.cmd == "sites":
            print("\n".join(site_rows(sites)))
            return 0
        missing = check_doc((args.repo / DOC).read_text(encoding="utf-8"), sites)
        game = sum(1 for s in sites if not s.chartboost)
        print(f"{len(sites)} timing sites ({game} outside Chartboost), {len(missing)} missing")
        for row in site_rows(missing):
            print("  " + row)
        return 1 if missing else 0
    except (OSError, ValueError, KeyError) as e:
        print(f"error: {e}", file=sys.stderr)
        if isinstance(e, FileNotFoundError):
            hint = "run scripts/bootstrap and scripts/re-analyze first"
            print(hint, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
