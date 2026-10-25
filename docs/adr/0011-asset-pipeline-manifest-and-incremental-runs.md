# 11. Asset pipeline: one outcome per bundle file, a hashed manifest, code-hash invalidation

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #25 (epic #97)

## Context

ADR 0010 gives the pipeline ownership of `assets-out/` (except `assets-out/viewers/`). Issue #25 asks for one command that converts the extracted bundle, with three exit criteria: zero conversion errors on the real IPA, two runs give identical outputs and the second is a no-op, and every `.app` file is either converted or explicitly listed as copied or ignored.

Facts that shape the design:

- `mtport.formats.classify` already maps each of the 2,172 bundle files to exactly one format (M1 exit criterion, `tests/test_formats_real.py`).
- A few conversions read a second bundle file: an `.att` grid's width and an `.obj` record's position come from the matching `.map`, the script listings quote `en.lproj/Scripts.strings`, and the `png/**/*.dat` packs are checked against the length table in the `MapleThief` binary (docs/formats/png-dat.md).
- A full run takes about 3.5 s on the real bundle (Apple M4, Python 3.14), so being incremental matters less for speed than for "a second run writes nothing" and for removing outputs that no longer have a source.
- Parsers keep changing during M2 and M3, and a cache that misses a parser change would leave stale outputs that look current.

## Options considered

1. **Walk the bundle with per-folder converters**, `make`-style timestamps for incrementality. Simple, but coverage is only as good as the folder list, an unknown file passes silently, and timestamps don't notice a parser change or a dependency on another file.
2. **Drive the run from `mtport.formats`**, record each file in a manifest with content hashes, and give each converter a hand-maintained version number. Correct coverage, but someone has to remember to bump a version after editing a parser.
3. **Option 2, with the version derived from the code**: a rule's key hashes the source files of the pipeline and of the parser modules the rule uses (with every `mtport` module they import).

## Decision

We use **option 3**.

- Every format in `mtport.formats.FORMATS` has exactly one rule in `mtport.pipeline.RULES` (a few paths override it in `OVERRIDES`, for example the code-signing plists). A rule's outcome is `converted`, `copied` (a standard format, checked by its decoder, then copied byte for byte) or `ignored` (with a reason). An unclassified file, or a format with no rule, is an error. So the manifest lists every bundle file by construction.
- Outputs are named after the source path plus a new extension (`map/00.map.json`, `script/x/y.out.dis`). CgBI and copied files keep their path, and a pack `png/a/b.dat` becomes the directory `png/a/b/`. Output paths are checked: none may be under `viewers/`, be `manifest.json` or leave `assets-out/`. Two sources whose outputs clash (same path ignoring case, or a file where the other needs a directory) fail the later one.
- `assets-out/manifest.json` holds, per source file: sha256, size, format, outcome, reason, rule key, the sha256 of every dependency the rule declares (`deps`), and every output with its sha256. It also records the schema version and the key of every rule. It has sorted keys, `\n` line ends and no timestamps or absolute paths, so it is the same in every checkout and directory.
- A file is skipped when its hash, format, rule key and dependency hashes equal the manifest's, and every recorded output exists with its recorded hash. Outputs that the new manifest no longer lists are deleted (only paths that pass the output-path check) and emptied directories are pruned. Files are written atomically and only when their bytes change, so a no-op run touches nothing, not even the manifest.
- A rule key is `<rule>:<16 hex digits>` of sha256 over the rule's name, outcome and reason and the source files involved, keyed by file name so that `python3 -m mtport.pipeline` and an import agree. Editing `pipeline.py` redoes every file (about 3.5 s). Editing one parser redoes only the rules that use it.

## Consequences

- "Every file is converted or listed" and "no errors" are checked by one real-asset test (`tests/test_pipeline_real.py`) instead of a hand-kept list. A new file type in a future bundle stops the run until it gets a format and a rule.
- Nobody has to remember version bumps. The cost is extra work after harmless edits (a comment in a parser redoes that parser's files), which takes seconds.
- Code that a rule reaches only through a lazy import inside a function is not seen by the module scan. Rules list such modules explicitly (`fat`, `macho` for the packs). If that misses a change, deleting `assets-out/manifest.json` forces a full run.
- `mtport.pngdat extract` no longer has a default output directory: `assets-out/png/` is the pipeline's, and `extract` writes wherever `--out` says.
