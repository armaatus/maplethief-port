# 84. Offline polish: the dead online UI hidden, Game Center on this computer

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #154
- **Builds on:** [ADR 0026](0026-objective-c-strategy.md) (host overrides of guest methods), [ADR 0034](0034-offline-services.md) (GameKit emulated, `gamecenter.json`), [ADR 0049](0049-menus-online-buttons-language-and-menu-flows.md) (the online buttons refused, the menu flow tests), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (the game events of the observed functions), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry and the presets)

## Context

The title menu (`MainMenuView`, [re/objc-shell.md](../re/objc-shell.md#mainmenuview)) still offers what the online iPhone game offered in 2010. Since ADR 0049 none of it does harm, but it does nothing either:

- **The promo banner** (`MainMenuAD.png`, `-clickADButton:`) and its close button cover the lower third of the title. The banner advertises *MapleStory Live* in the App Store.
- **More Games** (`-clickMoreGame:`) and **About's four links** (`-clickLinkURL1:`–`4:`, invisible buttons over the About text) open Nexon pages that are gone ([re/network.md → URLs](../re/network.md#urls)). The port refuses `openURL:` (ADR 0049), so a click does nothing.
- **Game Center** (`-clickGameCenter:` → `-[EAGLView viewGameCenterRank:]` `0x4E09`) presents a `GKLeaderboardViewController`, which ADR 0034 finishes on the next pump. The menu flickers back and nothing is shown. The scores the game reports (`CMapleWorld::changeMap` `0x81369`, `CHero::LevelUp` `0x65089`: `(level x 999000 + EXP) / 1000` to `com.nexonmobile.maplestory.thief.totalranking`) are saved in `gamecenter.json` but never seen.
- **Achievements:** the shipped game reports none (ADR 0034).

Facts from the decompiles (Ghidra, 2026-10-30):

- `-[MainMenuView initWithFrame:]` `0xAD9F9` creates the image views: `moregame` (ivar +152, `MainMenuMoreGame.png`, the button's picture) and, only if `-[EAGLView getGameCenter]` `0x4F25` (`m_bIsGameCenter`), `gamecenter` (+156).
- `-[MainMenuView drawRect:]` `0xABC85` creates every button through `-addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:actionForTouchDown:actionForTouchUpInside:` `0xAB58D` (r2, r3: the image names; on the stack: the frame, the insets, the touch-down and the touch-up selectors; returns the `UIButton`). The banner's buttons go to ivars +140 (`cygnusAD`) and +144 (`cygnusADClose`). Game Center's button is at (376, 262, 57, 57), More Games' at (436, 262).
- `-clickAbout:` `0xA8A0D` builds About's links with the same method.
- The title (`CStateTitle`) has no UI of its own: "the title" and "the main menu" are both `MainMenuView`.

#154 asks for these to be hidden or replaced in Enhanced, with local leaderboards and achievements, and with Original as it was.

## Options considered

1. **Leave them (ADR 0049).** Honest to the original offline, but the banner hides a third of the title and four buttons are dead.
2. **Patch the generated code or the nib.** `drawRect:` is 7.5 KB of recompiled code. Skipping its calls would mean a recompiler patch per button, and the generated code must stay a pure function of the binary.
3. **Host overrides that wrap the two creating methods (chosen).** Run the game's own method, then send `setHidden:YES` to what it made when that is dead. The host UIKit neither draws, hits nor focuses a hidden view (`touch.cpp`, `focus.cpp`, the compositor), and the game never asks. Nothing else changes: the layout, the animations and the live buttons are the game's.
4. **Draw replacement art over them.** Needs art the repo cannot ship (game art stays out of git).

For the leaderboard:

- **(a) A UIKit host view inside `GKLeaderboardViewController`.** It would need a table view and fonts the port does not have, and it would land in `--shot`s and goldens.
- **(b) Drawn by the frontend (chosen).** GameKit keeps the controller presented while a host hook shows the scores. The frontend draws them with SDL's debug text into the window only, like the price line (ADR 0072) and the frame-time overlay (ADR 0071). A key, a pad button or a click closes it, and the game dismisses its controller as on the device.

## Decision

### Two settings (Gameplay)

| Key, flag | Original | Original + controller | Enhanced | What it does |
|---|---|---|---|---|
| `hide_online_ui`, `--hide-online-ui` | off | on | on | hides the banner and its close button, More Games (button and image) and About's four links; with `local_game_center = off`, Game Center's button and image too |
| `local_game_center`, `--local-game-center` | off | off | on | Game Center's button opens the local leaderboard; local achievements unlock, with a toast |

- `hide_online_ui` follows ADR 0070's planned row. Off is the default, so a run without a settings file shows the menu as before. The defaults now differ from Original + controller in this key as well (`presets: no settings file is today's defaults` pins both lists).
- With `hide_online_ui = on` and `local_game_center = off`, Game Center's button would still close at once, so it counts as dead and is hidden.
- With both off nothing is installed: no override, no listener, no hook. The boot's override count and every Original run are unchanged.

### The dead UI: two wrapping overrides on `MainMenuView`

`runtime/src/offline/offline_ui.cpp`, `maplethief/offline.h`. The frontend sets the flags before the boot (`mt_offline_ui_configure`), and the boot calls `mt_offline_ui_install` after `mt_objc_install_overrides` (ADR 0026). Each override saves the method's guest IMP and calls it with the sender's registers and stack (`lr` set to `MT_HLE_RETURN`), then:

- `-initWithFrame:` hides the `moregame` image view (`MT_OFFLINE_HIDE_LINKS`) and the `gamecenter` one (`MT_OFFLINE_HIDE_GAME_CENTER`);
- `-addButtonWithImageName:…actionForTouchUpInside:` hides the returned button when its touch-up action is `clickADButton:`, `clickADClose:`, `clickMoreGame:` or `clickLinkURL1:`–`4:` (links), or `clickGameCenter:` (Game Center).

A missing `MainMenuView` is logged and nothing is installed. The log says what is hidden (`offline: 2 MainMenuView overrides hide …`).

### The local leaderboard

- **GameKit** (`runtime/src/stubs/gamekit.cpp`) keeps, besides ADR 0034's one score per category, a score per category **and player**. `gamecenter.json` gains `"entries": [{"category", "player", "best", "last", "reports"}]`. The array is written only when it has entries, so a file without it is written as before, and format version 1 still reads both ways.
- **The player** is the local player's alias (`mt_gamekit_set_player_alias`; default `Player`). With `local_game_center = on`, loading or creating a character in save slot *n* sets it to `Slot n+1`, so each slot has its own row.
- **The hook.** `mt_gamekit_set_leaderboard_hook(fn, user)`: with a hook, `-[GKLeaderboardViewController viewDidAppear:]` calls it with the category and keeps the controller (retained) until `mt_gamekit_leaderboard_close`. Only then does the delegate get `leaderboardViewControllerDidFinish:` on the next pump. Without a hook it finishes at once (ADR 0034). A new runtime drops a controller that is still waiting.
- **The screen** (`platform/src/local_game_center.{h,cpp}`, `LocalGameCenter`) lists the category's entries, best first, at most 20 rows: rank, player, best, last and reports, or a line saying no score was reported yet. While it is open it takes every input event. Esc, Backspace, Enter or Space, the pad's South, East or Start, or a click or a tap closes it on the release, so the game sees neither the press nor the release. It is drawn after the screen image and every read-back, so it is never in a `--shot`, a golden or a `--capture`.

### Local achievements

`runtime/src/offline/achievements.cpp`. The game has none, so the port defines a small table. Achievements unlock from the observed functions' game events (ADR 0053, a listener installed only with `local_game_center = on`) and are saved as GameKit achievements at 100 % (`gamecenter.json`'s `achievements`, so Game Center's "never lowers" rule applies):

| Identifier | Title | Unlocks on |
|---|---|---|
| `maplethief.local.new_character` | A New Thief | a new character (`CDataMgr::LoadSaveData` of an empty slot) |
| `maplethief.local.level_10` | Rookie | level-up to 10 or more |
| `maplethief.local.level_30` | Seasoned Thief | level 30 |
| `maplethief.local.level_50` | Veteran | level 50 |
| `maplethief.local.level_70` | Master Thief | level 70 |
| `maplethief.local.first_fall` | Knocked Out | the death popup (`0x68`) |
| `maplethief.local.revived` | Back on Your Feet | a revive (`0x6140D`) |

Each unlocks once. A new unlock queues a notice (`mt_achievement_next_notice`, at most 8 waiting), and the frontend shows it as a toast in the window's top right corner for 4 s, one after the other. `achievement_rows()` and `achievement_summary()` in `local_game_center.h` give #89's settings menu the list.

### What stays

- **The cash shop tile** with `cash_shop = off`: it is an in-game GL key pad panel, not the title or main menu. Its restore path still works, and a cancelled purchase is the device's own offline behaviour (ADR 0034, ADR 0072).
- **The in-game "Save data to server" popups** are the game's own and are not touched.
- **The About text** still names the Nexon addresses: it is text, not a link.

## Evidence

- `tests/cpp/stubs_test.cpp` (`objc_test`, CI): `GameKit: the local leaderboard keeps each player's scores across a restart` (scores under two aliases, a new runtime reads the same entries, best first, per category), `GameKit: with the host's hook the leaderboard waits until the host closes it`, `local achievements: the game events unlock each once, it persists and queues a toast`, `offline UI: the dead online buttons and images of MainMenuView are hidden` (a synthetic host `MainMenuView`: the originals called with their arguments, dead buttons and views hidden, live ones not, nothing installed without flags).
- `tests/cpp/local_game_center_test.cpp` (`platform_test`, CI): the leaderboard's lines from a synthetic `gamecenter.json`, the achievements list and toasts, the screen's input and its drawing on a software renderer. `presets_test.cpp`: the two rows' columns.
- `menus.offline_ui` (`tools/mtport/menuflows.py`, real assets, local): `--preset enhanced` and `--hide-online-ui on` against Original. The titles differ from Original's. Taps where the banner, its close button, More Games and the About links were press nothing, open no URL and leave the screen as it was. In Enhanced, Game Center opens the local leaderboard, a tap closes it and the game dismisses its controller. With `hide_online_ui` alone, the Game Center button is gone and cannot be pressed.
- Real assets, local: `play.save_quit` writes the total ranking's entry (7 reports, best 4,995 at level 5) to `gamecenter.json`.

## Consequences

- Enhanced's title shows Play, Help, About and Game Center over the full art. Original is unchanged, byte for byte.
- `gamecenter.json` may hold `entries`. An older port ignores the key and writes it away on its next save. That only loses the per-player rows, never the scores ADR 0034 keeps.
- The achievements are the port's, not Nexon's. Their identifiers carry the `maplethief.local.` prefix so they never pose as the game's.
- The overrides key on the touch-up selectors and two ivar offsets of this one binary (v1.5). The image has one `MainMenuView`, and `offline UI` logs when it is missing.
- #89's settings menu can show the achievements (`achievement_rows`) and toggle both keys like any registry row. Both are `game_only`, read at the boot.

## Addendum: limits on reading `gamecenter.json` (#413, 2026-11-13)

ADR 0154 calls `gamecenter.json` untrusted: a shared data folder carries it. The reader (`runtime/src/stubs/json.cpp`, `store_load` in `gamekit.cpp`) now refuses a file over 1 MiB before reading it, a document of more than 4,096 elements (array items and object members, all containers together; each lookup is a linear scan, so more would cost quadratic time), and a `\u0000` escape, which would cut the C strings the stubs keep. A refused file is handled like any unreadable one: logged, read as empty, and kept aside as `gamecenter.json.bad` at the next save. Nothing else in the data folder changes. The port's own file is a few kilobytes and a few dozen elements. Tests (`stubs_test.cpp`): `gamecenter.json's JSON: at most 4,096 elements, no \u0000, numbers of at most 63 characters (#413)` and `GameKit: a gamecenter.json over 1 MiB, 4,096 elements or with \u0000 is kept aside (#413)`.

## Addendum: the dead cash buttons (2026-11-27)

Accepted (delegated by Joris, 2026-11-13; #400, from #376's verdicts in [re/enhancements-round-3.md](../re/enhancements-round-3.md#the-cash-buttons)).

**Context.** With `cash_shop = off` (every preset, [ADR 0072](0072-offline-cash-shop-and-revives.md)) the field's Cash button opens a shop whose every tile is cancelled, and the death popup's Revival and Grade revival show a wait and cancel. They are dead in the same way as Game Center's button without the local leaderboard.

**Decision.** No new key: `hide_online_ui = on` hides them while `cash_shop = off`. So Original + controller and Enhanced hide them, Original shows them, and `cash_shop = meso` shows them in every preset.

- **The flag.** The frontend adds `MT_OFFLINE_HIDE_CASH` to the flags (`platform/src/game.cpp`) only when both settings say so.
- **The overrides.** The key pad's two classes make their buttons with a method of the same selector as `MainMenuView`'s: `-[TouchKeyPadView addButtonWithImageName:…]` `0xBBF51` (panel 0's Map, Bag and Cash, from `-initKeyPadType0:` `0xBA6A1`) and `-[PopupKeyPadView addButtonWithImageName:…]` `0xB1865` (every button of panel 7, from `-initWithFrame:` `0xB33F9`). With the flag, both are wrapped like `MainMenuView`'s, and the returned button is hidden when its touch-up action is `releaseCashButton:` (−26), `releaseRevivalButton:` (−25) or `releaseGradeRevivalButton:` (−35). Neither class sends `-setHidden:`, and the engine moves the popup's buttons only with `-setFrame:`, so they stay hidden. The log says `offline: 2 key pad overrides hide the Cash button and the death popup's two revive buttons; their keys send nothing`.
- **The keys.** `mt_input_press` drops `cash_shop`, `revive` and `grade_revive` while the flag is set (`mt_offline_ui_drops_action`). That covers the keyboard (C, R, G), the pad (LT in the field, X and Y on the death popup) and every other caller of the runtime's input. The port's own use of the revive binding (pick up and drop, ADR 0129) is decided in the frontend before that and still works. The field's Cash key gets no button label (ADR 0101: its label's alpha is 0). The popup's prompts and the controller focus (ADR 0051) list only views that are not hidden, so they drop the two revives by themselves.
- **The death popup keeps its layout.** A shot of the popup at 3:2 with the two buttons hidden (`play.cash_buttons_hidden`'s `death_popup.png`, a local run, not in git) shows Return to town and Go to main menu in the lower half of the GL box and the upper half blank. The blank area reads as the box's space above its buttons rather than as something missing. Moving the two buttons up by two rows would only move the blank area below them, where it reads worse, and it would need a host transform with touch redirects, prompts and focus moved with it (the compass's mechanism, ADR 0098's addendum). So they stay where the game puts them.
- **What stays.** The cash shop itself (panel 9) is still reachable only from the Cash key, which is now gone. A save made in it is unchanged. The price line (ADR 0072) has nothing to quote while the revives are hidden, since it shows only with `cash_shop = meso`.

**Evidence.**

- `tests/cpp/stubs_test.cpp` (`objc_test`, CI): `offline UI: the dead Cash and revive buttons are hidden with the cash shop off`. On a synthetic `TouchKeyPadView` and `PopupKeyPadView`, the three buttons are hidden only with `MT_OFFLINE_HIDE_CASH`. The game's helper runs first with its arguments. Map, Bag, Return to town, Go to main menu and OK stay shown. With the links' flag alone, or none, nothing is installed and nothing is hidden.
- `tests/cpp/gamepad_test.cpp` (`platform_test`, CI): `input runtime: the hidden cash buttons' keys send nothing (#400)`. `tests/cpp/hud_labels_test.cpp`: `hud labels: no label on the Cash key while it is hidden (#400)`.
- Real assets, local: `play.cash_buttons_hidden`. In Henesys the Cash button is hidden and Map and Bag are not. A tap where it was and the pad's LT leave the field in game mode 0. On the death popup the revives are hidden, X and Y change nothing and ask for no payment, A returns to town, and on a second fall B opens "go to the main menu?" (`0x6D`). `play.cash_shop_off` (Original) and `play.cash_shop_meso` check the Cash button and both revives shown, and the meso run still buys EXP Explosion 40% and a POW! Package as ADR 0072 says.
