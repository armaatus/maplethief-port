# 136. The iOS app: an Xcode project from CMake, signed locally, compiled in CI without the game

- **Status:** Accepted
- **Date:** 2026-11-10
- **Issue:** #83 (epic #275)
- **Builds on:** [ADR 0002](0002-language-and-toolchain.md) (languages), [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0025](0025-guest-memory-and-address-space.md) (memory model), [ADR 0055](0055-ios-safe-area-landscape-orientations-and-input.md) (safe area and input), [ADR 0064](0064-macos-game-app.md) (the game app with the bundle inside), [ADR 0100](0100-public-repository-and-player-releases.md) (each user's own IPA), [ADR 0126](0126-dev-branch-release-pull-requests-and-ci-by-target.md) (CI by target), [ADR 0133](0133-one-command-builds.md) (`scripts/build`)

## Context

iOS is the last target in CLAUDE.md with no build. Facts:

- The owner's Mac has only the Command Line Tools: no Xcode, no iOS SDK, no simulator. GitHub's macOS runners do have Xcode, and CI never has the IPA, so CI can compile only our own code.
- `SDL_PLATFORM_IOS` code in the frontend had never been compiled for iOS before this issue; it was only syntax-checked with the macOS SDK.
- An iPhone runs only a signed app. A personal install needs the owner's Apple account, which CMake or CI don't have.
- The game's files must stay on the user's machine (ADR 0100).
- CMake's Xcode generator doesn't allow per-configuration options on a single source file (CMake refuses the generator expression), and `CMAKE_BUILD_TYPE` is empty because Xcode has several configurations in one project.
- ADR 0025 requires checked generated code and the arena alone on iOS. `MT_MEM_RESERVE_FOR_BUILD` gave `AUTO` for checked code, which tries a 4 GiB reservation first.

## Options considered

**Project generator.** (1) The Xcode generator through a preset: the project opens in Xcode, which is what signing and installing need anyway. Accepted. (2) Ninja for a command-line build plus a hand-written `xcodeproj`: a project to maintain and two ways to build. Rejected.

**Where the game's files come from.** (1) Copied from `work/ipa` at build time into `MapleThief.app/Game`, as the macOS game app does. Accepted: the app is self-contained, and nothing derived from the IPA enters git. (2) The IPA import screen of a player build. Rejected for now: an iPhone has no file dialog for this ADR's scope, and a sideloaded build is always made on the user's own Mac from their own IPA anyway.

**Signing.** (1) Automatic signing with a team id given as `MAPLETHIEF_IOS_TEAM`, in the project, plus Xcode's own signing tab for anyone who prefers that. Accepted. (2) Signing by `codesign` after the build: it needs the provisioning profile by hand. Rejected.

**CI.** (1) A separate `ios` job on `macos-latest` that compiles the frontend and runtime with a `ios-ci` preset (no generated code, no IPA, no signing), on the runs that build every OS. Accepted. (2) In the `build` matrix: it has every OS's steps and every test step, none of which apply. Rejected.

## Decision

**Presets.** `ios-app` and `ios-ci` (both hidden base `ios-base`: Xcode generator, `CMAKE_SYSTEM_NAME=iOS`, `iphoneos`, arm64, iOS 16.0, `MAPLETHIEF_MEM_MODEL=checked`). `ios-app` sets `MAPLETHIEF_IOS_APP` and needs `work/gen` and `work/ipa`. `ios-ci` sets `MAPLETHIEF_GEN_DIR` empty and code signing off, so it builds the placeholder shell. Build presets `ios-app-{debug,release}`, `ios-ci-{debug,release}`; workflow `ios-ci`.

**The memory model (ADR 0025) is enforced at every level.** `maplethief_mem_checked` always answers checked on iOS, and `reserved` is a configure error. `MT_MEM_RESERVE_FOR_BUILD` is `MT_MEM_RESERVE_ARENA` on iOS, and `maplethief/memory.h` is a compile error if an iOS unit has `MT_MEM_CHECKED 0`.

**The app.** `cmake/IosApp.cmake`. `maplethief_ios_target` adds the asset catalog `platform/icon/Assets.xcassets` (the port's own icon, drawn opaque and full-bleed by `mtport.appicon`, plus the launch image and colour), sets automatic signing with `MAPLETHIEF_IOS_TEAM`, the bundle id `MAPLETHIEF_IOS_BUNDLE_ID` (default `io.github.armaatus.maplethief`) and the device family. With `MAPLETHIEF_IOS_APP` it copies the extracted bundle to `MapleThief.app/Game` as a POST_BUILD step, which in Xcode runs before signing. `MT_EMBEDDED_BUNDLE="Game"`, found through `SDL_GetBasePath()` as on the Mac. `Info.plist` carries #73's audit list: landscape left and right only, `UIRequiresFullScreen`, `UIStatusBarHidden`, `CADisableMinimumFrameDurationOnPhone`, `UIRequiredDeviceCapabilities` arm64, and `UILaunchScreen` from the catalog. The frontend links UIKit, GameController and CoreHaptics; a new iOS source file goes in `_maplethief_ios_sources` in `platform/CMakeLists.txt`.

**Xcode generator.** `maplethief_debug_opt` with sources optimises the whole target in Debug under Xcode (the per-source form is refused). `MT_BUILD_TYPE`/`MT_BUILD_OPT` come from `$<CONFIG>` on a multi-config generator. `tests/` and `cmake/Packaging.cmake` are skipped on iOS: nothing runs tests there and the app is not packaged.

**`scripts/build ios`** (`mtport.build`, ADR 0133): stops first with what is missing (Xcode or its iOS SDK, via `doctor`'s check); then bootstrap, recompile, configure `ios-app` (again when the team or bundle id changes), build, and copy the app to `dist/ios/MapleThief.app` (`dist/ios/debug/` for Debug). It signs with `-allowProvisioningUpdates` when `MAPLETHIEF_IOS_TEAM` is set. Otherwise it builds unsigned and says what to do next. `build ios` is rejected on anything but a Mac.

**CI.** Job `ios` in `ci.yml`, on `macos-latest`, only where every OS runs (release pull request and manual runs, ADR 0126) and never on self-hosted runners. It runs the `ios-ci` workflow (Debug and Release) and checks the built app is arm64 and its `Info.plist` lints. It links the frontend and runtime, but not the generated code: that needs the IPA.

## Consequences

- iOS now compiles in CI for the first time. Run the job to see the compile state of our own code for iOS. The game itself can only be built and run where the IPA is, which is a `needs-human` hand-off on #83.
- In an Xcode Debug build, every source of the runtime is optimised, not just the hot ones (the per-source form is refused). This only affects stepping through the runtime in an Xcode Debug build.
- `CODE_SIGNING_ALLOWED=NO` builds still make an app that a device won't install; that's stated in the build's own summary line.
- A free Apple account's build lasts 7 days and needs a bundle id of its own (`docs/play/iphone.md`).
- **How we'd know this was wrong:** the iOS job fails on its own code once everything is merged, or a device build refuses to launch for reasons that come from this setup rather than the game (signing, memory, plist).
