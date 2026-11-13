# The original game's bugs

The bugs of MapleStory Thief Edition 1.5 itself, as the port found them, and what the port does with each ([#155](https://github.com/armaatus/maplethief-port/issues/155), epic [#105](https://github.com/armaatus/maplethief-port/issues/105)). A bug of the port is not listed here: it is fixed.

Addresses are armv7 VM addresses; function pointers carry the Thumb bit.

## The decisions

Each bug gets one of three decisions:

- **Fixed in Enhanced.** A guest function hook ([ADR 0069](../adr/0069-guest-function-hooks.md)) changes the one function, and only while the setting `fix_original_bugs` is on. That setting is on in the Enhanced preset and off in Original and Original + controller ([ADR 0070](../adr/0070-settings-registry-and-presets.md)); the flag is `--fix-original-bugs on|off`. With it off, the game does exactly what the device does, the bug included. This decision is for bugs that **end the game or leave the player stuck**, and, since [#379](https://github.com/armaatus/maplethief-port/issues/379), for **content defects**: content the game ships and reaches but gets wrong, where the game's own data shows what was meant (a warp to a portal that does not exist, a missing translation, a quest no script completes). A fix never changes balance: anything that changes gameplay belongs under Assists.
- **Neutralised in every mode.** The port's host side never lets the trigger reach the game, whatever the preset. This decision is for triggers the port's host creates itself (its clock, its files), where keeping the bug would make the port worse than the device without making it more faithful.
- **Kept.** The port does what the device does. Either nothing in normal play reaches the bug, or fixing it would change what the game is.

Content the game ships but never reaches (no map, NPC or script leads to it) is not a bug, and `fix_original_bugs` never touches it. Bringing it back is **restored content**: a setting of its own, `restored_content`, off in every preset, Enhanced included ([below](#unreachable-content); ADR 0069's #379 addendum). Neither setting ever changes the Original preset.

Each fix has a test with the setting off (the original behaviour) and on.

## The list

| # | Bug | Address | Trigger | Effect | Decision |
|---|---|---|---|---|---|
| 1 | [Play before the main menu](#1-play-before-the-engines-main-menu) | `-[MainMenuView clickPlay:]` `0xAAAB9`, `-[ChooseCharacterView drawRect:]` `0xA632D` (`objectAtIndex:` at `0xA6584`, return `0xA6589`) | Play released in the first 5 title ticks (250 ms at the default speed) | `NSRangeException`, the app ends | **Fixed in Enhanced** (hook on `0xAAAB9`) |
| 2 | [The portal items quit the game](#2-the-ngb-portal-items-quit-the-game) | `OpenPortalURLAndExit` `0x22955` → `0x8FF9` (`exit(1)`) | OK on the main menu's popup 1, sub-menu 3, items 2–4 | the app quits | **Fixed in Enhanced** (hook on `0x22955`) |
| 3 | [`GetTimeMs` jumps at 2³¹](#3-the-2-ms-jump-of-gettimems) | `GetTimeMs` `0xA0D5`, `CMTimer::OnRunLoopTimer` `0x21F51` | running across a 2³¹ ms crossing of the wall clock (every 49.7 days; next 2026-12-16 17:56:53 UTC) | no tick for 49.7 days: a frozen game | **Neutralised in every mode** (the port's clocks) |
| 4 | [Unchecked `config` bytes](#4-unchecked-config-bytes) | `CDataMgr::ApplyGameSpeed` `0x37C41`; `0x37B85`, `CStateGame::LoadGame` `0x86C1D` | `config` byte 1 or 5 above 2 (a hand-edited or damaged file) | byte 1 = 3: a tick on every 1 ms poll; larger: whatever follows the table | **Neutralised in every mode** (`mt_saves_sanitise`, #78) |
| 5 | [Saves overwritten in place](#5-saves-overwritten-in-place) | write `0xB785` → `0xB729` | the app killed or the disk full during a save | a half-written `theif`/`quest` | **Neutralised in every mode** (atomic writes, #78) |
| 6 | [The save loaders trust their fields](#6-the-save-loaders-trust-their-fields) | `CHero::LoadSave` `0x677E9`, `CStateGame::LoadGame` `0x86C1D`, `QuestSave_Apply` `0x83631` | a hand-edited or damaged `theif`/`quest` | out-of-range table reads, unknown | **Kept** |
| 7 | [Format loaders without bounds](#7-format-loaders-without-bounds) | `.map`, `.att`, `.mapprj`, `.sprprj`, bundle reads (`0xB689`) | a malformed data file | heap overflows, wrong tiles | **Kept** (no shipped file triggers them) |
| 8 | [Save quirks](#8-save-quirks) | `SaveSaveData` `0x38711`, `SaveQuestData` `0x38F55`, `SaveFile_ThiefSlotExists` `0xB6E9`, the receipt lists | every save | uninitialised bytes, a 1-byte record overlap, an always-false probe, stale bytes | **Kept** (save compatibility) |
| 9 | [A clock set back stalls the ticks](#9-a-clock-set-back-stalls-the-ticks) | `CMTimer::OnRunLoopTimer` `0x21F51` | the host's wall clock set back while the game runs | no tick until the clock catches up | **Kept** |
| 10 | [The main menu's quit item](#10-the-main-menus-quit-item) | `CStateMainMenu::OnKeyPressed` `0x9462D` → `0x25771` (`StopAllBGM`, `exit(1)`) | OK on the engine's popup 0 (main-menu item 4) | the app quits | **Kept** (the player asked to quit) |
| 11 | [OK on a quest pages a quest it never stored](#11-ok-on-a-quest-pages-a-quest-it-never-stored) | `PopupMenu_QuestTabOnKey` `0x5462D`, `PopupMenu_PaintQuestTab` `0x4ED75` (`0x4F178`, the fault at `0x4F1C4`) | OK (−5) on a row of the popup menu's quest tab: the port's keys | a memory fault, the app ends (SIGBUS); after a touch on another quest, the wrong quest's title | **Neutralised in every mode** (hook on `0x5462D`) |
| 12 | [The shop's keys stop at row 2](#12-the-shops-keys-stop-at-row-2) | `Shop_OnKey` `0x46DB9`, `Shop_Paint` `0x56E91` | up / down in an NPC shop: the port's keys | the cursor sticks at the third row, the list never scrolls | **Neutralised in every mode** (hook on `0x46DB9`, ADR 0102) |
| 13 | [The item and equipment grids' keys assume 6 columns](#13-the-item-and-equipment-grids-keys-assume-6-columns) | `CDataMgr::keyPopupItemInven` `0x5285D`, `PopupMenu_EquipTabOnKey` `0x519F9` | the d-pad on the popup menu's item or equipment grid: the port's keys | down skips to an empty cell; the drawn grid never follows | **Neutralised in every mode** (hooks on both, ADR 0102) |
| 14 | [`common/portal620` warps to a portal that does not exist](#14-commonportal620-warps-to-a-portal-that-does-not-exist) | `common/portal620.out` (`changeMap(6, 1, 2)`), the map change `0x868D5` (the object loop at `0x86A02`) | the portal on map 6.2 | the hero lands on 6.1 where it stood on 6.2, not at a portal | **Fixed in Enhanced** (to do, #388) |
| 15 | [A dialogue key missing in Korean](#15-a-dialogue-key-missing-in-korean) | `ko.lproj/Scripts.strings`; `chapter3/ZenumistChief.out`; `-[NSBundle localizedStringForKey:value:table:]` (host, `runtime/src/foundation/ns_bundle.cpp`) | the Zenumist Chief's first line with the language Korean | the box shows the key itself | **Fixed in Enhanced** (to do, #388) |
| 16 | [Chapter 1's main quest 20 is never completed](#16-chapter-1s-main-quest-20-is-never-completed) | `chapter1/Pirate.out` (`toOrbis`), `clearAll` `0x996ED` | chapter 1's end | main 20 stays in progress until `clearAll` resets it with every quest | **Fixed in Enhanced** (to do, #388) |

### 1. Play before the engine's main menu

Found in [#122](https://github.com/armaatus/maplethief-port/issues/122); the whole trace is in [ANALYSIS.md → Program flow](../ANALYSIS.md#program-flow).

- **Cause.** `-[EAGLView startApp]` `0x4501` adds the `MainMenuView` before the first tick, and Play's button (46, 148, 120, 48) is live from its first `drawRect:`. The engine enters `CStateMainMenu` only on `CStateTitle`'s 5th paint, and only `CStateMainMenu::Enter` `0x97201` → `LoadCharacterSlots` `0x96845` fills `EAGLView`'s `robinImages`. `-[MainMenuView clickPlay:]` `0xAAAB9` makes a `ChooseCharacterView` whatever the engine's state is, and its `drawRect:` `0xA632D` reads `robinImages[0]` with no bounds check.
- **On the device.** Play released in those 250 ms raises `NSRangeException`, and the app ends. Nothing is drawn at Play's place yet, so only a blind tap gets there.
- **In the port.** A tap or a click does the same (`platform.early_tap`). The controller focus has guarded its own confirm since [#129](https://github.com/armaatus/maplethief-port/issues/129) in every mode, because the focus highlight made the window easy to hit (ADR 0051).
- **The fix** (`mt_hooks_install_early_play_guard`, `runtime/src/hooks/builtin.cpp`). It hooks `-[MainMenuView clickPlay:]`. While the engine's current state (`SkyCanvas` `0x11A950`, `+0xC`) is still a `CStateTitle` (vtable pointer `0x119C98`, the same check as #129's), the hook drops the click and logs `hooks: -[MainMenuView clickPlay:] before the engine's main menu: dropped`. The original's only other effect is a pending OK, which the empty `CStateTitle::OnKeyPressed` `0x97E65` ignores. Any later click runs the original.
- **Tests.**
  - `runtime_test` "hooks: Play before the engine's main menu is dropped when on (#122)" (CI, synthetic guest): dropped in `CStateTitle`; passed with no canvas, in another state, and with the hook off.
  - `platform.early_tap` (real assets, the setting off): `--fixed-step 1 --tap 0.1,106,165` ends with exit 5 and `objectAtIndex:` from `0x000A6589`.
  - `platform.early_tap_fixed` (real assets, `--fix-original-bugs on`): the same tap is dropped, the run raises nothing, and Play at 4.5 s starts a new game (`Documents/theif0` written).

### 2. The NGB portal items quit the game

From [re/network.md](network.md#never-opened-the-nexon-portal-urls) and [#143](https://github.com/armaatus/maplethief-port/issues/143). `CStateMainMenu::OnKeyPressed` `0x9462D` passes `http://w.nexonmobile.com/CP/{winc581,gift,gamezone}.php?GV=35` to `OpenPortalURLAndExit` `0x22955`. It calls `0x6015`, an empty stub where the Korean Nexon Game Box hand-off was, and then `exit(1)` through `0x8FF9`. No URL is opened. Whether the English build can reach these items by touch is still open.

- **The fix** (`mt_hooks_install_portal_no_exit`, since #143). The hook replaces `0x22955`: it logs the URL and returns, and `OnKeyPressed` takes the switch's break after the call (`b 0x961A2` → `b 0x961EA`), so the menu stays. Discovery lifts the code after the three calls because the hook is marked `returns` (ADR 0069).
- **Tests.**
  - `runtime_test` "hooks: OpenPortalURLAndExit returns instead of quitting when on" (CI, synthetic guest).
  - `runtime.hooks_portal_on` / `runtime.hooks_portal_off` (real assets). They take the Enhanced and the Original preset's `fix_original_bugs` and send OK to items 2–4 through the game's own `OnKeyPressed`. On: the three calls are handled and the game goes on for 2 s. Off: `exit(1)`, as on the device.

### 3. The 2³¹ ms jump of `GetTimeMs`

From [re/timing-threading.md](timing-threading.md#the-clock). `GetTimeMs` `0xA0D5` is `(int32)(tv_sec * 1000 + tv_usec / 1000)` of `gettimeofday`. It jumps from +2³¹ − 1 to −2³¹ every 2³² ms (49.7 days of wall-clock time), for example at 2026-10-28 00:54:06 and 2026-12-16 17:56:53 UTC. A copy running or suspended at that instant sees `now − lastFrame` in `CMTimer::OnRunLoopTimer` `0x21F51` fall to about −2³². No tick fires for the next 49.7 days: no input, no drawing, until a relaunch.

- **The port's clocks.** Since the trigger is the host's clock, the port neutralises it in every mode:
  - **Fixed step** ([ADR 0044](../adr/0044-virtual-clock-and-device-golden-screens.md)): the virtual clock starts at 2025-01-01T00:00:00Z, `GetTimeMs` 522,812,416, 18.8 days of virtual time before the jump.
  - **Real time** (`LaunchClock`, `platform/src/clock.h`, #155): `gettimeofday` and `CFAbsoluteTimeGetCurrent` read the wall clock moved back at launch by 0 to 3 times 2³⁰ ms (at most 37.3 days), so `GetTimeMs` starts in [0, 2³⁰). The jump is then at least 2³⁰ ms (12.4 days) of running away. With `pacing = display` (#80, ADR 0075) the paced clock is the libc clock instead, and it carries the same shift in its start (`paced_start_unix_us`, `platform/src/pacing.h`); no `LaunchClock` is installed beside it.

  Before #155 the real-time mode read the host's wall clock as it is, and a session across 2026-12-16 17:56:53 UTC would have frozen.
- **What stays the device's.** The low 30 bits of `GetTimeMs` (the random seed still differs per launch), the gaps of a suspend or a sleep, and a clock set by hand ([9](#9-a-clock-set-back-stalls-the-ticks)). The date the guest sees is up to 37 days early, and nothing in the game shows it.
- **Tests.**
  - `platform_test` "clock: the launch clock starts GetTimeMs below 2^30, clear of its 2^31 jump" (CI): a launch 376 ms before the 2026-12-16 jump starts at `0x3FFFFE88`, a launch at any time starts in [0, 2³⁰) with its low 30 bits kept, and the installed clock is the libc clock.
  - "pacing: with pacing = display the guest's GetTimeMs starts below 2^30 (#155's shift)" (CI): the paced clock, installed as the libc clock, starts in [0, 2³⁰) for launches in each quarter of `GetTimeMs` and now, with its low 30 bits kept, and runs on without a jump when locked to 60 Hz.
  - "clock: the start keeps the game's GetTimeMs below 2^30" (CI) for the fixed step.
  - The original behaviour is not run: it takes 49.7 days.

### 4. Unchecked `config` bytes

From [formats/save.md](../formats/save.md#config-6-bytes) and [ADR 0074](../adr/0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (#78).

- **Byte 1, the speed.** `CDataMgr::ApplyGameSpeed` `0x37C41` indexes the tick lengths `{65, 50, 40}` at `0x108C58` with it unchecked. 3 reads the next word, the popup id `0x108C64` (−1 at start), so the timer fires on every 1 ms poll; larger values read whatever follows.
- **Byte 5.** It indexes `{0, 1, 3}` at `0x108C4C` in `0x37B85` and `CStateGame::LoadGame` `0x86C1D` the same way.

The options menu only writes 0–2, so the trigger is a hand-edited, copied or damaged file. In every mode `mt_saves_sanitise` reads a value above 2 as 2 and logs it, and the file is left as it is. Tests: `runtime_test`'s saves cases (CI) and `play.config_speed` (real assets: a `config` with byte 1 = 3 runs at the 40 ms speed) (#78).

### 5. Saves overwritten in place

The game writes a save at offset 0 of the existing file and calls `synchronizeFile` (`0xB785` → `0xB729`, `0x6C41`). The app killed, or the disk full, in the middle leaves a mix of old and new bytes, and there is no checksum to notice it. In every mode the port writes the bytes to `<name>.mt-tmp` and renames that over the file (`mt_saves_replace_file`, ADR 0074, #78; `play.save_quit`, `play.save_relaunch`). A file keeps its last saved bytes or gets all the new ones.

### 6. The save loaders trust their fields

The game writes only values it made, but it reads `theif` and `quest` back with no check ([formats/save.md](../formats/save.md)):

- `CHero::LoadSave` `0x677E9`:
  - `branch` and `job_tier` (bytes 0, 1) pick the hero sprite and rebuild the skill slots (`0x7FD69`);
  - the equipment ids (`0x015 + 16i`, `0x3C2 + 16i`) and etc ids (`0x325 + 3i`) go to the item tables' lookups (`0x69A51`, `0x699A1`) as they are;
  - the worn items are stored at `hero+0xDC[slot]` by `0x64CA1`, with the slot from the item's table row;
  - each skill level (`0x3B5 + i`) goes to `0x7F99D` with its last argument 1 (loading), which skips the clamps a level bought with skill points gets (the points left, and the maximum from `0x7EF98`);
  - the shortcuts (`0x412`) are copied as they are.
- `CStateGame::LoadGame` `0x86C1D` copies `area` and `map` (`0x41C`, `0x41D`) into the world and loads `<area>.mapprj` and `"%d%d.map"`.
- `QuestSave_Apply` `0x83631` restores the quest records by `key_a`/`key_b`.

What an out-of-range value does further down was not traced. **Kept**: only a hand-edited or damaged save gets there, never the game's own writes. Neutralising them would mean a save validator with every table's bounds, and changing a value the player put there on purpose. If damaged saves turn up in practice, `mt_saves_sanitise` is the place for such checks (ADR 0074).

### 7. Format loaders without bounds

The engine trusts its data files, and the shipped files are all well-formed. Each parser in `tools/mtport/` checks every shipped file (the local "parse every shipped file" tests). **Kept**:

- `.map`: a run or literal crossing the end of a layer writes past `new i16[cells]`, and a truncated file stops the load with the rest of the cells uninitialised ([formats/map.md](../formats/map.md#decode-routine-0xd6d1)).
- `.att`: `i` is never compared with `attW · attH`, so a long file overflows the grid ([formats/att.md](../formats/att.md)).
- `.mapprj`: the multi-sheet tile walk's `− 1` puts global tile 160 on tile 1 of the second sheet, and tile 0 of every later sheet is unreachable. No shipped data takes that path ([formats/mapprj.md](../formats/mapprj.md)).
- `.sprprj` and the other bundles: counts and lengths are trusted, and `0xB689` reads a bundle file with no bounds at all ([formats/sprprj.md](../formats/sprprj.md), [formats/obj.md](../formats/obj.md)).

### 8. Save quirks

From [formats/save.md](../formats/save.md). **Kept**, because a real device's saves have to load in the port byte for byte and none of these changes what the player sees:

- `SaveSaveData` `0x38711` never writes `theif` bytes `0x009`–`0x00C` and `0x315`–`0x324`, and `SaveQuestData` `0x38F55` never writes `quest` bytes `0x12A4`–`0x12AC`. They hold whatever was on the heap. Nothing reads them.
- Each quest record writes 0x1CA bytes into a 0x1C9 stride: the last byte of `rewards[4].value_2` is the next record's `key_a`. This matters only for a quest with a fifth reward of kind 2.
- `SaveFile_ThiefSlotExists` `0xB6E9` probes `thief%d`, spelled correctly, for slots 0 and 1. The game only writes `theif%d`, so the probe is always false.
- A receipt list rewritten shorter keeps its old bytes after the new end, because files are never truncated. The count in front governs what is read.

### 9. A clock set back stalls the ticks

From [re/timing-threading.md](timing-threading.md#the-clock). If the wall clock moves back while the game runs, `now − lastFrame` is negative, and no tick fires until the clock has caught up again. A clock moved forward gives one tick at once and adds the jump to every running sub-timer. **Kept**: the port's real-time clock follows the wall clock as the device's did ([3](#3-the-2-ms-jump-of-gettimems)). Only a clock set by hand moves that far: network time adjusts gradually. A monotonic clock would avoid it, but would also hide the gaps of a sleep from the game's timers.

### 10. The main menu's quit item

`CStateMainMenu::OnKeyPressed` `0x9462D`: item 4 of the engine's main-menu list opens popup 0, and its OK calls `0x25771`, which stops the music (`CMSoundPlayer::StopAllBGM`) and calls `exit(1)` through `0x8FF9`. On iOS an app that quits itself is against the platform's conventions. Still, this is a quit the player asked for and confirmed. **Kept**: the port ends the run, as on the device. Like the portal items, the item belongs to the engine's own menu list, behind the UIKit `MainMenuView`, and whether the English build can reach it is open.

### 11. OK on a quest pages a quest it never stored

Found by [#164](https://github.com/armaatus/maplethief-port/issues/164)'s real-asset runs and traced in [#167](https://github.com/armaatus/maplethief-port/issues/167).

- **Cause.** A quest's page in the popup menu's quest tab (`CDataMgr +0xAC` = 1) is painted by `PopupMenu_PaintQuestTab` `0x4ED75` from two fields: the quest in `CDataMgr +0x530` (read at `0x4F178`; its +0x14 at `0x4F1C4`, its title and requirements after) and the page text the opener made in `+0x110`. The two ways to open the page do not store the same things:
  - a double tap on a row goes to `PopupMenu_QuestTabOnTouch` `0x502D9` (from `CDataMgr::keyPopupItemInven_2` `0x554F1`). It looks the quest up, stores it in `+0x530` (`0x504A2`) and its number in `+0xE4`, and builds `+0x110`;
  - OK on the cursor's row goes to `PopupMenu_QuestTabOnKey` `0x5462D` (from `Menu_OnKey` `0x53B5D`). It looks the quest up the same way (`QuestMgr_MainQuest` `0x7A19D` when byte `+0xB4` is set, else `QuestMgr_SideQuest` `0x7A10D` with `+0xE4` plus the scroll's first row, `0x548D4`–`0x5491C`), builds `+0x110` and sets `+0xAC`, but **never stores the quest in `+0x530`**. Nothing else writes that word.
- **On the device.** The keys never reach the quest tab. The popup menu's key pad panel (1) has no buttons, and the widget event that sends −5 (`TouchWidget_OnEvent` id 11, value 9) is on no item ([controls/keypad.md](../controls/keypad.md#gl-touch-widgets-popup-menu)). So the page is only ever opened by a touch, which stores the quest first.
- **In the port before #167.** The controller and keyboard send OK there (ADR 0048, #146). With no touch on a quest before, `+0x530` holds whatever the heap had (`0x26BC2856` in the run from `play.chapter_1_c`'s save), and the paint's read of its +0x14 faults: `Bus error: 10` in Release, `guest memory fault (kind 12) at pc 0x0004F1C4, detail 0x26BC286A` with checked memory (Debug). After a touch on another quest, OK pages the touched quest's title over the chosen quest's text.
- **The fix** (`mt_hooks_install_quest_page_fix`, `runtime/src/hooks/builtin.cpp`). It wraps `0x5462D`: it runs the original and, when that call opened the page (`+0xAC` 0 before, 1 after), looks the quest up as the original did, with the same two lookups, and stores it in `+0x530`. Any other key, and OK with the page already open, is the original alone. The trigger is the port's own input, which the device never sends there, so the hook is **on in every mode**: the game installs it at start-up (`game.cpp`), and so do the scripted runs (`tests/cpp/game_driver.h`). A touch on a row is unchanged. The fix is the function's handler; #166's list scroll wraps it (ADR 0069's #166 x #167 addendum), so a key runs the original, then the fix, then the scroll.
- **Tests.**
  - `runtime_test` "hooks: OK on the quest tab stores the quest whose page it opened (#167)" and "hooks: with the quest page fix off, OK leaves the data manager's quest unset" (CI, synthetic guest with stand-ins for the two lookups): the main quest, a scrolled side quest, a row with no quest, OK on an open page, B, and the original's result and r4–lr kept. "hooks: a wrapper around the quest page fix runs after it, the fix on or off": a wrapper on the same function sees the stored quest.
  - `play.popup_menu_tabs` (real assets, from `play.chapter_1_c`'s save): A on the main quest (Missing Master, 0/10) and on both side quests (Alex's Worry 1/21, Missing Father 1/22) opens each one's page with that quest in `+0x530`, and B closes it back to the list. A double tap on the main quest's row (the iOS way) opens its page too.
  - Without the hook, the same flow (A on the main quest) ends with the fault above.

### 12. The shop's keys stop at row 2

Found by Joris's controller play (2026-11-04), [ADR 0102](../adr/0102-controller-lists-and-the-character-select-hand-off.md).

- **Cause.** `Shop_OnKey` `0x46DB9` moves the tab's cursor (`shop +0x60 + 4 × tab`) inside the key scroll's view (`*(shop +0x114)[tab]`: +4 the first row, +0x14 the same in points). `Shop_Paint` `0x56E91` rewrites that view every frame from `MarketKeyPadView`'s touch drag (moved point + scroll point, unless `|forceScrollValue|` ≥ 2), clamped to [38 × (5 − rows), 0]. The keys never move the drag, so the view snaps back and the cursor cannot pass the third row. The list never scrolls.
- **On the device.** Panel 6 sends no up or down, and a touch picks a row. The keys are the port's (ADR 0048).
- **The fix** (`platform/src/list_keys.cpp`, on in every mode). A hook on `0x46DB9` takes up and down while no popup is open. It moves the cursor one row within the tab's list and the drag just far enough to show it, then calls `ResetSubTimers01` `0x386E1` as the original does. Other keys run the original; a tab change puts the drag back at the top.
- **Tests.** `platform_test` `list keys: the shop's *` (synthetic). `input.menu_shop_scroll` (real assets): with the hook off, five downs leave the cursor on row 2; with it on, the cursor reaches row 5 with row 1 drawn first, buys there, and walks the Sell tab to its last row.

### 13. The item and equipment grids' keys assume 6 columns

Found with 12, [ADR 0102](../adr/0102-controller-lists-and-the-character-select-hand-off.md).

- **Cause.** `CDataMgr::keyPopupItemInven` `0x5285D` (cursor `CDataMgr +0xD8`) and `PopupMenu_EquipTabOnKey` `0x519F9` (`+0xDC`) step 6 cells for up and down and scroll `*(CDataMgr +0x10C)` past the rows they count as shown. The grids (`*0xF14B8`, `*0xF14C0`) are drawn 4 to a row and scrolled by the widget's drag (+0x6C) only. Down from cell 0 lands on cell 6, off the drawn row, and the drawn grid never follows.
- **On the device.** No key reaches these handlers; a touch picks a cell.
- **The fix** (`platform/src/list_keys.cpp`, on in every mode). Hooks on both handlers take the four directions while the grid is shown and has cells, and neither the shortcut panel (`+0xCC`) nor the quick slot choice (`+0xBC`) is open. Left and right go one cell, up and down to the nearest cell of the row drawn above or below; ends hold; up from the top row runs the original (back to the tab bar). The cursor is the cell's index with the key scroll at 0, and the drag follows the cell within the engine's clamp.
- **Tests.** `platform_test` `list keys: grid moves *`, `list keys: the item grid's keys *` (synthetic). `input.menu_popup_grids` (real assets): with the hooks off, down goes to cell 6 of 4; with them on, the d-pad walks the 4 cells and up returns to the tab bar.

### 14. `common/portal620` warps to a portal that does not exist

From [ANALYSIS.md → Script VM](../ANALYSIS.md) (reachability) and [#379](https://github.com/armaatus/maplethief-port/issues/379).

- **Cause.** Map 6.2 has one portal, `portal620` (kind 0). Its script, `common/portal620.out` (no chapter overrides it), calls `changeMap(6, 1, 2)`: map 6.1, portal 2. Map 6.1 has portals of kind 0 (`portal610`, to 6.0) and kind 1 (`portal611`, which leads to 6.2's portal 0). No object of kind 2 exists on it. The way back from 6.2 was clearly meant to be portal 1, the one that leads there.
- **What the game does.** The map change `0x868D5` loads 6.1 and walks its objects for a `kind` equal to the target portal (`0x86A02`–`0x86A1A`); only a match writes the hero's x and y (`hero +4`, `+8`) and moves the camera. With no match nothing is written, so the hero keeps the coordinates it had on 6.2, about (56, 751): on 6.1 that is 32 points above `portal610` at the map's left edge, instead of at `portal611` on the right. This is read from the routine's code; no run has taken the portal yet. The hero is not stuck: 6.1's portals still work.
- **In the port today.** The same as the device in every mode.
- **The repair.** A hook on `CMapleWorld::changeMap` `0x81369` (or on the native's handler `0x99969`): with the setting on, a warp to (6, 1, 2) becomes (6, 1, 1). Touched: one hook, no data. **Risk: low.** Only where the hero appears changes; nothing else warps to 6.1's portal 2.
- **Tests (to do).** A synthetic `runtime_test` case for the rewrite, on and off; a real-asset run that takes `portal620` and reads the hero's position on 6.1 with the setting off (the 6.2 coordinates) and on (at `portal611`).

### 15. A dialogue key missing in Korean

From [ANALYSIS.md → Script VM](../ANALYSIS.md) (dialogue text).

- **Cause.** All 1,761 key-shaped constants in the scripts resolve in `en.lproj/Scripts.strings`. In `ko.lproj` one does not: `Chapter3_Quest_Zenumist_Chief.dialog.1`, the first line of the Zenumist Chief (`chapter3/ZenumistChief.out`). `regMsgNPC` localises the key at once through `LocalizedCString` `0x6DD1`, and `-[NSBundle localizedStringForKey:value:table:]` returns the key when the table lacks it.
- **On the device.** A Korean device shows the key in the Chief's speech box. English (and every other language, which falls back to English) is not affected.
- **In the port today.** The same: the host's `bundle_localized_string` returns the key when neither a text pack (ADR 0108) nor the language's table has it.
- **The repair.** In the host's lookup, with the setting on, a key missing from the language's table falls back to the English table's string, as iOS does for a key a localisation lacks when the development region has it. Touched: `runtime/src/foundation/ns_bundle.cpp` and the setting passed to the CF layer; no hook. **Risk: low.** One line of the Chief shows in English on a Korean game instead of a key; the English game is unchanged. Writing a Korean line of our own would add text the game never had, so it is not the repair.
- **Tests (to do).** A synthetic bundle with a key in `en` and not in `ko`: the key with the setting off, the English string with it on.

### 16. Chapter 1's main quest 20 is never completed

Found by [#162](https://github.com/armaatus/maplethief-port/issues/162) (`play.chapter_1_k`, `_l`; [testing/chapter-1-quests.md](../testing/chapter-1-quests.md)).

- **Cause.** Stan gives main 20 (Master in Danger 2) after main 19. The Pirate (2.0) offers the trip to Orbis once side 11 is done, and `toOrbis` calls `clearAll` `0x996ED`, which sets every quest back to 0 and starts chapter 2. No script calls `getReward` for main 20 or sets it to 2, so it is the one main quest that never reads 2. The other chapters' last main quests without a `getReward` (chapter 0's 6, chapter 2's 9, chapter 3's 8) are finished by a scene or the chapter's end, as main 20 is, but main 20's own step (go to the Pirate) leads straight into the reset.
- **On the device.** Main 20 is in progress (1) in the quest tab right up to the trip; the chapter goes on normally. Nothing is lost: main 20 has no rewards registered.
- **In the port today.** The same; `play.chapter_1_l` checks `main 20:1` before the trip and 0 after it.
- **The repair.** With the setting on, main 20 is completed through the game's own completion path when the hero accepts the trip, before `clearAll` runs, so the player sees it complete as the chapter ends. Touched: a hook on the `clearAll` native's handler `0x996ED` that completes chapter 1's main 20 first while it is active (1), or an in-memory override of `chapter1/Pirate.out`'s `toOrbis`. The hook is simpler and leaves the script bytes alone. **Risk: low to medium.** The visible effect is small (what a completion shows was not traced: a notice, the quest tab entry), and `clearAll` resets the state either way, so saves and chapter 2 are unchanged. The follow-up first traces what setting a quest to 2 shows, and records it here.
- **Tests (to do).** A real-asset run from `play.chapter_1_k`'s save: off, main 20 reads 1 before `clearAll`; on, it reads 2 there, and chapter 2 starts on 4.1 as before.

## Unreachable content

Content the game ships but no reachable path leads to ([#379](https://github.com/armaatus/maplethief-port/issues/379)). It is not a bug, and `fix_original_bugs` leaves it alone. With the setting `restored_content` on (off in every preset; ADR 0069's #379 addendum), the port may make it reachable ([#389](https://github.com/armaatus/maplethief-port/issues/389)). In every mode, today, the port does what the device does: none of it is reachable.

| Content | What exists | What a restore touches | Risk |
|---|---|---|---|
| [Monster Carnival](mechanics.md#monster-carnival-the-level-scaling-mode) | The three rooms (maps 0.5, 4.18, 6.3), the rules (`CStateGame::StartMonsterCarnival` `0x82271`, `resultMonCa` `0x819E5`), the tickets (`item_etc` 47–49) and the scripts that start it, `common/Spiegelman1`–`3.out`, which no map object names | An NPC object added in memory to one map's object list at load, naming the `Spiegelman` scripts, on a map the game itself suggests (where is not known: the follow-up looks for the intent in the data and the strings) | **High.** The place is a guess; the rules have a sign slip ([mechanics](mechanics.md#monster-carnival-the-level-scaling-mode)); the EXP rewards change the level curve; whether the tickets can be obtained was not checked. The no-save rule already covers the rooms (ADR 0089) |
| Shops 0, 1 and 16 | 61, 30 and 7 `market.dat` listings; the shop screen `0x4628D` opens any shop number. Shop 0 also holds one listing of shop 8's (row 146, a data slip) | An extra choice in an existing merchant's list that calls `showMarket` with the shop number, by an in-memory override of a `common/Equip*` or `Etc*` script, or a hook on the shop's opening | **Medium.** The stock may be a test or a cut tier; prices and items change the economy. Who sells it is a guess |
| The 21 unreached maps | 90 maps, of which chapter 3 reaches 69. Among the 21: the three Carnival rooms and the maps of the 4 portals no chapter reaches (`portal450`, `portal451`, `portal480`, `portal481`); the compendium's map pages list each one | Per map: a portal or warp added in memory, or an existing portal script's target changed | **High,** and per map: some may be unfinished (no objects: 4.18 and 6.3 have none), and a dead end or a fall with no way back would leave the player stuck. Each map gets its own check before it is restored |
| The Pigmy hatch's online part | The dialog's states (`CStateGame::SetPigmeState` `0xF` → `0x10`) and its failure text ([network.md](network.md)). No packet is ever sent: the online part was cut from the iOS build | Nothing to restore: the client has no request for it, and what the server did is unknown | **Out of bounds.** Inventing it would add content the game never had. It stays the device's offline failure in every mode |

## Adding a fix

1. Name the function in `recomp/hooks.py` (`HOOKED`; `returns=True` if it never returns), `MT_HOOK_FN_*` in `runtime/include/maplethief/hooks.h` and the line in `k_hookable` (`runtime/src/hooks/hooks.cpp`). Regenerate `work/gen` (`python -m recomp all`).
2. Write the handler in `runtime/src/hooks/builtin.cpp` and register it from `mt_hooks_install_original_bug_fixes`, so `fix_original_bugs` switches it. A fix neutralised in every mode (a trigger only the port creates, as in [11](#11-ok-on-a-quest-pages-a-quest-it-never-stored)) gets an install function of its own, called with 1 from `game.cpp` and from `tests/cpp/game_driver.h`'s boot.
3. Add a test that shows the original behaviour with the setting off and the fix with it on (synthetic in `runtime_test` where possible, real assets where needed), and a row above.
