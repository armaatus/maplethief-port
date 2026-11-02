"""Ghidra names for the program flow (#12, docs/re/program-flow.md).

Two kinds of names go into ``work/re/names/flow.json``:

1. :data:`FLOW_NAMES`: the functions on the path from ``start`` to a rendered frame (boot,
   the ``Clet_*`` lifecycle, ``SkyCanvas``, ``CMTimer``, the singletons, the state machine's
   constructors and transitions, the game loader and the two static constructors). Each was
   read in the decompiler; the evidence is in the comment and in docs/re/program-flow.md.
2. **Localization-key methods**: a function that is not a vtable slot (those are #9's, in
   ``rtti.json``) and looks up ``C.M.N`` keys for exactly one ``(C, M)`` is ``C::M``, the same
   rule #9 uses for slots (``CAniManager.makeAniChapter1.1`` -> ``CAniManager::makeAniChapter1``).
   Two functions with the same key get ``_2``, ``_3`` in address order.

Names that another names file already gives (objc, rtti, save, scriptvm, imports, ...) are
left to that file: the generator skips an address that is named there under the same name and
fails if a curated entry disagrees, so ``apply-names`` never sees a conflict.

Command line::

    python3 -m mtport.flow names [--repo DIR] [--slice PATH]   # work/re/names/flow.json
"""

from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path

from mtport import ghidra_common as gc
from mtport import macho, names, rtti

SOURCE = "flow"
NAMES_OUT = "work/re/names/flow.json"
NAMES_DIR = "work/re/names"

