# 133. One-command builds: `just` over `scripts/build` over `mtport.build`, results in `dist/`

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #141 (epic #269)
- **Builds on:** [ADR 0004](0004-build-presets-and-generators.md) (presets), [ADR 0023](0023-local-ci-and-packaging.md) (`scripts/local-ci`, kits), [ADR 0024](0024-release-contents-and-flow.md) (what a release may carry), [ADR 0064](0064-macos-game-app.md) (`mac-app`), [ADR 0065](0065-linux-and-steam-deck-build.md) (the Linux folder), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (`win-player`), [ADR 0127](0127-data-folder-next-to-the-game.md) (the data folder)

## Context

A playable build took several manual steps, and `just build` left its result somewhere different for each OS, inside `build/`. Issue #141 asks for one command per platform that takes a fresh clone to a game in a known place, a `doctor`, recipes for the single steps, and a fallback for anyone without `just`. Facts:

- The IPA is 18 MB and hashes in a fraction of a second; `python3 -m recomp all` takes 20 to 45 s, and rewrites only what changed; `cmake --preset` costs seconds; the Linux build runs in a container from a clean export and has no incremental state at all.
- `mac-app` is already the self-contained, ad-hoc-signed, universal app. The bundle search of ADR 0125 already finds `game/MapleThief.app` next to an executable.
- `win-release` links the dynamic MSVC runtime and compiles the checkout's `work/ipa` path in. `win-player` has the static runtime and no such path.
- A Debug build on Windows, macOS and Linux is the same preset family with the game's bundle left in `work/ipa`.
- The game's data folder sits next to the game (ADR 0127), so a rebuild that replaces the game must not take the saves with it. The Linux folder's staging did.
- Windows parity can't be checked here, and the PowerShell script is the file most likely to drift.

## Options considered

### Where the logic lives

1. **A Python module, `tools/mtport/build.py`, behind `scripts/build` and `scripts\build.ps1`,** the pattern of `mtport.bootstrap` and `mtport.pipeline`: the scripts only find a Python (and on Windows the Visual Studio environment). Arguments are parsed once, every OS gets the same stamps, staging, `clean` and `doctor`, and the synthetic tests run on every CI OS. Accepted.
2. **Bash and PowerShell, each with its own copy.** This is what `scripts/build` was. Two copies of argument parsing, staging and stamps, one of which can't be run here. Rejected.
3. **Everything in the `justfile`.** Then a clone without `just` has no way in, which the issue excludes. Rejected.

### What the Windows folder is built from

1. **`win-player` with the bundle in `game/` next to the exe.** Static runtime, no compiled-in path, and the release's own preset, so the folder runs on another PC with the existing bundle search. Accepted.
2. **`win-release`** (the previous `just build`). Needs the MSVC runtime installed and a compiled-in path that doesn't exist elsewhere, so it is not really portable. Rejected.

### What `package` means

1. **The own-code kit of ADR 0023 and ADR 0024, from a clean checkout, through `scripts/local-ci --release`.** A kit never sees `work/`, so a build in a checkout that has the generated code can't reach one. Accepted, for Mac and Linux; Windows kits come from CI (ADR 0126), and the command says so.
2. **An archive of `dist/`.** That is what the Windows zip and Linux tarball already are, and `dist/` is never shared. Rejected.
3. **`cmake --workflow` of a release preset in the checkout.** It would package a shell linked with the generated code when `work/gen` exists. Rejected.

### Git LFS in `doctor`

The issue lists it, but since ADR 0100 the IPA is not in LFS and nothing fetches LFS objects, so `doctor` doesn't check for something unused. `bootstrap` still recognises an old LFS pointer file, and that message stays. Dropped.

## Decision

**The chain.** `just <recipe>` calls `scripts/build` (Windows: `scripts\build.ps1`), which calls `python -m mtport.build`. `scripts/test` and `scripts/local-ci` are called directly by their recipes, so `just test` and `just ci` are those scripts. Arguments are parsed only in `mtport.build`: `build [mac|win|linux] [debug|release]` (the default command), `run [debug] [-- GAME ARGS]`, `bootstrap`, `assets`, `recomp`, `shaders`, `package`, `clean [work]`, `doctor`. `macos`, `windows` and a leading dash keep working. `ios` stops and names #83. `scripts\build.ps1` asks the module (`--needs-msvc`) whether the words compile with MSVC, so it enters the Visual Studio environment without reading the words itself.

**Results.** Each result lands in git-ignored `dist/<platform>/`, a Debug build in `dist/<platform>/debug/`, for debugging only. A line says what was built and how to start it, printed and written to `BUILD.txt` there:

- `mac`: `dist/mac/MapleThief.app`, the `mac-app` build, copied with `ditto` so the signature survives, then checked: `codesign --verify --deep --strict` and `lipo -archs` for both architectures, on the copy.
- `win`: `dist/win/MapleThief-game-<version>-windows-x64/` (the `win-player` exe, `game/MapleThief.app`, `README.txt`) and the same as a `.zip`.
- `linux`: `scripts/linux-game build --out dist/linux` (folder and tarball, as ADR 0065), `--debug` for the `linux-debug` preset.

`build` does not run `assets` (the game doesn't need it, as before), and `shaders` stays a separate step because the header is committed; `build` warns when the shaders are older than their sources.

**Stamps.** A step is skipped when its inputs match:

- bootstrap: `work/bootstrap.json` names this IPA's SHA-256, which is hashed each time, so a changed IPA is noticed, and the extracted files exist;
- recompile: `work/gen/.recomp-stamp.json` holds a hash of the armv7 slice and of `recomp/` and `tools/mtport/` (the recompiler's sources), and `work/gen/gen.cmake` exists;
- configure: `build/<preset>/build.ninja` exists, is newer than `CMakePresets.json`, and the generated code didn't just change;
- staging: a content digest of what was built in `dist/<platform>/.stamp.json`, so a no-change build copies nothing. The checks on the Mac copy always run;
- Linux, which has no incremental state: the same stamp over HEAD's tree, the generated code and the IPA, so no Docker at all when nothing changed.

The compile step still runs through CMake's and Ninja's own incremental build, which is fast when nothing changed.

**Saves stay.** A rebuild and `clean` keep `data/` and `user-data.txt` next to each game (ADR 0127), and the tarball and zip never hold them. `clean` removes `dist/` and the `build/` directories that `build` writes, and says what it removed and kept. `work/` and `assets-out/` only go with the word `work`; `original/` is never touched. The release guard's forbidden directories gain `dist/` (ADR 0024: local only).

**`doctor`** checks Python, CMake, Ninja, the compiler for this OS, Docker, the IPA and disk space, each with its fix; the IPA's fix points at [Your game file](../play/your-ipa.md).

## Consequences

- From a fresh clone with the IPA in `original/`, one command gives a playable, signed app, and a second run with nothing changed takes seconds: bootstrap and recompile cost one hash each.
- A checkout with uncommitted changes builds the Linux game from HEAD, as before; the command says so.
- `just` splits its arguments on spaces, so a path with a space needs quotes through `just` or the script directly (relevant to #85's `--ipa`).
- `dist/` is a second place a build lives next to `build/`: an old build's saves in `build/.../data` stay there until moved (`--migrate-from`).
- Follow-ups: `build ios` is #83; `build win` on a real PC is a `needs-human` check on this issue; #85 adds `--ipa` as one more option in `mtport.build`.
- **How we'd know this was wrong:** a fresh clone that needs a second command, a no-change run that isn't a matter of seconds, or a Windows run that behaves differently from the Mac's.
