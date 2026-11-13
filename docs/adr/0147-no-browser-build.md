# 147. No browser build: feasible, but not built or hosted

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#383](https://github.com/armaatus/maplethief-port/issues/383) (research, Backlog)
- **Builds on:** [ADR 0025](0025-guest-memory-and-address-space.md) (the arena and the checked model), [ADR 0039](0039-rendering-backend-software-rasterizer.md) (the software rasterizer behind SDL_Renderer), [ADR 0100](0100-public-repository-and-player-releases.md) (what a release may carry, the withdrawal plan), [ADR 0146](0146-distribution-on-itch-io.md) (the itch.io page)

## Context

#383 asks whether a browser build (WebAssembly through Emscripten) is possible, and whether it would be wise. The player's IPA would be read in the browser and nothing would be uploaded, but a hosted, playable page is the most visible thing the project could publish.

The issue's first task was to build the placeholder shell with Emscripten and see what breaks. Installing the Emscripten SDK was refused on the agent's Mac (a download of outside code that the session's permissions do not allow), so this ADR rests on reading the runtime and platform code against what Emscripten and SDL3 offer, not on a compiler run. The findings below are the places where the code assumes something a browser does not give.

**What would carry over unchanged.** SDL3 3.4.18 has an Emscripten backend (window, input, gamepads through the Gamepad API, audio through Web Audio). The software rasterizer (ADR 0039) presents through SDL_Renderer, which has a WebGL backend; SDL_GPU has no web backend, so `gfx_backend = gpu` would be off. The generated code and the runtime are C and C++ with no inline assembly on the guest path. The checked memory model (`MT_MEM_CHECKED 1`) already runs on iOS and the Switch with the arena alone. The header-only dependencies (dr_mp3, dr_flac, stb) and the fonts are portable.

**What would break, in order of cost:**

1. **The main loop.** A browser page cannot block. The host loop runs inside the guest's `UIApplicationMain` (the `app_main` hook in `runtime/src/boot/process.c`), and `platform/src/main.cpp`, `game.cpp`, `import_screen.cpp`, `preset_picker.cpp` and the overlays each spin their own `while (... SDL_PollEvent)` loop. There are three ways out:
   - **Asyncify** rewrites every function that can reach a yield. Calls into the generated code go through dispatch tables and `objc_msgSend`, so Asyncify must assume almost all of the 41 MB of generated C can yield: a large code-size and speed cost.
   - **JSPI** (JavaScript Promise Integration) suspends the wasm stack without rewriting it, but not every browser ships it.
   - **Inverting the loops** into one per-frame step (SDL3's main callbacks, `SDL_AppIterate`) and letting `emscripten_set_main_loop` unwind the host frames below `UIApplicationMain`, which never returns anyway. Each nested loop becomes a state of the step. The guest's exit and trap paths longjmp back to `mt_process_run`'s `setjmp` (boot.h), a frame that would no longer exist, so they would end through a flag that the step checks instead.
   The third is the only one that keeps the code fast on every browser, and it touches every loop in the platform layer.
2. **Guest memory.** wasm has no `mmap(PROT_NONE)` and no `mprotect`, so `runtime/src/mem/vm.cpp` would need a web branch, as the Switch got one (ADR 0025's addendum). The 512 MiB arena would be one allocation in linear memory (wasm32 allows up to 4 GiB). Untouched pages cost little resident memory on desktop browsers; mobile Safari's limits on a page's memory are the unknown that only a device can answer. Nothing would fault in hardware: not page zero, not the 64 KiB guard under each guest stack, not the sealed image. The checked model still traps every access past the arena with the guest PC, which is what the Switch already lives with.
3. **Threads.** GCD's global queues run on host threads (`runtime/src/gcd/gcd.cpp`); the import screen extracts on a `std::thread`; `update_check.cpp` uses one too. Threads in wasm need SharedArrayBuffer, which needs the page served with cross-origin isolation headers (COOP/COEP) that not every host lets one set. The game itself uses GCD only for Chartboost's background loads (offline in the port, ADR 0034) and for main-queue `dispatch_async` ([ANALYSIS.md](../ANALYSIS.md)), so a single-threaded build could run global-queue blocks on the main thread, extract the IPA in slices between frames, and drop the update check.
4. **setjmp and longjmp** (the SjLj unwinder, `sjlj.c`; GCD's workers; `mt_trap`) work under Emscripten's wasm exception handling (`-sSUPPORT_LONGJMP=wasm`), at a cost in every function that calls `setjmp`.
5. **Files.** The IPA would come in through a file input or a drop onto the page and land in Emscripten's in-memory file system. The existing import (exact SHA-256, safe extraction) would run unchanged on it, and the extracted bundle (27 MB) and the save data would be persisted in IndexedDB or the Origin Private File System. Browsers may evict that storage unless the page is granted persistent storage, so a save export would be needed before anyone trusts it with a character.
6. **Speed.** The software rasterizer costs about 13 ms of CPU a game tick on a map on an M4, natively (ADR 0145). wasm runs this kind of code roughly 1.5–2× slower, still inside a 20 Hz tick on a desktop, and doubtful on phones, where a native build exists anyway.

Estimated work: a new `web-release` preset with an Emscripten toolchain, the loop inversion, the `vm.cpp` branch, a single-threaded GCD mode, file import and persistence, and a CI job. About two weeks; the loop inversion is most of it and is the part that touches shared code.

**Takedown exposure.** The question is what would be hosted, and by whom.

- *What:* the same files a player release carries under ADR 0100 (the runtime and the recompiled code, no assets), as HTML, JavaScript and a `.wasm` module.
- *By whom:* GitHub Pages from this repository, an HTML5 build on the itch.io page (ADR 0146), or a domain of Joris's own.

The legal substance is the same as the player downloads that ADR 0100 accepted: a translation of Nexon's program, with the game's files coming from the player's own copy. What changes is exposure:

- **Visibility.** A link that opens the game in one click is shared, embedded and listed far more widely than a download that needs an install and an IPA import. Rights holders act on what is visible, and browser ports of well-known games have drawn takedowns.
- **Rehosting with the assets.** A web build is a static folder. Anyone can put it on a site next to the IPA's extracted files and get a playable copy of the game, credited to this project. A desktop build can be redistributed with the assets too, but a web build turns that into a public, clickable copy, which is the thing most likely to provoke a notice.
- **Collateral.** A notice against GitHub Pages lands on the repository, which ADR 0100 wants to keep standing when the builds are withdrawn. A notice against an itch.io HTML5 build lands on the same page as the desktop and Switch downloads (ADR 0146). A domain of Joris's own puts him in front, by name.
- **Little to gain.** Every platform a player of this game has (macOS, Windows, Linux, the Steam Deck, iOS and the Switch) already has a native build that runs faster and keeps its saves in a real file.

## Options considered

1. **Build and host a web version** (on itch.io or GitHub Pages). About two weeks of work, the most visible artefact the project could make, and the one whose takedown would take the most with it. Rejected.
2. **Build a web version but publish it only as a download** that players serve locally. A static folder needs a local web server to load a `.wasm` module, so it is harder to start than the native build it would sit next to, and it is still the folder that someone rehosts with the assets. Rejected.
3. **No browser build now; record what it would take.** **Chosen.**

## Decision

- **No browser build.** No Emscripten preset, no web branch in the runtime, and no hosted or downloadable web version. No follow-up issue is filed.
- **What a later attempt starts from** is the list in the Context: the loop inversion through SDL3's main callbacks, a `vm.cpp` branch with the arena in linear memory and the checked model, single-threaded GCD, the import into an in-memory file system persisted in IndexedDB or OPFS, and no SDL_GPU.
- **Reopening** takes a new ADR, and two things together: the player releases have stood for a while without an objection from Nexon, and there is a host whose takedown would not reach the repository or the itch.io page. Even then, rehosting with the assets stays the open risk.

## Consequences

- The project's most visible artefacts stay the GitHub Releases and the itch.io page; the withdrawal plan of ADR 0100 and ADR 0146 covers everything that is published.
- Players on a Chromebook or a locked-down machine have no build. None of the project's targets needs a browser to reach.
- The loop inversion is not done for the browser's sake. If another target needs it (SDL on iOS prefers its main callbacks), it can be done on its own merits, and it would remove the largest item from a later web attempt.
- Nothing in the repository changes except this ADR and its line in [ARCHITECTURE.md](../ARCHITECTURE.md).