# (function pointer, name, comment). Function pointers carry the Thumb bit; 0x27E0 is ARM.
FLOW_NAMES: tuple[tuple[int, str, str], ...] = (
    # -- boot --------------------------------------------------------------------------
    (0x27E0, "start", "crt1: argc/argv/envp from the stack, main(), exit()"),
    (0x280D, "main", "NSAutoreleasePool; UIApplicationMain(argc, argv, nil, nil); release"),
    (0x65E1, "Device_SetModel", "0x11A420 = 0 iPhone, 2 iPhone Simulator, 1 other"),
    (0x65F5, "Device_GetModel", "returns 0x11A420"),
    (0x660D, "Device_SetKorean", "0x11A424 = 1 when the first preferred language is ko"),
    (0x6621, "Device_IsKorean", "returns 0x11A424"),
    (0x9909, "Screen_Init", "(480, 320): size globals, surface 0x11AA88, 11 touch widgets"),
    (0xA2529, "TouchWidget_Init", "(widget, kind, rect, ...): one 0xBC-byte touch widget"),
    (0x8B85, "TouchWidgets_Dispatch", "TouchKeyPadView touches -> every active touch widget"),
    (0x5EF5, "Screen_SetBackingSize", "(w, h) of the GL renderbuffer, from createGLes"),
    (0x5EDD, "Logo_Free", "frees the logo image 0x11A414 (drawMapleLogo__)"),
    (0x61F1, "Screen_GetSurface", "returns the 0x1C-byte surface 0x11AA88"),
    (0x62ED, "Surface_GetWidth", "surface+0"),
    (0x6301, "Surface_GetHeight", "surface+4"),
    (0x6D65, "FlushLCD", "[[EAGLView_Instance Instance].objView flushLCD]: present, rebind"),
    # -- Clet lifecycle ------------------------------------------------------------------
    (0x5C2D, "Clet_Start", 'prints "startClet"; SkyCanvas, singletons, CStateTitle, Enter'),
    (0x5B8D, "Clet_Pause", "applicationWillResignActive: CMTimer stop, sound off, Pause"),
    (0x5B01, "Clet_Resume", "applicationDidBecomeActive: CMTimer start, sound on"),
    (0x5A75, "Clet_HandleEvent", "(type, p1, p2): 7 = pause, else SkyCanvas::PostKeyEvent"),
    (0x5FC1, "NullStub_5FC0", "returns 0; Clet_Start calls it with (0, 2, 0, 0)"),
    (0x653D, "NullStub_653C", "returns 0; Clet_Start calls it with (3, 1)"),
    (0x5FE1, "NullStub_5FE0", "returns 0; first call of Clet_Resume"),
    (0x5FF5, "NullStub_5FF4", "returns 0; first call of Clet_Pause"),
    (0x6009, "NullStub_6008", "empty; last call of Clet_HandleEvent"),
    # -- SkyCanvas: the frame -------------------------------------------------------------
    (0x26529, "SkyCanvas::SkyCanvas", "0xFC bytes; 0x11A950 = this; SkyCanvas.SkyCanvas.1"),
    (0x2567D, "SkyCanvas::GetInstance", "returns 0x11A950"),
    (0x2590D, "SkyCanvas::Tick", "keypad -> OnKeyPressed/OnKeyReleased, Update, Paint, flush"),
    (0x25695, "SkyCanvas::PaintState", "current state's Paint when a paint is pending (+0xB7)"),
    (0x25889, "SkyCanvas::RepaintRect", "PaintState, then FlushLCD of a rectangle"),
    (0x258C9, "SkyCanvas::Repaint", "PaintState, then FlushLCD of the whole surface"),
    (0x25801, "SkyCanvas::PostKeyEvent", "(type, code): 2 press, 3 release, 4 -> +0xEC"),
    (0x2578D, "SkyCanvas::KeyPress", "pending press +0xD8 (0x10004 -> -5, 0x10005 -> -16)"),
    (0x25735, "SkyCanvas::KeyRelease", "pending release +0xE8"),
    (0x25721, "SkyCanvas::KeyEvent4", "stores the code at +0xEC (event type 4)"),
    # -- CMTimer: the clock -----------------------------------------------------------------
    (0x22285, "CMTimer::GetInstance", "new(4) + CMTimer::CMTimer, 0x11A470"),
    (0x22165, "CMTimer::CMTimer", "zeroes the static timer fields, starts the run-loop timer"),
    (0x220B1, "CMTimer::StartRunLoopTimer", "holder 0x11A4F0 -> OnRunLoopTimer, armed 1 ms"),
    (0x21F51, "CMTimer::OnRunLoopTimer", "sub-timers; Tick once a frame interval passed; +1 ms"),
    (0x21DA1, "CMTimer::SetFrameInterval", "(i64 ms) -> 0x11A478"),
    (0x21DC9, "CMTimer::Start", "0x11A480 = 1"),
    (0x21DE9, "CMTimer::Stop", "0x11A480 = 0"),
    (0x21E35, "CMTimer::IsRunning", "returns 0x11A480"),
    (0x21E09, "CMTimer::MarkPause", "0x11A4A0 = 1, pause time -> 0x11A498"),
    (0x21E81, "CMTimer::ResetSubTimer", "(i): sub-timer i (0-3) on, elapsed 0"),
    (0x21EE5, "CMTimer::SubTimerElapsed", "(i): elapsed ms of sub-timer i"),
    (0x8E29, "RunLoopTimer_Arm", "(holder, ms): CFRunLoopTimerCreate (main loop) or re-arm"),
    (0x6065, "RunLoopTimer_Init", "(holder, callback)"),
    (0x5F99, "RunLoopTimer_Callout", "CFRunLoopTimer callout: holder->callback(holder, arg)"),
    (0xA0D5, "GetTimeMs", "gettimeofday() in ms (i64)"),
    # -- singletons made by Clet_Start ------------------------------------------------------
    (0x1B68D, "CMResMgr::GetInstance", "new(0x6FC) + constructor, 0x11A448"),
    (0x1A65D, "CMResMgr::CMResMgr", "stores the CMResMgr vtable"),
    (0x1E099, "CMSoundPlayer::GetInstance", "constructor 0x1D9B1"),
    (0x1D9B1, "CMSoundPlayer::CMSoundPlayer", "stores the CMSoundPlayer vtable"),
    (0x797D9, "CPopup::GetInstance", "constructor 0x7991D"),
    (0x7991D, "CPopup::CPopup", "stores the CPopup vtable"),
    (0x5BAC1, "CDataMgr::GetInstance", "constructor 0x5AE39"),
    (0x5AE39, "CDataMgr::CDataMgr", "stores the CDataMgr vtable"),
    (0xB85D, "Singleton_11A444_GetInstance", "new(1), no vtable: class unknown"),
    (0x20AAD, "CMSprite_InitList", "0x11A468 = PtrArray_New100() (the sprite list)"),
    (0x2149D, "CMSpriteImage_InitList", "0x11A46C = PtrArray_New100() (the image list)"),
    (0x25661, "PtrArray_New100", "PtrArray_New(100)"),
    (0x255D5, "PtrArray_New", "(capacity): {capacity, 0, new u32[capacity] zeroed}"),
    # -- the state machine ------------------------------------------------------------------
    (0xA995, "IStateAdaptor::IStateAdaptor", "IState + UtilAdaptor bases, parent +8 = 0"),
    (0xB14D, "IState::IState", "stores the IState vtable"),
    (0x98865, "CStateTitle::CStateTitle", "0x2C bytes; device id, language, model -> CDataMgr"),
    (0x97BBD, "CStateMainMenu::CStateMainMenu", "stores the CStateMainMenu vtables"),
    (0x940D9, "CStateGame::CStateGame", "0x4D8 bytes"),
    (0xAF65, "IStateAdaptor::ChangeState", "(this, target): ChangeStateWithParam(.., 0)"),
    (0xADED, "IStateAdaptor::ChangeStateWithParam", "Exit, pop parents, SetCurrentState, Enter"),
    (0x980B5, "CStateTitle::LoadSettings", "config + cupon; speed -> CMTimer::SetFrameInterval"),
    (0x97EDD, "CStateTitle::GoToMainMenu", "from Paint: new CStateMainMenu, ChangeState"),
    (0x54BF9, "Game_ExitToMainMenu", "saves, Chartboost interstitial, showMainMenu:, menu"),
    (0x83C71, "CStateGame::PaintExitToMenu", "from Paint: Chartboost interstitial, menu"),
    # -- loading the game -------------------------------------------------------------------
    (0x6B705, "CLoaderThread::CLoaderThread", "(state, mode): 0x11A9B8, 0x11A9B4; no thread"),
    (0x6AEF9, "CLoaderThread::Run", "-> CLoaderThread::Load"),
    (0x6AEB5, "CLoaderThread::Load", "mode 1: CStateGame::LoadMap, else LoadGame"),
    (0x86C1D, "CStateGame::LoadGame", "CMobs +0x34, CMapleWorld +0x28, CHero +0x30, ..."),
    (0x868D5, "CStateGame::LoadMap", "map load; runs the NPC script pass 0x839BD"),
    (0x839BD, "CStateGame::RunNpcStateScripts", "slot 0 (chkQuest) of each NPC script"),
    (0x63191, "CHero::InteractWithObject", "portal / NPC: runs the object's script (script-vm.md)"),
    (0x55C85, "CDataMgr::Update", "(mgr, state): called by CStateGame::Update every frame"),
    (0x6AF09, "Loading_Progress", "called between load steps; draws, FlushLCD"),
    (0x6DEDD, "CMapleWorld::CMapleWorld", "vtable, +4 = 0, +8 = PtrArray_New100()"),
    (0x68A59, "CHero::CHero", "0x164 bytes"),
    (0x775DD, "CMobs::CMobs", "0x194 bytes"),
    # -- input (#66, docs/controls/keypad.md) ------------------------------------------------
    (0x817F1, "CStateGame::OnFieldKeyPressed", "held key -> +0x68; 400 ms double tap latches walk"),
    (0x807B9, "CStateGame::OnFieldKeyReleased", "clears +0x68 unless latched; stops walk/climb"),
    (0x8163D, "CStateGame::DoHeroKeyAction", "(state, key): from Paint every frame, held key"),
    (0x63665, "CHero::KeyUpDown", "(hero, 0 up / 1 down): from DoHeroKeyAction"),
    (0x60DD9, "CHero::KeyWalk", "(hero, 0 left / 1 right): from DoHeroKeyAction"),
    (0x66DD9, "CHero::KeyAttack", "(hero, key): -5/'5' attack, '7' '9' '0' '#' skill slots"),
    (0x61651, "CHero::KeyJump", "'2', and after the two side steps of '1'/'3'"),
    (0x53B5D, "Menu_OnKey", "mode 1 (popup menu): close, cycle 6 tabs, enter a submenu"),
    (0x46DB9, "Shop_OnKey", "mode 2 (NPC shop): list, tabs, select"),
    (0x48E39, "Map_OnKeySub0", "mode 3 (map), submode +0x1C = 0"),
    (0x48BD5, "Map_OnKeySub1", "mode 3 (map), submode +0x1C = 1"),
    (0x489ED, "Map_OnKeySub2", "mode 3 (map), submode +0x1C = 2"),
    (0x59BB1, "Map_PaintSub0", "mode 3 paint, submode 0"),
    (0x59119, "Map_PaintSub1", "mode 3 paint, submode 1; moves the MapKeyPadView buttons"),
    (0x58AE9, "Map_PaintSub2", "mode 3 paint, submode 2 (world map): town taps -> submode 1"),
    (0x3EAA1, "Popup_Open", "(mgr, id, text, kind): Shell_EnableAlertKey, id -> 0x108C64"),
    (0x3E945, "Popup_Close", "Shell_DisableAlertKey, popup id 0x108C64 = -1"),
    (0x78E91, "Popup_PlaceShellButtons", "moves the PopupKeyPadView buttons by popup kind"),
    (0x791F5, "Popup_Paint", "draws the popup; kind 0 parks the Empty button over the text"),
    (0xA0EAD, "TouchWidget_OnEvent", "event sink of the 11 GL touch widgets (popup menu)"),
    (0x5A465, "HUD_Paint", "(g, state): the field HUD: HP / MP / EXP, QuickSlots_Paint mode 1"),
    (0x4CAED, "QuickSlots_Paint", "(g, mode, x, y, state): attack icon, quick slots 1-4 (GL)"),
    # -- the screen width's readers (#86, ADR 0090, docs/re/widescreen.md) -------------------
    (0xC069, "CMMap_ClampCamera", "(map): camera +0x70/+0x74 to 0 ... map size - SkyCanvas +0x88"),
    (0xC175, "CMMap_FollowCamera", "(map, x, y, smooth): target = p - screen / 2, /3, clamp"),
    (0x24DC9, "DrawImage", "(image, x, y): CMImage at x, y; the field's backdrop +0x454 at 0, 0"),
    (0x7CCD, "Sprite_DrawClipped", "clips each quad on the CPU to the surface and the clip rect"),
    # -- the hero's hits and levels (#72, ADR 0053: rumble) ---------------------------------
    (0x65B55, "CHero::TakeDamage", "(hero, dmg, from_left): dmg > 0: Vibrate(100), HP -= dmg"),
    (0x65979, "CHero::Die", "HP 0, flags +0x0C = 0x10, +0x158 EXP penalty, sound 10"),
    (0x65F05, "CHero::CheckMobContact", "a mob's rect touches the hero: TakeDamage(its attack)"),
    (0x65DC5, "CHero::CheckAttackHit", "a mob attack's rect hits: TakeDamage, once per attack"),
    (0x65089, "CHero::LevelUp", "level +0x80 + 1 (max 99), max HP/MP, HP/MP full, Game Center"),
    (0x65919, "CHero::ExpToLevelUp", "the EXP the hero's level +0x80 needs (setExp's step)"),
    # -- static constructors (__mod_init_func) --------------------------------------------
    (0xA925, "_GLOBAL__I_TextCache", "__mod_init_func[0]"),
    (0xA93D, "TextCache_StaticInit", "(1, 0xFFFF): TextCache_Create(0x11A428), atexit"),
    (0xA479, "TextCache_Create", "50 (0xFC6A8) records of 0x418 bytes: text, texture -1"),
    (0x6639, "TextCache_StaticFini", "TextCache_Destroy(0x11A428)"),
    (0xA601, "TextCache_Destroy", "frees the records and the array"),
    (0x9031, "TextCache_DrawText", "UIKit text (MarkerFelt-Thin) -> GL texture, cached"),
    (0x9E041, "_GLOBAL__I_TaggedString", "__mod_init_func[1]"),
    (0x9E059, "TaggedString_StaticInit", "(1, 0xFFFF): constructs 0x11AA10, atexit"),
    (0x9EBD5, "TaggedString::TaggedString_default", "hash/mark 0xFFFF, type 2"),
    (0x9DBB1, "TaggedString_StaticFini", "~TaggedString(0x11AA10)"),
)


