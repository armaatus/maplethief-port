# Roadmap

Milestones are worked **in order**. A milestone is done when every child issue is closed and its exit criteria hold. Each milestone has an epic issue on GitHub (label `type:epic`) that lists its child issues. Progress is tracked on the **MapleThief Port** project board.

## M0 – Foundation

Make the repo buildable and reproducible before any reverse engineering. Choose the languages and toolchains (ADR), set up a CMake superbuild with `tools`/`recomp`/`runtime`/`platform` targets, decide how dependencies are vendored (SDL3, OpenAL Soft or alternative, an MP3 decoder, a disassembler), write the `scripts/bootstrap` flow that verifies and extracts the IPA and splits the slices, and add a test harness plus CI on macOS/Windows/Linux. **Exit criteria:** a fresh clone + your own IPA in `original/` + `scripts/bootstrap` + one CMake command produces an empty SDL3 window on macOS, Windows and Linux, and CI is green on all three without access to the IPA.

## M1 – Reverse engineering & format specs

Build an accurate map of the binary and its data. That means a scripted headless Ghidra project, recovered ObjC metadata and C++ vtables, identified library code (Chartboost, JSON, libstdc++ pieces), a full import table with call counts, the program flow from `UIApplicationMain` to the game loop, the threading/timing model and the save format. Every data format in the bundle gets a spec in `docs/formats/` and a tested parser. **Exit criteria:** every file type in the `.app` has a spec and a parser that round-trips or fully decodes all shipped files without errors; `docs/re/` contains the ObjC, imports, RTTI and program-flow documents; and each of the 2,655 functions is tagged as *app*, *engine*, *library* or *unknown*.

## M2 – Asset pipeline & viewers

Turn the parsers into a usable pipeline and validate them visually. One command converts the IPA into `assets-out/` (normal PNGs, tables as JSON, disassembled scripts). Standalone viewers render maps, sprites/animations and data tables. **Exit criteria:** `tools` pipeline runs cleanly on the IPA, and all 90 maps and 138 sprite projects render in the viewers in a way that matches the game visually (checked by me against the real game, `needs-human` where needed).

## M3 – Static recompiler

Translate the armv7 slice into compilable C. This covers the recompiler design ADR, the Mach-O loader, function discovery (including Thumb/ARM interworking, jump tables and tail calls), integer/VFP lifting, indirect call dispatch, import thunks, SjLj exceptions, output partitioning and a snippet-level test suite. **Exit criteria:** ≥ 99% of discovered functions are lifted and compile with zero errors on clang and MSVC/clang-cl; the snippet suite passes; and a coverage report lists every unlifted function with a reason.

## M4 – HLE runtime core

Make the recompiled program run. That needs guest memory, process startup (static constructors, `UIApplicationMain`), libc and C++ ABI pieces, the Objective-C strategy (ADR) and implementation, CoreFoundation/Foundation/GCD/CoreGraphics subsets, stubs for ads/IAP/Game Center/network/movie/web view, and good tracing. **Exit criteria:** the game boots through `application:didFinishLaunching…`, creates its EAGL context and issues its **first GL frame** without hitting an unimplemented import; and the unimplemented-import report for the boot path is empty.

## M5 – Graphics

Implement GLES 1.1 fixed-function on a modern backend (ADR), with texture formats, premultiplied alpha, the EAGL framebuffer and presentation, and scaling modes. **Exit criteria:** title, main menu and the first map render pixel-identically (within tolerance) to reference screenshots from real hardware at 480×320, and correctly at integer and smooth scaled resolutions with letterboxing.

## M6 – Audio

Implement OpenAL (or a translation layer), `ExtAudioFile` WAV decoding, `AVAudioPlayer` MP3 BGM with looping/volume, and audio-session stubs plus master volume/mute. **Exit criteria:** all 44 sounds play when the game triggers them, BGM loops seamlessly and switches per map, and there are no audio dropouts during a 30-minute session.

## M7 – Input & controller support

Make the game fully playable without a touchscreen. That covers mouse → touch emulation, a documented keypad layout, a controller layer that drives keypad actions directly, keyboard bindings, controller navigation for every menu/popup, remapping with button glyphs, hiding the on-screen keypad, optional rumble, and iOS controllers. **Exit criteria:** every screen and action in the game can be reached and used with only a gamepad, and with only a keyboard; bindings are remappable and persist.

## M8 – Full game boot & playthrough

Get from title screen to credits. That includes the main menu, terms screen, character creation/selection, the first playable map, NPCs/quests/scripts, save/load persistence and then every chapter, mini-game and ending, with stable frame pacing. **Exit criteria:** a full playthrough of all three chapters with every ending verified by me (`needs-human`), saves surviving restarts, and steady frame pacing at the original game speed.

## M9 – Platform builds

