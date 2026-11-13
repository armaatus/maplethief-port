# 150. The Android target: one arm64 APK without assets, the IPA through the system file picker, the software renderer first

- **Status:** Accepted (Joris, 2026-11-13, [on #355](https://github.com/armaatus/maplethief-port/issues/355#issuecomment-6854396249)); the toolchain pins, the API levels, the build shape, the package name and the data folder are delegated calls (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#355](https://github.com/armaatus/maplethief-port/issues/355) (epic [#292](https://github.com/armaatus/maplethief-port/issues/292))
- **Builds on:** [ADR 0002](0002-language-and-toolchain.md) (languages), [ADR 0003](0003-dependencies.md) (dependencies), [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0023](0023-local-ci-and-packaging.md) (containers), [ADR 0025](0025-guest-memory-and-address-space.md) (guest memory), [ADR 0039](0039-rendering-backend-software-rasterizer.md) and [ADR 0060](0060-gpu-rendering-backend.md) (rendering), [ADR 0100](0100-public-repository-and-player-releases.md) (player releases), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (player builds and the release guard), [ADR 0127](0127-data-folder-next-to-the-game.md) (data folder), [ADR 0135](0135-accept-an-ipa-by-its-main-binary.md) (which IPA is accepted), [ADR 0136](0136-ios-app-through-xcode.md) (iOS), [ADR 0138](0138-nintendo-switch-homebrew-target.md) (the Switch, the model for this ADR)

## Context

The epic's research (2026-11-13) sets out why Android fits: the generated C is compiled ahead of time, so nothing needs a JIT; the CPU is AArch64 little endian, which the generated code already builds for (macOS, iOS, the Switch); the checked memory model needs no 4 GiB reservation; and the game was made for a phone, so the touch key pad, its editor, the safe-area layout, the app lifecycle and haptics already exist for iOS. Joris decided go on 2026-11-13. This ADR settles what every other child of the epic builds on. Nothing here ran on a phone or an emulator yet; the facts below come from SDL's sources and Google's documentation, checked on 2026-11-13.

**SDL 3.4.18 on Android** (the version every other target pins). From `docs/README-android.md` and `android-project/` in the pinned `SDL3-3.4.18.tar.gz`:

| | SDL 3.4.18's own project template |
|---|---|
| Android SDK | 35 or later (`compileSdkVersion 35`, `targetSdkVersion 35`) |
| Minimum API | 21 (Android 5.0) |
| NDK | `ndkVersion = "28.2.13676358"` (r28c). The README in the archive still says r15c or later; SDL's wiki and an upstream change of May 2026 raise the stated minimum to r28c |
| ABI | `abiFilters 'arm64-v8a'` |
| Gradle / Android Gradle Plugin | Gradle 8.12, AGP 8.7.3 (which needs JDK 17) |
| How the app starts | `SDLActivity` (Java, part of SDL, Zlib) loads the native libraries named by `getLibraries()` and calls `SDL_main` in `libmain.so` through JNI (`src/core/android/SDL_android.c`) |
| Renderers | `opengles2`, `vulkan`, `gpu` (SDL_GPU on Vulkan), `software` |
| Files | `SDL_IOFromFile` and `SDL_ShowOpenFileDialog` take and return Storage Access Framework content URIs (`content://…`); `SDL_GetAndroidExternalStoragePath()` is the app's external files folder, `SDL_GetAndroidInternalStoragePath()` its internal one, both without a permission |

**The NDK.** On 2026-11-13 the latest stable NDK is r29 (`29.0.14206865`), the latest long-term release r27d, and r28c is the one SDL's template is built and tested with. Since r28 the NDK links shared libraries with 16 KiB page alignment by default, which Android 15 devices with 16 KiB pages require.

**API levels.** Google Play wants new apps to target API 36 since 31 August 2026, but this port is not on the Play Store (epic #292's scope), and the system's installer (Android 14 and later) only refuses apps that target below API 23. Android 8.0 (API 26) and later are on almost every arm64 phone in use; it brings AAudio, which SDL prefers over OpenSL ES, and `ACTION_OPEN_DOCUMENT` has existed since API 19.

**Host page size.** Some Android 15 and later devices run 16 KiB pages, as Apple arm64 does. ADR 0025 already handles 4 KiB and 16 KiB hosts (`runtime/src/mem/memory.cpp` refuses any other size).

**Building on the Mac and in CI.** Google ships the NDK and the SDK's build tools (`aapt2`, `apksigner`'s helpers) for macOS (universal), Windows x86_64 and Linux x86_64; there is no Linux arm64 build. So a Linux container for the Android build is `linux/amd64` and runs under emulation on Apple silicon. The Android emulator's `arm64-v8a` system images run natively on Apple silicon (Hypervisor.framework); on GitHub's x86_64 Linux runners only x86_64 images run fast, and this port builds no x86_64 APK.

**Distribution and Google's developer verification** (research of 2026-11-13, Google's developer verification pages and FAQ). Since 30 September 2026 certified Android devices in four countries (Brazil, Indonesia, Singapore, Thailand) refuse to install an app whose package name is not registered to a verified developer, when the app comes from a participating store. Google says it will extend this globally in 2027. Sideloading by `adb` is exempt. A player can turn on the *advanced flow* once (developer options, a restart, a 24-hour wait) and then install unverified apps. A developer can register package names with a **full account** (25 USD, a government ID) or a free **limited distribution account** (an email address, no ID, installs on at most 20 devices).

## Options considered

### Format

1. **One `arm64-v8a` APK, sideloaded from the release page.** **Chosen** (Joris). One file per release, as the Switch has one NRO; every phone that can run the game is arm64.
2. **An Android App Bundle (AAB) or split APKs.** Rejected: they are made for a store that builds the APKs; a player can't install an AAB directly.
3. **`armeabi-v7a` or `x86_64` APKs as well.** Rejected (epic #292: no 32-bit, no x86 Android). A 32-bit build would need the reserved model's 4 GiB trick or more address space than a 32-bit process has to spare, and the generated code is not tested there.

### Renderer

1. **The software rasterizer through SDL's `opengles2` renderer.** **Chosen** (Joris), as the Switch started. GLES 2 is on every Android phone; the rasterizer is the same code as on every other target.
2. **`gfx_backend = gpu` (SDL_GPU on Vulkan) as the default.** Rejected for now: Vulkan on Android varies by device and driver, and no phone has been measured. It stays an option for a later issue, with ADR 0060's SPIR-V shaders already built.

### How the build is driven

1. **Gradle's `externalNativeBuild` runs our CMake.** Rejected: the presets, `scripts/build`, CTest and the generated code's CMake would sit behind Gradle, unlike every other target.
2. **Our CMake builds `libmain.so` with the NDK's toolchain file, and a step in the same build packages it with Gradle.** **Chosen.** `cmake --workflow --preset android-release` stays the one command, as `switch-release` is.

### Where the data folder sits

1. **The app's internal files folder.** Rejected: nobody but the app can reach it, so a player can't copy a save off the phone.
2. **The app's external files folder** (`/storage/emulated/0/Android/data/<package>/files/`). **Chosen.** It needs no permission, and a computer reaches it over USB. Android 11 and later hide it from most file managers on the phone itself, so the in-game export and import of saves go through the system picker (#359).
3. **A folder the player picks once (SAF tree permission).** Rejected for the data folder: every file access would go through content URIs, and `runtime/`'s atomic saves (ADR 0074) need real paths and renames.

## Decision

**Format.** The Android target is one APK, `MapleThief-<version>-android-arm64.apk`, for `arm64-v8a` only, installed by the player from the release page (or by `adb`). Its package name is `io.github.armaatus.maplethief` (delegated call: the project's own GitHub namespace, no name of the game's publisher), its label `MapleThief`, its icon our own (ADR 0125). It is landscape only (`sensorLandscape`) and declares no permission. It sets `android:hasFragileUserData="true"`, so the system's uninstall dialog offers to keep the data folder. No AAB, no split APKs, no 32-bit or x86 build, no Android TV build, no Play Store or F-Droid listing.

**Toolchain pins** (delegated calls; SDL 3.4.18's tested set, so the first build meets no surprise):

| | Pin |
|---|---|
| NDK | r28c, `28.2.13676358` |
| Android SDK platform (compile and target) | API 35 (`platforms;android-35`), build tools `35.0.0` |
| Minimum API | 26 (Android 8.0) |
| Gradle / AGP / JDK | Gradle 8.12 (wrapper with its `distributionSha256Sum`), AGP 8.7.3, Eclipse Temurin JDK 17 |
| CMake | the Linux container's 4.4.4 (ADR 0003's minimum is 3.28); the SDK's own CMake is not used |
| Emulator images | `system-images;android-35;google_apis;arm64-v8a`, and the 16 KiB page image `system-images;android-35;google_apis_ps16k;arm64-v8a` |

Every SDK package is installed by `sdkmanager` at an exact version and every download checked by its SHA-256, by #356. Raising a pin (r29, API 36) is an ordinary change that names its reason and passes the same checks; nothing here needs the newest release.

**The build.** The `android-debug` and `android-release` presets use the NDK's `build/cmake/android.toolchain.cmake` with `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=26` and `ANDROID_STL=c++_static`. The game is `libmain.so`, and SDL3 is linked into it statically (ADR 0003), so `SDLActivity.getLibraries()` is overridden by our activity, `MapleThiefActivity`, to load `main` alone. A small Gradle project under `platform/android/` (our own manifest, activity and icon, and SDL's Java sources from the pinned archive, never copied into git) packages the built `libmain.so` as a prebuilt library; Gradle never runs CMake. The build runs in a pinned `linux/amd64` container under `tools/containers/android/` (ADR 0023), which is what CI and `scripts/local-ci` use, and on the Mac natively with the same pins installed into `build/_android-sdk/` (git-ignored), which is the fast path for development and the only one that can start the emulator. No AndroidX or other Java library is used. With the generated code (`--gen work/gen`, local only) the APK is the game; without it the placeholder shell, as everywhere.

**Testing.** The Android emulator on this Mac, with the arm64 images above, is the agent's device: installs, the runtime's seam tests (#357), the boot to the first map (#358), the import (#359), touch, controllers through the emulator's input and the lifecycle (#360), and the release checklist's emulator half (#361) all run there, with screenshots of our own runs kept out of git. Runs on the 16 KiB page image are part of each child's check. A real phone is still needed for what the emulator can't show: frame pacing and heat on a real GPU and display, real Bluetooth and USB controllers, a call interrupting the game, and the player's install from the release page. Those runs, and nothing else, are `needs-human`. CI builds the APK and runs the host-side tests; it does not start an emulator.

**Memory.** The `android-*` presets set `MAPLETHIEF_MEM_MODEL=checked`, as `ios-base` and the Switch do, and `maplethief_mem_checked()` makes `reserved` a configure error on Android. The runtime reserves only the 512 MiB arena with `mmap` (`PROT_NONE`) and commits pages on demand: the POSIX backend in `runtime/src/mem/vm.cpp` carries over, and #357 proves it on the emulator with both page sizes. The low-memory killer is the risk, not the address space; #360 maps `onTrimMemory` and low memory to the autosave.

**Rendering.** The software rasterizer (ADR 0039) is the default. The frontend sets `SDL_HINT_RENDER_DRIVER` to `opengles2` before creating the renderer, and the presenter (ADR 0042, ADR 0043) places the picture in the full screen, inside the display cut-out's safe area (ADR 0055). `gfx_backend = gpu` on Vulkan is not offered until an issue measures it on phones; `platform_caps.h` hides the row on Android until then (#360).

**Where the files live.** The data folder of ADR 0127 is the app's external files folder, `SDL_GetAndroidExternalStoragePath()`, with the location `android` and the reason `platform`:

```text
/storage/emulated/0/Android/data/io.github.armaatus.maplethief/files/
  game/                       the imported bundle, regenerable from the IPA
  data/                       settings.ini, bindings.ini, saves/, backups/, states/, screenshots/,
                              exports/, import/, logs/, crashes/ (ADR 0127's layout; no window.ini)
```

If the external folder is not mounted, the internal one (`SDL_GetAndroidInternalStoragePath()`) takes its place with the reason `not-writable`, said on screen as ADR 0127's fallbacks are. There is no per-user option and no marker. Uninstalling removes the folder unless the player keeps the data in the uninstall dialog; `docs/play/android.md` says so and points at the save export.

**How the IPA comes in.** The import screen (#182) gets an Android route: `SDL_ShowOpenFileDialog` opens the system's document picker (Storage Access Framework, `ACTION_OPEN_DOCUMENT`, filters `*.ipa` and any file, since many file providers report no type for an IPA). The returned content URI is copied through `SDL_IOFromFile` into the app's cache folder, the copy goes through the import unchanged (ADR 0135's check of the exact 1.5 file, the safe extraction, a temporary folder renamed into `game/`), and the copy is deleted whatever the result. The player's original file is never written. A refusal shows its reason as on desktop. The same picker serves the save export and import.

**What ships.** The player build under ADR 0100: `libmain.so` with the runtime and the recompiled code, our activity, SDL's Java classes, our icon and the licence notices, and no file from the IPA's bundle and nothing converted from one. The release guard's player mode reads the APK's zip entries (#361). The APK links SDL (Zlib, its Java shim included), the NDK's libc++ (Apache-2.0 with LLVM exceptions, linked statically) and the system's own libraries (Bionic, `libGLESv2`, `libEGL`, `libandroid`, `liblog`, `libaaudio`, `libOpenSLES`, which are not shipped). The notices go into the APK's `assets/licenses/` and `THIRD_PARTY.md`.

**Signing.** An APK installs only when signed. The release APK is signed with APK Signature Scheme v2 and v3 by `apksigner`, with a key Joris generates once on his own machine with `keytool` (RSA 4096, a validity of 50 years, a distinguished name of `CN=MapleThief port`, no person's or company's name), kept in a key store outside the repository with its password, and backed up by him. It never enters the repository, CI, a release asset or an agent's worktree; `scripts/release` signs on the Mac at release time and asks for the key store's path and password (#361). An update installs over an older version only if both carry the same key, so losing the key means every player reinstalls and loses data they did not export. CI and development builds use a debug key generated in the build folder, which the release guard refuses for a published APK. `versionCode` grows with every release, derived from the release version by #361.

**Distribution.** The APK is an asset of the GitHub Release beside the other player builds and on the itch.io page (ADR 0146) as its own channel. No store.

**What changes in 2027 (a known later risk).** When Google extends developer verification globally, a certified phone will refuse to install the APK from the release page unless one of these holds:

1. Joris registers the package name and the key's certificate with a **limited distribution account** (free, no ID): installs then work on at most 20 devices, which does not fit a public release;
2. Joris registers it with a **full account** (25 USD and a government ID, which ties his identity to the app);
3. the player turns on the **advanced flow** once (developer options, a restart, a 24-hour wait), or installs with `adb` from a computer.

Until the rollout reaches a player's country nothing changes. This ADR does not choose among the three: when Google publishes the date, a new ADR does, and it is Joris's call because two of the options are his identity or money (CLAUDE.md's hand-offs). Until then `docs/play/android.md` describes the third option as the fallback for a player whose phone refuses the APK.

**Not a target:** the Play Store, F-Droid (it builds from source, and the generated code needs the game), 32-bit and x86 Android, Android TV, ChromeOS as its own target, Vulkan as the default renderer.

## Consequences

- Android gets the same SDL version, the same renderer path and the same memory model as targets already tested, so most of the epic is wiring: presets and a container (#356), the seams (#357), the frontend (#358), the import (#359), input and lifecycle (#360), the player build (#361).
- The Mac can build and test the APK end to end on the emulator, so the agent does every check but the real-phone runs; `needs-human` on #356 to #361 is only for a real phone and for signing with Joris's key.
- The container under emulation on Apple silicon is slow; the native Mac SDK is the everyday path, and the two must keep the same pins (#356 keeps them in one file both read).
- The data folder can disappear with the app. The export through the picker and `hasFragileUserData` are the mitigations, and the play page says it plainly.
- SDL's Java sources are taken from the pinned archive at build time, so a change in SDL's Java shim arrives with an SDL bump and its checks, never by hand.
- A second ADR on distribution is expected in 2027 (developer verification).

**How we'd know this was wrong:**

- The emulator's arm64 image runs the game but phones of the kind players have don't hold 20 ticks a second on the software path (#358's telemetry and a real phone's run): then `gfx_backend = gpu` on Vulkan becomes an issue with its own measurements, and the default changes in a new ADR.
- The low-memory killer stops the game in the background on common phones despite the checked arena: then the arena's committed size and the bundle's memory need a look before anything else.
- SDL 3.4's Android backend can't read the content URIs a common file provider returns: then the import gets its own small Java helper that copies the stream, behind the same import screen.
- Google's 2027 rules leave no practical route for a free public APK: then the APK stays for `adb` and advanced-flow installs, the play page says so, and the itch.io channel is reconsidered.
