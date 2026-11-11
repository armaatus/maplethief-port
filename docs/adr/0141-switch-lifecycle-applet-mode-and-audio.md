# 141. The Switch's lifecycle, applet mode and audio: SDL's events into Lifecycle, a frozen spell is a pause, a measured memory budget, CPU boost for loads only

- **Status:** Accepted
- **Date:** 2026-11-11
- **Issue:** [#240](https://github.com/armaatus/maplethief-port/issues/240) (epic [#233](https://github.com/armaatus/maplethief-port/issues/233))
- **Builds on:** [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the target, sdl3-nx), [ADR 0063](0063-app-lifecycle-background-and-minimise.md) (the lifecycle), [ADR 0089](0089-autosave-on-quit-and-background.md) (autosave), [ADR 0045](0045-audio-mixer-and-output.md) and [ADR 0046](0046-avaudioplayer-mp3-streams-and-loop-points.md) (audio), [ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md) (telemetry), [ADR 0025](0025-guest-memory-and-address-space.md)'s Horizon addendum (the commit map)

## Context

ADR 0138 chose `nx-mod/sdl3-nx` partly for its lifecycle events. #240 maps them onto ADR 0063's `Lifecycle` and ADR 0089's autosave, and handles applet mode and the CPU boost. Nothing here ran on a console; the behaviour comes from reading the pinned SDL source (`src/video/switch/SDL_switchvideo.c` at `07ea26a3`) and libnx 4.12.0's `applet.h`. The hardware checks are `needs-human` on #240.

What sdl3-nx does:

- It sets focus handling mode `SuspendHomeSleepNotify` for applications (applets get focus changes anyway) and hooks libnx's applet messages.
- `AppletFocusState_Background` (libnx: "HOME menu open / console is sleeping") sends `SDL_EVENT_WILL_ENTER_BACKGROUND` and `DID_ENTER_BACKGROUND`. `InFocus` after it sends `WILL_ENTER_FOREGROUND` and `DID_ENTER_FOREGROUND`. `OutOfFocus` (a library applet over the app, such as the software keyboard or the error applet) sends nothing.
- When `appletMainLoop` returns false (the app was closed from the HOME menu), it sends `SDL_EVENT_TERMINATING` and then `SDL_EVENT_QUIT`.
- The process is **frozen** while in the background, and is told only when it comes back. `appletMainLoop` handles one message per `SDL_PumpEvents`, so the background and foreground pairs arrive one or more pumps apart, often in the same turn.

The gaps that leaves:

1. The frozen spell looks like a slow frame to ADR 0071's telemetry and like a late audio callback to ADR 0045's `LateDetector`. Either would put false dropouts in the 30-minute log.
2. The game cannot save *before* the HOME menu shows, because it is frozen first.
3. Applet mode (the Homebrew Menu started from the Album) gives the process much less memory than title takeover, and nothing measures it.
4. Long loads run at the normal CPU clock.

## Decision

**The mapping is the desktop's `Lifecycle`, unchanged.** The Switch builds with `kMinimiseResigns` (its window never minimises). sdl3-nx's app events reach the event watch, so the HOME menu or sleep queues one `applicationWillResignActive:`, the return one `applicationDidBecomeActive:`, and a close one `applicationWillTerminate:`, whatever the mix of pumps. ADR 0089's `Autosaver::before_deliver` runs before each delivery, so:

- closing from the HOME menu autosaves (at the resign when the Background message comes first, else at the terminate), then terminates. libnx gives the app time to handle the exit request.
- HOME or sleep and back autosaves when the game comes back, at the resign. It is late but harmless. Gap 2 stays open by design: `NoSuspend` would let the game save before the menu shows, but would keep it running (and the audio device playing) under the HOME menu, which a console app must not do.
- the settings menu's Quit is the desktop's `SDL_EVENT_QUIT`: autosave, `applicationWillTerminate:`, the run ends, and `main` returns to the Homebrew Menu.
- `OutOfFocus` is not leaving. The game runs on under a library applet, as sdl3-nx decides.

`platform/src/switch_applet.h`'s `SdlSwitchModel` restates sdl3-nx's rules, and `tests/cpp/lifecycle_test.cpp` feeds its events to `Lifecycle` pump by pump (the CI criterion). If the fork changes its rules, the model is what to update.

**A frozen spell is a pause (gap 1).** `Lifecycle::backgrounds()` counts the times the app entered the background. The turn after a change calls `FrameStats::gap`, as the desktop's hidden wait does. `LateDetector` counts a callback more than 1 s after the previous one as a *suspension* (with the time it lasted), not as late. The run's summary logs `game: audio: N suspensions of the process (S s), not counted as late`, and the line comparing the time the stream ran with the audio it pulled leaves that time out. The same holds for a desktop that sleeps. A real stall over a second is still visible in the frame telemetry.

**Applet-mode memory, measured (gap 3).** `platform/switch/switch_app.cpp`'s `memory_sample` reads the following, and `AppletWatch` (in `switch_applet.h`, called once per run-loop turn from `game.cpp`) logs it at each map load (a new area and map outside the loading modes 6 to 8) and at each peak 8 MiB above the last one logged:

- the guest's committed bytes, from the commit map (new `mt_mem_committed_bytes`, which counts only on the Switch);
- newlib's heap, which the loader hands all of the process's memory and from which the commit map takes the guest's pages: its size from libnx's heap bounds, and its bytes in use from `mallinfo().uordblks`;
- the process's total and used memory, from `svcGetInfo` (`TotalMemorySize`, `UsedMemorySize`).

The verdict is a pure function, `judge_memory`: the game fits while the heap keeps `kAppletMarginBytes` (64 MiB, provisional until the hardware run) free. In applet mode (`appletGetAppletType` is not an application), a sample that does not fit shows `applet_mode_message`, once, in the window's notice corner for `kDataNoticeSeconds`, and logs it. The message says to start the Homebrew Menu through a game title. The run's end logs the peak with "applet mode fits" or "does not fit". The numbers are Joris's run.

**CPU boost for long loads only (gap 4).** `mt::switch_nx::CpuBoost` is a scoped, counted `appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad)`: on while at least one is alive, `Normal` after the last, and a refusal is logged once. FastLoad also throttles the GPU to its minimum, which a loading screen does not mind. `AppletWatch` holds one from its start (the guest's boot and the bundle's load) until the first turn, and again while the game is in the loading modes 6 to 8. `boost_wanted` is never true in play. #243's import takes its own `CpuBoost`.

**Audio is unchanged.** The mixer renders 44.1 kHz stereo float, as on every target (ADR 0045). SDL's device stream converts it to audren's 48 kHz `S16`, and the open line logs both rates. The game's sounds at 8, 22.05 and 44.1 kHz are resampled by the mixer as before. Music pauses with the app through the game's own `Clet_Pause` (ADR 0063's evidence: `-[AVAudioPlayer stop]`), and while frozen nothing plays at all.

**libnx hooks: for the log only.** `NxApp` hooks the applet messages and logs each one (`applet: focus Background`, `applet: exit requested (closed from the HOME menu)`, `applet: resumed`, the operation and performance modes). sdl3-nx's own hook does the work. The log shows on a console what actually came, in what order, which the hardware steps on #240 check.

## Consequences

- HOME, sleep and closing from HOME go through the same code and tests as the desktop and iOS. No Switch-only lifecycle path to maintain.
- The 30-minute audio log counts only real dropouts. A frozen spell is a line of its own.
- Applet mode either fits with a logged headroom or tells the player what to do before the game runs out mid-map. A commit that still fails ends the run with the crash report, as before.
- `mt_mem_committed_bytes` is new public runtime API. It returns 0 off the Switch.

**How we'd know this was wrong:**

- The hardware run shows sdl3-nx's events differ: the `applet:` lines and the `lifecycle:` lines disagree with `SdlSwitchModel`. Update the model and the tests, and fill the gap with a libnx hook in `switch_app.cpp`.
- Closing from the HOME menu leaves no autosave: then the process is not given time after the exit request, and an autosave on return from the background (already there) is the only one.
- The peak in applet mode is far from the 64 MiB margin either way: set `kAppletMarginBytes` from the measurement.
