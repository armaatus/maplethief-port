"""Hooked functions (#143, ADR 0069): the lifter splits each function of recomp/hooks.py into
its unchanged body and a wrapper that asks the runtime's hook (``MT_FN_HOOK``), for no other
function and only when asked, and the runtime's list (maplethief/hooks.h, runtime/src/hooks/)
is the same. Synthetic Thumb-2; no game code."""

from __future__ import annotations

import re

from recomp.discover import discover_function
from recomp.hooks import HOOKED, HOOKED_PTRS, MAY_RETURN
from recomp.lift.core import LiftOptions, lift_function
from recomp.prototype import IMAGE_OPTIONS
from recomp.snippet import _SnippetProgram
from recomp_harness import SNIPPET_BASE
from repo_paths import REPO

BODY = bytes.fromhex("01207047")  # movs r0, #1 / bx lr
PTR = SNIPPET_BASE | 1
NAME = f"f_{PTR:08X}"


def _lift(opts: LiftOptions) -> str:
    fn = discover_function(BODY, SNIPPET_BASE, PTR, SNIPPET_BASE + len(BODY))
    return lift_function(fn, _SnippetProgram(BODY, SNIPPET_BASE), opts).source


def test_a_hooked_function_is_its_body_and_a_wrapper_that_asks_the_hook() -> None:
    plain = _lift(LiftOptions())
    hooked = _lift(LiftOptions(hook=frozenset({PTR})))
    body, wrapper = hooked.split("\n}\n\n", 1)
    # The body is the plain function, renamed and static; nothing else in it changed.
    assert body + "\n}\n" == plain.replace(
        f"void {NAME}(mt_cpu", f"static void {NAME}_original(mt_cpu", 1
    )
    assert wrapper.splitlines() == [
        f"void {NAME}(mt_cpu *MT_RESTRICT cpu)",
        "{",
        f"    MT_FN_HOOK(0x{PTR:08X}u, {NAME}_original);",
        "}",
    ]
    # Observed and hooked: the body reports its entry, the wrapper only asks the hook.
    both = _lift(LiftOptions(observe=frozenset({PTR}), hook=frozenset({PTR})))
    assert both.index("MT_FN_OBSERVE") < both.index(f"\nvoid {NAME}(mt_cpu")


def test_other_functions_and_the_default_options_have_no_hook() -> None:
    assert "MT_FN_HOOK" not in _lift(LiftOptions())
    assert "MT_FN_HOOK" not in _lift(LiftOptions(hook=frozenset({PTR + 0x10})))
    # Snippets and the differential test lift with the defaults; only the whole-image lift
    # hooks, and it hooks exactly the listed functions.
    assert LiftOptions().hook == frozenset()
    assert IMAGE_OPTIONS.hook == HOOKED_PTRS


def test_the_runtime_knows_the_same_functions() -> None:
    header = (REPO / "runtime" / "include" / "maplethief" / "hooks.h").read_text(encoding="utf-8")
    defines = {
        name: int(value, 16)
        for name, value in re.findall(r"#define (MT_HOOK_FN_\w+) (0x[0-9A-F]+)u", header)
    }
    assert sorted(defines.values()) == sorted(HOOKED)
    assert defines["MT_HOOK_FN_OPEN_PORTAL_URL_AND_EXIT"] == 0x22955
    assert defines["MT_HOOK_FN_MAIN_MENU_CLICK_PLAY"] == 0xAAAB9  # #122, #155
    # The registry's table of hookable functions names every one of them.
    source = (REPO / "runtime" / "src" / "hooks" / "hooks.cpp").read_text(encoding="utf-8")
    table = re.search(r"k_hookable\[\] = \{(.*?)\};", source, re.S)
    assert table is not None
    assert sorted(re.findall(r"MT_HOOK_FN_\w+", table.group(1))) == sorted(defines)
    # Function pointers of Thumb functions (bit 0 set), each with a name and a reason.
    assert all(ptr & 1 for ptr in HOOKED)
    assert all(h.name and h.why for h in HOOKED.values())
    assert MAY_RETURN == {0x22954}