class FlowError(ValueError):
    """The curated names don't fit the image or the other names files."""


def key_names(img: rtti.Image, skip: set[int]) -> list[tuple[int, str, str]]:
    """``C::M`` for each function start not in ``skip`` (locations, Thumb bit clear) whose
    ``C.M.N`` localization keys all name one ``(C, M)``; ``_2``, ``_3`` for repeats."""
    found = []
    for fp in sorted(img.starts, key=lambda a: a & ~1):
        if fp & ~1 in skip:
            continue
        keys = Counter()
        for s in img.string_refs(fp):
            m = rtti.METHOD_KEY.fullmatch(s)
            if m:
                keys[m.group(1, 2)] += 1
        if len(keys) != 1:
            continue
        (cls, method), n = next(iter(keys.items()))
        found.append((fp, f"{cls}::{method}", f"looks up {n} {cls}.{method}.N key(s)"))
    seen: Counter = Counter()
    out = []
    for fp, name, why in found:
        seen[name] += 1
        out.append((fp, name if seen[name] == 1 else f"{name}_{seen[name]}", why))
    return out


def build(image: macho.MachO, taken: dict[int, str]) -> list[dict]:
    """The names-file entries: curated ones checked against the image and ``taken``, then the
    key-derived ones for the functions nothing names yet."""
    starts = set(image.function_starts)
    problems = []
    entries = []
    curated = set()
    for fp, name, comment in FLOW_NAMES:
        if fp not in starts:
            problems.append(f"0x{fp:X} {name}: not an LC_FUNCTION_STARTS entry (or wrong mode)")
        loc = fp & ~1
        if loc in curated:
            problems.append(f"0x{fp:X} {name}: listed twice")
        curated.add(loc)
        other = taken.get(loc)
        if other is not None and other != name:
            problems.append(f"0x{fp:X}: flow says {name!r}, another names file says {other!r}")
        if other is None:
            entries.append(
                {"address": f"0x{fp:X}", "name": name, "kind": "function", "comment": comment}
            )
    if problems:
        raise FlowError("; ".join(problems))
    for fp, name, comment in key_names(rtti.Image(image), curated | set(taken)):
        entries.append(
            {"address": f"0x{fp:X}", "name": name, "kind": "function", "comment": comment}
        )
    entries.sort(key=lambda e: names.parse_address(e["address"]) & ~1)
    doc = {"source": SOURCE, "names": entries}
    names.parse(doc, NAMES_OUT)  # the contract's own checks
    return entries


def names_json(entries: list[dict]) -> str:
    return gc.dump_json({"source": SOURCE, "names": entries})


def generate(repo: Path, slice_path: Path | None = None) -> list[dict]:
    from mtport import bootstrap

    path = slice_path or repo / bootstrap.slice_path(bootstrap.PRIMARY_ARCH)
    entries = build(macho.load(path), names.other_names(repo / NAMES_DIR, Path(NAMES_OUT).name))
    gc.write_text(repo / NAMES_OUT, names_json(entries))
    keyed = sum(e["comment"].startswith("looks up ") for e in entries)
    print(
        f"wrote {NAMES_OUT} ({len(entries)} names: {len(entries) - keyed} curated, "
        f"{keyed} from localization keys)"
    )
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python3 -m mtport.flow", description=__doc__.splitlines()[0]
    )
    parser.add_argument("command", choices=("names",))
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--slice", type=Path, default=None)
    args = parser.parse_args(argv)
    try:
        generate(args.repo, args.slice)
    except (OSError, FlowError, names.NamesError, macho.MachOError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
