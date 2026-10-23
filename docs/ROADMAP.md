# Roadmap

Milestones are worked **in order**. A milestone is done when every child issue is closed and its exit criteria hold. Each milestone has an epic issue on GitHub (label `type:epic`) that lists its child issues. Progress is tracked on the **MapleThief Port** project board.

## M0 – Foundation

Make the repo buildable and reproducible before any reverse engineering. Choose the languages and toolchains (ADR), set up a CMake superbuild with `tools`/`recomp`/`runtime`/`platform` targets, decide how dependencies are vendored (SDL3, OpenAL Soft or alternative, an MP3 decoder, a disassembler), write the `scripts/bootstrap` flow that verifies and extracts the IPA and splits the slices, and add a test harness plus CI on macOS/Windows/Linux. **Exit criteria:** a fresh clone + `git lfs pull` + `scripts/bootstrap` + one CMake command produces an empty SDL3 window on macOS, Windows and Linux, and CI is green on all three without access to the IPA.

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

Produce real deliverables per platform: an ad-hoc signed universal macOS `.app`, a Windows x64 portable zip, an iOS arm64 app for sideloading, a Linux/Steam Deck build, and a "build from your own IPA" flow for a user pointing at their own copy. **Exit criteria:** each platform's artifact launches the game on a clean machine (my devices for iOS and Steam Deck), with written build instructions.

## M10 – Enhancements

Opt-in improvements that preserve the original game logic: widescreen/extended view where the engine allows it, 60 fps presentation with fixed-timestep logic, HD UI scaling and filters, a settings menu, and cheap, safe quality-of-life features. **Exit criteria:** each enhancement can be toggled, defaults to off or original behavior where it changes gameplay, and doesn't break the M8 playthrough or the M11 regression suite.

## M11 – QA & v1.0

Lock it down: an automated boot-to-gameplay smoke test with input replay, a full playthrough checklist per platform, final docs (build guide, controls, known issues) and a private v1.0 tag. **Exit criteria:** the regression suite passes on all platforms, the per-platform playthrough checklists are signed off by me, the docs are complete, and `v1.0` is tagged with private release notes and **no binaries attached**.
