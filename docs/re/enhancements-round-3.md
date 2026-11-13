# Enhancements round 3: what the widescreen, minimap, rumble and cash-shop ADRs left open

[#376](https://github.com/armaatus/maplethief-port/issues/376) asked, for every enhancement an ADR stopped short of, where it would hook in, what it risks for the game's logic, what it costs and whether it is worth an epic. This page gives each item's facts and verdict. The four that hold up are the children of epic [#401](https://github.com/armaatus/maplethief-port/issues/401).

Addresses are armv7 VM addresses; function pointers carry the Thumb bit. The facts come from the ADRs and RE notes linked in each section and from the port's source as of 2026-11-13 (`runtime/src/hooks/widescreen.cpp`, `platform/src/minimap.h`, `platform/src/rumble.cpp`, `runtime/src/cg/text.cpp`, `platform/src/price_line.cpp`). No new decompilation and no run was needed: every hook point below is one the port already uses.

## Verdicts

| Item | Left open by | Hook point | Risk to game logic | Effort | Verdict |
|---|---|---|---|---|---|
| Menus, popups, the shop and the map screen at 3:2 | ADR 0090, line 71 | done: #169 centred them over the wide field | none | none | **No issue.** Line 71 predates #169's addendum. Widening their layouts was rejected there |
| Loading (modes 6–8), the waiting box (5) | ADR 0090, line 97 | `mt_widescreen_decide` | none | none | **No issue.** Loading draws no field beside it; mode 5 is reached by no run |
| Scripted scenes (modes 9, 10) | ADR 0090, line 97 | `mt_widescreen_decide` plus a per-scene allowlist | none (only drawing reads the width); the risk is visual | M | **Go**: #396 |
| Minimap mobs | ADR 0098, line 28 | `read_minimap`, the mob list `CStateGame +0x34` | none (read only) | S | **Go**, opt-in: #397 |
| Minimap map name | ADR 0098, line 64 | the game's `g_middleSpotNames` `0x111A50` in guest memory; a text rasteriser API | none (read only) | S–M | **Go**, opt-in, with the mobs: #397 |
| Rumble per event | ADR 0070, lines 127 and 171; ADR 0053 | `Rumble`'s three sources, a filter | none (host only) | S | **Go**: #399 |
| The cash buttons | ADR 0072, lines 59 and 155 | ADR 0084's host overrides (`setHidden:YES`) | none (presentation; the keys' bindings go too) | S | **Go**, under `hide_online_ui`: #400 |
| Larger text | ADR 0093 | none practical | high (every layout) | L+ | **No issue.** See below |
| "Clean edges" | ADR 0041 | the GLES blend or the texture upload | none | S | **No issue for now.** See below |

## Scenes at the field's width

**Now.** `mt_widescreen_decide` returns 480 in every game mode above 4 except the cash shop (`0xB`). A scene that starts on a wide map therefore narrows the view at its first frame: the wings turn black and the camera, clamped and followed for 480 points, moves; at the scene's end it widens again. That happens on the boss maps' entrances, Malady's closing scene on 3.8 (ANALYSIS.md, chapter 1's endgame) and the intro on 2.8, among others.

