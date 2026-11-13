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
