# Timing and threading

This page covers the game's clock, the frame timer, what one tick runs, the real-time timers that sit beside it, and every thread, queue, timer, lock and barrier in the armv7 slice ([#21](https://github.com/armaatus/maplethief-port/issues/21)). It builds on the frame loop in [program-flow.md](program-flow.md#the-frame-loop). It ends with what the runtime has to reproduce.

Addresses are armv7 VM addresses. Function pointers carry the Thumb bit, instruction sites don't. *Tag* is the [#10](libraries.md) tag of the function.

The evidence comes from `tools/ghidra/run-script decompile.py` listings and Capstone disassembly of the functions named here. Call sites come from `work/re/imports.json` ([imports.md](imports.md)).

**Result.**

- **Frame rate.** The game runs at one fixed tick per frame. A tick is **65, 50 or 40 ms** (about 15.4, 20 or 25 ticks/s), picked by the *game speed* option. **The default is 50 ms, 20 ticks/s.**
- **Logic is fixed-step.** Nothing is scaled by elapsed time. Each tick runs input, `Update` and `Paint` exactly once. Most of the simulation (hero, mobs, sprite animation) runs inside `CStateGame::Paint`.
- **Overruns drift.** A late tick is never caught up or skipped: the next tick is simply measured from the start of the late one.
- **One thread.** All game code runs on the main thread, driven by one 1 ms `CFRunLoopTimer`. The game creates no threads, and `CLoaderThread` is not one.
- **Chartboost is the only code that uses other threads.** It runs blocks on a GCD global queue. With Chartboost stubbed ([network.md](network.md#chartboost)), the port needs no guest threads at all.

## Machine checks

`python3 -m mtport.timing check` and `tests/test_timing_real.py` confirm three things:

- Every site of a timing or threading import or message in `work/re/imports.json` is named on this page by its address. A site inside a Chartboost function may instead be covered by that function's address in [Chartboost](#chartboost).
- Every exclusive-access, barrier or swap instruction is covered in the same way. They are found by `armscan.find_instructions` (`ldrex*`/`strex*`/`clrex`/`dmb`/`dsb`/`isb`/`swp*`).
- None of the [selectors that are never sent](#never-used) has a selector reference.

The lists are `TIMING_IMPORTS`, `TIMING_SELECTORS`, `ABSENT_SELECTORS` and `ATOMIC_MNEMONICS` in [`tools/mtport/timing.py`](../../tools/mtport/timing.py). The coverage rule is shared with #23's network check (`tools/mtport/sitecheck.py`). The run finds **211 sites: 55 in game code (engine and app-objc), 156 in Chartboost, 66 of them `dmb`**.

Ghidra names for the functions below come from `python3 -m mtport.timing names` (`work/re/names/timing.json`, 11 functions and 2 data labels). `scripts/re-analyze` runs it.

## The clock

| Function | What it reads | Used for |
|---|---|---|
| `GetTimeMs` `0xA0D5` | `gettimeofday` (site `0xA0E6`). It returns `(int64)(int32)(tv_sec * 1000 + tv_usec / 1000)`: the multiply-add is a 32-bit `mla` at `0xA106` and the result is sign-extended (`asrs r2, r1, #31`). | every game timer: the frame gate, the sub-timers and the [ms timers](#real-time-timers) |
| `RunLoopTimer_Arm` `0x8E29` | `CFAbsoluteTimeGetCurrent` (sites `0x8E84`, `0x8F18`) | only to compute the timer's next fire date, `now + ms / 1000.0` |

No other clock is imported: no `mach_absolute_time`, `CACurrentMediaTime`, `time` or `clock`. `NSDate` is used only by Chartboost.

`GetTimeMs` is wall-clock time, wrapped to 32 bits. The value jumps from +2³¹−1 to −2³¹ every 2³² ms (49.7 days), for example at 2026-10-28 00:54:06 UTC and 2026-12-16 17:56:53 UTC.

**The 2³¹ jump freezes the game.** If the app is running or suspended at that instant, `now − lastFrame` in `CMTimer::OnRunLoopTimer` drops to about −2³², so no tick fires for the next 49.7 days. There is no input, no drawing and no keep-alive. Only the UIKit views still respond, and a relaunch fixes it. The port starts `GetTimeMs` in [0, 2³⁰) in every mode, so a session has at least 12.4 days before the jump: the fixed-step mode's virtual clock (ADR 0044) and, in real time, the launch clock of #155 ([original-bugs.md](original-bugs.md#3-the-2-ms-jump-of-gettimems)).

A clock that jumps backwards stalls the ticks in the same way until the time catches up. A clock that jumps forward gives one immediate tick and adds the jump to every running sub-timer.

## The frame timer

The one-thread clock is set up by `CMTimer` and `RunLoopTimer_Arm`. `CMTimer` keeps all of its state in static globals. [program-flow.md](program-flow.md#the-frame-loop) has the boot side.

| Step | Code | Detail |
|---|---|---|
| create | `CMTimer::CMTimer` `0x22165` (from `CMTimer::GetInstance` `0x22285` in `Clet_Start`) | Zeroes the globals and sets the interval `0x11A478` (i64) to **10 ms**. Then `CMTimer::StartRunLoopTimer` `0x220B1` (`lastTick` `0x11A488` = `lastFrame` `0x11A4D8` = `GetTimeMs()`; holder `0x11A4F0` set up by `RunLoopTimer_Init` `0x6065`), then `CMTimer::Start` `0x21DC9` (running flag `0x11A480` = 1). |
| first arm | `RunLoopTimer_Arm(holder, 1 ms)` `0x8E29` | `CFRunLoopTimerCreate` (`0x8ED8`): first fire at `now + 0.001`, repeat interval 0.001 s (the double at `0x8F68`), callout `RunLoopTimer_Callout` `0x5F99`. Added with `CFRunLoopAddTimer` (`0x8EF6`) to `CFRunLoopGetMain()` (`0x8EE2`) in `kCFRunLoopCommonModes` (`0x8EF0`), so it keeps firing while UIKit tracks touches. |
| each callout | `CMTimer::OnRunLoopTimer` `0x21F51` | (1) `delta = now − lastTick`, `lastTick = now`, and `delta` is added to each active sub-timer. (2) If `now − lastFrame ≥ interval` (signed 64-bit compare): `lastFrame = now`, then **`SkyCanvas::Tick`** `0x2590D`. (3) `RunLoopTimer_Arm(holder, 1)`, which is now `CFRunLoopTimerSetNextFireDate(now + 0.001)` (`0x8F4A`). |

What this means:

- **Polling.** The gate is polled about once per millisecond. The next poll is 1 ms after the previous callout *returns*, not on a fixed 1 ms grid.
- **Tick period.** `lastFrame` is taken *before* the tick runs, so a tick that takes `T` ms starts the next one at about `max(interval, T) + ≤ 1 ms` (rounded to whole ms by `GetTimeMs`). At 50 ms the game ticks slightly slower than 20/s, about 19.6–20/s.
- **Overrun.** `lastFrame = now`, not `lastFrame += interval`. A slow tick is never followed by a burst of catch-up ticks, and no tick is ever skipped: the game slows down (drifts). `CFRunLoopTimer` itself doesn't queue missed fires either.
- **Pause.** The running flag only gates `Update` and `Paint` inside the tick. `Clet_Pause` (`applicationWillResignActive:`) calls `CMTimer::Stop` `0x21DE9` and `CMTimer::MarkPause` `0x21E09`. While stopped, the tick still polls the keypad and calls `OnKeyPressed`/`OnKeyReleased`. `MarkPause` saves the time in `0x11A498`, which nothing reads.
- **Suspend and loads.** While the app is suspended, or a load blocks the run loop, nothing fires. The first callout afterwards adds the whole gap to every running sub-timer and ticks at once.
- **Re-entry.** `SkyCanvas::Tick` skips input, `Update` and `Paint` while `SkyCanvas+0xB7` (paint in progress) is set. This guards against re-entry from a nested run loop. Nothing in the game runs one, so this never happens.

### Interval and game speed

| Interval | Set by | When |
|---|---|---|
| 10 ms | `CMTimer::CMTimer` `0x22165` | Boot. `Clet_Start` replaces it before the run loop first fires, so no tick ever runs at 10 ms. |
| `g_gameSpeedIntervals[config[1]]` | `CDataMgr::ApplyGameSpeed` `0x37C41` → `CMTimer::SetFrameInterval` `0x21DA1` (call at `0x37C6E`) | `CStateTitle::Enter` `0x98751` → `CStateTitle::LoadSettings(3)` `0x980B5` → `CDataMgr::LoadConfig` `0x38611` → `ApplyGameSpeed`, still inside `Clet_Start`. Again when the options menu `0x46A29` changes the speed. |
| 50 ms | `CStateTitle::LoadSettings` at `0x980F4` | Only for `LoadSettings(0)`, which nothing calls (`Enter` passes 3, `Paint` passes 3 or 2). Dead. |

`g_gameSpeedIntervals` `0x108C58` is `i32 {65, 50, 40, −1}`. It is indexed by byte 1 of `config` ([save.md](../formats/save.md#config-6-bytes)): 0 → 65 ms, 1 → 50 ms (the default `LoadConfig` writes), 2 → 40 ms.

A corrupt speed byte of 3 gives −1. That ticks on every poll, about 1,000 ticks/s. A byte of 4 or more reads past the table.

Since the logic is fixed-step, **the speed option scales the whole game**: movement, animation, AI and script timing all run 1.3× faster at 40 ms than at 50 ms. Only the [real-time timers](#real-time-timers) keep their wall-clock length.

Nothing but `ApplyGameSpeed` (`MT_LD32` at `0x37C60` in the recompiled code) reads the table, so the port's `game_speed` writes one length into all three entries before the process runs and every Speed option ticks at it ([ADR 0087](../adr/0087-game-speed-and-fast-forward.md)). The config's byte is left alone.

**A 1 ms fixed step ties with the re-arm.** On a virtual clock that steps exactly 1 ms a turn, the next fire date (callout + 0.001 s) equals the next turn's time, and `CFAbsoluteTime`'s rounding makes the timer miss about every other turn. A tick then starts about 0.75 ms late at every speed (measured on the real game: the 50 ms default at −1.51 % of 20/s, other lengths −0.48 % to −2.01 %). A 1.001 ms step gives +0.00 % at every length (`play.game_speed`). A device's timer fires about every millisecond, not on such a grid, so this is a property of the test clock, not of the game.

## What one tick runs

`SkyCanvas::Tick` `0x2590D` ([program-flow.md](program-flow.md#skycanvastick-0x2590d)) does the following, once each:

1. Keypad polling and `OnKeyPressed`/`OnKeyReleased`.
2. **`state->Update()`** (slot 10).
3. **`state->Paint()`** (slot 9), then `FlushLCD` (`presentRenderbuffer:`).
4. The socket keep-alive check.

No argument carries elapsed time. The state functions don't read the clock for movement either. Rendering and logic are locked 1:1, so the frame rate *is* the tick rate.

- **`CStateGame::Update`** `0x874E1` only calls `CDataMgr::Update` `0x55C85`. That runs the pending popup/menu key handler when `CDataMgr+0x20` is set (cases 2–13, the popup and menu key handlers such as `CDataMgr::keyPopupItemInven_2` `0x554F1`).
- **`CStateGame::Paint`** `0x87501` (24,425 bytes) runs the world. The sprite-animation step, `CMAction` update `0x1E585` ([sprprj.md](../formats/sprprj.md#actions-and-animation-timing)), is called from `Paint` and from functions only `Paint` calls (`0x5E6C1`, `0x5E7B1`, `0x61A19`), and from the hero and mob per-object functions (`CHero::Flush` `0x62ADD`, `CMob::Flush` `0x7458D`, their `vf3`). So **one sprite frame-delay unit is one tick**.
- **Loading** runs inside `Paint` too. `CLoaderThread` (below) blocks the run loop, and `Loading_Progress` `0x6AF09` redraws and presents between load steps. It also steps `CMAction` update `0x1E585`, so the loading animation advances once per load step, not with time.

For a port that wants 60 fps rendering ([ROADMAP](../ROADMAP.md) enhancements), this means drawing and simulation would first have to be separated inside `CStateGame::Paint`. The original has no interpolation point.

### `CLoaderThread`

It isn't a thread. `CStateGame::Paint` makes `new(0x30)` + `CLoaderThread::CLoaderThread(state, mode)` `0x6B705`. The constructor stores the state in `0x11A9B8` and the mode in `0x11A9B4`.

`Paint` then calls `CLoaderThread::Run` `0x6AEF9` → `CLoaderThread::Load` `0x6AEB5` → `CStateGame::LoadGame` `0x86C1D` (mode 0) or `CStateGame::LoadMap` `0x868D5` (mode 1), synchronously, on the main thread. Nothing creates a thread, so nothing synchronizes. The name is presumably left over from the phone version.

## Real-time timers

Some durations are wall-clock milliseconds, independent of the game speed and of how long ticks take.

**Sub-timers.** `CMTimer` keeps 4 sub-timers:

- on flags at `0x11A4A4[i]`;
- i64 elapsed ms at `0x11A4B0 + 8i`;
- `CMTimer::ResetSubTimer(i)` `0x21E81` turns one on at 0;
- `CMTimer::SubTimerElapsed(i)` `0x21EE5` reads one.

They advance in every callout, ticking or not.

| # | Reset by | Read by | Meaning |
|---|---|---|---|
| 0 | `CDataMgr::LoadConfig` `0x38611` (only when it creates the default config), `ResetSubTimers01` `0x386E1` | nobody | unused |
| 1 | `ResetSubTimers01` `0x386E1`, `0x4AB01`, `0x56E91` | `0x4AB01` (call `0x4ABB6`) | ≥ 500 ms touch-hold in the `marketKeyPadView` touch code (`0x4AB01`, under `0x56E91`, called from `CStateGame::Paint`) |
| 2 | `0x81FF1`, `0x8201D`, `0x8217D` (set `CStateGame+0x400` to 0, 2, 1) | `CStateGame::Paint` (calls `0x8D24A`, `0x8D284`) | `CStateGame+0x408` = elapsed **seconds** (`/1000`) of a timed in-game phase |
| 3 | `CMSocket::Connect` `0x1C419` (call `0x1C58E`), `SkyCanvas::Tick` (call `0x264C8`) | `SkyCanvas::Tick` (call `0x2648E`) | the 10 s socket keep-alive ([network.md](network.md)) |

**Other `GetTimeMs` users.**

| Function | Does |
|---|---|
| `SkyCanvas::KeyPress` `0x2578D` | stores the press time at `SkyCanvas+0xE0` |
| `CDataMgr` popup handlers `0x49481`, `CDataMgr::drawSkillInfoBox` `0x4C2C1`, `0x51685`, `0x5220D`, `0x524C1`, `CDataMgr::keyPopupItemInven_2` `0x554F1` | message-box timing. `CDataMgr+0xF0` is the start and `+0xF8` the current time. They compare the difference against 3,000 ms and 2,000 ms, and `0x524C1` moves the start back by 3 s. |
| `TouchWidget_StartRepeat` `0xA18B1`, `TouchWidget_RepeatDue` `0xA17AD` | touch-widget auto-repeat. Due when `ms < elapsed ≤ 5·ms`; a longer gap restarts it. |
| `TouchWidget_HandleTouch` `0xA18E5` (from `TouchWidgets_Dispatch` `0x8B85`) | touch timestamps; a second tap within 300 ms counts as a double tap |
| `Rand_SeedFromTime` `0x24DE9` | the random seed (below) |

### Random numbers

`Rand_Next` `0x225E5` is the MSVC `rand()` LCG: `seed = seed · 214013 + 2531011`, result `(seed >> 16) & 0x7FFF`. It is used through `Rand_Range(lo, hi)` `0x22641` (19 callers), `Rand_Chance(n, d)` `0x22685` and `Rand_Percent(p)` `0x226B9` (10 callers).

The seed `g_randSeed` `0x11A510` is set once, from the low 32 bits of `GetTimeMs()`, by `CMUtil::CMUtil` `0x24EA1`. That is the base-class constructor of `CMResMgr`, `CMSoundPlayer`, `CMPopup` and others. It also builds a CRC-32 table (`0xEDB88320`), and it seeds only while the seed is still 0.

So **the clock at boot fixes every random outcome**. A deterministic clock makes the whole game reproducible.

## Threads, queues and callbacks

| Context | Created by | What runs there | How it reaches the game |
|---|---|---|---|
| **Main thread** (UIKit) | `UIApplicationMain` in `main` `0x280D` | All game code: the frame timer, every `IState` method, UIKit actions and touches, the ObjC views. | The only context. |
| `CMSocket` run-loop source | `CMSocket::Connect` `0x1C419`: `CFSocketCreateRunLoopSource` (`0x1C626`) added with `CFRunLoopAddSource` (`0x1C648`) to `CFRunLoopGetCurrent()` (`0x1C630`, the main loop) in `kCFRunLoopCommonModes` (`0x1C644`) | `CMSocket_Callback` `0x1C651` | Main thread, between ticks. |
| Audio-session interruption listener | `CMSoundPlayer::CMSoundPlayer` `0x1D9B1` → `0x1D8B1`: `AudioSessionInitialize(NULL, NULL, 0x1CBDD, 0)` (`0x1D8D2`) | `0x1CBDD`: begin interruption (1) → `0x1E314`, end (0) → `0x1E258` (presumably sound suspend and resume) | A `NULL` run loop means the main run loop. |
| OpenAL mixer, `AVAudioPlayer` | the frameworks | mixing and decoding | Internal to the frameworks; no game code runs there. |
| Game Center completion handlers | 7 stack blocks in `GameCenterManager`, one per GameKit call (table below) | Block invokes `0xC0419`, `0xC04D5`, `0xC0571`, `0xC05E5`, `0xC0835`, `0xC08A9`, `0xC091D`, on whatever thread GameKit chooses | Each block (`0xC05E5` first builds the achievements dictionary) calls `-[GameCenterManager callDelegateOnMainThread:withArg:error:]` `0xC0145`. That copies the arguments into a block (`0xC018E`, invoke `0xC03D5`) and `dispatch_async`s it to the main queue (`0xC01BC`, `0xC01C0`). On the main thread, `-[GameCenterManager callDelegate:withArg:error:]` `0xC01D1` asserts `[NSThread isMainThread]` (`0xC01F8`, `0xC020C`) and calls the delegate with `performSelector:withObject:withObject:` (`0xC02B8`) or `performSelector:withObject:` (`0xC02DE`). This is the same pattern as Apple's GKTapper sample code. **This is the game's only GCD use.** |
| StoreKit observer | `-[InAppPurchaseManager init]` `0xBF72D`: `addTransactionObserver:` (`0xBF7BE`) | the `SKPaymentTransactionObserver` methods | StoreKit calls them on the main thread. |
| Chartboost GCD work | Chartboost | image downloads and caching on a global queue | `dispatch_sync`/`dispatch_async` back to the main queue. See [Chartboost](#chartboost). |

GameKit requests and their completion blocks (block literal, then the send that takes it):

| Method | Block | Send |
|---|---|---|
| `-[GameCenterManager AuthenticationUser]` `0xC006D` | `0xC00EE` | `authenticateWithCompletionHandler:` `0xC0126` |
| `-[GameCenterManager FindPlayerByid:]` `0xBFA09` | `0xBFA42` | `+loadPlayersForIdentifiers:withCompletionHandler:` `0xBFA92` |
| `-[GameCenterManager ResetAchievements]` `0xBFAB5` | `0xBFB10` | `+resetAchievementsWithCompletionHandler:` `0xBFB36` |
| `-[GameCenterManager SubmitAchievement:PercentComplete:]` `0xBFB51` | `0xBFBDC`, `0xBFDBA` | `loadAchievementsWithCompletionHandler:` `0xBFC0E`, `reportAchievementWithCompletionHandler:` `0xBFDDC` |
| `-[GameCenterManager ReportScore:forCategory:]` `0xBFE41` | `0xBFF02` | `reportScoreWithCompletionHandler:` `0xBFF24` |
| `-[GameCenterManager ReloadHighScoreByCategory:]` `0xBFF41` | `0xC0032` | `loadScoresWithCompletionHandler:` `0xC0058` |

The block invokes send `callDelegateOnMainThread:withArg:error:` at `0xC0476`, `0xC0506`, `0xC0598`, `0xC07B8`, `0xC085C`, `0xC08D0` and `0xC0A5E`.

### Every other game site

| Site | Function | What | Note |
|---|---|---|---|
| `0x56C8`; `0x57B0`, `0x57C0` | `+[EAGLView_Instance allocWithZone:]` `0x5699` | `objc_sync_enter`; `objc_sync_exit` | `@synchronized(self)` around the singleton allocation. The second exit is the exception path. |
| `0x5862`; `0x5928`, `0x5944` | `+[EAGLView_Instance Instance]` `0x5839` | `objc_sync_enter`; `objc_sync_exit` | the same, around `[[self alloc] init]`. It is called only on the main thread, so the lock is never contended. |
| `0x4762` | `-[EAGLView drawMapleLogo__]` `0x4701` | `-invalidate` | the `ttimer` ivar. Dead code: no selector reference to `drawMapleLogo__` ([program-flow.md](program-flow.md#boot)). |
| `0xA2F10`, `0xA2F32` | `-[ChooseCharacterView animateStatus:]` `0xA29F1` | `-isValid`, `-invalidate` | invalidates the timer it was called with. Dead code: there is no selector reference to `animateStatus:`, and no game code schedules an `NSTimer`. |
| `0xA2754`, `0xA4548`, `0xA5F0C`, `0xAC5EE` | `-[ChooseCharacterView animateStatusAnimation]` `0xA2725`, `okNewAlert:` `0xA44E5`, `clickStart:` `0xA5A61`, `-[MainMenuView drawRect:]` `0xABC85` | `+[UIView beginAnimations:context:]` | Core Animation fades and moves of the UIKit menus, with durations and delays. No stop selector or delegate is set, so nothing calls back into the game. |

### Chartboost

All of the following sit in `lib:chartboost` functions. None runs once the four entry points the game calls are no-ops ([network.md](network.md#chartboost)).

| Function | Sites |
|---|---|
| `+[Chartboost sharedChartboost]` `0xC2E05`, `+[CBDefaultViewController sharedController]` `0xC755D`, `+[CBWebImageCache sharedCache]` `0xD2751` | `dispatch_once` singletons (one block each) |
| `-[CBAPIConnection sendRequest:]` `0xC5375` | `dispatch_after(dispatch_time(0, timeout), main queue, …)`: the request timeout (30 s default, at least 10 s) |
| `-[CBImpression initWithResponse:type:]` `0xCFD61` | `+[NSDate date]`; 3 × `dispatch_async(main queue)` |
| `-[CBNativeInterstitialView prepareWithResponse:]` `0xD0C05`, `-[CBNativeMoreAppsView prepareWithResponse:]` `0xD6639` | `dispatch_group_create`, image loads into the group, then `dispatch_async` of a waiter to the LOW (−2) global queue |
| `0xD1775`, `0xD7075` (the waiter blocks) | `dispatch_group_wait(group, FOREVER)`, `dispatch_release`, `dispatch_async(main queue)` |
| `-[CBWebImageCache loadImageWithURL:checksum:callback:group:]` `0xD2831` | `dispatch_async`/`dispatch_group_async` to the LOW global queue |
| `0xD2955` (its block) | cache lookup or download, then `dispatch_sync(main queue)` with the callback |
| `-[CBUnderfinedProgressBar initWithFrame:]` `0xC9421`, `dealloc` `0xC9521` | `+[NSTimer scheduledTimerWithTimeInterval:1/60 target:self selector:@selector(step:) …repeats:YES]`, `-invalidate` |
| `-[CBReachability startNotifier]` `0xC9C15`, `stopNotifier` `0xC9E3D` | `SCNetworkReachabilitySchedule/UnscheduleWithRunLoop(CFRunLoopGetCurrent(), kCFRunLoopDefaultMode)` |
| `-[Chartboost hasCachedInterstitial:]` `0xC3419`, `-[CBAPIRequest appendDeviceInfoParams]` `0xC645D` | `timeIntervalSinceNow`; `+[NSDate date]`, `timeIntervalSince1970` |
| `+[CBIdentity cbui]` `0xCA36D`, `+[CBIdentity cbuia]` `0xCA459` | `performSelector:` (synchronous: reads `UIDevice` identifiers) |
| `-[Chartboost loadInterstitial:shouldCache:]` `0xC36BD`, `loadMoreAppsShouldCache:` `0xC3A79`, `impressionClickTriggered:forURL:moreData:` `0xC4729`, `-[CBDefaultViewController dismissInterstitial:]` `0xC7E4D`, `dismissMoreApps:` `0xC822D`, `-[CBNativeMoreAppsView initWithFrame:]` `0xD5FE5`, `tableView:cellForRowAtIndexPath:` `0xD7235`, `0xD6D09`, `0xD6E69`, `-[CBNativeMoreAppsFeaturedCell initWithStyle:reuseIdentifier:]` `0xD7AB9`, `-[CBNativeMoreAppsRegularCell initWithStyle:reuseIdentifier:]` `0xD8199`, the `ATFunctionalCollectionExtensions` categories on `NSArray` (`each:` `0xD3139`, `map:` `0xD31B9`, `filter:` `0xD3315`, `inject:initial:` `0xD33F9`) and `NSDictionary` (`0xD3599`, `0xD3619`, `0xD36FD`, `0xD37E5`) | stack blocks: completion handlers, animation blocks and enumeration, run synchronously or on the main thread |

### Atomics and barriers

The sweep finds **66 `dmb ish` and nothing else**: no `ldrex`/`strex`, `clrex`, `dsb`, `isb` or `swp`. All 66 are in Chartboost's synthesized `atomic` scalar property accessors (one `dmb` in a getter, two in a setter):

| Class | Accessors |
|---|---|
| `Chartboost` | `delegate` `0xC51E9`, `setDelegate:` `0xC51FD`, `timeout` `0xC5215`, `setTimeout:` `0xC5229` |
| `CBAPIConnection` | `requestIdMax` `0xC613D`, `setRequestIdMax:` `0xC6151` |
| `CBDefaultViewController` | `viewIsVisible` `0xC8C01`, `loadingViewIsVisible` `0xC8C19` |
| `CBURLOpener` | `delegate` `0xC9BE9`, `setDelegate:` `0xC9BFD` |
| `CBJsonParser` | `maxDepth` `0xCAF71`, `setMaxDepth:` `0xCAF85` |
| `CBJsonStreamParser` | `supportMultipleDocuments` `0xCBBA9`, `setSupportMultipleDocuments:` `0xCBBC1`, `delegate` `0xCBC15`, `setDelegate:` `0xCBC29`, `maxDepth` `0xCBC41`, `setMaxDepth:` `0xCBC55` |
| `CBJsonStreamParserAdapter` | `delegate` `0xCC499`, `setDelegate:` `0xCC4AD`, `levelsToSkip` `0xCC4C5`, `setLevelsToSkip:` `0xCC4D9` |
| `CBJsonStreamWriter` | `maxDepth` `0xCDFE5`, `setMaxDepth:` `0xCDFF9`, `humanReadable` `0xCE041`, `setHumanReadable:` `0xCE059`, `sortKeys` `0xCE071`, `setSortKeys:` `0xCE089`, `delegate` `0xCE0A1`, `setDelegate:` `0xCE0B5` |
| `CBJsonUTF8Stream` | `index` `0xCF8C5`, `setIndex:` `0xCF8D9` |
| `CBJsonWriter` | `sortKeys` `0xCFC99`, `setSortKeys:` `0xCFCB1`, `humanReadable` `0xCFCC9`, `setHumanReadable:` `0xCFCE1`, `maxDepth` `0xCFD35`, `setMaxDepth:` `0xCFD49` |
| `CBImpression` | `state` `0xD0939`, `setState:` `0xD094D`, `type` `0xD0965`, `setType:` `0xD0979`, `view` `0xD09CD`, `delegate` `0xD09E1`, `setDelegate:` `0xD09F5` |

The game code has no barrier or atomic instruction. ANALYSIS.md's earlier "a few `ldrex/strex`" came from a linear sweep that decoded literal pools as code.

### Never used

- **Threads and locks.** No `pthread_*`, `NSOperation` or `NSLock` import.
- **Selectors.** None of these has a selector reference: `detachNewThreadSelector:toTarget:withObject:`, `initWithTarget:selector:object:`, `currentThread`, `mainThread`, `performSelectorOnMainThread:withObject:waitUntilDone:`, `performSelectorInBackground:withObject:`, `performSelector:withObject:afterDelay:`, `cancelPreviousPerformRequestsWithTarget:`, `sleepForTimeInterval:`, `sleepUntilDate:`, `timerWithTimeInterval:…`, `addTimer:forMode:`, `currentRunLoop`, `mainRunLoop`, `runUntilDate:`, `runMode:beforeDate:`, `fire`, `displayLinkWithTarget:selector:`, `addOperation:`, `addOperationWithBlock:`, `lock`, `unlock`.
- **Other.** `__NSConcreteGlobalBlock` has no reference.

## For the runtime

What the HLE runtime has to reproduce:

1. **One guest thread.** Run all recompiled code on one host thread, the guest "main thread". `+[NSThread isMainThread]` returns YES. The game needs no guest threads, no thread-safe guest heap and no atomics. `dmb` lowers to nothing, or a compiler barrier.

2. **An exact run-loop timer, not a direct `Tick` call.**
   - Implement `CFRunLoopTimerCreate`/`AddTimer`/`SetNextFireDate` faithfully: fire at the requested absolute time, one callout per due timer, no queued catch-up. Then let the recompiled `CMTimer::OnRunLoopTimer` do its own gating. That keeps `lastFrame`, the sub-timers, the drift on overruns and the pause semantics exactly as the original.
   - The host loop has to wake with ~1 ms precision, for example `SDL_DelayPrecise`, or high-resolution waitable timers on Windows.
   - **Presenting must not block on vsync**: present with vsync off, or with a mailbox/triple-buffer swap. A host loop locked to 60 Hz would round every tick up to a whole number of 16.7 ms frames. At the 40 ms speed that is 50 ms (3 vsyncs), 20 instead of 25 ticks/s. At 50 ms, `GetTimeMs` rounding makes it 3 or 4 vsyncs, so the ticks jitter.
   - Simply calling `SkyCanvas::Tick` every *interval* ms would also run about 2 % fast and skip the sub-timer bookkeeping.
   - (#80, [ADR 0075](../adr/0075-display-locked-frame-pacing.md).) The port locks to the display only where that rounding is exact. When the refresh rate is a whole multiple of the tick rate (60 or 120 Hz at 50 ms), the guest clock steps at the refreshes by *interval* / n, so the game's own gate opens every n refreshes. Otherwise it uses the wall clock with the 1 ms poll.

3. **One main-thread pump.** In each host-loop turn, drain, in run-loop order:
   - due timers (`CMTimer`, plus any `NSTimer` and `dispatch_after`);
   - run-loop sources (the `CMSocket` callback, if the stubs of #52 ever deliver one);
   - the main dispatch queue, FIFO (the `GameCenterManager` callbacks);
   - SDL input as UIKit touches.

   Nothing may run inside a tick except what the guest calls itself.

4. **A clock with the original semantics, minus the freeze.**
   - Serve `gettimeofday` and `CFAbsoluteTimeGetCurrent` from one host *monotonic* clock, so that wall-clock changes don't stall or jump the game.
   - Add an offset that puts `GetTimeMs` at a value in `[0, 2³⁰)` at launch. For example, take the real ms-of-epoch modulo 2³⁰. The random seed then still differs per launch, but the 2³¹ jump can't occur in under 12 days of uptime.
   - Offer a deterministic mode (fixed offset, virtual clock advanced by the host) for replays and golden-image tests: the boot time seeds `Rand_Next`, and the tick schedule follows the clock.

5. **GCD can run inline.** (#50 chose worker threads that run one at a time instead, with the same main-thread pump: [ADR 0033](../adr/0033-gcd-worker-threads-and-main-queue.md).)
   - `dispatch_async(main queue)` enqueues on the pump.
   - `dispatch_async`/`dispatch_group_async` to a global queue (Chartboost only) can run the block on the main thread at the next pump. Then `dispatch_group_wait` returns at once, and `dispatch_sync(main queue)` from such a block runs inline instead of deadlocking.
   - `dispatch_after` becomes a run-loop timer. `dispatch_once` becomes a flag. `dispatch_release` and `dispatch_group_create` are trivial.

   With Chartboost stubbed, only `dispatch_async(_dispatch_main_q)` from `GameCenterManager` is reached.

6. **Other stubs.**
   - `objc_sync_enter`/`exit` can be no-ops, or a recursive counter. They must tolerate the SjLj exception path's second `objc_sync_exit`.
   - `NSTimer` (Chartboost only) maps onto the same run-loop timers.
   - Completion handlers of stubbed GameKit and StoreKit calls are delivered from the pump, never synchronously inside the request. The originals are asynchronous, and `callDelegate:` asserts the main thread.

7. **Lifecycle.** Call `applicationWillResignActive:`/`applicationDidBecomeActive:` on focus loss or minimize as iOS did. Stop pumping the timer while the window is hidden, as an iOS suspend would. The original then folds the gap into the sub-timers on return. (#128 did this for a minimise and the iOS background, not for focus loss: [ADR 0063](../adr/0063-app-lifecycle-background-and-minimise.md).)

## Open questions

1. Which game phase sub-timer 2 times (`CStateGame+0x400` modes 0–2, `+0x408` seconds), and who reads `SkyCanvas+0xE0`, the key press time.
2. What the popup handlers' 2,000 and 3,000 ms thresholds govern on screen (auto-close or fade). This can be checked once the runtime renders.
3. On which thread iOS 4–6 GameKit delivered the completion handlers. It doesn't matter for the port, because the game marshals them to the main queue itself.
