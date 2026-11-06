# Quality of life: the candidates and their verdicts

The small comfort options of [#90](https://github.com/armaatus/maplethief-port/issues/90): where each would hook into the game, what it risks, what it costs, and what the port does. The three judged cheap and safe are implemented, each behind a setting that is off by default ([ADR 0092](../adr/0092-quality-of-life-assists-and-ui-animations.md)); the cutscene skip followed in #174 ([ADR 0096](../adr/0096-scene-skip-runs-every-frame-faster.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the generated code of the 1.5 binary (`work/gen`), the decompiles named in the linked documents, and the real-asset runs under [Verification](#verification).

## Verdicts

| Candidate | Hook point | Risk | Effort | Decision |
|---|---|---|---|---|
| Turbo attack | None in the game: the frontend re-presses the attack action while it is held ([assists.h](../../platform/src/assists.h)) | Low. The game's own rules apply to each press. Never in menus, where attack is OK. | S | **Done:** `turbo_attack` (Assists, off) |
| Auto-potion | None in the game: the frontend taps the quick slot that holds an HP potion, read as `CHero::KeyAttack` `0x66DD9` reads it | Low. A real key press, so the game decides; it changes play, hence an Assist. | S | **Done:** `auto_potion = off \| 25 \| 50 \| 75` (Assists, off) |
| Faster UIKit menu animations | The host's `+[UIView commitAnimations]` (`runtime/src/uikit/view.cpp`), not guest code | None found. Only the presentation changes; no game code waits on an animation. | S | **Done:** `ui_animations = normal \| fast \| instant` (Video, normal in every preset) |
| Auto-run and hold-or-toggle | The frontend's action layer (`Actions`), like turbo | Low, but needs a rule per action: which actions may latch, what cancels a latch (a menu, a popup, the other direction). | M | **Implemented** in [#172](https://github.com/armaatus/maplethief-port/issues/172): `auto_run` and `hold_toggle`, only walking (and the port's fast-forward) latch ([ADR 0094](../adr/0094-controller-extras-auto-run-and-hold-or-toggle.md)) |
| Fast text | None: the text already shows whole. `CChatBox::FirstPage` `0x34CD1` / `NextPage` `0x34A05` wrap a page into whole lines when it is set, `CChatBox::paint` `0x32BBD` draws them all every frame, and OK turns the page in the same tick ([below](#fast-text)) | — (nothing to hook) | — | **Not added** ([#173](https://github.com/armaatus/maplethief-port/issues/173)): `fast_text` would do nothing |
| Cutscene skip | The scene runner `CStateGame::PaintExitToMenu` `0x83C71` steps a `CAniManager` command list ([audio.md](audio.md#scenes)); `StartScene` `0x853E5` and `QueueScene` `0x80935` start them (game modes 9 and 10, [autosave.md](autosave.md)) | High: scenes set quest flags, move the hero, change maps and start boss fights in their commands, and the hero and NPCs move between steps every frame: no step can be skipped. | L | **Done** ([#174](https://github.com/armaatus/maplethief-port/issues/174)): `skip_intros` (Gameplay, off) runs every frame of a scene ×8 and taps each speech box ([below](#cutscene-skip)) |
| Auto-loot | The drop update's contact test: `CDropItems::Update` `0x5BC75` asks `CMapleObject_Touches` `0x6CF01` (its call at `0x5BCEE`) whether the hero's box meets each lying drop's ([below](#auto-loot)) | Low through that test: only the rect of one call changes, and the pick-up after it is the game's. Moving drops or calling the pick-up from the host (rejected) would change the drop list the game walks. | M | **Done** ([#175](https://github.com/armaatus/maplethief-port/issues/175)): `auto_loot = off \| on` (Assists, off; [ADR 0097](../adr/0097-auto-loot-through-the-games-contact-test.md)) |
| Minimap | The hero's position (`CHero +0x04`, `+0x08`) and the map's `.att` grid ([formats/att.md](../formats/att.md)), drawn by the port in the border or over the picture. The key pad's panel 2 is a "minimap" panel the game never fills in 1.5 ([objc-shell.md](objc-shell.md)). | Low (port-drawn, read only). | M | **Done** in [#176](https://github.com/armaatus/maplethief-port/issues/176) ([ADR 0098](../adr/0098-port-drawn-minimap.md)): `minimap = off \| border \| overlay` |
| Save anywhere | — | Boss state is not saved, and the game refuses to save on 16 maps on purpose (`0x54BF9`). | — | **Rejected** ([ADR 0089](../adr/0089-autosave-on-quit-and-background.md), [autosave.md](autosave.md)); autosave within the game's rules is #150 |
| Save states | The turn hook, between ticks: a snapshot of guest memory and the port's host state, kept apart from the game's saves ([below](#save-states), [save-states.md](save-states.md)) | High if taken anywhere: the host side holds 43 kinds of state, GL texels and audio voices. Safe only at a safe point (the field, nothing open, no scene), where a whole-process snapshot replays exactly (`play.save_state_fork`). | L (three parts) | **Accepted with limits** ([#190](https://github.com/armaatus/maplethief-port/issues/190), [ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md)): an in-process state at safe points, built in [#202](https://github.com/armaatus/maplethief-port/issues/202), [#203](https://github.com/armaatus/maplethief-port/issues/203), [#204](https://github.com/armaatus/maplethief-port/issues/204) (`save_states`, Assists, off) |
| Screen shake | — | — | — | **#158** (Accessibility) |
| Mob HP bars | Already native: `CMob::Paint` `0x72685` draws the name and a 40 × 2 HP gauge over a mob for 20 ticks after each hit it survives (`CMob +0xD0`, [hud-extras.md](hud-extras.md)) | — | — | **Dropped** ([#191](https://github.com/armaatus/maplethief-port/issues/191), [ADR 0109](../adr/0109-hud-extras-exp-percentage.md)) |
| EXP percentage | The hero's EXP `+0x90` over hero.dat's need for its level `+0x80`, read as `CHero::ExpToLevelUp` `0x65919` reads it; the HUD has only a 2-point EXP line ([hud-extras.md](hud-extras.md#the-exp)) | None (port-drawn, read-only, window only) | S | **Done** ([#191](https://github.com/armaatus/maplethief-port/issues/191)): `exp_percent = off \| on` (Video, on in Enhanced; [ADR 0109](../adr/0109-hud-extras-exp-percentage.md)) |
| Pick-up log | Already native: `CHero::addInven` `0x64309` and `setMeso` `0x618E1` print "<item> Obtained!" and "Meso obtained.(+N)" to the notify list for 5 s ([hud-extras.md](hud-extras.md#pick-ups-and-meso)) | — | — | **Dropped** ([#191](https://github.com/armaatus/maplethief-port/issues/191), ADR 0109) |
| Damage numbers | Already native (`CDamageFloatings`) | — | — | Nothing to do |

## Turbo attack

**Why it is needed.** In the field only walking is a held action: `CStateGame::DoHeroKeyAction` `0x8163D` runs every frame and keeps only left and right; attack, skills and jump fire once per press ([ANALYSIS.md](../ANALYSIS.md), Input). Holding attack attacks once.

**How.** While the player holds the attack action in the field, `Assists` (`platform/src/assists.cpp`) queues a release and a press of `MT_ACTION_ATTACK` every 6 game ticks, through `mt_input_release` / `mt_input_press`. Both are queued in one turn, so the runtime delivers them a tick apart (ADR 0048: one step per tick) and the action's hold count never drops: when the player lets go, the release is theirs as before. A pair is queued only when nothing else waits to be delivered.

- **Where.** Only with the key pad's panel 0, the engine in `CStateGame` (vtable `0x119B78`), game mode 0 (`0x11AA00`), no popup (`0x108C64 == −1`), and the hero alive (`CHero +0x10` not 0 or 1). In a popup and in the menus attack means OK; a repeat there could confirm the death popup's "Return to town", so turbo never pulses there.
- **Why 6 ticks.** One attack keeps the hero in an attack state (6 or 7) for 5 ticks. With 4 ticks, every other press landed during the attack and was dropped (8 attacks from 14 repeats in 3 s). With 6, each repeat starts an attack (10 attacks in 3 s at 50 ms).
- **The game's rules still apply.** A held walk key blocks any new press until it is released (ANALYSIS.md), so turbo does not attack while walking; that is the game's own behaviour with a finger on both keys.

## Auto-potion

**The quick slots.** `CHero::KeyAttack` `0x66DD9` (`hero`, `code`) returns at once for a dying or dead hero (`+0x10` 0 or 1). It maps `'5'` to slot 0 and `'7'` `'9'` `'0'` `'#'` to slots 1–4. Slot `n` is the pair at `CHero +0xE0 + 8n`: `{kind, id}`, kind 0 the attack, 1 an item, 2 a skill (`0x66E7E`–`0x66EB6`). For an item it walks the hero's 48 item pointers at `[CHero +0xD8]` and uses the first whose id (`item +0x90`) matches, through `CHero::UseItem` `0x66695` (`0x66F0E`). `UseItem` reads the item's count `+0x100` and type `+0x8C` as words. The type is `item_etc.dat`'s (1 a potion) and `+0xC4` the HP it restores ([formats/data-tables.md](../formats/data-tables.md#item_etcdat)).

**How.** In the field as for turbo, with HP (`+0x94`) under the setting's percentage of max HP (`+0x9C`), `Assists` finds the first quick slot 1–4 of kind 1 whose item is a potion with HP to restore and a count above 0, and taps that slot's action (`MT_ACTION_SLOT1`–`4`): pressed, released 2 ticks later, as a finger's 0.1 s tap. Then nothing for 20 game ticks (1 s at the game's 50 ms), so the game's HP change shows before the next check. The potion is used by the game's own code from its key, so its count, sound and effect are the game's.

**When a tap would be lost.** `SkyCanvas::Tick` takes one key a tick, attack first, and a held walk blocks every new press (ANALYSIS.md, Input). The first version pressed and released in one turn: in a fight (`input.menu_shops_save` with `auto_potion = 50`) every such press fell on a tick where the bot held attack or a direction, and no potion was drunk (5 presses, 5 potions left). So the tap now waits while the player walks, holds that slot, or anything is still queued, and a held attack is let go for the tap. With `turbo_attack` on, attack is pressed again after the tap and turbo does not pulse during it; without, a held attack does nothing more anyway, so it stays let go (the player's release is then ignored by the runtime, as for any action not held). With this, each tap drank a potion (5 → 4 → 3 → 2 → 1).

A fresh character has the Red Potion (id 3, HP +50, 5 of them) on slot 1 and the Blue Potion (MP only) on slot 2 ([ANALYSIS.md](../ANALYSIS.md)): `auto_potion` drinks the Red Potion and never the Blue one.

## Auto-loot

**The pick-up, decompiled** ([#175](https://github.com/armaatus/maplethief-port/issues/175); the shared Ghidra project, `tools/ghidra/run-script decompile.py`). `CStateGame::Paint` `0x87501` calls `CDropItems::Update` `0x5BC75` (`CStateGame +0x40`) once a tick, in game mode 0 and not under a few popups (2, 0x12, 0x16, 0x17, 0x20, 0x26, 0x27, 0x2A, 0x6E). It walks the live list (`+0x08`). For each drop:

- **Lying (state `+0x10` = 0).** `0x6CE5D` computes the drop's box from `x` `+0x04`, `y` `+0x08` (its foot) and its size `+0x38`, `+0x3C`: left `x − w/2`, top `y − h + 1`, right left + w, bottom top + h, at `+0x40`–`+0x4C` (no box while flag `+0x0C` bit 0x10 is set). Then **`CMapleObject_Touches` `0x6CF01` (hero, left, top, right, bottom on the stack)**, the call at `0x5BCEE`, computes the hero's box the same way and answers whether the two meet (half-open: `right > hero.left`, `bottom > hero.top`, `hero.right > left`, `hero.bottom > top`). This one call is the contact test.
  - **Contact.** Meso (item kind `+0x84` = 1 whose row in the item table `*0xF148C` has 0 in column 0x41; `addInven` then calls `setMeso`) is always taken. Otherwise a drop not already refused (byte `+0x10C`) is taken when `CHero::isAddInven` `0x6003D` (hero, drop, 1) finds room: a stack of the same id with space (`+0xFC` maximum, `+0x100` count; consumables and etc items, not throwing stars) or a free place among the 48 of its kind (`CHero +0xD4` equipment, `+0xD8` the rest, `0x5F219`). Without room it prints the "inventory full" line (red), and the drop is marked refused (`+0x10C` = 1) and stays. Taken: state 2 and a 3-tick wait (`+0x108`).
  - **No contact.** The refused mark is cleared.
  - Either way the drop's life `+0x104` counts down; at 0 it is removed (from the world's list too when not in contact).
- **Taken (state 2).** After the wait the drop flies to the hero's box top (`hero +0x44`): `dy / 2` a tick, at least 8, and the horizontal distance spread over the same number of steps. Once past the hero's top it leaves both lists, `CHero::addInven` `0x64309` takes it (meso by `setMeso`; else stacks first, then a free place; the "… acquired" line; `CQuestList::checkQuestRequireItem`, which advances a quest that needs the item) and sound 0x1D plays.

**How.** `auto_loot` hooks `0x6CF01` (ADR 0069). A call whose return address is `0x5BCF3`, the drop update's, in the field (`CStateGame`, mode 0, no popup, the key pad's panel 0) with the hero alive, has the drop's rect widened by 160 points left and right (a third of the screen) and 32 up and down (two map tiles) before the original test runs; the stack word (bottom) is restored after the call. Every other call (the hero's and the mobs' touches, 5 other callers) runs the original. Everything from contact on, as listed above, is the game's: meso, room, the refusal and its line, the flight, stacks, quest items, the sound.

**Why not the others.** Moving drops to the hero changes state the game draws and saves, and can cross walls and floors; calling the pick-up from the host copies or skips the update's branches and changes the list during its walk; growing each drop's size inside the update changes its drawing and its flight. Only the contact test's argument is changed here.

**What changes in play.** A drop landing within reach flies to the hero at once, so the hero rarely walks to drops. A refused drop's mark is cleared only when the hero leaves the reach, so the "inventory full" line shows once per drop coming into reach rather than per touch.

## UIKit menu animations

**What animates.** `MainMenuView` (the title menu's fade and slide-in sequence, run once) and `ChooseCharacterView` (the stats panel's scroll) use `+[UIView beginAnimations:context:]` … `+commitAnimations` with `setAnimationDuration:`, `setAnimationDelay:` and `setAnimationCurve:` ([objc-shell.md](objc-shell.md)). Neither sets a did-stop selector or a delegate. No game code schedules an `NSTimer` (the only `scheduledTimerWithTimeInterval:` send is Chartboost's), and nothing reads presentation values back: `UIView`'s getters return model values, which change at once.

**How.** `mt_uikit_set_animation_scale(scale)` multiplies each block's delay and duration at commit. `fast` is 0.25 and `instant` 0: a duration of 0 starts no animation and the new value shows at once. The guest sees nothing of it.

**The early Play guard stays.** With the title menu shown sooner, Play can be pressed sooner. `-[MainMenuView clickPlay:]` before the engine has left `CStateTitle` crashes the original (#122). The port's guards cover it whatever the animations do: the controller focus does not confirm before the engine's main menu (#129, ADR 0051), and `fix_original_bugs` drops such a click (ADR 0069).

## Fast text

**Verdict: there is nothing to speed up, so the port adds no `fast_text`** ([#173](https://github.com/armaatus/maplethief-port/issues/173)). The NPC dialogue and the scenes' speech box show each page's text whole on the frame it is set, and OK moves on in the tick it arrives. Neither has a reveal counter or a minimum time. A `fast_text` setting would be a toggle that changes nothing, so ADR 0070's planned key is dropped (its addendum). The decompiles are of `CChatBox::paint`, its caller `CStateGame::Paint` `0x87501`, the key handler `CStateGame::OnKeyPressed` `0x8D4C9` (game mode 4) and the box's methods, which `tools/mtport/flow.py` now names.

**The box.** The world's chat box (`CStateGame +0x5C`, built by `CChatBox::CChatBox` `0x363CD`) holds a list of pages (`+0x14`, `CMsg {kind, text, face}`: `CChatBox::AddNpcPage` `0x358DD` and `AddHeroPage` `0x35A09`, from the script natives `regMsgNPC` / `regMsgHero`), the quest rows (`+0x20`, `CSelItem`, `AddRow` `0x357B5`) and the required and reward items (`+0x24`, `+0x28`). `showMsg` calls `CChatBox::Show` `0x3573D` (kind `+0x04`, mode `+0x08`: 0 OK, 1 and 3 Accept / Refuse, 2 a list of rows), which sets game mode 4, shows the NPC panel (`Shell_EnableNPCKey`) and calls `CChatBox::FirstPage` `0x34CD1`.

**Text is wrapped whole when a page is set.** `FirstPage` (page 0) and `CChatBox::NextPage` `0x34A05` (page + 1; false past the last) free the old lines and call `Text_Wrap` `0x22EC5` (owner, text, 168 px, &count). It returns a `new[]` array of whole line strings, stored at `+0x18` with their count at `+0x1C`. They also set the hero's face (`CHero::SetFace` `0x6299D`) and the key scroll (`KeyScroll_Init`). Only these and `CChatBox::Clear` `0x35B35` (which frees them) write `+0x18`.

**Paint draws every line, every frame.** `CChatBox::paint` sizes the box from the line count (clamped to 162–290 px), then draws `+0x1C` lines from `+0x18` with `Text_DrawColored` `0x23C71`. That draws a whole C string; no length or counter is passed. The only offset is the scroll `+0x4C`, which the up and down keys move by one line (`CChatBox::ScrollUp` `0x34995` / `ScrollDown` `0x34905`, up to `+0x50`, the overflow paint computes for a long Accept / Refuse page). The same function places the NPC panel's OK or Accept / Refuse buttons and the six quest-row buttons, and copies `questSelector` into `+0x30`. That is why hooking it would have been risky, but there is nothing in it to change.

**OK takes effect in the tick it arrives.** In game mode 4 `OnKeyPressed` handles OK (`-5`, `'5'`) and Back (`-16`). For a script's box (`+0x04` 0) it calls `NextPage`, and on the last page `CChatBox::Close` `0x35E89` (Back: `Close(box, 1)`; Accept / Refuse by the row `+0x30`). Close hands the answer to the script's callback (`getEvtIndex` reads `CChatBox::GetAnswer` `0x31E11`). There is no timer and no count of frames shown. The scenes' speech box (`CChatBox::drawChatBox` `0x35189`, from `Paint` while `CStateGame +0x1E9` is set) shows two lines of `+0x4D0` from `+0x4C8`, and OK outside mode 4 adds 2 or clears `+0x1E9`.

**The one wait is a general one.** `OnKeyPressed` returns at once in game modes 6–8 (loading), while the screen fades (`CStateGame +0xB4`: `Paint` steps `+0xBC` from 0 to 3, one alpha step of 0x40 a frame, then clears it; `+0xB8` 1 fades out), while the hero's `+0x119` is set and while byte `+0x108` of the manager at `*0xF1454` (the one `Popup_Open` takes) is set. A dialog a script opens as its map arrives (MacArthur's first one on 2.8) shows during that fade-in, so a press in its first 4 ticks is dropped. The fade is the map's, not the text's. Shortening it would change a map change for every key, which is not text speed.

**A real-asset trace** (`play.chat_text`, Release arm64, a new game, 50 ms ticks, 2026-11-02). The task presses A for one tick as soon as a box shows and the game takes keys (not loading, no fade), and checks every box's lines each tick:

| Tick | What |
|---|---|
| 452–470 | The intro scene on 2.10: 5 speech boxes, each with 2 whole lines (the last 1) on its first tick. A on the box's first tick, the box gone one tick later |
| 499–500 | Map 2.8 loads (game modes 8, 7); the fade-in starts (`+0xB4`) |
| 501 | MacArthur's dialog: page 0 shown with its 4 whole lines (81 characters); keys dropped (fade) |
| 505 | Fade over; A down |
| 506 | Page 1: 5 whole lines; A down at 507 |
| 508 | Page 2: 7 whole lines; A down at 509 |
| 510 | Page 3: 5 whole lines; A down at 511; the box closes at 512 and quest 0 (Talking to NPCs) is active |

Every one of the 9 boxes had all its lines on its first tick, and no box's text changed while it showed. Each A turned the page or closed the box one tick after the press. The only other wait was the 4-tick fade-in. With presses every tick, the 4-page dialog takes 11 ticks (0.55 s) from opening to closing; the floor is the input's own pace (a press and a release, a tick each).

**Not done.** Auto-advance (OK pressed for the player when a page has shown long enough) would shorten dialogues, but it is a different feature: it answers for the player, and on a mode 1 / 3 page it would accept a quest. Nobody has asked for it; it is not filed.

## Cutscene skip

**Verdict ([#174](https://github.com/armaatus/maplethief-port/issues/174), [ADR 0096](../adr/0096-scene-skip-runs-every-frame-faster.md)):** a scene can't skip steps without changing what it leaves behind, but it can run faster. With `skip_intros = on` (Gameplay, off in every preset, `--skip-intros`), the port runs every frame of a scene, faster, and taps each speech box. Every command of every step runs in the game's order, on the game's own ticks. The game can't tell this from a player who taps each box at once, so the state after the scene is the same as after a watched one (see [Verification](#verification): 64 and 32 fields equal on the intro and on Balog's entrance).

### How a scene runs

`CStateGame::StartScene` `0x853E5` (scene `n`, the table below) builds the scene's command list with its `CAniManager::makeAni*` builder into `CStateGame +0x38`. It then sets game mode 9 and the cut-scene key pad (panel 3) and resets the scene's camera (`0x814E1`). `QueueScene` `0x80935` and `QueueBossScene` `0x80965` only queue one (game mode 10, the scene in `+0x1E4`); the next load starts it. The intro (scene 0) is started by `LoadGame` `0x86C1D` itself.

Each `CStateGame::Paint` `0x87501` in game mode 9 calls the runner `CStateGame::PaintExitToMenu` `0x83C71` once:

1. `Scene_NextStep` `0x26A11` (list, `+0x1E9`) returns the step to run and counts one more (the list's `+0x14`), or −1 while a speech box waits (`+0x1E9` set). The commands are in step order. A step with no command is still one frame.
2. `Scene_NextCommand` `0x2690D` returns each command of that step in turn. The runner switches on `command − 1` (a `tbh` at `0x83D26`) and reads the arguments with `Scene_ArgP1` `0x26981` and `Scene_ArgP2` `0x269C9`.
3. Back in `Paint`, every frame, whether or not a step ran: the hero, the NPCs and the mobs update, and each scene sprite moves by its speed (`+0x4BC`, `+0x4C0`). The camera shakes and scrolls ([screen-shake.md](screen-shake.md)). The fades, the pictures, the letterbox bars and the speech box (`CChatBox::drawChatBox`, speaker `+0x4B4`, two lines from `+0x4C8`) are drawn.

**Only a key ends a speech box.** `CStateGame::OnKeyPressed` `0x8D4C9`, in a scene with no popup, shows the next two lines (`+0x4C8 += 2`), or clears `+0x1E9` after the last ones. The cut-scene panel's only button covers the bottom strip and sends OK (−5, [controls/keypad.md](../controls/keypad.md#panel-3-cut-scenes)). Nothing else writes `+0x1E9`: only the runner's command 50 (set) and 51, and `ResetSceneCamera` (clear).

**A scene ends with exactly one end command, on its last step** (`python3 -m mtport.scenes check`): 28, a warp or the main menu; 52, the next scene's map; or 58, the field of the same map, starting the boss if p2 = 0. Each frees the list (`CStateGame::FreeScene` `0x80A6D`).

### The commands

Each of the 64 command numbers, from the runner's switch. **Picture** and **sound** commands change only what is drawn or heard. **Box** commands are the speech box the runner waits on. **State** commands change the game: the hero, the NPCs, the map, the scripts or the mobs. **End** commands end the scene. 3, 4 and 11 have no case and do nothing.

| Command | Kind | Does |
|---|---|---|
| 1, 2 | picture | letterbox bars on / off (`+0x47C`: black 50-point strips at the top and bottom) |
| 5, 6 | state | hero x, y = p2 |
| 7 | state | hero walk speed (`CHero +0x20`) = p2 |
| 8 | state | hero face = p2 (`CHero::SetFace` `0x6299D`, `+0x14C`) |
| 9 | state | hero walks, right if p2 = 0, else left: `vx` (`+0x74`) = ± speed, action 3 |
| 10 | state | hero stops: `vx` = `vy` = 0, action 2 |
| 12 | state | an NPC of type p2 created (`CNpcs_AddSceneNpc` `0x76EB5`), at the list's next index (the builders' p1) |
| 13, 14 | state | NPC p1 x, y = p2 |
| 15 | state | NPC p1 speed = p2 (0 also stops it) |
| 16 | state | NPC p1 walks, right if p2 = 0 (`CNpc::Walk` `0x6FCBD`) |
| 17 | state | NPC p1 stops, action p2 (3: stands still) |
| 18 | sound | vibrate p2 ms |
| 19, 20 | picture | camera shake on (amplitude p2) / off |
| 21 | picture | the scene camera follows the hero (`+0x47D`) |
| 22, 23 | picture | camera x / y centred on p2 (`CMMap +0x70` / `+0x74`, clamped), scroll stopped |
| 24, 25 | picture | camera scroll x / y per frame = p2 (`+0x480` / `+0x484`) |
| 26, 27 | picture | camera follow off; follow and scroll off |
| 28 | end | hero reset (face 0, facing right, speed 5, still). p1 = −1: to the main menu (`CStateMainMenu`, the Game Center ending); else a warp to map p1.p2 (`+0x98`, `+0x9C`, game mode 8) |
| 29, 53 | picture | fade in from / out to colour p1 over p2 frames (`+0x4B8`, `+0x494`, `+0x4C4`) |
| 30–36, 61 | picture | picture slot p1 (0–4): the scene's image p2 (`Scene_Image` `0x26AB1`), its x, y and `+0x4A8`, removed, fading in or out, `+0x4AC` |
| 37–41 | state | map object p1 (`CMMap +0x48`): x, y, `+0x0A`, `+0x06`, `+0x08` = p2 |
| 42–48 | picture | scene sprites (`CStateGame +0x48`): one of image p2, its x, y, speed per frame, flag `+0x20` |
| 49 | picture | effect p2 near the hero (`CEffects_AddAt` `0x5E965`) |
| 50 | box | speech box: speaker p1, text p2 wrapped to the screen (`Text_Wrap` `0x22EC5`); waits for a key (`+0x1E9`) |
| 51 | box | speech box closed: picture slot p2 cleared, its lines freed, `+0x1EC` = 0 |
| 52 | end | hero reset; the next scene `+0x1E4` = p1, and its map (2: 2.11, 17: 8.6, 18: 7.6, 19: 8.4); game mode 8, key pad off |
| 54 | state | `CStateGame::RunNpcStateScripts` `0x839BD`: the NPCs' state scripts (quests, flags) |
| 55, 56, 57 | sound | all music stopped, then music p1 (resumed if p2 = 1); all music stopped; vibration stopped (empty) ([audio.md](audio.md#scenes)) |
| 58 | end | hero reset; p2 = 0: the boss starts (`CNpcs_StartBosses` `0x7579D`: mobs 0x40–0x45 to their attack state); game mode 0 and the key pad (`+0x3FC` = 1): the field of the same map |
| 59 | state | the scene's NPCs removed (`CNpcs_RemoveSceneNpcs` `0x76D89`) |
| 60 | state | boss p2 (0x40, 0x42, 0x43, 0x44) placed (`CStateGame::PlaceSceneBoss` `0x83BE1`) |
| 62 | box | the speech box's `+0x1EC` = p2 (cleared by 51) |
| 63, 64 | picture | scan lines over the picture on / off (`+0x4CC`; no scene uses them) |

### Which steps change state

The 21 scenes (`python3 -m mtport.scenes generate`; `list` prints every command):

<!-- BEGIN generated:scenes -->
<!-- Generated by `python3 -m mtport.scenes generate`: do not edit by hand. -->
| Scene | Builder | Steps | Speech boxes | State commands: steps | Ends (step: command) |
|---|---|---|---|---|---|
| 0 | `CAniManager::makeAni1` `0x302F5` | 444 | 4 | hero 45, 125, 240, 347, 357, 443; NPCs 1, 49, 145, 150, 274, 277, 279, 290, 300, 355, 359, 372, 376; map 350, 379, 387 | 444: 28 (2, 8) |
| 1 | `CAniManager::makeAniEndingZacum` `0x2BE55` | 202 | 11 | hero 4, 35, 45, 82, 85, 87, 88, 91, 107, 157, 162, 167, 174; NPCs 5, 66, 82; map 3, 92; scripts 3 | 202: 52 (2, 0) |
| 2 | `CAniManager::makeAniEndingHenesis` `0x26F89` | 430 | 5 | hero 2, 355 | 430: 28 (2, 0) |
| 3 | `CAniManager::makeAniMelodyIn` `0x2FE21` | 31 | 3 | hero 2, 20, 27; map 2 | 31: 58 (0, 0) |
| 4 | `CAniManager::makeAniMelodyEnd` `0x2F825` | 84 | 2 | hero 3, 27, 30, 70; NPCs 4, 22, 26; map 30; scripts 3; mobs 26 | 84: 58 (0, 1) |
| 5 | `CAniManager::makeAniWolfIn` `0x2F319` | 61 | 3 | hero 2, 45, 57; NPCs 2 | 61: 58 (0, 0) |
| 6 | `CAniManager::makeAniWolfEnd` `0x2EDD9` | 41 | 2 | hero 3, 25, 32; NPCs 4, 27, 31; scripts 31; mobs 31 | 41: 58 (0, 1) |
| 7 | `CAniManager::makeAniYetiIn` `0x2E5E1` | 101 | 3 | hero 2, 85, 92; NPCs 2, 3, 68, 100 | 101: 58 (0, 0) |
| 8 | `CAniManager::makeAniYetiEnd` `0x2E03D` | 49 | 3 | hero 3; NPCs 4, 32, 36; scripts 3; mobs 36 | 49: 58 (0, 1) |
| 9 | `CAniManager::makeAniHomunIn` `0x2DA85` | 61 | 4 | hero 2, 3, 27, 40, 50, 52 | 61: 58 (0, 0) |
| 10 | `CAniManager::makeAniHomunEnd` `0x2D351` | 56 | 5 | hero 3; NPCs 4, 42, 46; scripts 3; mobs 46 | 56: 58 (0, 1) |
| 11 | `CAniManager::makeAniChapter1` `0x2BA59` | 45 | 0 | hero 1 | 45: 58 (0, 1) |
| 12 | `CAniManager::makeAniChapter2` `0x2B65D` | 45 | 0 | hero 1 | 45: 58 (0, 1) |
| 13 | `CAniManager::makeAniChapter3` `0x2B261` | 45 | 0 | hero 1 | 45: 58 (0, 1) |
| 14 | `CAniManager::makeAniBalogIn` `0x2CBD1` | 79 | 6 | hero 2, 40, 50, 62 | 79: 58 (0, 0) |
| 15 | `CAniManager::makeAniChapter1Perion` `0x28521` | 105 | 7 | hero 2, 40, 60, 70, 80, 87; map 3; scripts 103 | 105: 58 (0, 0) |
| 16 | `CAniManager::makeAniMagatiaHumanoid1` `0x291D9` | 70 | 3 | hero 1 | 70: 52 (17, 0) |
| 17 | `CAniManager::makeAniMagatiaHumanoid2` `0x2AAF5` | 222 | 7 | hero 1; NPCs 1, 84, 165 | 222: 52 (18, 0) |
| 18 | `CAniManager::makeAniMagatiaHumanoid3` `0x29765` | 286 | 9 | hero 1; NPCs 2, 3, 4, 5, 6, 13, 49, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 120, 146, 147, 148, 193; map 2, 52 | 286: 52 (19, 0) |
| 19 | `CAniManager::makeAniMagatiaHumanoid4` `0x28D81` | 56 | 3 | hero 1 | 56: 58 (0, 1) |
| 20 | `CAniManager::makeAniChapter2Perion` `0x27AED` | 165 | 10 | hero 2, 45, 65, 75, 135; map 92, 125; scripts 163 | 165: 58 (0, 0) |

1169 commands in 21 scenes: 460 picture, 78 sound, 230 box, 380 state, 21 end. Steps are the runner's frames (one per `Paint` while no box waits); an end command's arguments are (p1, p2).
<!-- END generated:scenes -->

### Why the skip runs every frame

Three shortcuts were considered. Each changes the outcome:

- **Jump to the end command.** Positions come from frames, not only from commands. In `makeAniChapter1Perion` and `makeAniChapter2Perion`, the hero walks at speed 3 from step 2 to step 40 or 45 (command 9, then 10), and the scene ends in the same field (58). Jumping leaves the hero 114 or 129 points from where the scene puts them. The same happens with the hero in `makeAniMelodyEnd` (30–70) and `makeAniHomunIn` (2–27). The jump would also skip the script runs (54) and the boss placing (60) before the end.
- **Run all the remaining steps in one frame.** The commands would all run, but the walks between them would take no frames. Same result as above.
- **Skip only the picture and sound commands.** The steps still take their frames. Nothing is gained but the speech boxes' waits, and the music commands' effect on the map's music after the scene would be lost.

So the skip keeps every frame and makes each one cheaper in wall time. `platform/src/scene_skip.h` does two things while the engine's `CStateGame` is in game mode 9 with no popup:

- **The guest clock runs ×8** (`kSceneSkipRate`). The rate is folded into fast-forward's ([ADR 0087](../adr/0087-game-speed-and-fast-forward.md)): the wall plan ×8, a present at most once a refresh, the waits divided by the rate. Each tick still sees the game's own interval. The sub-timers, `GetTimeMs` and the random numbers see the same game time as in a watched scene.
- **Each speech box gets a tap of OK.** On panel 3, when nothing is queued, the frontend presses OK through `mt_input_press` and releases it 2 ticks later, as a finger's tap. The next tap waits until the release has been delivered. `OnKeyPressed` then shows the next lines or closes the box, as for the player.

Nothing in guest memory is written, and no guest hook is added, so #158's handler on the runner stays as it is.

**What still differs from a watched scene: only the frames a box waits.** While a box is up, `Paint` keeps moving whatever has a speed. Two kinds of scene do this:

- In five scenes an NPC walks while boxes show: `makeAniEndingZacum`, `makeAniHomunEnd`, `makeAniYetiEnd`, `makeAniWolfEnd` and `makeAniMelodyEnd`. Each such NPC is a scene NPC. Command 59 removes it, or the scene ends with a map change (52).
- In no scene does the hero walk while a box is up.
- A boss's own counters run too. Balrog, on the map during `makeAniBalogIn`, counts its attack cycle at mob `+0xF8` every frame of the scene, boxes included. When it attacks after the scene depends on how many frames the scene took, so on how fast the player tapped, in the original as well. With the skip's taps the scene takes 81 ticks, against 181 in the watched test. Both leave the counter at 19 and Balrog attacks 20 ticks after the scene. With the flow's A presses added to the skip's taps (87 ticks), Balrog attacks 6 ticks sooner after the scene.

So how long a box waits leaves the quests, the map, the hero and the items as they are, in the original or with the skip. It can shift the phase of a boss's attack cycle, as a player's tapping speed does. A skipped scene is the one a player who taps at once would see.

In fixed-step runs (the tests, ADR 0044) the clock has no rate, so there the skip is only the taps.

## Save states

**What it is.** A snapshot of the running port, quick-saved and quick-loaded by the player (F5 / F9), apart from the game's own saves, which it never reads or writes ([#190](https://github.com/armaatus/maplethief-port/issues/190)).

**Where it can be taken.** Only between ticks, in the field with nothing open: game mode 0, popup −1, key pad panel 0, no scene, no touch down, no UIView animation, GCD idle, no file open. Elsewhere it is refused with the reason. A boss's entrance is a scene, so a state before a boss is taken in the field before it.

**What it holds.** Guest memory (the writable image, the heap below its top, the main stack), the heap's host bookkeeping, the cpu and sjlj state, the ObjC runtime's mutable tables and every host state by type, the GL state and texels, the audio buffers, voices and players, and the guest time. The inventory with each state's plan is [save-states.md](save-states.md).

**The evidence.** A whole-process snapshot by fork at step 46,000 of the regression replay (the tutorial's field) played the next 200 ticks exactly as the run that never left it: 201 checkpoints of screen, audio and state equal (`play.save_state_fork`, 2026-11-05).

**The verdict.** Accepted with those limits as an in-process serialization ([ADR 0105](../adr/0105-save-states-in-process-snapshot-at-safe-points.md)); fork itself is no way to ship it (no Windows or iOS, no file). Built in three parts: the file and the core (#202), graphics, audio and UIKit (#203), the Assist `save_states = off | on` (#204).

## Minimap

**Where the map is.** The game's loaded collision grid: `CStateGame +0x2C` is the `CMMap`, whose `.att` grid is `+0x3C` (size `+0x40`), `attW · attH` cells at `+0x28` / `+0x2C`, each `+0x0C` × `+0x10` pixels ([formats/att.md](../formats/att.md)). The objects are `+0x48` / `+0x4C` with their runtime `visible` flag ([formats/obj.md](../formats/obj.md)). The hero is `CStateGame +0x30` (`+0x04`, `+0x08`: its feet). Read once a present, never written, every range checked ([ADR 0098](../adr/0098-port-drawn-minimap.md)).

**What it shows.** Footholds, ladders and walls (a box pixel takes the most important cell it covers), portals (kinds 0–2; the hidden portal 3 is left out, as in the game's own map), visible NPCs and the hero. Only in the field (game mode 0); a popup over the field keeps it.

**Where.** `minimap = border` in the bar left of the picture (or below it), else over the screen image's top edge right of the compass; `overlay` always there. With widescreen and the key pad at the view's edges, the visible view's top left corner instead, the compass moved right of it ([ADR 0098's addendum](../adr/0098-port-drawn-minimap.md#addendum-the-corner-with-the-key-pad-at-the-edges-2026-11-04)). A window overlay after the read-backs: never in a shot or a golden.

## Verification

Synthetic (CI):

- `platform_test`, `assists:` cases: the quick slot found as `KeyAttack` reads it (an empty slot, MP only, none left, a scroll, a skill), the field guards (popup, popup menu, another state, dying, dead, unmapped memory), the threshold arithmetic, the cooldown, the pulses and their guards, the runtime's hold kept through a pair, the rows and `MAPLETHIEF_SETTINGS`' list.
- `objc_test`, `views: the host's animation scale …`: a quarter and 0, the clamp and the refusal of bad values.
- `platform_test`, `scene_skip:` cases (#174): the scene view (game mode 9 in `CStateGame`, the box byte, the step; no scene in modes 0, 8, 10, in a popup or another state), the ×8 rate only while skipping, the OK tap and its release after 2 ticks (none in the release's turn, off panel 3, while queued, without a box or with the setting off), the row.
- `tests/test_scenes.py`: the 64 commands classified, the ends, the table and its checks on synthetic commands, the names file. With the slice (local): every builder's 1,169 commands classified, one end on each scene's last step, the intro 444 steps / 4 boxes / 28 (2, 8), Balog 79 steps / 6 boxes / 58 (0, 0), this file's generated block up to date.
- `platform_test`, `assists: auto_loot …` cases: the reach and the half-open box test; only the drop update's return address widened (the five other callers and a near miss pass); the stack argument restored; nothing in a popup, the popup menu, another state, with the hero dead or a menu's panel; off is the original test.

Real assets (local, Release arm64, `play_real_test`, a new game to the tutorial's map 2.8):

- `play.auto_potion` (each tap 2 ticks): quick slot 1 holds the Red Potion (item 3, +50, 5). HP set to 2 of 10: off, nothing in 3 s; `auto_potion = 50`, one Red Potion drunk through slot 1, HP 2 → 10, none after it. HP low with the popup menu open: nothing until it closes, then the next potion.
- `play.turbo_attack`: X held 3 s: 1 attack with turbo off, 10 with it on (9 repeats). No repeat with X held in the popup menu.
- `play.chat_text` (#173): through the intro with A as soon as each box shows: 4 NPC pages and 5 speech boxes, each with its whole text on its first tick and unchanged while shown; each A turns the page one tick later; the first page waits 4 ticks for the map's fade-in ([Fast text](#fast-text)).
- `play.auto_loot` (#175; after the tutorial, on 2.3, the bot hunting): off, 3 pick-ups, each in contact (gap 0 × 0 between the hero's box and the drop's, both at `+0x40` from that tick's test); turned on with the hero standing, the drop 27 points from its box was taken (x 730 → 730, meso 544 → 562, items 10 → 11); on, 10 pick-ups, 6 across a gap (no gap wider than the reach); both inventories full (the free places pointing at an item the hero has, every stack at its maximum: the test's writes, undone after), 3 drops refused, 2 of them out of contact, all 3 still on the ground, no item taken (4,800 → 4,800), meso 408 → 444.
- Off is unchanged byte for byte: `play.first_map` on the epic tip (`23d23ca`) and on #175's branch with the setting off gave the same 1,331 log lines (18,000 ticks; 197 drops, 160 picked up, meso +1,128, items +93) and the same 34 output files (screenshots, saves, `play.log`).
- `play.ui_animations_normal`, `_fast`, `_instant`: at most 15 UIKit animations at once on the title menu with `normal` (the last at 1.30 s of the run) and `fast` (the last at 0.32 s); none with `instant`. Each run reaches the field of 2.8 from a new game.
- `play.skip_intros_watched` / `_skipped` (#174): the intro watched (A through its boxes) takes 508 scene ticks and 26 presses; skipped with no input, 446 ticks and 5 taps. After the map load and after MacArthur's dialog, the skipped run equals the watched one in 64 of 64 fields (map, hero, quick slots, items, quests, mobs, the scene's state), and the game's `rand()` state is the same.
- `play.skip_intros_boss_watched` / `_skipped`: Balrog's entrance (scene 14) from `play.save_quit`'s save moved to 10.5. Watched 181 ticks and 9 presses, skipped 81 ticks and 7 taps; 32 of 32 fields equal (Balrog 69 at (344, 239), HP 80000, its attack countdown; the hero at (48, 239)), `rand()` the same.
- Real time (the game's paced clock, dummy video driver): the intro's 447 ticks took 4.2 s with the skip on, against 22.4 s or more watched (the frames alone). Host-bound: software GL costs about 11 ms a frame.

- `play.save_state_fork` (#190, Debug arm64, 2026-11-05): the regression replay to step 46,000 (2.8, the field, nothing open), a fork there; the parent's 200 ticks and then the snapshot's 200 ticks give 201 equal checkpoints (digest `4d063d80e179a629`, 109 distinct frames, the hero at 44 places) ([save-states.md](save-states.md#the-prototype)).
- `play.save_state` (#203, Debug arm64, 2026-11-05): an in-process save and restore at two points of the regression replay, 2.8 (step 47,377) and the snail fight on 2.9 (step 73,628, a kill in the window); at each the restored run's 201 checkpoints equal the uninterrupted run's (digests `f970fd2b49c31151`, `819786c3ce7d80e1`) ([save-states.md](save-states.md#the-restore-203)).
- `play.minimap` (#176, Debug arm64): first_map's route to 2.3 with `minimap = overlay`. The game's grid on 2.3 (120 × 60 cells) equals `map/23.att`; both markers are in `23.obj`; the view shows in every field turn (80,019, maps 2.8, 2.9, 2.0, 2.3) and in none of the 2,844 map-screen turns; the hero's dot is at his position in all 32,601 turns on 2.3 and moved with him 154 times (93 left, 61 right), never the wrong way.

The regression suite with each feature on (`ctest -L real_assets -j4`, Release arm64, 95 tests, `MAPLETHIEF_SETTINGS`; `platform.device_golden` skips locally in every run, off included):

| Setting | Result |
|---|---|
| none (all off) | 95 of 95 |
| `turbo_attack = on` | 95 of 95 |
| `auto_potion = 50` | 93 of 95: `input.menu_shops_save` (and `input.menu_shops`, which needs its save). The scripted bot of the hunt drinks its five Red Potions earlier, falls on 2.3, and its way back from town stays at (56, 271) in Henesys with full HP until the 600 s limit. A changed outcome of an Assist plus a limit of the bot, not a fault: with `auto_potion = 25` both pass. |
| `ui_animations = instant` | 94 of 95: `platform.title_60s`, which checks that the title's slide-in animates between 500 and 1200 ms. With no animation the frames are the same: the setting doing what it says. |
| `skip_intros = on` (#174; 109 tests, none (all off) 109 of 109) | 104 of 109 at first, 105 of 109 after the fix. `play.autosave_map_10_5` failed: its own A presses added to the skip's taps made Balrog attack before its guard (see [Cutscene skip](#cutscene-skip)); it presses only on its own panels while the skip is on now, and passes. The other four run the game in real time across the new game's intro and time its clock, so the ×8 skip shows: `platform.game_speed` (its fast-forward runs see ×8, not ×2 or ×3, while the intro plays), `platform.perf_telemetry` (the ticks line counts the late ticks against 55 ms ÷ 8), `platform.game_autosave` (the intro is over sooner, so the hero is not on map 2.0 at the 12 s background) and `platform.frame_rate` (its two runs' tick counts no longer match: 1245 and 1253). The setting doing what it says, so since the wave 8 integration these four pass `--skip-intros off` (the flag wins over `MAPLETHIEF_SETTINGS`). On the integrated tip two more failed with it on: `platform.device_golden` and `platform.keypad_hide` shoot the intro's dialog at a fixed time, which the ×8 clock moves to another frame, so `devicegolden.game_args` passes `--skip-intros off` for both. The full run gave 112 of 114 before that pin, and both pass with it (2026-11-03). |
| `auto_loot = on` (#175, 2026-11-02, 106 tests on the branch's tip; off: 106 of 106) | 106 of 106. In the run with the suite at `-j4` beside other builds (load average about 20), `platform.game_speed` (its real-time tick rate, 3 % allowed) and `platform.gpu_compare` (its GPU time budget) missed their limits, as `game_speed` also did with the setting off at that load; both passed again with `auto_loot = on` at a load of 4. |
| Epic #201's settings together (2026-11-06, Debug arm64, 135 tests on the integrated tip, `-j3`): `pause_on_disconnect = on; flashes = 50; audio_mono = on; brightness = 110; contrast = 95; saturation = 110; gamma = 1.10; pad_pointer = both; exp_percent = on; keypad_haptics = light; fullscreen_display = current; save_states = on` (off: 135 of 135) | 133 of 135 at first, 135 of 135 after the fix. `platform.audio_capture` (its stereo runs must have frames whose channels differ, which `audio_mono = on` takes away) and `platform.audio_pack` (the pack's tone checked per channel) ran mono throughout: the setting doing what it says. Both pass `--audio-mono off` now (the mono run of `audio_capture` keeps `on`; the flag wins over `MAPLETHIEF_SETTINGS`), and pass with the settings on. |

