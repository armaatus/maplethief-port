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
    0x8079: Hooked(
        "FillRect",
        "(surface, x, y, w, h at sp, graphics context at sp + 4): a filled rectangle in GL; "
        "widescreen menus (#169, ADR 0090) stretch the popup menu's dim over the wide view",
    ),
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
    0xD259: Hooked(
        "CMMap::LoadObj",
        "(map, area, map): reads the map's .obj into its object list; restored_content (#427) "
        "adds Monster Carnival's NPC (kind 42, a Spiegelman script) to Kerning City, Orbis and "
        "Magatia's first maps after it, in memory",
    ),
    0x23D1D: Hooked(
        "Canvas_FillRect",
        "(x, y, w, h): FillRect in the canvas's colour (and the global alpha while one is set); "
        "flashes below 100 (#186, ADR 0093) scale a scene's fade-in from a colour "
        "(CStateGame::Paint's call at 0x8A6EA)",
    ),
    0x23D95: Hooked(
        "Canvas_FillRectColor",
        "(x, y, w, h, colour at sp): Canvas_FillRect in the colour; flashes below 100 (#186, "
        "ADR 0093) scale the hero's full-screen white and black fills (CHero::Paint's six calls)",
    ),
    0x22955: Hooked(
        "OpenPortalURLAndExit",
        "(owner, url): the NGB portal items of the main menu; the original quits with exit(1), "
        "the hook returns to the menu (Enhanced)",
        returns=True,
    ),
    0x22989: Hooked(
        "Text_CharCount",
        "(owner, text): the characters of a C string for Text_Wrap, stepping 3 bytes for any "
        "byte >= 0x80 (Hangul in UTF-8); with a text pack (#189, ADR 0108) the port steps by "
        "each character's UTF-8 length",
    ),
    0x22A1D: Hooked(
        "Text_SubChars",
        "(owner, text, from, to): a new[] copy of characters [from, to) of a C string, stepped "
        "as Text_CharCount; with a text pack (#189, ADR 0108) by each character's UTF-8 length",
    ),
    0x22EC5: Hooked(
        "Text_Wrap",
        "(owner, text, width, &count): a new[] array of whole lines; widescreen scenes (#396, "
        "ADR 0090) wrap the scene speech box (the runner's command 50) at the 3:2 width",
    ),
    0x24DC9: Hooked(
        "DrawImage",
        "(image, x, y): an image at its top-left corner; widescreen (#86, ADR 0090) stretches "
        "the field's backdrop (CStateGame +0x454, a fixed 480 x 320 image) to the view's width",
    ),
    0x34A05: Hooked(
        "CChatBox::NextPage",
        "(box): the next NPC dialogue page, its lines wrapped whole (+0x18, count +0x1C); the "
        "text events (#391, ADR 0187) read the page after it for the speech",
    ),
    0x34CD1: Hooked(
        "CChatBox::FirstPage",
        "(box): the first NPC dialogue page, as NextPage; the text events (#391, ADR 0187) "
        "read it after it",
    ),
    0x35189: Hooked(
        "CChatBox::drawChatBox",
        "(box, speaker, lines, count): the scene speech box, from CStateGame::Paint; "
        "widescreen scenes (#396, ADR 0090) draw it at 480 in the wide view's centre",
    ),
    0x38F55: Hooked(
        "CDataMgr::SaveQuestData",
        "(data manager, hero): the second of the game's save pair (after SaveSaveData 0x38711); "
        "the wrapper runs it, then backs up each slot whose files changed (#148, ADR 0078)",
    ),
    0x3EAA1: Hooked(
        "Popup_Open",
        "(mgr, id, text, kind): the engine's popup; the text events (#392) read its message, "
        "kind and button labels (CMPopup at *0x11A96C) after it for the speech",
    ),
    0x46DB9: Hooked(
        "Shop_OnKey",
        "(shop, key, game): the NPC shop's keys (game mode 2); up and down move the list's "
        "cursor by its view row but the paint resets that view from the touch drag, so past "
        "the third row the cursor sticks; the platform's handler moves the cursor by item and "
        "scrolls the drawn list (MarketKeyPadView's drag) to it (docs/re/original-bugs.md)",
    ),
    0x4CAED: Hooked(
        "QuickSlots_Paint",
        "(g, mode, x, y, state): the attack key's icon and the quick slots in GL; in the field "
        "(mode 1) a custom keypad_layout (#159, ADR 0086) moves them with their keys",
    ),
    0x519F9: Hooked(
        "PopupMenu_EquipTabOnKey",
        "(data manager, key, game): the popup menu equipment grid's keys; up and down step 6 "
        "cells in a grid of 4 columns and move a key scroll that nothing draws; the platform's "
        "handler moves the cursor by the grid and scrolls the drawn grid (the widget's drag) to "
        "it (ADR 0102)",
    ),
    0x5285D: Hooked(
        "CDataMgr::keyPopupItemInven",
        "(data manager, key, game): the popup menu item grid's keys; as the equipment grid's "
        "(ADR 0102)",
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
    0x54BF9: Hooked(
        "Game_ExitToMainMenu",
        "(data manager, key, game): the popup menu option tab's keys and its popups 13 (save) "
        "and 14 (exit to the main menu); the exit never saves. autosave = on_quit (#179) saves "
        "first, through the game's own pair, when Yes on popup 14 is confirmed "
        "(docs/re/autosave.md)",
    ),
    0x56B99: Hooked(
        "PopupMenu_Paint",
        "(data manager, game): the popup menu (game mode 1), laid out for 480 points; "
        "widescreen menus (#169, ADR 0090) draw it at 480 in the wide view's centre",
    ),
    0x56E91: Hooked(
        "Shop_Paint",
        "(data manager, game): the NPC shop (game mode 2); widescreen menus (#169) draw it at "
        "480 in the wide view's centre",
    ),
    0x58AE9: Hooked(
        "Map_PaintSub2",
        "(data manager, game): a map screen view (game mode 3); widescreen menus (#169) draw it "
        "at 480 in the wide view's centre",
    ),
    0x59119: Hooked(
        "Map_PaintSub1",
        "(data manager, game): a map screen view (game mode 3); widescreen menus (#169) draw it "
        "at 480 in the wide view's centre",
    ),
    0x59BB1: Hooked(
        "Map_PaintSub0",
        "(data manager, game): a map screen view (game mode 3); widescreen menus (#169) draw it "
        "at 480 in the wide view's centre",
    ),
    0x5A465: Hooked(
        "HUD_Paint",
        "(g, state): the field HUD (HP / MP / EXP, the quick slots through QuickSlots_Paint) at "
        "fixed 480-point places; with widescreen (#168, ADR 0090) it runs with the screen width "
        "at 480 and its draws moved to the middle of the wide view",
    ),
    0x6CF01: Hooked(
        "CMapleObject_Touches",
        "(object, left, top, right, bottom at sp): whether the object's box (0x6CE5D) meets the "
        "rect; CDropItems::Update 0x5BC75 asks it for the hero and each lying drop (the call at "
        "0x5BCEE); auto_loot (#175) widens the drop's rect for that call only",
    ),
    0x78339: Hooked(
        "CNotifyTextList::Add",
        "(list, text, colour): a notify line (pick-ups, meso, EXP, the map's name); the text "
        "events (#392) read the text for the speech",
    ),
    0x81369: Hooked(
        "CMapleWorld::changeMap",
        "(this = CStateGame, area, map, portal, x at sp, y at sp + 4): a warp's target, from "
        "the changeMap native 0x99969; common/portal620 warps to (6, 1, 2), a portal 6.1 lacks, "
        "and the hook makes it (6, 1, 1) (fix_original_bugs, #388, docs/re/original-bugs.md)",
    ),
    0x83C71: Hooked(
        "CStateGame::PaintExitToMenu",
        "(this): the scene runner, called by CStateGame::Paint before the shake is drawn; with "
        "screen_shake below 100 (#158, ADR 0093) it sees the game's amplitude and a new one it "
        "sets is scaled for the rest of the frame",
    ),
    0x853E5: Hooked(
        "CStateGame::StartScene",
        "(this, scene): builds scene `scene`'s command list and sets game mode 9; a boss killed "
        "while the hero is knocked back (CHero +0x119) leaves the scene's keys dropped, so the "
        "hook clears the knock-back first (fix_original_bugs, #420, docs/re/original-bugs.md 19)",
    ),
    0x87501: Hooked(
        "CStateGame::Paint",
        "(this): the field's frame; a scene's camera shake (+0x47E on, amplitude +0x488) is "
        "drawn here, so screen_shake below 100 (#158, ADR 0093) scales the amplitude for the call",
    ),
    0x8D4C9: Hooked(
        "CStateGame::OnKeyPressed",
        "(this, key): the field's keys and its popups, among them the death popup 0x68 and its "
        '"Go to main menu" popup 0x6D, which leaves without a save; autosave = on_quit (#336) '
        "checks its guards when Yes on 0x6D is confirmed, and finds the hero dead "
        "(docs/re/autosave.md)",
    ),
    0x998D1: Hooked(
        "ScriptNative_showMarket",
        "(L): the script native showMarket (39), which opens the shop its argument names "
        "through the shop screen 0x4628D; restored_content (#426) offers shops 0, 1 and 16 "
        "beside the usual one in a list box when a merchant who sells them opens his",
    ),
    0x9BC11: Hooked(
        "Lua::execute",
        "(L, tfunc, base): the script VM's opcode loop, which returns the first result's stack "
        "index; --native-script-vm on (#386, ADR 0226) runs it as host code over the same guest "
        "memory (runtime/src/scriptvm/)",
    ),
    0x9B37D: Hooked(
        "Lua::callNative",
        "(L, index, base): runs native `index` of the script VM with the arguments above base "
        "and returns the first result's stack index; the script corpus test (#431) puts a "
        "recording stub there, so the recompiled and the host VM see the same answers",
    ),
    0x9CE2D: Hooked(
        "Lua::runCallback",
        "(L): the dialogue box's close runs the script callback regCallBack stored at L + 8 "
        "and returns its result; restored_content (#426) opens the shop chosen in its own list "
        "box there",
    ),
    0x9EE95: Hooked(
        "TFunc::fetchU8",
        "(tfunc): the script VM's next code byte; the script corpus test (#431) observes the "
        "recompiled Lua::execute's opcode fetches (the call at 0x9BC2A) and passes",
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
