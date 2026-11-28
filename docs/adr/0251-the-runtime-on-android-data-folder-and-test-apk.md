# 251. The runtime on Android: the data folder in the external files folder, the seam tests in a test APK

- **Status:** Accepted (delegated by Joris, 2026-11-28)
- **Date:** 2026-11-28
- **Issue:** [#357](https://github.com/armaatus/maplethief-port/issues/357) (epic [#292](https://github.com/armaatus/maplethief-port/issues/292))
- **Builds on:** [ADR 0150](0150-android-target.md) (the target), [ADR 0163](0163-android-toolchain-pins-gradle-step-and-emulator-runs.md) (the build and the emulator), [ADR 0025](0025-guest-memory-and-address-space.md) (guest memory), [ADR 0030](0030-runtime-diagnostics.md) (the crash reporter), [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md) (atomic saves), [ADR 0127](0127-data-folder-next-to-the-game.md) (the data folder)

## Context

ADR 0150 put the runtime on Android with the checked memory model, the arena reserved with `mmap`, `game/` and `data/` in the app's external files folder, and the crash report reaching the next start as on iOS. #356 built the APK; nothing yet checked the runtime's platform seams on an Android device, and the data folder's resolver had no Android rule: on Android `SDL_GetBasePath` gives nothing and `SDL_GetPrefPath` the app's internal files folder, so the resolver fell back to a folder no player can reach with a file manager. On Horizon, #236 checked the same seams with a test NRO (`MapleThiefTests.nro`, `scripts/switch-test`).

What the seams need on Android (bionic, API 26 to 35, arm64), found while building #357:

- **Memory.** The POSIX backend of `runtime/src/mem/vm.cpp` (reserve with `mmap(PROT_NONE, MAP_NORESERVE)`, commit with `mprotect`, decommit with `mmap(MAP_FIXED)`) works unchanged; the page size comes from `sysconf`, so the 16 KiB image takes the same code.
- **Crash reports.** `crash.cpp`'s `sigaction` handlers (`__linux__ && __aarch64__`) run in an app's process before the system's own (debuggerd's) and hand the signal on. The frontend already treats Android as iOS (no helper process; the report in the data folder's `crashes/`, offered at the next start); it needs only the data folder.
- **Files and saves.** The external files folder is FUSE (Android 11+) over ext4 or F2FS; `rename` over an existing file and `fsync` of a directory work there, so ADR 0074's one-step replacement applies.
- **Processes.** An app may start a program only from its extracted native library folder (W^X since API 29); fork works.
- **No `HOME`** in an app's process, and the runtime cannot know the app's folders without the activity.

## Decision

**The data folder.** A new rule in `platform/src/data_location.cpp`, location `android`, reason `platform`: the app's external files folder (`SDL_GetAndroidExternalStoragePath`, `/storage/emulated/0/Android/data/io.github.armaatus.maplethief/files`) when `SDL_GetAndroidExternalStorageState` says it is readable and writable; a file manager reaches it, and the app needs no permission for it. ADR 0127's layout goes inside it, `game/` included (there is no `<base>`). When the external storage is not mounted writable, the app's internal files folder (`SDL_GetAndroidInternalStoragePath`), reason `fallback-not-writable`, so the game still saves. No flag, marker or probe applies; `MAPLETHIEF_DATA_DIR` still wins. The settings menu calls it the app folder, as on iOS. The runtime's own default data folder (`mt_stubs_data_dir` without the frontend) is empty on Android: the frontend always sets it.

**No Android backend of its own** for guest memory, crash handling, file writes or threads: the POSIX code is the Android code, and the device run is what proves it.

**The test APK.** `MapleThiefTests.apk` (package `io.github.armaatus.maplethief.tests`), built by the `android-*` presets beside `MapleThief.apk` (target `MapleThiefTests_apk`, so CI's `android` job builds it), from a Gradle project of its own under `platform/android/tests/` with the same pins, tools and debug key:

- `libMapleThiefTests.so`: the runtime's suite (`runtime_test`'s sources: memory, the Horizon-named OS layer cases, diagnostics, imports, ...) with `saves_test.cpp`, the same set as `MapleThiefTests.nro`. `doctest_main.cpp`'s main is `mt_android_doctest_main` there.
- `TestsActivity` calls it through JNI (`tests/cpp/android_test_host.cpp`) on a thread with a 64 MiB stack. stdout and stderr go line by line to logcat (tag `MapleThiefTests`) and to `results.txt` in the app's external files folder; the first lines name the Android release, the page size and the folders, the last one is `android: all tests passed (exit status 0)` or `android: tests failed (exit status N)`.
- `TMPDIR` is `<external files>/tmp`, so the saves' and files' cases run on the file system a player's saves live on; one Android-only case checks that.
- `crash_child` is packed as `libcrash_child.so`, and the APK's native libraries are extracted (`useLegacyPackaging true`, `extractNativeLibs`), so `diag_test.cpp`'s crash cases start it from the native library folder (`MT_CRASH_CHILD_PATH`) and check the hardware-fault report as on desktop.

**`scripts/android-test`** `[--image 4k|16k | --serial S] [--preset P] [--build] [-- DOCTEST_ARGS]` installs the APK with adb on the emulator (booted by `scripts/android-emulator`, now shared with `scripts/android-run`) or on any device adb sees, starts the activity with the doctest arguments as a string array extra, waits for the last line and keeps `results.txt` and logcat in `build/test-output/android/`. No CTest runs it; like `scripts/switch-test` it is a local run.

**The AVD's data partition** is written as 2 GiB, but the API 35 images raise it to 6 GiB, and the emulator refuses to start with less than about 7.4 GB free on the disk. `scripts/android-emulator` reports the emulator's own last lines when it ends before booting instead of waiting.

## Consequences

- A player's saves, logs, crash reports and the imported `game/` are in one folder a file manager shows (`Android/data/io.github.armaatus.maplethief/files`). Uninstalling removes it unless the player keeps the data (`hasFragileUserData`, ADR 0150).
- The same seam tests run on desktop (CTest), on Horizon (`MapleThiefTests.nro`) and on Android (`MapleThiefTests.apk`); a new OS-layer case reaches all three by being in `runtime_test`'s sources.
- The `android` CI job builds one more library and APK (a few minutes in the container).

**How we'd know this was wrong:**

- A phone whose external storage is missing or removable-only leaves players without a reachable folder: then the export through the picker (#359) is the way out, and this rule gets a second look.
- Android stops letting an app start a program from its native library folder: then the crash cases move to a `fork` in the test process.
- A device's FUSE layer turns out to break `rename` over a file or a directory `fsync`: then the two-step replacement of the Switch (ADR 0074's Horizon addendum) becomes Android's too, which the test APK's saves cases would show first.
