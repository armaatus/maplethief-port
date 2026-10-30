# 63. The app lifecycle: minimise and the background reach the delegate, nothing is presented while hidden, and the guest waits

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #128 (the desktop part; the iPhone check waits for #83)
- **Builds on:** [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`mt_uikit_lifecycle_event`, the turn hook), [ADR 0042](0042-frontend-game-loop-and-presentation.md) (the frontend loop: the present hook and the turn hook), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the fixed-step mode's virtual clock), [ADR 0045](0045-audio-mixer-and-output.md) (`FocusMute`, the game's own pause of its sound)

## Context

The game's delegate implements the three lifecycle messages: `applicationWillResignActive:` `0x2BD9` prints `phone suspend` and calls `Clet_Pause` (the timer stops, the music stops, the state's `Pause`), `applicationDidBecomeActive:` `0x2BB5` prints `phone resume` and calls `Clet_Resume` (the timer starts, the music restarts), and `applicationWillTerminate:` `0x2ACD` prints `The Game is Terminated!` and deletes the current state and the `CMResMgr` singleton ([program-flow.md](../re/program-flow.md), [audio.md](../re/audio.md)). The runtime has had `mt_uikit_lifecycle_event` since #54, but the frontend only ever sent the launch's `applicationDidBecomeActive:`. #73's iOS audit found the gap:

- on the desktop, minimising the window changes nothing: the game keeps ticking and playing;
- on iOS the game would keep ticking in the background, the frontend would keep presenting (iOS allows no GPU work from the background), and the game's own pause would never run.

