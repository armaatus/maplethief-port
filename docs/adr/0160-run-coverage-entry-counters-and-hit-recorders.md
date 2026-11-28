# 160. Run coverage: entry counters, hit recorders behind one switch, and a merge and report tool

- **Status:** Accepted (delegated by Joris, 2026-11-13)
- **Date:** 2026-11-26
- **Issue:** [#295](https://github.com/armaatus/maplethief-port/issues/295) (epic [#285](https://github.com/armaatus/maplethief-port/issues/285))
- **Builds on:** [ADR 0030](0030-runtime-diagnostics.md) (the diagnostics, `MT_TRACE_ENTRY`, `MAPLETHIEF_COVERAGE`), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (observed functions), [ADR 0069](0069-guest-function-hooks.md) (hooks), [ADR 0104](0104-input-replay-and-regression-checkpoints.md) (the regression replay and its digest)

## Context

Epic #285 wants "verified" to become a number: how much of the game's code and data the automated runs have executed, so that each later child can show what it added. What existed:

- `MT_TRACE_ENTRY` (ADR 0030) counts the entries of every generated function when `maplethief_gen` is compiled with it, and `MAPLETHIEF_COVERAGE=PATH` writes them at exit. Only the title soak used it (`python3 -m mtport.titlerun coverage`); no play test did.
- Nothing recorded which scripts, script natives, maps, HLE imports or (class, selector) pairs a run touched. The HLE trace logs calls but keeps nothing, and the static import analysis cannot see dynamic sends.
- 675 of the 2,655 functions are Chartboost's and never run by design ([re/libraries.md](../re/libraries.md)), so game code is 1,980 functions.

The constraint: with the counters off, the generated code and the regression digest (`scripts/regress`) stay the same.

## Options considered

**Where natives and maps are seen.** Both are guest code: `Lua::callNative` `0x9B37D` gets the native's index in `r1`, the map loader `0x86699` gets `(this, area, map, newArea)`.

1. Observed functions (ADR 0053) or hookable functions (ADR 0069): the recompiler emits a call at their entry. That changes the generated code for every build.
2. A recorder in the test driver, reading guest state each turn: it misses a map entered and left within a turn, sees no native, and only the driver's tests would have it.
3. **The entry counter's own macro.** `MT_FN_TRACE_ENTRY` (`calls.h`) is already per function and compiled only with `MT_TRACE_ENTRY 1`. On a function's first entry the runtime can answer whether it wants every entry of that function; the macro keeps the answer in a second function-local static. The generated C does not change at all, and the build without counters expands the macro to nothing as before.

**Where scripts are seen.** The four script runners build a path and read it through `ResFile_ReadAll`, which is CoreFoundation's resource read (`CFURLCreateDataAndPropertiesFromResource`). The HLE knows the bundle path of every successful read, so a recorder there needs no guest knowledge and no counters build. `Lua::loadBuffer` sees only the bytes (the file name inside is always `Temp.dat`).

**Imports and selectors.** Imports: the HLE trace already wraps the import table's slots with a trampoline (ADR 0030 option 3); while the coverage is on it wraps every function import and counts, tracing only the slots its filter names. Selectors: the ObjC runtime's `deliver` is the one place every delivered message passes, host sends included.

**The switch.** A new variable, or the one that exists. `MAPLETHIEF_COVERAGE=PATH` already means "write the coverage"; the hits go into the same file.

## Decision

- **The `mac-coverage` preset** (configure, build, test): `mac-debug` plus `MAPLETHIEF_TRACE_ENTRY=ON`, in `build/mac-coverage/`.
- **One switch: `MAPLETHIEF_COVERAGE=PATH`.** `%p` in PATH becomes the process id, so every process of a test keeps its own file. With it set:
  - the function-entry coverage, as before (counters build);
  - **hit recorders** (`maplethief/diag.h`: `mt_hit`, `mt_hit_count`/`mt_hit_add`, `mt_hits_format`), a store of (kind, key) with a count, unique by a numeric id within a kind so that a key is formatted once: `import` (every call through the import table, `mt_trace_import_call`), `selector` (`-[Class sel]` by the class the lookup starts from, `deliver` in `objc.cpp`), `script` (every bundle read under `script/`, `cf/bundle.cpp`), `native` (`"<index> <name>"`, the name from `g_scriptNativeNames` `0x117950`) and `map` (`"<area>.<map>"`), the last two from watchers (`runtime/src/events/run_hits.cpp`, registered by the boot with `mt_coverage_watch`; counters build only);
  - the file gets the functions (`0xPTR entries name`) and then one line `hit <kind> <count> <key>` per key.
- **Watched functions.** `mt_trace_fn_first` returns the function's pointer when the coverage is on and a watcher is registered for it, else 0; `MT_FN_TRACE_ENTRY` keeps that in a static and calls `mt_trace_fn_watched` on every entry while it is non-zero. Up to 16 watchers. A watcher only reads registers and guest memory.
- **`python3 -m mtport.runcoverage`**: `merge` sums coverage files (a merged file keeps a `# run NAME FUNCTIONS` line per run); `report` writes `work/coverage/run-coverage.{md,json}`: executed and total per function tag (`work/re/function-tags.json`, #10), game code without Chartboost, every unexecuted function with address, tag and name, the scripts read (of the bundle's `script/*.out`), natives (of 65), maps (of the bundle's `map/*.map`), the game's function imports and the selectors, each with its count; `--baseline OLD.json` adds what is new since. `run` builds the preset and runs the chosen CTest tests one at a time (with the fixture setups they need first, `-FA .*` so CTest adds none itself), each into `work/coverage/runs/<test>/`, then merges and reports.
- The reports stay in git-ignored `work/`. [testing/run-coverage.md](../testing/run-coverage.md) has the how-to and the baseline's numbers (numbers and addresses only).

## Consequences

- A child of epic #285 runs `mtport.runcoverage run --tests …` and `report --baseline` to show what its runs added.
- Off (no counters, no variable) nothing changes: the generated code is the recompiler's, byte for byte; the import table holds the implementations; `deliver` and the resource read each test one flag. The regression digest is the same.
- The coverage build runs slower (an increment and a branch per guest call, a hash lookup per import call and per message while the variable is set). It is for coverage runs, not for timing.
- Selectors are counted by the class the lookup starts at: a message to a subclass instance that the superclass implements counts as the subclass's pair. That is the pair the guest sent.
- How we'd know this was wrong: a run whose report misses a map or a script that its log shows (a load path that bypasses the loader or the resource read), or a regression digest that differs with the variable set (a recorder that changed the guest).

## Evidence

See [testing/run-coverage.md](../testing/run-coverage.md) for the baseline. Unit tests: `diag_test.cpp` (`run coverage: *`: hits by kind and key, `%p`, the import counting with and without the trace, every entry of a watched function), `tests/test_runcoverage.py` (parsing, merging, the report over a synthetic universe, the baseline comparison, the test selection with fixtures, a run against a stand-in for CTest).
