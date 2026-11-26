# 163. The Android toolchain: one pins file, checked archives, CMake drives Gradle, the emulator by script

- **Status:** Accepted (delegated by Joris, 2026-11-26)
- **Date:** 2026-11-26
- **Issue:** [#356](https://github.com/armaatus/maplethief-port/issues/356) (epic [#292](https://github.com/armaatus/maplethief-port/issues/292))
- **Builds on:** [ADR 0150](0150-android-target.md) (the target and its pins), [ADR 0003](0003-dependencies.md) (pinned dependencies), [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0023](0023-local-ci-and-packaging.md) (containers, local CI), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (our own icon), [ADR 0139](0139-switch-build-checks-stubs-and-hello-path.md) (the Switch build, the model for this one)

## Context

ADR 0150 fixed what the Android build is: the NDK r28c, API 35 and 26, Gradle 8.12 with AGP 8.7.3 on JDK 17, `libmain.so` built by our CMake and packaged by Gradle, a `linux/amd64` container for CI and the same pins natively on the Mac, the arm64 emulator as the agent's device. It left open how the pins are kept in one place, how they are installed and checked, how CMake hands the library to Gradle, and how the emulator runs are made. #356 had to answer those to build the first APK. Facts found on the way (2026-11-26):

- Google's SDK repository (`repository2-3.xml`) publishes a SHA-1 per archive, not a SHA-256. `sdkmanager` installs only the newest revision of the emulator and of a system image, so it can't hold those to a pin.
- AGP 8.7 shortens resource paths in a release APK (`res/mipmap-xhdpi-v4/ic_launcher.png` becomes `res/FS.png`); only the manifest, read by `aapt2 dump badging`, says which file is the icon. Its PNG crunching keeps the pixels.
- The NDK's toolchain file compiles every build type with `-g`; the Release `libmain.so` of the placeholder is 9.4 MB stripped.
- Bionic has `posix_spawn` only from API 28, and an app's process is the system's `app_process`, not a program that can be started again.
- The whole frontend and runtime compiled for `aarch64-linux-android26` with that one exception and no warning under CI's `-Werror`.

## Decision

**One pins file.** `tools/containers/android/pins.env` holds every version and every archive (URL and SHA-256) as plain `KEY=value` lines: the NDK, the platform, the build tools, platform-tools, the emulator, the two system images, Temurin 17 and Gradle. It sits in the container's build context so the Dockerfile can copy it. `cmake/AndroidToolchain.cmake` reads the NDK's path from it, `platform/android/android.cmake` the JDK, Gradle and build tools, `platform/android/settings.gradle` the API levels and the AGP version (the `plugins` block can't read a variable, so `build.gradle` fails when the applied AGP differs from the pin). The SHA-256s are ours, computed from the downloaded archives; the commit that changes a pin says why.

**Installed from checked archives, not by `sdkmanager`.** `tools/containers/android/install-sdk.sh SDK_ROOT darwin|linux PACKAGE...` downloads each archive, refuses it unless its SHA-256 matches, unpacks it beside its place and moves it into sdkmanager's layout (`ndk/<version>`, `platforms/android-35`, `system-images/android-35/google_apis/arm64-v8a`, ...), with a stamp that makes a second run touch no network. The container runs it into `/opt/android-sdk` (the build's packages only); `scripts/android-sdk install [--image 4k|16k]` runs it on the Mac into `build/_android-sdk/` (git-ignored, about 3.6 GB, plus 1 GB for the emulator and about 4 GB per image), and `remove-image` deletes an image and its AVD to make room for the other. No licence file is written: AGP needs one only to download a missing package, and every package it needs is installed. No `cmdline-tools`: `scripts/android-run` writes the AVD's `config.ini` itself. This replaces ADR 0150's "installed by `sdkmanager`": the pins are stricter this way (an exact emulator and image revision, SHA-256 instead of SHA-1).

**CMake drives Gradle, and Gradle never runs CMake.** On Android `MapleThief` is a shared library with `OUTPUT_NAME main`. The target `MapleThief_apk` (the `android-*` build presets build it alone) copies `libmain.so` into `build/<preset>/platform/android/jniLibs/arm64-v8a/`, strips the copy except in Debug, draws the launcher icons (`mtport.appicon android-icons`: the port's own plate at the five densities), generates the build tree's debug key with the pinned JDK's `keytool` once, and runs the pinned Gradle on `platform/android/` with `-Pmaplethief.*` properties for every path in the build tree. SDL's Java sources come from `android-project/app/src/main/java` of the archive FetchContent unpacked. `JAVA_HOME`, `ANDROID_HOME`, `ANDROID_USER_HOME` (the build tree) and `GRADLE_USER_HOME` (`<download dir>/android-gradle-home`, where AGP's own downloads are cached with the rest) are set for that one command, and the project cache goes to the build tree, so nothing is written to the home folder or the source tree. The result is `build/<preset>/MapleThief.apk`; `android-release`'s package step zips it into the kit `MapleThief-kit-<version>-android-arm64.zip`. There is no Gradle wrapper: its jar is a binary we would commit, and the pinned distribution is installed with its SHA-256 like everything else. `versionCode` is 1 until #361 derives it from the release version.

**The manifest declares no permission**, not even SDL template's `VIBRATE`; rumble and haptics decide it with #360. `allowBackup` is off (the saves leave the phone through the export), `hasFragileUserData` on (ADR 0150).

**The APK's check.** CTest's only test of an Android build is `platform.android_apk` (`python3 -m mtport.apk check`, the `android-*` test presets): `lib/arm64-v8a/libmain.so` is the only native library, stored at a 16 KiB boundary, an AArch64 shared object whose `LOAD` segments are aligned to 16 KiB or more; a v2 signing block; no IPA entry and nothing under `Payload/`; and through the pinned `aapt2 dump badging` the package, label, version name and API levels, and each density's icon, decoded and compared pixel for pixel with `mtport.appicon`. The parser's tests use synthetic APKs (`tests/test_apk.py`). The release guard's APK mode stays #361's.

**Where the frontend differs.** The crash dialog's helper process (`crash_report.cpp`) is skipped on Android, as on iOS: the report is still written, and the next start's offer still reads it.

**The container and CI.** `tools/containers/android/Dockerfile`: the Linux CI container's Ubuntu digest and apt snapshot (Ninja, Python 3, curl, unzip), CMake 4.4.4, and `install-sdk.sh linux ndk platform build-tools jdk gradle`. `scripts/build-android` builds it (tagged with a hash of the Dockerfile, the pins and the installer), exports a commit without the IPA and runs `cmake --workflow --preset android-release` inside, like `scripts/build-switch`; on Apple silicon it runs under emulation. CI's `android` job runs it on every pull request whose changes reach the Android build (`scripts/ci-scope` prints `android=`), with `build/_downloads` cached by the hash of `Dependencies.cmake`, the pins and the Gradle files. `scripts/local-ci` has the same `android` job. `scripts/build android` (`just build android`) builds natively when the Mac has the pinned SDK, in the container otherwise, into `dist/android/`.

**The emulator runs.** `scripts/android-run [APK] --image 4k|16k` writes the AVD (`build/_android-sdk/avd/maplethief-<image>.avd`: arm64, 4 cores, 3 GiB, a 1080 x 2340 phone, SwiftShader), boots it without a window, checks `getconf PAGESIZE`, `adb install`s the APK, starts `MapleThiefActivity`, waits for MapleThief's start-up log line in logcat, takes a screenshot and stops the emulator. The log, the screenshot and the emulator's output go to `build/android-run/<image>/`, never to git.

## Consequences

- One edit of `pins.env` moves the Mac, the container, CMake and Gradle together; nothing else holds a version.
- A pinned archive that Google or Adoptium replaces fails the install with both hashes, and the pin is re-derived on purpose.
- The Mac needs about 4.6 GB for building and the emulator, and about 4 GB more per system image; with a full disk the two images run one after the other (`scripts/android-sdk remove-image`).
- The emulator runs SwiftShader, so its frame times say nothing about a phone's; the log line and the screenshot are what it proves.
- The container build under emulation on Apple silicon takes several minutes more than the native one; CI's amd64 runners don't pay that.

**How we'd know this was wrong:**

- AGP starts to need something only `sdkmanager` writes (a `package.xml`, a licence hash) to build offline: then the installer writes it, or `cmdline-tools` joins the pins.
- Gradle's dependency downloads (AGP and its tree, from Google's Maven) prove flaky in CI: then they get Gradle's dependency verification (`verification-metadata.xml`) or are baked into the image.
- The stripped Release `libmain.so` of the game grows past what players accept: then the build turns on `-ffunction-sections` garbage collection and LTO for the generated code, measured first.
