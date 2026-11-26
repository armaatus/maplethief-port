"""Observed functions (#72, ADR 0053): guest functions whose entry the generated code reports
to the runtime.

The whole-image lift (:func:`recomp.prototype.lift`) emits ``MT_FN_OBSERVE(0x...u);`` at the
start of each function listed here, after ``MT_FN_ENTER()`` and before its first instruction,
so the runtime's ``mt_observe_entry(cpu, fn)`` (``maplethief/recomp/calls.h``, implemented in
``runtime/src/events/events.cpp``) sees the arguments in ``r0``-``r3`` as the caller passed
them. The observer only reads; the guest's behaviour is the same with or without it. Snippets
and the differential test lift without observers (:class:`recomp.lift.core.LiftOptions`).

The runtime switches on the same function pointers (``MT_EVENTS_FN_*`` in
``runtime/include/maplethief/events.h``); ``tests/test_recomp_observe.py`` checks that the two
lists agree.
"""

from __future__ import annotations

# function pointer -> (name, what the runtime makes of the entry)
OBSERVED: dict[int, tuple[str, str]] = {
    0x65089: ("CHero::LevelUp", "(hero): a level-up event if the level +0x80 is below 99"),
    0x65B55: (
        "CHero::TakeDamage",
        "(hero, damage, from_left): a damage event if damage > 0 and the hero is not dead",
    ),
    0x3EAA1: (
        "Popup_Open",
        "(mgr, id, text, kind): a death-popup event for id 0x68 (the cash shop's revive prices)",
    ),
    0x6140D: ("CHero revive", "(hero): a revived event (HP and MP to their maxima)"),
    0x39989: (
        "CDataMgr::LoadSaveData",
        "(mgr, slot): a new-character event when the slot has no cached buffer, else a load",
    ),
    # Run events (#403, ADR 0156): what a speedrun splits on.
    0x996ED: (
        "ScriptNative_clearAll",
        "(vm): a chapter-end event with the chapter that ends (vm +0x04 -> +0x58 -> +0x04)",
    ),
    0x655AD: (
        "CHero::SetClassUp",
        "(hero, branch, tier): a job-advance event; its one caller is the native setClassUp",
    ),
    0x73349: ("CMob::Die", "(mob, hero): a boss-killed event for mob ids (+0xA0) 64 and 66-69"),
    0x81369: (
        "CMapleWorld::changeMap",
        "(world, area, map, portal): a map-change event unless a warp is pending (+0xB4)",
    ),
}

OBSERVED_PTRS: frozenset[int] = frozenset(OBSERVED)

__all__ = ["OBSERVED", "OBSERVED_PTRS"]
