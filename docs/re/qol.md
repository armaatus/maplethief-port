# Quality of life: the candidates and their verdicts

The small comfort options of [#90](https://github.com/armaatus/maplethief-port/issues/90): where each would hook into the game, what it risks, what it costs, and what the port does. The three judged cheap and safe are implemented, each behind a setting that is off by default ([ADR 0092](../adr/0092-quality-of-life-assists-and-ui-animations.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the generated code of the 1.5 binary (`work/gen`), the decompiles named in the linked documents, and the real-asset runs under [Verification](#verification).

## Verdicts

| Candidate | Hook point | Risk | Effort | Decision |
|---|---|---|---|---|
| Turbo attack | None in the game: the frontend re-presses the attack action while it is held ([assists.h](../../platform/src/assists.h)) | Low. The game's own rules apply to each press. Never in menus, where attack is OK. | S | **Done:** `turbo_attack` (Assists, off) |
| Auto-potion | None in the game: the frontend taps the quick slot that holds an HP potion, read as `CHero::KeyAttack` `0x66DD9` reads it | Low. A real key press, so the game decides; it changes play, hence an Assist. | S | **Done:** `auto_potion = off \| 25 \| 50 \| 75` (Assists, off) |
| Faster UIKit menu animations | The host's `+[UIView commitAnimations]` (`runtime/src/uikit/view.cpp`), not guest code | None found. Only the presentation changes; no game code waits on an animation. | S | **Done:** `ui_animations = normal \| fast \| instant` (Video, normal in every preset) |
| Auto-run and hold-or-toggle | The frontend's action layer (`Actions`), like turbo | Low, but needs a rule per action: which actions may latch, what cancels a latch (a menu, a popup, the other direction). | M | **Follow-up:** [#172](https://github.com/armaatus/maplethief-port/issues/172) |
| Fast text | None: the text already shows whole. `CChatBox::FirstPage` `0x34CD1` / `NextPage` `0x34A05` wrap a page into whole lines when it is set, `CChatBox::paint` `0x32BBD` draws them all every frame, and OK turns the page in the same tick ([below](#fast-text)) | — (nothing to hook) | — | **Not added** ([#173](https://github.com/armaatus/maplethief-port/issues/173)): `fast_text` would do nothing |
| Cutscene skip | The scene runner `CStateGame::PaintExitToMenu` `0x83C71` steps a `CAniManager` command list ([audio.md](audio.md#scenes)); `StartScene` `0x853E5` and `QueueScene` `0x80935` start them (game modes 9 and 10, [autosave.md](autosave.md)) | High: scenes set quest flags, move the hero, change maps and start boss fights in their commands. Skipping must still run every state-changing command. | M–L | **Follow-up:** [#174](https://github.com/armaatus/maplethief-port/issues/174) |
| Auto-loot | Drops are picked up on contact (`CDropItems::Update` `0x5BC75`, [ANALYSIS.md](../ANALYSIS.md)) | Medium: moving drops or calling the pick-up from the host changes play and the drop list the game walks each tick. | M | **Follow-up:** [#175](https://github.com/armaatus/maplethief-port/issues/175) (an Assist if done) |
| Minimap | The hero's position (`CHero +0x04`, `+0x08`) and the map's `.att` grid ([formats/att.md](../formats/att.md)), drawn by the port in the border or over the picture. The key pad's panel 2 is a "minimap" panel the game never fills in 1.5 ([objc-shell.md](objc-shell.md)). | Low (port-drawn, read only). | M | **Follow-up:** [#176](https://github.com/armaatus/maplethief-port/issues/176) |
| Save anywhere | — | Boss state is not saved, and the game refuses to save on 16 maps on purpose (`0x54BF9`). | — | **Rejected** ([ADR 0089](../adr/0089-autosave-on-quit-and-background.md), [autosave.md](autosave.md)); autosave within the game's rules is #150 |
| Screen shake | — | — | — | **#158** (Accessibility) |
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

## Verification

Synthetic (CI):

- `platform_test`, `assists:` cases: the quick slot found as `KeyAttack` reads it (an empty slot, MP only, none left, a scroll, a skill), the field guards (popup, popup menu, another state, dying, dead, unmapped memory), the threshold arithmetic, the cooldown, the pulses and their guards, the runtime's hold kept through a pair, the rows and `MAPLETHIEF_SETTINGS`' list.
- `objc_test`, `views: the host's animation scale …`: a quarter and 0, the clamp and the refusal of bad values.

Real assets (local, Release arm64, `play_real_test`, a new game to the tutorial's map 2.8):

- `play.auto_potion` (each tap 2 ticks): quick slot 1 holds the Red Potion (item 3, +50, 5). HP set to 2 of 10: off, nothing in 3 s; `auto_potion = 50`, one Red Potion drunk through slot 1, HP 2 → 10, none after it. HP low with the popup menu open: nothing until it closes, then the next potion.
- `play.turbo_attack`: X held 3 s: 1 attack with turbo off, 10 with it on (9 repeats). No repeat with X held in the popup menu.
- `play.chat_text` (#173): through the intro with A as soon as each box shows: 4 NPC pages and 5 speech boxes, each with its whole text on its first tick and unchanged while shown; each A turns the page one tick later; the first page waits 4 ticks for the map's fade-in ([Fast text](#fast-text)).
- `play.ui_animations_normal`, `_fast`, `_instant`: at most 15 UIKit animations at once on the title menu with `normal` (the last at 1.30 s of the run) and `fast` (the last at 0.32 s); none with `instant`. Each run reaches the field of 2.8 from a new game.

The regression suite with each feature on (`ctest -L real_assets -j4`, Release arm64, 95 tests, `MAPLETHIEF_SETTINGS`; `platform.device_golden` skips locally in every run, off included):

| Setting | Result |
|---|---|
| none (all off) | 95 of 95 |
| `turbo_attack = on` | 95 of 95 |
| `auto_potion = 50` | 93 of 95: `input.menu_shops_save` (and `input.menu_shops`, which needs its save). The scripted bot of the hunt drinks its five Red Potions earlier, falls on 2.3, and its way back from town stays at (56, 271) in Henesys with full HP until the 600 s limit. A changed outcome of an Assist plus a limit of the bot, not a fault: with `auto_potion = 25` both pass. |
| `ui_animations = instant` | 94 of 95: `platform.title_60s`, which checks that the title's slide-in animates between 500 and 1200 ms. With no animation the frames are the same: the setting doing what it says. |

