# Quality of life: the candidates and their verdicts

The small comfort options of [#90](https://github.com/armaatus/maplethief-port/issues/90): where each would hook into the game, what it risks, what it costs, and what the port does. The three judged cheap and safe are implemented, each behind a setting that is off by default ([ADR 0092](../adr/0092-quality-of-life-assists-and-ui-animations.md)). Addresses are armv7 VM addresses; functions carry the Thumb bit. The evidence is the generated code of the 1.5 binary (`work/gen`), the decompiles named in the linked documents, and the real-asset runs under [Verification](#verification).

## Verdicts

| Candidate | Hook point | Risk | Effort | Decision |
|---|---|---|---|---|
| Turbo attack | None in the game: the frontend re-presses the attack action while it is held ([assists.h](../../platform/src/assists.h)) | Low. The game's own rules apply to each press. Never in menus, where attack is OK. | S | **Done:** `turbo_attack` (Assists, off) |
| Auto-potion | None in the game: the frontend taps the quick slot that holds an HP potion, read as `CHero::KeyAttack` `0x66DD9` reads it | Low. A real key press, so the game decides; it changes play, hence an Assist. | S | **Done:** `auto_potion = off \| 25 \| 50 \| 75` (Assists, off) |
| Faster UIKit menu animations | The host's `+[UIView commitAnimations]` (`runtime/src/uikit/view.cpp`), not guest code | None found. Only the presentation changes; no game code waits on an animation. | S | **Done:** `ui_animations = normal \| fast \| instant` (Video, normal in every preset) |
| Auto-run and hold-or-toggle | The frontend's action layer (`Actions`), like turbo | Low, but needs a rule per action: which actions may latch, what cancels a latch (a menu, a popup, the other direction). | M | **Follow-up:** [#172](https://github.com/armaatus/maplethief-port/issues/172) |
| Fast text | `CChatBox::paint` `0x32BBD` draws the NPC dialogue every frame. Whether its text appears letter by letter, and from which counter, is unknown. | Medium: the chat box drives the NPC panel's buttons and quest rows from the same function. | M (research first) | **Follow-up:** [#173](https://github.com/armaatus/maplethief-port/issues/173) |
| Cutscene skip | The scene runner `CStateGame::PaintExitToMenu` `0x83C71` steps a `CAniManager` command list ([audio.md](audio.md#scenes)); `StartScene` `0x853E5` and `QueueScene` `0x80935` start them (game modes 9 and 10, [autosave.md](autosave.md)) | High: scenes set quest flags, move the hero, change maps and start boss fights in their commands. Skipping must still run every state-changing command. | M–L | **Follow-up:** [#174](https://github.com/armaatus/maplethief-port/issues/174) |
| Auto-loot | The drop update's contact test: `CDropItems::Update` `0x5BC75` asks `CMapleObject_Touches` `0x6CF01` (its call at `0x5BCEE`) whether the hero's box meets each lying drop's ([below](#auto-loot)) | Low through that test: only the rect of one call changes, and the pick-up after it is the game's. Moving drops or calling the pick-up from the host (rejected) would change the drop list the game walks. | M | **Done** ([#175](https://github.com/armaatus/maplethief-port/issues/175)): `auto_loot = off \| on` (Assists, off; [ADR 0097](../adr/0097-auto-loot-through-the-games-contact-test.md)) |
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

## Verification

Synthetic (CI):

- `platform_test`, `assists:` cases: the quick slot found as `KeyAttack` reads it (an empty slot, MP only, none left, a scroll, a skill), the field guards (popup, popup menu, another state, dying, dead, unmapped memory), the threshold arithmetic, the cooldown, the pulses and their guards, the runtime's hold kept through a pair, the rows and `MAPLETHIEF_SETTINGS`' list.
- `objc_test`, `views: the host's animation scale …`: a quarter and 0, the clamp and the refusal of bad values.
- `platform_test`, `assists: auto_loot …` cases: the reach and the half-open box test; only the drop update's return address widened (the five other callers and a near miss pass); the stack argument restored; nothing in a popup, the popup menu, another state, with the hero dead or a menu's panel; off is the original test.

Real assets (local, Release arm64, `play_real_test`, a new game to the tutorial's map 2.8):

- `play.auto_potion` (each tap 2 ticks): quick slot 1 holds the Red Potion (item 3, +50, 5). HP set to 2 of 10: off, nothing in 3 s; `auto_potion = 50`, one Red Potion drunk through slot 1, HP 2 → 10, none after it. HP low with the popup menu open: nothing until it closes, then the next potion.
- `play.turbo_attack`: X held 3 s: 1 attack with turbo off, 10 with it on (9 repeats). No repeat with X held in the popup menu.
- `play.auto_loot` (#175; after the tutorial, on 2.3, the bot hunting): off, 3 pick-ups, each in contact (gap 0 × 0 between the hero's box and the drop's, both at `+0x40` from that tick's test); turned on with the hero standing, the drop 27 points from its box was taken (x 730 → 730, meso 544 → 562, items 10 → 11); on, 10 pick-ups, 6 across a gap (no gap wider than the reach); both inventories full (the free places pointing at an item the hero has, every stack at its maximum: the test's writes, undone after), 3 drops refused, 2 of them out of contact, all 3 still on the ground, no item taken (4,800 → 4,800), meso 408 → 444.
- Off is unchanged byte for byte: `play.first_map` on the epic tip (`23d23ca`) and on #175's branch with the setting off gave the same 1,331 log lines (18,000 ticks; 197 drops, 160 picked up, meso +1,128, items +93) and the same 34 output files (screenshots, saves, `play.log`).
- `play.ui_animations_normal`, `_fast`, `_instant`: at most 15 UIKit animations at once on the title menu with `normal` (the last at 1.30 s of the run) and `fast` (the last at 0.32 s); none with `instant`. Each run reaches the field of 2.8 from a new game.

The regression suite with each feature on (`ctest -L real_assets -j4`, Release arm64, 95 tests, `MAPLETHIEF_SETTINGS`; `platform.device_golden` skips locally in every run, off included):

| Setting | Result |
|---|---|
| none (all off) | 95 of 95 |
| `turbo_attack = on` | 95 of 95 |
| `auto_potion = 50` | 93 of 95: `input.menu_shops_save` (and `input.menu_shops`, which needs its save). The scripted bot of the hunt drinks its five Red Potions earlier, falls on 2.3, and its way back from town stays at (56, 271) in Henesys with full HP until the 600 s limit. A changed outcome of an Assist plus a limit of the bot, not a fault: with `auto_potion = 25` both pass. |
| `ui_animations = instant` | 94 of 95: `platform.title_60s`, which checks that the title's slide-in animates between 500 and 1200 ms. With no animation the frames are the same: the setting doing what it says. |

