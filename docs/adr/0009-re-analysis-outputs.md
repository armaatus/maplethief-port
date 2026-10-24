# 9. RE analysis outputs: xref sources, function tags and generated docs

- **Status:** Accepted
- **Date:** 2026-10-25
- **Issue:** #10, #11, #22 (epic #96)

## Context

ADR 0007 made headless Ghidra the RE base: a clean import, a deterministic export (`work/ghidra/{functions,xrefs,strings}.json`) and the names contract. The analyses of epic #96 build on that export and add three kinds of output that later work relies on:

- **Code sites.** The import table work list ([#11](https://github.com/armaatus/maplethief-port/issues/11), [docs/re/imports.md](../re/imports.md)) and the ObjC shell inventory ([#22](https://github.com/armaatus/maplethief-port/issues/22)) need every call of a stub, every load of a pointer slot and, at every `objc_msgSend`, the selector and receiver. The code is position-independent Thumb-2 at -O0: almost every address is built with `ldr rX, [pc, #n]` or `movw`/`movt` and then `add rX, pc`, and values pass through stack slots. Ghidra's references cover most of these, but not the selector and receiver at a send, and the ObjC Message Analyzer that would add them is off because it made the exports non-reproducible (ADR 0007).
- **Function tags** ([#10](https://github.com/armaatus/maplethief-port/issues/10), [docs/re/libraries.md](../re/libraries.md)). Every one of the 2,655 `LC_FUNCTION_STARTS` functions needs exactly one of `app-objc`, `engine`, `lib:<name>` or `unknown`, because they decide what gets recompiled, what is stubbed and what is rewritten. The evidence is uneven: ObjC metadata and the entry point are certain, while reachability and link order are only suggestive.
- **Docs in `docs/re/`** that are derived from the slice and the export, committed and reviewed like any other doc, and that must not drift from the analysis behind them.

## Options considered

Code sites:

1. **Ghidra's xrefs only**: one source, but no send arguments, and a Ghidra run is needed for everything.
2. **A Capstone sweep in Python only** (`tools/mtport/armscan.py`): resolves the PC-relative idioms and the send arguments with a small forward data-flow pass, runs on a synthetic image in CI, but would be the only check on itself.
3. **Both, cross-checked**: armscan produces the sites, and the generators compare them with Ghidra's xrefs and report every difference.

Function tags:

1. **Ghidra Function ID / library signatures**: there are no FID databases for these libraries (Chartboost, Apple's OpenAL sample code, crt1), and the result would be a Ghidra artifact rather than a reviewable rule.
2. **Scoring or voting over all evidence**: copes with conflicting evidence but makes a tag hard to explain.
3. **An ordered rule list, first claim wins**: each rule is one kind of evidence, strongest first, and each assignment records its rule and a reason with addresses.

Generated docs:

1. **Whole-file generators**: the module writes the entire Markdown file, prose included, as Python strings (`objc.md`, `cpp-classes.md`, `imports.md`, `libraries.md`; `rtti.NOTES`, `libtags.FINDINGS`).
2. **Generated blocks inside a hand-written doc**: the Markdown is written by hand, and the generator rewrites only the bodies between `<!-- BEGIN generated:KEY -->` and `<!-- END generated:KEY -->`, failing on a missing, repeated or unknown block (`objcshell.render`, [docs/re/objc-shell.md](../re/objc-shell.md)).
3. **Hand-written docs with no generator**: no drift check at all (the research docs of #12, #19, #21, #23; #21 and #23 add a site-coverage test, `mtport.sitecheck`).

## Decision

- **Capstone `armscan` is the second xref and data-flow source, next to Ghidra, and the two are cross-checked** (option 3). `armscan` sweeps each function linearly (skipping literal pools and jump tables), follows registers and stack slots to absolute addresses, and records calls, loads, stores, PC-relative addresses and the arguments of every send and direct call. `mtport.imports` compares its call and load sites with `xrefs.json` per import and prints the agreement in `imports.md` (333 of 333 imports identical). The selector and receiver of a send come from `armscan` alone, because Ghidra has no equivalent with the Message Analyzer off. A new analysis that needs code sites uses `armscan` and compares with Ghidra where Ghidra has the same fact.
- **Function tags come from `libtags`' ordered rule set** (option 3): `objc-imp`, `entry-point`, `export`, `signature`, `vtable`, `static-init`, `objc-file`, `closure`, `reach`, `layout`, `unknown` (`libtags.RULE_DOCS`). The first rule that claims a function wins, so stronger evidence always beats weaker evidence, and every function's tag names the rule and a reason (`python3 -m mtport.libtags show ADDR`). `generate` fails unless every function start has exactly one tag. The tags are computed outside Ghidra from the export and put on the Ghidra functions by `apply_tags.py`, which reads them back into `work/re/ghidra-tags.json` for a test. A new kind of evidence becomes a new rule at the position its strength warrants.
- **New docs in `docs/re/` use generated blocks inside hand-written Markdown** (option 2). The prose is the part that needs review and editing, and it stays Markdown, while the generator owns only the tables and lists it can recompute. The four existing whole-file docs stay as they are: they are reference tables with little prose, and nothing is gained by converting them now. Option 1 remains acceptable for a new doc that is purely a table. A hand-written doc that claims to cover every site of something gets a coverage test like `mtport.sitecheck`.
- **Generated docs depend on the canonical export from `scripts/re-analyze`.** That script runs the names generators, a clean `tools/ghidra/import` (which writes the export), `libtags generate`, the Ghidra scripts `apply_tags.py` and `pngdat_calls.py`, and the generators that read the export. A step after `import` must not change what an export would contain (a `reexport` afterwards is byte-identical), so the export written by `import` stays the one input. Every committed generated doc has a local test that it equals the generator's output (`repo_paths.assert_committed_doc`), and `git status` must be clean after `scripts/re-analyze`.

## Consequences

- Two sources disagreeing shows up as a reported difference, not as a silent gap. The cost is that `armscan` is our own decoder of data flow, with known limits (stores through escaped pointers are not tracked), and it has to be kept correct on its own synthetic tests.
- Tags are explainable one function at a time, and rules can be reordered or added without touching the others. A wrong strong rule wins over correct weak evidence, so new rules go in at the lowest position that still works.
- New docs keep their judgement in Markdown and their numbers checked. Two generated-doc models exist side by side until someone has a reason to convert the whole-file ones.
- Changing the order of `scripts/re-analyze`, or adding a step that writes to the Ghidra project, has to keep the export byte-identical, or re-export and regenerate after it.
- Follow-up consolidation of the shared analysis helpers (one Ghidra-export loader, one function-owner index, movw/movt decoding through `armscan`) is [#108](https://github.com/armaatus/maplethief-port/issues/108).
