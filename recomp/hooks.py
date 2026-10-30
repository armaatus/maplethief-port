"""Hooked functions (#143, ADR 0069): guest functions the host can replace or wrap.

The whole-image lift (:func:`recomp.prototype.lift`) lifts each function listed here in two
parts: its unchanged body as ``static void f_XXXXXXXX_original(mt_cpu *)`` and the function
itself as a wrapper, ``void f_XXXXXXXX(mt_cpu *) { MT_FN_HOOK(0x...u, f_XXXXXXXX_original); }``.
``MT_FN_HOOK`` (``maplethief/recomp/calls.h``) asks the runtime's ``mt_hook_entry(cpu, fn,
original)`` (``runtime/src/hooks/hooks.cpp``, ``maplethief/hooks.h``) to run the host's handler
for ``fn``. When no handler is registered or enabled, or the handler passes, the original runs
(a tail call): the guest behaves exactly as without the hook. Direct calls, tail calls and the
dispatch table all name ``f_XXXXXXXX``, so every way into the function goes through the hook.
Snippets and the differential test lift without hooks (:class:`recomp.lift.core.LiftOptions`).

``returns``: the function never returns in the original (it ends in ``exit``), and a handler
may make it return. Discovery (:func:`recomp.discover.discover`) still records it as noreturn,
but lets its callers' flow continue after the call, so the code the original never reached
there is lifted too.

The runtime names the same function pointers (``MT_HOOK_FN_*`` in
``runtime/include/maplethief/hooks.h``); ``tests/test_recomp_hooks.py`` checks that the two
lists agree.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Hooked:
    name: str
    why: str  # what the port's handler does
    returns: bool = False  # a handler may return from a function that never returns


# function pointer -> what is hooked and why
HOOKED: dict[int, Hooked] = {
    0x22955: Hooked(
        "OpenPortalURLAndExit",
        "(owner, url): the NGB portal items of the main menu; the original quits with exit(1), "
        "the hook returns to the menu (Enhanced)",
        returns=True,
    ),
    0xAAAB9: Hooked(
        "MainMenuView_clickPlay",
        "-[MainMenuView clickPlay:] (self, _cmd, sender): Play before the engine's main menu "
        "opens a character select whose drawRect: reads the still empty robinImages (#122); "
        "the hook drops that click (Enhanced, #155)",
    ),
}

HOOKED_PTRS: frozenset[int] = frozenset(HOOKED)
# Plain addresses (Thumb bit clear) of the hooked functions a handler may make return.
MAY_RETURN: frozenset[int] = frozenset(p & ~1 for p, h in HOOKED.items() if h.returns)


def original_name(name: str) -> str:
    """The C name of a hooked function's unchanged body."""
    return f"{name}_original"


__all__ = ["HOOKED", "HOOKED_PTRS", "MAY_RETURN", "Hooked", "original_name"]