Produce real deliverables per platform: an ad-hoc signed universal macOS `.app`, a Windows x64 portable zip, an iOS arm64 app for sideloading (it compiles in CI without the game, [ADR 0136](adr/0136-ios-app-through-xcode.md); the first device run reached gameplay on an iPhone, [#83](https://github.com/armaatus/maplethief-port/issues/83); the remaining device checks are [#73](https://github.com/armaatus/maplethief-port/issues/73), [#128](https://github.com/armaatus/maplethief-port/issues/128) and [#197](https://github.com/armaatus/maplethief-port/issues/197)), a Linux/Steam Deck build, and a "build from your own IPA" flow for a user pointing at their own copy. **Exit criteria:** each platform's artifact launches the game on a clean machine (my devices for iOS and Steam Deck), with written build instructions.

**The data folder** (epic [#259](https://github.com/armaatus/maplethief-port/issues/259), [ADR 0127](adr/0127-data-folder-next-to-the-game.md)). On macOS, Windows and Linux everything the port writes for a player (settings, bindings, saves with `gamecenter.json`, backups, save states, screenshots, logs) lives in one `data/` folder next to the game, and the imported bundle in `game/` beside it. A player backs up by copying `data/`, resets by deleting it and moves the game by moving its folder. The per-user folder is an option (the Saves tab's *Data location*, `--data-location user`) and the fallback when the game folder can't be written (Program Files, a read-only volume, macOS App Translocation), which the game says. Saves in the old per-user folder are copied in on the first start, never moved. iOS keeps the app's own `Documents/`; the Switch uses `sdmc:/switch/maplethief/data/`.

**The Nintendo Switch** (epic [#233](https://github.com/armaatus/maplethief-port/issues/233), [ADR 0138](adr/0138-nintendo-switch-homebrew-target.md)). `MapleThief.nro`, a homebrew app for the Switch under custom firmware (the original console and its OLED and Lite models), built by the same CMake project from the pinned devkitA64 container. The player copies it to `sdmc:/switch/maplethief/`, starts it from the Homebrew Menu and imports their own IPA once. **Exit criteria:** `cmake --workflow --preset switch-release` builds the NRO, and the `switch` job of local CI and CI is green; on a real Switch (`needs-human`) the NRO runs in applet mode and in title takeover, imports the IPA and reaches the first map, plays with Joy-Con, a Pro Controller and touch, shows the right picture handheld and docked, and keeps the game and its saves through HOME, sleep, resume and a restart; ticks run at 20 a second on a map with at least 99 % of frames held 3 vblanks, handheld and docked; and the player build carries no file from the IPA's bundle (ADR 0100).

**Android** (milestone M12, epic [#292](https://github.com/armaatus/maplethief-port/issues/292), [ADR 0150](adr/0150-android-target.md)). One arm64 APK without assets, built by the same CMake project from a pinned container (and natively on the Mac), sideloaded from the release page. The player installs it, picks their own IPA once in the system's file picker, and plays with touch or a controller; the software rasterizer draws, and the data folder is the app's external files folder. The agent tests on the Android emulator on the Mac; a real phone is needed only for what the emulator can't show, and the release key is Joris's. **Exit criteria:** `cmake --workflow --preset android-release` builds the APK in the pinned container from a clean checkout, and the `android` job of CI is green without the IPA; on the emulator and on a real phone (`needs-human`) the APK imports the IPA, reaches the first map, plays with touch and a controller, keeps the game through background and foreground and keeps a save across a restart; ticks run at 20 a second on a map at default settings (ADR 0071); and the release guard's player mode passes on the APK (ADR 0100). Google's developer verification, global in 2027, is a known later risk.

## M10 – Enhancements

Opt-in improvements that preserve the original game logic: widescreen/extended view where the engine allows it, 60 fps presentation with fixed-timestep logic, HD UI scaling and filters, a settings menu, and cheap, safe quality-of-life features. **Exit criteria:** each enhancement can be toggled, defaults to off or original behavior where it changes gameplay, and doesn't break the M8 playthrough or the M11 regression suite.

**The preset model** ([ADR 0070](adr/0070-settings-registry-and-presets.md), #144). One setting, `preset`, picks the whole experience, and a first-launch picker shows the presets before anything else:

- **Original** behaves exactly like the iOS app: every enhancement off, the original pacing, the touch key pad always shown, no prompts. Its gameplay is byte-identical to a run without a settings file.
- **Original + controller** is Original plus controller comfort only: the key pad hides on pad or keyboard input, the focus highlight, button prompts, the 4-way stick.
- **Enhanced** is the best version that keeps the core gameplay: everything in the Enhanced column of #89's table.
- **Custom** is whatever the player changed. Changing any key a preset controls makes it Custom.

Every setting is a row of one registry with its value in each preset. The settings are grouped as Enhancements (Video, Audio, Controls, Gameplay, Accessibility) and a visibly separate **Assists** group for anything that changes gameplay (game speed, turbo, auto-potion, cheats). Assists are off in every preset, and the code checks this. Personal values (volumes, strengths, bindings, accessibility, language) keep their own value in every preset. Each M10 child adds its setting as one registry row with its three columns (ADR 0070 lists the planned keys).

## M11 – QA & v1.0

Lock it down: an automated boot-to-gameplay smoke test with input replay, a full playthrough checklist per platform, final docs (build guide, controls, known issues), ready-to-play player builds ([#182](https://github.com/armaatus/maplethief-port/issues/182)) and a public v1.0 release. The [MapleThief Compendium](https://armaatus.github.io/maplethief-port/) site is live (epic [#210](https://github.com/armaatus/maplethief-port/issues/210)). **Exit criteria:** the regression suite passes on all platforms, the per-platform playthrough checklists are signed off by me, the docs are complete, and `v1.0.0` is tagged with release notes as a public GitHub Release and on itch.io. Windows, macOS and Linux gate the tag: #79, #82, #92, #93, #198 and #264 are closed first ([ADR 0147](adr/0147-what-v1-is-and-what-gates-it.md)). The iPhone app and the Switch build ship with v1.0 as **preview**: playable, but not signed off on the device yet, and labelled so until #73, #128 and #197 (iPhone) and #245 (Switch) close. Which hand-off gates the tag is listed in [the hand-off index](testing/handoffs/README.md#v10-gates). The release carries the own-code port kits ([ADR 0024](adr/0024-release-contents-and-flow.md)) and, per [ADR 0100](adr/0100-public-repository-and-player-releases.md), a player build per platform: the runtime with the recompiled code, which imports the assets from the player's own IPA on first launch. It **never carries the game's assets** (no file from the IPA's bundle, nothing converted from one), and the builds that bundle them (`mac-app`, `scripts/linux-game`) stay personal.

## M12 – Reach and upkeep

After v1.0: downloads that install without workarounds, an Android build, the GPU renderer as the desktop default, and automation that keeps every platform green between releases. Four epics:

- **Upkeep automation** ([#290](https://github.com/armaatus/maplethief-port/issues/290)). **Exit criteria:** a weekly run builds and tests every platform on `dev` and opens an issue when it fails; a nightly run on a self-hosted runner plays the real game and reports; dependency updates arrive as pull requests or as a report; a release is one command per machine, with a runbook a second person could follow.
- **Install without friction** ([#291](https://github.com/armaatus/maplethief-port/issues/291)). **Exit criteria:** an accepted ADR on signing ([ADR 0148](adr/0148-downloads-stay-unsigned.md)), what it chose is in the release flow and the play docs lost the steps it made unnecessary; the Linux download runs on glibc 2.31 or 2.35 and comes as an AppImage and a Flatpak bundle too; a repacked copy of the right game is accepted; the NRO is submitted to the Homebrew App Store, and Windows has a Scoop manifest.
- **Android** ([#292](https://github.com/armaatus/maplethief-port/issues/292), [ADR 0150](adr/0150-android-target.md)). **Exit criteria:** `cmake --workflow --preset android-release` builds the APK in a pinned container and CI's `android` job is green without the IPA; on a real phone the APK installs, imports the player's IPA, reaches the first map, plays with touch and a controller, survives background and foreground and keeps a save across a restart (`needs-human`); 20 ticks a second on a map at default settings; the release guard's player mode passes on the APK; the ADR, ARCHITECTURE, this roadmap, CLAUDE.md, THIRD_PARTY.md and a play page agree.
- **GPU renderer to parity** ([#293](https://github.com/armaatus/maplethief-port/issues/293)). **Exit criteria:** the synthetic GPU tests run in CI on a software device; [#131](https://github.com/armaatus/maplethief-port/issues/131) is answered and what it found is fixed; on desktop the GPU renderer is the default with a tested fall back to software; `render_scale = auto` holds its budget on the GPU path.

Each epic is done when every child is closed or recorded as split or `needs-human`.

## M13 – Community content

Packs, translations and in-game tools, so players can make, share and use content without a source checkout (epic [#294](https://github.com/armaatus/maplethief-port/issues/294)). **Exit criteria:** an accepted ADR on the pack manifest, several packs at once, and what the project hosts or links ([ADR 0149](adr/0149-pack-manifest-several-packs-and-what-the-project-hosts.md)); a packs screen on every platform, with on-screen reasons for a refused pack; every pack and save tool a player needs runs from the game or a download; the port's menus draw accented Latin and Hangul and ship in at least two more languages; every child is closed or recorded as split or `needs-human`.

## Backlog

Ideas with one research issue each and no committed order; nothing here blocks a release, and an idea moves to a numbered milestone when its research issue ends in an epic. Epics parked here: text-to-speech ([#398](https://github.com/armaatus/maplethief-port/issues/398)), Enhancements round 3 ([#401](https://github.com/armaatus/maplethief-port/issues/401)) and the speedrun and practice kit ([#402](https://github.com/armaatus/maplethief-port/issues/402)).
