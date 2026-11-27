# 135. Building from your own copy: accept an IPA by its main binary

- **Status:** Accepted
- **Date:** 2026-11-09
- **Issue:** #85 (epic #269)
- **Builds on:** [ADR 0100](0100-public-repository-and-player-releases.md) (each user's own copy), [ADR 0125](0125-player-builds-icon-guard-modes-and-release-flow.md) (the player import), [ADR 0133](0133-one-command-builds.md) (one command, one place that reads arguments)

## Context

Issue #85 asks for one command from a fresh clone and a copy of the game anywhere on disk to a working build, with clear messages for a wrong file. Facts:

- `scripts/bootstrap` demanded the IPA's exact size and SHA-256. An IPA is a zip, so a copy that was only repacked has neither, though the game inside is identical, and it was refused.
- What the port reads from the IPA is the main binary and the bundle's files. `original/SHA256SUMS` already pins the main binary and its two slices, and the bootstrap checks them after extracting.
- `platform/src/ipa_import.cpp` (the player's first-launch import) already says why a file is wrong, in a fixed order, and `tools/mtport/macho.py` already reads Mach-O load commands.
- `scripts/build` and `scripts\build.ps1` change to the repository before the module runs.

## Options considered

### What counts as the game

1. **The main binary's hash.** A repacked copy passes, and the hash can only match the one binary we support. Accepted.
2. **The IPA's exact hash, as before.** Refuses a copy that differs only in its zip packing, and gives no useful message. Rejected.
3. **Every file's hash.** It would need a committed list of hashes of Nexon's files, for no gain: a bundle file that was tampered with only affects that user's own build. Rejected.

### How the file reaches the build

1. **A checked copy to `original/MapleThief-1.5.ipa`,** the one input every later step already reads, git-ignored. Accepted.
2. **Pass the path through to the bootstrap and the stamps.** Every step that names `original/` would need the path too, and the next `just build` without `--ipa` would have nothing to read. Rejected.
3. **A symbolic link in `original/`.** Needs special rights on Windows, and breaks when the file moves. Rejected.

### A file already in `original/`

1. **Never replace it.** The same game stays and is used: the build only reads what the hash has vouched for. Anything else is a refusal the user sorts out. Accepted.
2. **Replace it when it is the same game.** This loses an exact copy for a repacked one, and the build gains nothing. Rejected.

## Decision

**Verification lives in `tools/mtport/bootstrap.py` (`check_ipa_file`)**, shared by `scripts/bootstrap` and `--ipa`, and reads the file without writing anything. The exact file (size and SHA-256) passes at once, as before. Any other file is opened as a zip and passes if `Payload/MapleThief.app/MapleThief` matches `original/SHA256SUMS`, after the same entry checks the extraction makes. Otherwise it is refused with the first reason that applies, in the player import's order: not a zip; no `Payload/<name>.app`; another app; a zip password; no main binary; a non-zero `cryptid` in any slice (`MachO.cryptid()`); no readable version or a version other than 1.5; damaged or changed. Every refusal ends by pointing at [Your game file](../play/your-ipa.md), and no message says how to get or decrypt a copy.

**`--ipa PATH`** is an argument of `mtport.build`, for `build` and `bootstrap`. `build` runs `doctor`'s checks for what that platform needs (the new `Check.needed_for`; the Linux build needs Docker, not CMake), then `adopt_ipa` checks the file and copies it into place, atomically, verifying the copy. A relative path is read from the caller's folder: the scripts pass it in `MAPLETHIEF_CALLER_DIR`. A plain `scripts/build` doesn't run the tool check, so an incremental build isn't stopped by, say, low disk space.

**`scripts/build-from-ipa PATH` and `scripts\build-from-ipa.ps1`** only call `scripts/build` with `--ipa=PATH`. There is no second pipeline.

**The player import is unchanged.** It still takes only the exact file, because it runs on a player's machine without `original/SHA256SUMS` and with a smaller job: refusing is the safe default there.

## Consequences

- A repacked copy works from `original/` too, through a plain `scripts/bootstrap`; the bootstrap's stamp holds that file's own hash, so a changed IPA is still noticed.
- A re-signed copy changes the main binary and is refused as damaged or changed. The docs say so.
- A repacked copy with missing or altered bundle files passes the check and only hurts that user's own build. Nothing derived from it is ever distributed.
- `just` splits arguments on spaces (ADR 0133): a path with a space needs quotes inside the quotes through `just`, or the scripts directly.
- The Windows half of the issue's acceptance criterion is a `needs-human` check on #85.
- **How we'd know this was wrong:** a copy the game can't run passing the check, a real IPA refused for a reason it doesn't have, or a message that leads someone to ask how to obtain a file.

## Addendum (#413): extraction is capped like the player import

ADR 0154 (g) found that `extract_ipa` wrote whatever a repacked IPA held, with no bound. It now has the player import's caps, as module constants of `tools/mtport/bootstrap.py`: at most 4,096 entries (`MAX_ENTRIES`, `kMaxZipEntries`), 64 MiB an entry (`MAX_ENTRY_BYTES`, `kMaxIpaEntryBytes`) and 1 GiB in total (`MAX_TOTAL_BYTES`). The count and the declared sizes are checked with the entry names (`safe_members`), so `check_ipa_file` refuses such a copy before anything is copied; while extracting, the bytes themselves are counted again, because a zip's sizes can lie. A refusal removes the half-written folder and leaves `work/ipa/` as it was. The known IPA is far inside every cap.

## Addendum (#350): the download accepts a repack too

**Status:** Accepted (delegated by Joris, 2026-11-13), 2026-11-27. Revises "The player import is unchanged" above.

The player import compared the whole file with one SHA-256, so a player whose copy was the right game in another zip was turned away, with building from source as the only way out (KNOWN_ISSUES). The player build's recompiled code matches one executable, so that check must stay; what a repack can change is the zip around the files, and in principle the other files.

**Options.** (1) The armv7 slice alone, as the bootstrap does with the main binary: rejected for the download, because the player has no build of their own to blame, and a repack with changed data files would hand the game's loaders crafted input (ADR 0154 trusts the game's data only because a hash vouched for it). (2) The armv7 slice plus a per-file hash list made at build time: accepted. (3) A committed list: rejected as in the decision above; nothing about Nexon's files belongs in git.

**Decision.**

- **The list.** `tools/mtport/ipa_files.py` writes, from the builder's own extracted copy, the path and SHA-256 of every file below `Payload/MapleThief.app/` that the game reads, as C++ initializers in `work/gen/ipa_files.inc`. Left out, because the game never reads them: the executable (checked by its slice), code signing (`_CodeSignature/`, `CodeResources`, `ResourceRules.plist`, `embedded.mobileprovision`, `SC_Info/`, `PkgInfo`), and the two strays [ANALYSIS](../ANALYSIS.md) names (the empty `MapleThief_armv6`, the Chartboost `README.md`). The 1.5 IPA gives 2,165 entries. `scripts/bootstrap` writes it after the split, so it is wherever `work/gen/` is: the player builds' clean clones link `work/gen/`, and the Switch and Linux containers mount only `work/gen/`. This is within ADR 0100: it is derived from each user's own copy, holds hashes and names only (no byte of the game), lives in git-ignored `work/`, and is never committed; the content guard treats `ipa_files.inc` as a generated file (by name, and by hash as a `work/gen/` file).
- **The build.** `platform/CMakeLists.txt` compiles the list into `ipa_import.cpp` when `MAPLETHIEF_IPA_FILES` (default `<gen>/ipa_files.inc`) exists; the player build fails at configure time without it. A build without it accepts only the exact file, as before (CI's).
- **The import** (desktop, `--import-ipa` and the Switch's SD-card route share `ipa_import.cpp`). The exact file passes at once. Otherwise the diagnosis runs as before up to "version 1.5 with other bytes"; then the file is a repack when the executable's armv7 slice matches `work/slices/MapleThief.armv7`'s line in `original/SHA256SUMS` (`kArmv7Sha256`) and every listed file is there with its hash. The refusal names the first file that is missing or differs ("This is MapleThief 1.5, but its file data/a.txt is not 1.5's: it is damaged or changed."), or says the executable is not 1.5's, with both slice hashes. Files the list does not name are ignored: not checked, and not extracted, so `game/` holds only the executable and verified files, and a file a repacker added (another language's `.lproj`, say) cannot be found by a bundle lookup. The executable check after extraction accepts the whole file's hash or the armv7 slice's.

**Consequences.**

- A repacked copy of the right game imports in the download; a re-signed one is still refused (signing changes the slice), now with that reason.
- A release machine must run `scripts/bootstrap` once after this change, or the player build stops and says so.
- The data-folder copy (`data_copy.cpp`) and the log's executable line still compare the whole executable. An executable whose other slice was stripped imports and runs, but is reported as different there; no such copy is known.
- **How we'd know this was wrong:** a copy the game reads differently passing the check (a file the game reads that the list leaves out), or a correct repack refused.
