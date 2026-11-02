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
