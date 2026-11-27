# 232. Coverage gaps: a static reason or a test for every function no run enters, and import models for the differential test

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#305](https://github.com/armaatus/maplethief-port/issues/305) (epic [#285](https://github.com/armaatus/maplethief-port/issues/285))
- **Builds on:** [ADR 0017](0017-unicorn-differential-testing.md) (the differential test), [ADR 0160](0160-run-coverage-entry-counters-and-hit-recorders.md) (run coverage)

## Context

The run coverage (ADR 0160) lists the game functions no automated run enters. #305 asks for a reason or a test for each: dead (with evidence), covered by the differential test, or executed by a run step still to add. ADR 0017 excluded every function that calls an import, and named import models as a possible extension.

What the binary offers as evidence, checked on 2026-11-27:

- Every function has at least one static reference (the recompiler's seeds: a call, an ObjC method list, a vtable, a data word). "No caller" alone proves nothing; dead code here is code whose references all come from dead code.
- The references are complete enough to follow. The Ghidra export has the calls, jumps and address loads; outside the code, the only references to functions are the ObjC method lists, the vtables, `__mod_init_func` and block descriptors. For selector loads, Ghidra and the repo's own ARM scanner (`mtport.armscan`) agree exactly: 946 selectors, the same loading functions for each.
- Most never-run ObjC methods have a selector with no `__objc_selrefs` slot at all: no code in the image can send them. The only string-to-selector lookup (`NSSelectorFromString`) is Chartboost's. The one nib, `MainWindow.nib`, connects two outlets (`delegate` and `window`) and no actions.
- Script natives are dispatched by `Lua::callNative`'s switch, so a native's handler is reachable exactly when a shipped script calls that native (`mtport.scriptout` decodes all 307).

## Options considered

**Dead: by hand or by a fixpoint.** A list by hand goes stale with every rename and re-analysis. A reachability fixpoint over the static references, seeded with the executed functions, recomputes the same answer from `work/` every time and names the evidence. Chosen.

**Which ObjC methods are roots.** (1) Every method: nothing would ever be dead, though most setters and many actions provably have no sender. (2) Only methods some code sends: UIKit's callbacks (`dealloc`, `touchesBegan:withEvent:`, delegate methods) would look dead. (3) **The methods the frameworks may send**: overrides of an imported class's method (the image sends the selector to `super`), delegate methods (the first keyword of a framework protocol the class adopts, or of a known framework delegate, since a class is often a delegate without declaring the protocol), the setters of nib outlets, and a short list of UIKit callbacks. Chosen.

**Covering functions that call imports.** (1) Run them in the game: many are reached only through states the bot cannot produce. (2) **Import models in the differential test**: the pure imports (integer division, `mem*`, `str*`) are small and have no outside state, so both sides can run the same model. Chosen for those. (3) Models of the runtime's HLE (Foundation, OpenGL): far larger, and the comparison would then test our HLE against itself. Not done.

## Decision

- **`python3 -m mtport.coveragegaps`** (`tools/mtport/coveragegaps.py`) classes every game function (Chartboost's never run by design, ADR 0160) that no run entered:
  - `dead`: not reachable. Live are the executed functions, the entry point and the static initializers, and every function with a live reference: a live function calls it, jumps to it or loads its address (also through a data structure such as a block descriptor, loaded within 16 bytes of the word); its selector is loaded by a live function or is a framework selector (above); a vtable holding it is loaded by a live function; its native is called by a shipped script. The evidence names the dead references.
  - `difftest`: live, and passes the differential test (`work/recomp/difftest.json`).
  - `unreachable`: live, and matched by a rule (`RULES`) that names the state no run produces.
  - `missed`: live, reachable by play, not entered yet.
  - A rule may also class a live function `dead` when the port or the shipped data never takes its path, with the reason (crt1's `start`, which the runtime replaces per ADR 0027; the Monster Carnival, which only unused scripts start; the movie player classes, which nothing creates; `alBufferDataStaticProc`, reached only from a branch for a music id).
  - Every `unreachable` and `missed` row names its follow-up: #432 (host-driven tests) and #433 (run steps). #305's first criterion is narrowed accordingly (comment on the issue).
- **`docs/re/coverage-gaps.md`** is generated (address, name, tag, class, evidence; names and addresses only) from a merged coverage file of the whole real-asset suite. `check` regenerates it from the doc's own list of unexecuted functions and the current `work/`; `tests/test_coveragegaps_real.py` does the same and fails when the doc is stale.
- **Import models** (`recomp/difftest/models.py`, `models.c`): `__divsi3`, `__modsi3`, `__udivsi3`, `__umodsi3`, `__divdi3`, `__udivdi3`, `memcmp`, `memcpy`, `memset`, `strcat`, `strcmp`, `strcpy`, `strlen`. The lifted code calls them through the driver's import table (C) and through a code hook on each stub (Unicorn), which runs the Python model on the window (loaded a page at a time) and returns to `lr`. Both sides access one byte at a time in one order, so a fault lands at the same address (pc = the stub). A write into `__TEXT` drops the case, as for the game's own stores. Division by zero returns 0 (the remainder: the dividend); results go to r0 (r0 and r1 for the 64-bit division); nothing else changes. Leaf selection accepts calls to the modeled stubs. The models are not libc: what is compared is the lifted code around the call. A CI test runs 400 cases through both sides without Unicorn.

## Consequences

- See Evidence for the numbers. Functions that stay `missed` or `unreachable` are follow-up work: run steps for the first, host-driven tests for the second (the issues are named in the doc).
- A re-analysis that changes a reference, a renamed function or a difftest change shows up as a stale doc in the real-asset test, without rerunning the play tests. A new coverage run means `generate` again.
- How we'd know this was wrong: a `dead` function entered by a later run (a reference the fixpoint missed: then the rule that killed it needs a root), or a framework callback classed dead (add it to the framework selectors).

## Evidence

The coverage of the whole real-asset suite on 2026-11-27 (`mac-coverage`, 304 coverage files; `play.hud_extras` and `play.chapter_1_k` failed under the coverage build, the second on its 600 s time-out, so `chapter_1_l` and `_m` did not run):

- **1,980 game functions; 1,490 executed by a run; 490 not.** Of these, **191 dead**, **81 difftested**, **124 unreachable** (#432) and **94 missed** (#433).
- Unreachable: about 100 destructors and `dealloc`s (teardown), memory warnings, audio interruptions, the push token, the cash shop's purchases, Game Center, deleting a coupon.
- Missed: mostly the UIKit buttons of the touch keypads (the bots press game keys), the main menu's character slots and About, the options tab, three script natives and their Lua helpers, and the state stack's push/pop.
- The differential test with the models: **857 leaves (was 786), 839 pass** (was 775), zero mismatches over 847,338 returning and 576,423 faulting cases; 42 leaves call a modeled import themselves (`__divsi3` 22, `memcpy` 7, `__modsi3` 5, `strcmp` 3, `__divdi3` 2, `strlen` 2, `memset` 1), 38 of them pass.