**Facts** ([re/widescreen.md](widescreen.md#the-screens-over-the-field-169), [re/qol.md](qol.md#cutscene-skip), [screen-shake.md](screen-shake.md)):

- 21 scene command lists, 1,169 commands. The runner `0x83C71` takes one step per `CStateGame::Paint` in mode 9; mode 10 draws the letterbox bars and then the scene `0x853E5`. The scene's id is `CStateGame +0x1E4`.
- Only drawing code reads the width (ADR 0090). The fades and the letterbox bars already span `+0x88`, the speech box is centred on `+0x90`, and the five shaking scenes move the camera in `Paint`, not by the width.
- So widening a scene changes no state. What it can change is what is seen: a script that stages an actor just past a 480-point stage (to walk in, or parked off stage) shows it early in the wings. ADR 0090 kept scenes at 3:2 for exactly this, "without checking each scene".

**Hook point.** `mt_widescreen_decide` keeps the field's E in modes 9 and 10 for listed scene ids. The check is mechanical: per scene and step, the camera and every actor's and scene sprite's x against the 480 and the E window. `python3 -m mtport.scenes` already walks every scene's commands.

**Verdict: go** (#396, M). The pop at a scene's start is the most visible seam left in the wide view, the decision function is the only code to change, and the per-scene check is automatable. Scenes that fail it stay 480.

## Minimap: mobs and the map's name

**Mobs.** The mob list is `CStateGame +0x34` (`CMobs`, also `0x11A9EC`): count `+0x04`, items `+0x08`; a `CMob`'s feet at `+0x04` / `+0x08`, dead flag `+0x0C` & `0x10`, mob.dat id `+0xA0` ([hud-extras.md](hud-extras.md#the-mobs-and-their-hp)). `read_minimap` already reads the `CStateGame` and the map's objects through a bounded `GuestSpan`; the mobs are one more bounded list. The PC game's minimap, which ADR 0098's addendum followed, shows no mobs, so they get their own key, off by default.

**The name.** ADR 0098 left it out because "the frontend has no cheap way to the game's localised map names". There is one now: [map-names.md](map-names.md) (#213) found that the game keeps every name in guest memory, already in the device's language and after a text pack: `g_middleSpotNames` `0x111A50`, `char[50]` UTF-8 from `LocalizedCString`, indexed by `WorldSpotGroup(AreaWorldSpot(area))` and `g_mapSpotIndex[area][map]` (`0x1077E0`). The area and map are `CStateGame +0x84` / `+0x88`. What is missing is drawing it: the frontend's only text is SDL's 8-pixel debug font, ASCII only, and Korean names are Hangul. The runtime already rasterises NanumGothic with its fallback font (`runtime/src/cg/text.cpp`, stb_truetype) for the guest; a small C API that rasterises one UTF-8 line into a bitmap gives the frontend the same fonts.

Where it goes: inside the box's top edge on a translucent strip, so the corner layout (at most 150 × 48 points, ending above y = 52 where the game's messages start) keeps its size.

**Verdict: go, both in one issue** (#397, M), each behind its own key (`minimap_mobs`, `minimap_name`, off in every preset like `minimap`). Both read only.

## Rumble per event

**Now** ([ADR 0053](../adr/0053-observed-functions-game-events-and-rumble.md)): three sources reach `Rumble` (`platform/src/rumble.cpp`). The game's own vibration (`CMSoundPlayer::Vibrate`, gated by the game's option: every hit, a mob's critical, the options preview, 22 scene steps), a hit observed at `CHero::TakeDamage` `0x65B55` whatever the game's option says, and a level-up at `CHero::LevelUp` `0x65089`, which the phone never vibrated for. So every preset rumbles on a level-up, and ADR 0070's `rumble` row still promises "the game's vibration only, once #72's follow-up splits it" for the Original presets. That follow-up was never filed.

**Hook point.** A filter in `Rumble` on the event kind: the sources are already separate calls.

**Call** (delegated by Joris, 2026-11-13; to veto on #399's PR): `rumble_events = game | all`. `game` keeps only the game's vibration; `all` is today's behaviour. Original and Original + controller `game`, Enhanced `all`. The default without a settings file stays `all`, so a run without one behaves as today (ADR 0084 already lets the defaults differ from a preset). Two values rather than ADR 0070's sketched "game + level-up": the hit rumble that ignores the game's option is the port's addition as much as the level-up, and Enhanced keeps both.

**Verdict: go** (#399, S). It closes a promise a settings row makes today.

## The cash buttons

**Now.** With `cash_shop = off` (every preset, [ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)) the field's Cash button opens a shop whose every tile is cancelled, and the death popup's two revive buttons show a wait and cancel. They are dead ends, like the title's online buttons that ADR 0084 hides.

**Facts** ([keypad.md](../controls/keypad.md)): the Cash button is a panel 0 `UIButton` at (433, 0, 47, 52), `clickCashButton:` / `releaseCashButton:` → −26. The revive buttons are panel 7 `UIButton`s (354 × 55, `releaseRevivalButton:` → −25, `releaseGradeRevivalButton:` → −35) that `CStateGame::Paint` moves over the GL popup box at (cx − 169, cy − 88) and (cx − 169, cy − 41), above Return to town and Go to main menu. The keyboard and the pad reach them by bindings (the pad's X and Y on the death popup), and the button labels and prompts draw glyphs beside them.

**Hook point.** ADR 0084's: host overrides that run the game's creating method and send `setHidden:YES` to the dead button, matched by its action selector. The host's UIKit neither draws, hits nor focuses a hidden view.

**Call** (delegated by Joris, 2026-11-13; to veto on #400's PR): no new key. `hide_online_ui = on` hides them while `cash_shop = off`, the same rule by which it hides Game Center's button while `local_game_center = off` leaves it dead. So Enhanced and Original + controller hide them; Original shows them; `cash_shop = meso` shows them everywhere. The death popup's two remaining buttons stay in place unless a shot of the popup shows the empty top half reads as broken; then they move up with a host transform and touch redirects (the compass's mechanism, ADR 0098's addendum). #400 records which.

**Verdict: go** (#400, S).

## Larger text

ADR 0093: the game's text is drawn into fixed 512 × 32 textures and placed by fixed layouts (the chat box wraps at 168 px, the notify list at 240 points, the popups at their own widths). Bigger glyphs overflow every one of them, and fixing that means rewriting the layouts of the chat box, the popups, the popup menu's tabs, the shops and the shell's UIKit labels. The accessibility bar it was measured against (9 px at 1280 × 800) is met by 2× at least, and `hd_text` makes the text sharp at any scale. **No issue**: high cost, a real risk of clipped or overlapping text in game screens, and the measured need is already met.

## "Clean edges"

ADR 0041: the game uploads premultiplied texels and blends with `GL_SRC_ALPHA`, so translucent edges are darkened twice, as on the device. A "clean edges" option would un-premultiply for display. It is cheap (one blend state or an upload flag behind a setting) and safe, but it makes the port look less like the device, and whether the darkening is visible at the port's scales has not been judged against reference shots from a device (#60). **No issue for now**: reopen it if #60's reference shots, or a player, show the dark fringes as a defect.
