# 159. Fuzz targets for NIBArchive, binary plists, JSON, saves and the state file

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-13
- **Issue:** [#323](https://github.com/armaatus/maplethief-port/issues/323) (epic [#287](https://github.com/armaatus/maplethief-port/issues/287))
- **Builds on:** [ADR 0154](0154-threat-model-for-player-supplied-files.md) (the parsers to cover), [ADR 0157](0157-asan-and-ubsan-preset-and-ci-job.md) (the sanitizers and the CI job), [ADR 0105](0105-save-states-in-process-snapshot-at-safe-points.md) (the state file and its sections), [ADR 0084](0084-offline-polish-hidden-online-ui-local-game-center.md) (`gamecenter.json`), [ADR 0100](0100-public-repository-and-player-releases.md) (nothing from the IPA in git)

## Context

ADR 0154 lists the parsers of untrusted files and gives each a fuzz target: #322 the first group (zip, the IPA import, PNG, WAV, MP3, FLAC, `.strings`, settings, bindings), #323 the structured formats: NIBArchive (`uikit/nib.cpp`), binary plists (`foundation/plist.cpp`), JSON (`stubs/json.cpp`, `gamecenter.json`), the save files (`foundation/saves.cpp`) and the state file (`snapshot/state_file.cpp`) with the load function of every section (ADR 0105: the core sections in `snapshot.cpp`, one per module, and one per host state serializer, 39 of them).

A read of the code before this issue found two edge cases: the plist reader expands shared references, so memory grows with the number of references times the size of the object they share; and the JSON reader turned a lone surrogate (`"\ud800"`) into the bytes of an encoded surrogate, which is not UTF-8.

Three questions needed an answer: how a state file's sections each get a target without one program per section; where the seed corpora come from, when the repository holds no binary from the game and a state file's sections depend on the runtime's own layout; and what each edge case becomes.

## Options considered

**The state file's sections.**

1. **One program per section** (about 60). Each a clean target, but 60 builds and 60 CPU-hours of long runs, and most of the code is the same.
2. **One target that picks the section from the first byte** (this ADR). A synthetic guest is built once and saved (the baseline); an input is a choice and a payload. Choice 0 is a whole state file; choice *k* replaces section *k* of the baseline and restores the result, so every other section is valid and the payload reaches its own reader; the last choices run one host state serializer's load on a zeroed block, as a restore runs it. The coverage of every section adds up in one corpus.
3. **Only whole files**. Each section's CRC would have to be right for its reader to run: the fuzzer would spend its time on CRCs.

**The seeds.**

1. **Binary seed files in git.** Readable by any fuzzer, but a state file's seed changes whenever a section's layout does, and a binary that only a program can check is not reviewable.
2. **Seeds made in code** (this ADR). Each target defines `seeds()`: the plist and NIB targets write their seeds with small writers; the save seeds are the synthetic files of `saves_test.cpp`; the state seeds come from the synthetic guest's own save. `--write-seeds DIR` (or `MT_FUZZ_WRITE_SEEDS=DIR` under libFuzzer) writes them; nothing comes from the IPA.

**The plist's shared references.**

1. **Refuse a shared reference.** CFBinaryPList writes them for repeated strings; the bundle's tables could use them.
2. **Bound the bytes copied** (this ADR): besides the existing bound on objects (count × 16 + 4,096), the data and string bytes copied by all expansions together are at most the file's size × 16 + 4,096. A file that shares a large object many times is refused with "shared references copy too many bytes". Memory stays linear in the file's size (ADR 0154, guarantee 3).
3. **Keep shared objects shared** (a DAG instead of a tree). The fix in principle, but every caller walks `PlistValue` as a tree.

**The JSON lone surrogate.**

1. **Replace it with U+FFFD.** Accepts a file the port never writes.
2. **Refuse it** (this ADR): a high surrogate must be followed by `\u` and a low one, and a low one alone is refused, both with "lone surrogate". The port's writer never escapes a surrogate, so no `gamecenter.json` it wrote is refused; the text pack reader (ADR 0108) refuses them too.

## Decision

Decided by the agent (delegated by Joris, 2026-11-13); Joris can veto it in the PR.

1. **Five targets** in `tests/fuzz/`, registered by `tests/fuzz/targets2.cmake`: `fuzz_nib`, `fuzz_plist`, `fuzz_json`, `fuzz_saves` and `fuzz_state`, each its `fuzz_<name>.cpp` and the shared driver `fuzz2_driver.cpp` (`fuzz2.h`), linked against the runtime. Their names are appended to the global property `MAPLETHIEF_FUZZ_TARGETS`.
2. **Two builds.** With `MAPLETHIEF_FUZZ` (clang, libFuzzer; the runtime compiled with `-fsanitize=fuzzer-no-link`) each target is a libFuzzer program. Without, each is a replayer (`--seeds`, `--write-seeds DIR`, files and folders), and CTest `fuzz.<name>.seeds` replays its seeds on every build that runs the tests, the `asan` job's included.
3. **Oracles besides the sanitizers.** Every target fails when a refusal has no reason. NIB: every index an accepted archive holds is in range, and the accessors `uikit/app.cpp` uses run over it. Plist: an accepted file's expansion stays inside the reader's bounds. JSON: a text of valid UTF-8 gives strings of valid UTF-8, and the writer's output of an accepted tree parses again to the same tree. Saves (a first byte picks the file name and the size of the reason's buffer): the reason is NUL-terminated inside its buffer, the verdict does not depend on that buffer, the sanitiser changes only `config`, at most two bytes, never a file the checks pass, and a second pass changes nothing; the cash shop's counters parser reads the counters files. State: a refused restore changes nothing (the guest saved again equals the baseline, section by section), an accepted one is undone by restoring the baseline, which must succeed, and no guest code runs (the targets' `mt_dispatch` fails).
4. **The state target's guest** is `snapshot_test.cpp`'s, with a live host state of 26 of the 39 serializers (the values, CG, UIKit views, controllers and layers, GameKit, a StoreKit request); the other serializers' loads start from an empty seed.
5. **Long runs** happen locally: at least one CPU-hour per target under ASan and UBSan before a target's change merges, the times in the pull request. The CI smoke and `scripts/fuzz` are #322's.
6. **The two edge cases** are fixed as above, each with a test: `Foundation: a binary plist's shared references are bounded by the bytes they copy (#323)` and `gamecenter.json's JSON: a lone surrogate is refused, not made invalid UTF-8 (#323)`.

## Consequences

- Every parser in ADR 0154's second group, and every section of the state file, has a target; their rows there name it.
- A change to a section's layout changes its seed with it; nothing in git goes stale.
- The state target needs a synthetic guest (512 MiB of checked arena) and runs at about 500 inputs a second; the parsers run at 50,000 to 125,000.
- A plist that shares one large object more than 16 times its own size is refused. None of the bundle's does (`foundation.real_bundle` and the targets over every shipped plist and `.strings` file).
- A `gamecenter.json` with a lone surrogate is unreadable, kept aside as `.bad` at the next save (ADR 0084).
- How we'd know this is wrong: a crash in one of these parsers from a file in the wild that the targets' corpora should have reached, or a long run that stops finding new coverage within minutes (then the seeds or the input format need work).
