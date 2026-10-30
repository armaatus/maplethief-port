"""Observed functions (#72, ADR 0053): the lifter reports the entry of the functions of
recomp/observe.py to the runtime (``MT_FN_OBSERVE``) and of no other, only when asked, and the
runtime's list (maplethief/events.h) is the same. Synthetic Thumb-2; no game code."""

from __future__ import annotations

import re

from recomp.discover import discover_function
from recomp.lift.core import LiftOptions, lift_function
from recomp.observe import OBSERVED, OBSERVED_PTRS
from recomp.prototype import IMAGE_OPTIONS
from recomp.snippet import _SnippetProgram
from recomp_harness import SNIPPET_BASE
from repo_paths import REPO

BODY = bytes.fromhex("01207047")  # movs r0, #1 / bx lr
PTR = SNIPPET_BASE | 1


def _lift(opts: LiftOptions) -> str:
    fn = discover_function(BODY, SNIPPET_BASE, PTR, SNIPPET_BASE + len(BODY))
    return lift_function(fn, _SnippetProgram(BODY, SNIPPET_BASE), opts).source


def test_an_observed_function_reports_its_entry_before_its_first_instruction() -> None:
    lines = _lift(LiftOptions(observe=frozenset({PTR}))).splitlines()
    call = f"    MT_FN_OBSERVE(0x{PTR:08X}u);"
    assert lines.count(call) == 1
    at = lines.index(call)
    assert lines.index("    MT_FN_ENTER();") < at
    # Before anything the function does: the flag locals are declarations, not guest code.
    first = next(i for i, line in enumerate(lines) if "movs r0, #1" in line)
    assert at < first
    assert all(line.startswith("    uint32_t ") for line in lines[lines.index("{") + 2 : at])


def test_other_functions_and_the_default_options_have_no_observer() -> None:
    assert "MT_FN_OBSERVE" not in _lift(LiftOptions())
    assert "MT_FN_OBSERVE" not in _lift(LiftOptions(observe=frozenset({PTR + 0x10})))
    # Snippets and the differential test lift with the defaults; only the whole-image lift
    # observes, and it observes exactly the listed functions.
    assert LiftOptions().observe == frozenset()
    assert IMAGE_OPTIONS.observe == OBSERVED_PTRS


def test_the_runtime_switches_on_the_same_functions() -> None:
    header = (REPO / "runtime" / "include" / "maplethief" / "events.h").read_text(encoding="utf-8")
    runtime = {
        name: int(value, 16)
        for name, value in re.findall(r"#define (MT_EVENTS_FN_\w+) (0x[0-9A-F]+)u", header)
    }
    assert sorted(runtime.values()) == sorted(OBSERVED)
    assert runtime["MT_EVENTS_FN_TAKE_DAMAGE"] == 0x65B55
    assert runtime["MT_EVENTS_FN_LEVEL_UP"] == 0x65089
    # Function pointers of Thumb functions (bit 0 set), with a name each.
    assert all(ptr & 1 for ptr in OBSERVED)
    assert all(name and why for name, why in OBSERVED.values())
