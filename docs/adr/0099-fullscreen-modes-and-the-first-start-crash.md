# 99. Fullscreen on and off, a fullscreen mode for macOS, and only a changed request asks the window

- **Status:** Accepted
- **Date:** 2026-11-03
- **Issue:** #179
- **Builds on:** [ADR 0077](0077-desktop-conveniences.md) (fullscreen, the hotkeys), [ADR 0070](0070-settings-registry-and-presets.md) (the registry and presets), [ADR 0076](0076-settings-menu.md) (the settings menu)

## Context

**A crash at the first start on macOS 27.** The player chose Enhanced (`fullscreen = desktop`) in the first-launch picker, and the game crashed in SkyLight (`SLSWindowServerClientSpaceAddWindowsAndRemoveFromSpaces`, a null `CFArray`) under `-[_NSEnterFullScreenTransitionController _performEnterFullScreen]`. The stack came from `Desktop::apply` → `SDL_SetWindowFullscreenMode` → `Cocoa_SyncWindow` → `SDL_PumpEvents`. The second start worked.

1. `main.cpp` applies the settings to the window after the picker: `Desktop::apply` asks for fullscreen. On macOS that starts AppKit's transition into a Space, which runs asynchronously.
2. `run_game` then applies the same settings again (`apply_settings_at_once`). `Desktop::apply` asked whenever `want != fullscreen()`, and the window was not fullscreen yet, so it asked a second time.
3. `SDL_SetWindowFullscreenMode` syncs the window, which pumps the event loop. The first transition, still queued, ran inside the second request, and two transitions overlapped.

The second start created the window fullscreen (`SDL_WINDOW_FULLSCREEN` at creation), so nothing was asked.

**Cmd+Ctrl+F toggled twice.** SDL 3.4's Cocoa backend adds a Window menu item, "Toggle Full Screen", on Cmd+Ctrl+F (`SDL_cocoaevents.m`). The port's own hotkey toggled as well.

**Borderless.** Players asked for fullscreen without the Space: no animation, the desktop behind, Cmd-Tab at once. On Windows and Linux SDL's desktop fullscreen already is a borderless window over the display. On macOS it is a Space unless `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` is off, and SDL reads that hint once, at video init (`data.allow_spaces`, `SDL_cocoavideo.m`). It has no per-window switch.

## Options considered

**The crash.**

1. **Ask only when the request changes.** `Desktop` keeps the last request (`want_fullscreen_`). The system's own changes (the green button) already come back as `SDL_EVENT_WINDOW_ENTER/LEAVE_FULLSCREEN` and update it. **Chosen.**
2. Skip the second apply in `run_game` only. This is narrower, and other double applies (the menu, the hotkeys) would stay open.
3. Wait for the transition after the first request (`SDL_SyncWindow`). This blocks the start for the animation.

**Cmd+Ctrl+F.** On macOS, leave it to the Window menu and follow the window's events into the setting (**chosen**). Removing SDL's menu item would need Objective-C to edit `NSMenu`.

**Borderless on macOS.**

1. **The hint at the next start.** `fullscreen_mode = desktop | borderless` sets `SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES` before `SDL_Init`. Borderless is SDL's own fullscreen without a Space. **Chosen** (Joris, 2026-11-03).
2. Exclusive fullscreen at the desktop's own mode. This switches live, but `Cocoa_SetDisplayMode` fades the display to black for about 0.8 s on each switch.
3. A borderless `NSWindow` of the port's own, sized to the screen, with the menu bar and the Dock hidden. This switches live and at once, but SDL would not know about it (its fullscreen flag stays off), so there is more code and a risk of SDL restoring the style.

## Decision

- **`fullscreen = off | on`** (Video, applies at once). `desktop` and `borderless` also read as on, in that mode: the value before #179 and the flag's shorthand (`--fullscreen off | on | desktop | borderless`). The next write stores `on` and the mode.
- **`fullscreen_mode = desktop | borderless`** (Video, `--fullscreen-mode`, from the next start). It only matters on macOS: `desktop` is a Space of its own (as the green button), `borderless` covers the desktop. `main.cpp` reads the settings before `SDL_Init` to set the hint.
- **Presets:** Original and Original + controller are `off`, `desktop`. Enhanced is `on`, `borderless`. A value that changes another row moves the preset to custom: `apply_setting` checks every row the value changed, so `--fullscreen desktop` under Enhanced is custom.
- **`Desktop::apply` asks only on a changed request** (`want != want_fullscreen_`). `fullscreen_requests()` counts what it asked.
- **Hotkeys:** F11 and Alt+Enter everywhere. Cmd+Ctrl+F is the port's except on macOS (`is_fullscreen_hotkey(key, mod, macos)`).
- In borderless mode on macOS SDL makes the window `NSWindowCollectionBehaviorFullScreenNone`, so the green button zooms instead of making a Space. In desktop mode the green button and the Window menu enter the Space, and the window's event sets `fullscreen = on`.

## Consequences

- The first start with Enhanced runs this run in desktop mode (the hint was read before the picker chose) and borderless from the next start. With the fix, that first run asks for fullscreen once.
- A change of `fullscreen_mode` in the settings menu shows `*` (at the next start).
- On Windows and Linux both modes are the same window.
- Tests: `desktop: a second apply with the same request while the window is not fullscreen yet asks nothing (#179: the first-start crash)`; the `fullscreen` / `fullscreen_mode` parsing, write-back, flags, presets and menu choices; the hotkey predicate per platform; `presets_test`'s columns; `platform.desktop_usage` (the flags' messages). The real macOS 27 run is Joris's (#179, needs-human).
