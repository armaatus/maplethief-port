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
    0xC069: Hooked(
        "CMMap_ClampCamera",
        "(map): keeps the camera (+0x70, +0x74) inside the map for the screen size "
        "(SkyCanvas +0x88, +0x8C); widescreen (#86, ADR 0090) sets the view width for the map "
        "first",
    ),
    0xC175: Hooked(
        "CMMap_FollowCamera",
        "(map, x, y, smooth): centres the camera on a point, then clamps it; widescreen (#86, "
        "ADR 0090) sets the view width for the map first",
    ),
    0x22955: Hooked(
        "OpenPortalURLAndExit",
        "(owner, url): the NGB portal items of the main menu; the original quits with exit(1), "
        "the hook returns to the menu (Enhanced)",
        returns=True,
    ),
    0x24DC9: Hooked(
        "DrawImage",
        "(image, x, y): an image at its top-left corner; widescreen (#86, ADR 0090) stretches "
        "the field's backdrop (CStateGame +0x454, a fixed 480 x 320 image) to the view's width",
    ),
    0x38F55: Hooked(
        "CDataMgr::SaveQuestData",
        "(data manager, hero): the second of the game's save pair (after SaveSaveData 0x38711); "
        "the wrapper runs it, then backs up each slot whose files changed (#148, ADR 0078)",
    ),
    0x4CAED: Hooked(
        "QuickSlots_Paint",
        "(g, mode, x, y, state): the attack key's icon and the quick slots in GL; in the field "
        "(mode 1) a custom keypad_layout (#159, ADR 0086) moves them with their keys",
    ),
    0x54051: Hooked(
        "PopupMenu_SkillTabOnKey",
        "(data manager, key, owner): the popup menu's skill list keys; the platform's wrapper "
        "runs it, then scrolls the drawn list (the widget's drag) to the keys' cursor (#166)",
    ),
    0x5462D: Hooked(
        "PopupMenu_QuestTabOnKey",
        "(data manager, key, owner): the popup menu quest tab's keys; OK opens a quest's page "
        "but never stores the quest in CDataMgr +0x530, which the page's paint reads (#167). "
        "The runtime's handler stores it, in every mode (docs/re/original-bugs.md); the "
        "platform's wrapper around it then scrolls the drawn side quest list to the keys' "
        "cursor (#166)",
    ),
    0xAAAB9: Hooked(
        "MainMenuView_clickPlay",
        "-[MainMenuView clickPlay:] (self, _cmd, sender): Play before the engine's main menu "
        "opens a character select whose drawRect: reads the still empty robinImages (#122); "
        "the hook drops that click (Enhanced, #155)",
    ),
    0xB4DB5: Hooked(
        "-[TouchKeyPadView colorAtPixel:onImage:]",
        "(self, _cmd, CGPoint, image): the key pad's hit mask lookup; a custom keypad_layout "
        "(#159, ADR 0086) maps the touch back to where the key it shows lies in the mask",
    ),
}

HOOKED_PTRS: frozenset[int] = frozenset(HOOKED)
# Plain addresses (Thumb bit clear) of the hooked functions a handler may make return.
MAY_RETURN: frozenset[int] = frozenset(p & ~1 for p, h in HOOKED.items() if h.returns)


def original_name(name: str) -> str:
    """The C name of a hooked function's unchanged body."""
    return f"{name}_original"


__all__ = ["HOOKED", "HOOKED_PTRS", "MAY_RETURN", "Hooked", "original_name"]