What SDL 3.4 sends (`SDL_video.c`'s `SDL_OnApplication*` handlers, `SDL_uikitappdelegate.m`):

- **iOS, resigning active** (the home gesture, Control Center, a call): `SDL_EVENT_WINDOW_MINIMIZED` for every window, keyboard focus lost, `SDL_EVENT_WILL_ENTER_BACKGROUND` (from `sceneWillResignActive`), then, only if the app really leaves the screen, `SDL_EVENT_DID_ENTER_BACKGROUND` (`sceneDidEnterBackground`).
- **iOS, coming back:** `SDL_EVENT_WILL_ENTER_FOREGROUND` (only after the background), `SDL_EVENT_DID_ENTER_FOREGROUND` (`sceneDidBecomeActive`), then `SDL_EVENT_WINDOW_RESTORED` for every window.
- **iOS, termination:** `SDL_EVENT_TERMINATING`.
- **The desktop** (Cocoa, Windows, X11/Wayland): none of the app events; a minimise is `SDL_EVENT_WINDOW_MINIMIZED`, and `SDL_EVENT_WINDOW_RESTORED` (or `_MAXIMIZED`, back into a maximised window) brings the window back.
- SDL's header says the app events "must be handled in a callback set with `SDL_AddEventWatch()`": on iOS they come from UIKit's callbacks, and the app can be suspended before the queue is polled.

To decide: what a desktop minimise means; how SDL's events become the three messages (once each, in order, whatever mix of events); when the messages may be sent (the frontend pumps SDL's events inside the guest's `presentRenderbuffer:` too, ADR 0042); whether the guest's run loop goes on while hidden; what a quit does; and what `--fixed-step` runs and the device goldens see.

## Options considered

**A desktop minimise.**

1. **Nothing** (today). Con: the game plays on, unseen, music and all; iOS paused it.
2. **Resign active only**, the guest running on. The game pauses itself (`Clet_Pause`), but its run loop and the software rasterizer keep turning for a window nobody sees.
3. **Resign active and hidden**, like the iPhone's home gesture: the delegate resigns, nothing is presented, the guest waits until the window comes back, then becomes active. Losing focus stays `FocusMute`'s (ADR 0045): a window behind another one is still on screen, as an iPhone app under Control Center is.

**When the messages are sent.**

1. **At once, wherever the event is seen.** Con: an event pumped inside `presentRenderbuffer:` would run `Clet_Pause` inside the game's own draw call.
2. **Queued, sent from the turn hook**, between run-loop turns, as UIKit sends them between events. The hidden state still changes at once, so the frame being presented when the event arrives is already not shown.

**The guest while hidden.**

1. **Run on in real time, not presented.** Con: on iOS the OS suspends the app anyway; on the desktop it costs a core for nothing, and the game's timer goes on.
2. **Wait in the turn hook**, as iOS suspends an app: no run-loop turn, no timer, no GL frame. When it comes back the first timer callout folds the whole gap into the game's sub-timers, as it did after a suspend on the device ([timing-threading.md](../re/timing-threading.md)).

**A quit.**

1. **End the run at once** (today). Con: the delegate is never told.
2. **`applicationWillTerminate:` from the next turn hook, then the end.** Con: a quit seen inside a long guest call (a map load presents its loading screen for seconds without returning to the run loop) waits for the call to return.

**The fixed-step mode.**

1. **Every event counts.** Con: a window minimised by hand changes a run that must be the same every time.
2. **Only scripted events count**, at virtual times (`--minimise`, `--background`, `--quit`), with the virtual clock jumping over the hidden time in whole steps; a quit still ends the run.

## Decision

- **`Lifecycle`** (`platform/src/lifecycle.{h,cpp}`) owns the mapping; `game.cpp` feeds it from its existing event dispatch (`handle_event`) and hooks. The delegate is *active* while nothing resigned it:
  - **the desktop:** the window is not minimised (minimise option 3: `SDL_EVENT_WINDOW_MINIMIZED` resigns, `_RESTORED` or `_MAXIMIZED` makes it active again);
  - **iOS** (`SDL_PLATFORM_IOS`; the constructor takes either mapping, so both are tested everywhere): no `WILL_ENTER_BACKGROUND` is waiting for its `DID_ENTER_FOREGROUND`. SDL's minimise and restore are ignored there: SDL sends them with every resign and become-active, and the app events say what happened.
  - Each change of *active* queues one message, so a resign is always followed by exactly one become-active, however many events led there (SDL's six-event iOS home-gesture sequence gives two). Focus lost and window hidden are not lifecycle events.
- **Hidden** means the desktop's minimised window, or iOS's `DID_ENTER_BACKGROUND` until `WILL_ENTER_FOREGROUND` (or `DID_ENTER_FOREGROUND`). While hidden, `present()` uploads and presents nothing (counted as "held back"), and the turn hook **waits** (guest option 2): it pumps SDL's events (`SDL_WaitEventTimeout`, at most 100 ms) and the script until the window is shown again, a quit comes, or `--seconds` runs out. Shots (`--shot`) still composite on the CPU; nothing reaches the GPU.
- **Delivery (option 2).** The app events are taken in an `SDL_AddEventWatch` callback, as SDL asks; the window events and the quits come from the polled queue, where `Lifecycle::handle` ignores the app events (the watch already had them). The queue is sent by `Lifecycle::deliver` from the turn hook: at its start (after the pump and the script), after a hidden wait, and before the run ends. A message's log line names its cause: `lifecycle: applicationWillResignActive: at 6.000 s (SDL_EVENT_WINDOW_MINIMIZED, scripted)`. The watch and the queue are guarded by a mutex: SDL may call a watch from the thread that pushes an event.
- **Quits (option 2).** `SDL_EVENT_QUIT`, the window's close button and `SDL_EVENT_TERMINATING` queue `applicationWillTerminate:` (once). The next turn hook sends it and ends the run ("UIApplicationMain: the host ended the run loop"), so no guest code runs after the game deleted its state. A quit seen in the present hook waits for that turn, for at most 2 s of real time; a guest that does not return to its run loop by then ends the run without the message (logged). The scripted ends (`--frames`, `--seconds`, the last `--shot`) and failed checks end the run as before, without it, so every existing check sees the same run.
- **The fixed-step mode (option 2).** `Lifecycle::set_real(false)`: real minimises, restores and app events are logged and ignored (`… ignored (the fixed-step mode takes only scripted lifecycle events)`); quits count. While hidden, the virtual clock advances straight to the next scripted event (or `--seconds`) in whole steps, and the capture renders the audio of that time (silence, after `Clet_Pause`). A run with the same command line reaches the same frames and writes the same WAV; the device goldens use no lifecycle option and are unchanged.
- **Test aids** (`main.cpp`), through the same `handle_event` path as real events, at seconds after the first run-loop turn:
  - `--minimise T,S`: minimised at T, restored S seconds later;
  - `--background T,S`: SDL's iOS sequence for the home gesture and back (`lifecycle_script` in `game.h`: at T a minimise, `WILL_ENTER_BACKGROUND`, `DID_ENTER_BACKGROUND`; at T + S `WILL_ENTER_FOREGROUND`, `DID_ENTER_FOREGROUND`, a restore);
  - `--quit T`: `SDL_EVENT_QUIT` at T.

## Evidence

- **CI** (`tests/cpp/lifecycle_test.cpp` in `platform_test`, synthetic):
  - The desktop mapping: a restore without a minimise, a second minimise, a second restore and a restore into a maximised window send nothing more; focus lost and hidden send nothing.
  - The iOS mapping: SDL's home-gesture sequence (6 events: one resign on `WILL_ENTER_BACKGROUND`, one become-active on `DID_ENTER_FOREGROUND`, hidden only between `DID_ENTER_BACKGROUND` and `WILL_ENTER_FOREGROUND`), Control Center (never hidden), a return without `WILL_ENTER_FOREGROUND`, and twice in a row; the same sequence on the desktop's mapping (one message each, the restore reactivates).
  - Quits: `TERMINATING`, `QUIT` and the close button send `applicationWillTerminate:` once, and nothing is sent after it. A delegate without the methods is told nothing and gets no unrecognized selector. The fixed-step filter. `--minimise`, `--background` and `--quit` as scripted events, by time.
  - Pushed events (`SDL_PushEvent` on a dummy-driver window, the app events through the event watch) into a synthetic guest: a delegate recording its messages and a GL view whose present hook pumps SDL's queue before presenting, as `game.cpp`'s does. With both mappings: the home gesture pushed during a present (that frame and three more in the background never reach the window, whose read-back still shows the last frame before), the way back, Control Center, `TERMINATING` + `QUIT`, more events after it. The delegate gets exactly resign, become-active, resign, become-active, terminate, each with the `UIApplication`.
  - `platform.lifecycle_usage`: 29 invalid command lines of the three flags exit 2.
- **Real assets** (local, `platform.game_lifecycle`, Debug, SDL's dummy drivers, the audio trace):
  - `--fixed-step 16.667 --minimise 6,4 --tap 11,106,165 --quit 16 --audio-capture`, twice: the main menu's music plays; at 6.000 s the minimise, `-[AVAudioPlayer stop]` and the guest's `phone suspend` / `OpenAL sound free` (Clet_Pause), `applicationWillResignActive:` once; hidden at window frame 173 / GL frame 120 and shown again at 10.000 s at the same frames (nothing presented); the capture is silent from 6.1 s to 10 s and the music is back at 10.5 s (`phone resume`, Clet_Resume restarts the track); the tap on Play at 11 s starts a new game (another track, the loading screen: 153 GL frames after the restore); at 16 s `applicationWillTerminate:` (`The Game is Terminated!`) and a clean end. The two runs' WAVs are identical.
  - `--fixed-step 16.667 --background 6,4 --quit 12`: SDL's iOS sequence, one message each, in order.
  - Real time, `--minimise 3,2 --quit 7` with the device stream: the same order.

## Consequences

- **#83 (the iOS build)** uses the iOS mapping by `SDL_PLATFORM_IOS` and must check on a device what the desktop cannot (#128's last criterion, `needs-human`): the home gesture and back, Control Center, a call; each time the game pauses and resumes, nothing crashes, and no "background GPU work" error is logged. If iOS suspends the app before the turn hook sends `applicationWillResignActive:` (the watch only queues it), the watch has to send it itself when the guest is between calls.
- **`SDL_EVENT_TERMINATING` on iOS** comes from `applicationWillTerminate`, after which iOS ends the process quickly; the message waits for the next turn hook, which a suspended app may not reach. The game's handler only frees memory, so missing it loses nothing.
- **#120 (the GPU backend)** keeps the gate at the top of `present()`: nothing uploads or presents while `Lifecycle::presenting()` is false.
- **How we'd know this was wrong:**
  - The game misbehaves after a long minimise (timers, a stalled tick): the gap folded into the sub-timers at once is too much, and the wait has to keep the guest's clock still instead (a virtual offset on the libc clock).
  - Players want the game to keep running while minimised (an idle grind): minimise option 2 behind a setting.
  - A desktop "hide" (Cmd-H, a covered window) should pause too: map `SDL_EVENT_WINDOW_HIDDEN` / `_OCCLUDED` like the minimise.

## Addendum: the remap overlay's hold (#70, 2026-10-30)

M7's remap overlay holds the turn hook while it is open ([ADR 0054](0054-bindings-file-remap-overlay-and-button-glyphs.md)). A minimise or the background while it is open keeps it open and draws nothing. Its messages wait in the queue and are delivered when the overlay closes, and a quit ends the hold. The details are in [ADR 0068's addendum](0068-gpu-presentation-without-read-back.md#addendum-the-remap-overlay-on-the-gpu-and-its-hold-under-the-lifecycle-70-with-130-and-128-2026-10-30).

## Addendum: pause when unfocused (#145, 2026-10-30)

`pause_unfocused = on` (a setting, [ADR 0077](0077-desktop-conveniences.md)) maps the desktop window's `SDL_EVENT_WINDOW_FOCUS_LOST` like a minimise: one `applicationWillResignActive:`, hidden (nothing presented, the guest waits), and `SDL_EVENT_WINDOW_FOCUS_GAINED` sends one `applicationDidBecomeActive:`. "Focus lost and window hidden are not lifecycle events" still holds with the setting off (the default). The iOS mapping ignores the focus. The window is still on screen, so `wait_hidden` redraws the last frame on a resize or an expose (`Lifecycle::paused_visible`). The fixed-step mode ignores the window's focus; `--unfocus T,S` scripts it.
