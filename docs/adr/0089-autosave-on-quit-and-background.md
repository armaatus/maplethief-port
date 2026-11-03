# 89. Autosave on quit and background, within the game's save rules

- **Status:** Accepted
- **Date:** 2026-10-31
- **Issue:** #150
- **Builds on:** [ADR 0063](0063-app-lifecycle-background-and-minimise.md) (the app lifecycle, sent from the turn hook), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (atomic save writes), [ADR 0078](0078-save-export-import-backups-and-checks.md) (the backup hook on `SaveQuestData`), [ADR 0070](0070-settings-registry-and-presets.md) (the settings registry), [ADR 0069](0069-guest-function-hooks.md) (`mt_call_guest`)

## Context

The game saves only when the player picks Save in the popup menu, plus a few fixed points (a quest reward item, a scroll's result, a granted purchase) ([formats/save.md](../formats/save.md#when-the-game-saves-and-loads)). It has no timer. On a desktop, closing the window loses everything since the last menu Save.

"Save anywhere" was rejected in #90. The game refuses to save on 16 maps (the tutorial, the five boss maps, three climbs, the Monster Carnival rooms), because boss state is not saved. An autosave has to respect that list and every other condition under which the game would not save. [docs/re/autosave.md](../re/autosave.md) has the conditions with their evidence.

## Options considered

1. **Press the menu's keys for the player** (open the popup menu, tab 5, item 0, Yes). This uses the game's exact path, but it takes several ticks, draws popups, and changes the game's state (the menu stops the hero). A quit would have to wait for the guest to run those ticks. It would also leave the "Successfully saved!" popup open on a background.
2. **Call the save pair directly, after the same guards** (`SaveSaveData` `0x38711`, `SaveQuestData` `0x38F55`), from the host, between two run-loop turns. No ticks, no popups. The guards are reads of guest memory.
3. **Hook the save pair's callers or the game's tick** to save at a safe point after the quit was asked. The quit would wait for an unknown time, or never come if the player is in a boss room.
4. **A timer autosave during play.** Not asked for; it changes play (a save after a bad move) and would fight the game's own save points.

## Decision

Option 2.

- **Setting:** `autosave = off | on_quit` (Gameplay; off in Original and Original + controller, on_quit in Enhanced; `--autosave`). Off is today's behaviour.
- **When:** `game.cpp`'s turn hook calls `Autosaver::before_deliver` (`platform/src/autosave.{h,cpp}`) right before `Lifecycle::deliver`. If a resign (`applicationWillResignActive:`: minimise, iOS background, `pause_unfocused` focus loss) or a terminate (`applicationWillTerminate:`: close, Cmd-Q, `SDL_EVENT_QUIT`, `--quit`) is queued (`Lifecycle::leaving`), it checks the guards once for that turn. Nothing runs inside a tick.
- **Guards:** the game's own conditions for reaching its Save (not in `CStateGame`, loading modes 6–8, a scene in modes 9–10, any mode but the field and the popup menu, an open popup, a pending warp, a knock-back, a scroll being applied, a dying or dead hero, the 16 maps). The port adds one: the hero must be standing, walking or climbing and not in the air or attacking. The game's menu stops the hero before it saves; the autosave cannot do that without changing the state it saves, so it skips instead.
- **The save:** both functions through `mt_call_guest`, with every register kept, inside an autorelease pool. They write through `mt_saves_replace_file` and the backup hook as the game's own saves do. The quit does not wait: a skipped autosave logs its guard and the quit goes on.
- **Evidence in the log:** `autosave: slot N saved at <trigger> (<t>): map A.M, ...` or `autosave: not saved at <trigger> (<t>): <guard>`.

## Consequences

- Closing the window or minimising in a town or field keeps the progress, as the menu's Save would. In a boss room, a scene, a popup or mid-jump nothing is saved, which is what the game itself would allow at that moment.
- The guards are reads of fixed guest addresses from the 1.5 binary. The checks only read image segments and the committed heap, so a wrong pointer skips instead of crashing.
- The extra hero guard can skip an autosave the game's menu would have allowed (after stopping the hero). A player who quits mid-jump loses the progress since the last save, as without autosave.
- #89's settings menu should quit through `SDL_EVENT_QUIT`, so the same path saves. On iOS (#83) the app may be suspended before the next turn after `WILL_ENTER_BACKGROUND`; whether the turn hook runs in time is checked on a device there.
- Tests: `platform_test`'s `platform.autosave:` cases (synthetic, CI); `play.autosave_*` (19 tests) and `platform.game_autosave` on real assets (local).

## Addendum (#105 wave 4 integration): every quit path

With #89, #145, #157 and #159 merged, every way out of the game reaches `Autosaver::before_deliver` the same way. Each is one of SDL's quit events. `handle_event` passes it to `Lifecycle::handle` before any overlay sees it; `SDL_EVENT_TERMINATING` goes through SDL's event watch (`Lifecycle::watch`) instead. The next run-loop turn then calls `before_deliver` before `applicationWillTerminate:`.

| Quit | SDL event |
|---|---|
| Cmd-Q, the app menu's Quit, the system's logout | `SDL_EVENT_QUIT` |
| The window's close button, Alt+F4 | `SDL_EVENT_WINDOW_CLOSE_REQUESTED` |
| iOS terminating the app | `SDL_EVENT_TERMINATING` |
| `--quit T` (tests) | a scripted `SDL_EVENT_QUIT` |

- **The settings menu (#89) has no Quit row.** While it, the remap overlay or the key pad editor (#159) holds the turn, a quit ends that hold (`run_overlays` and `run_keypad_editor` stop on `Lifecycle::terminating`). The turn hook then goes on to `before_deliver` as for any quit. The guards read the game's state, which an open overlay does not change.
- **#145's desktop hotkeys** (F11, Alt+Enter, Cmd+Ctrl+F, F12) don't quit. Cmd-Q stays the system's.
- **No autosave** after a quit the game doesn't answer within the grace (`kQuitGraceNs`: the guest stuck outside its run loop). That run ends without `applicationWillTerminate:`, as before.
- Tested by `autosave: Lifecycle::leaving is the first queued resign or quit` (each of the three events is a terminate leave) and `platform.game_autosave` (the scripted quit, real assets).

## Addendum (#179): the exit to the main menu and the settings menu's Quit

- **The exit to the main menu saves too.** The popup menu's option tab exits through popup 14, and the game never saves there; only popup 13 ("Do you want to save?") calls the save pair ([re/autosave.md](../re/autosave.md#the-exit-to-the-main-menu)). With `autosave = on_quit` (the name stays: it covers every way of leaving the game), `Game_ExitToMainMenu` `0x54BF9` is hooked (`MT_HOOK_FN_GAME_EXIT_TO_MAIN_MENU`). When OK (−5 or `'5'`) confirms Yes on popup 14, the hook calls the same pair behind the same guards, with popup 14 let through the popup guard, then passes to the game's exit. `0x54BF9` has one caller (`Menu_OnKey`), so the death popup's "Go to main menu" (`0x8D4C9`, popup `0x6D`) and a scene's exit (`0x83C71`) never pass there: they leave without a save, as the game does.
- It runs inside the game's key handling, where the game's own popup-13 save runs, not between turns. The hook restores every register before the game's exit goes on: the autorelease pool's push and pop are calls too.
- **The settings menu's Quit** (ADR 0076's addendum) sends `SDL_EVENT_QUIT`, so it takes the Cmd-Q row of the table above.
- Tests: `autosave: the exit to the main menu (#179): ...` (synthetic); `play.autosave_exit` (Yes on popup 14 saves slot 0 before the main menu) and `play.autosave_exit_off` (with `off` nothing is saved), real assets.
