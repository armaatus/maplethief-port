# Headless Ghidra for the armv7 slice

These scripts build a Ghidra project of `work/slices/MapleThief.armv7` from scratch, with no manual steps, and export functions, xrefs and strings as deterministic JSON into `work/ghidra/`. They also apply names that other analyses write to `work/re/names/*.json`. Design and rationale: [ADR 0007](../../docs/adr/0007-re-tooling-ghidra.md).

Nothing here is installed system-wide, and nothing it produces is committed. `build/` and `work/` are git-ignored, and so are `*.gpr` and `*.rep/`.

## Quick start

To rebuild everything the RE work produces (all names files, a clean project with every name applied, and the generated docs that read the export), run `scripts/re-analyze`. The steps below are what it runs for Ghidra itself.

```sh
scripts/bootstrap          # work/slices/MapleThief.armv7 (needs Python >= 3.10)
tools/ghidra/import        # first run also sets up Ghidra + JDK; ~1 min afterwards
ls work/ghidra/            # functions.json  xrefs.json  strings.json  analysis.log
```

Prerequisites are `uv`, a Python 3.10+ (set `PYTHON=` if `python3` is older) and, on macOS or Linux arm64, a C/C++ toolchain (Xcode Command Line Tools, or gcc/clang + make) for Ghidra's native decompiler. In a fresh worktree, set `MAPLETHIEF_DOWNLOAD_DIR=<main checkout>/build/_downloads` to reuse the cached archives.

| Command | Does |
|---|---|
| `tools/ghidra/setup` | Fetches and verifies Ghidra 12.2, Temurin JDK 25.0.4.1+1 and (macOS/Linux arm64 only) Gradle 9.8.0 into the download cache, unpacks them under `build/ghidra/`, builds Ghidra's natives if the zip has none for this host, and syncs the PyGhidra environment. The other commands run it automatically. Safe to re-run, and offline once the cache is filled. |
| `tools/ghidra/import [--names-dir DIR] [--out DIR] [--no-export]` | Deletes and re-creates the project, seeds the `LC_FUNCTION_STARTS` functions, runs auto-analysis, applies the names files, saves and exports. |
| `tools/ghidra/apply-names [--names-dir DIR] [--no-export]` | Applies `work/re/names/*.json` to the existing project, saves, re-exports. Idempotent. |
| `tools/ghidra/reexport [--out DIR]` | Re-exports from the existing project (e.g. after naming things in the GUI). |
| `tools/ghidra/run-script SCRIPT [ARGS...]` | Runs any Ghidra script (PyGhidra `.py` or Java) on the project and saves if it changed anything. A bare name is looked up in `tools/ghidra/scripts/`. |
| `PYTHONPATH=tools python3 -m mtport.names check [DIR]` | Validates names files without Ghidra (`show` also prints each one's Ghidra symbol). |

On Windows, run the same scripts from Git Bash with `uv` on `PATH` and `PYTHON=python` (e.g. `PYTHON=python scripts/re-analyze`). The export is meant to be byte-identical to the macOS one. Auto-analysis has no time limits, so a slower host no longer decodes less code (#112, ADR 0013).

Environment: `MAPLETHIEF_DOWNLOAD_DIR` (download cache, default `build/_downloads`), `MAPLETHIEF_GHIDRA_DIR` (unpacked tools, default `build/ghidra`), `MAPLETHIEF_GHIDRA_PROJECT_DIR` (project location, see below), `UV_PYTHON` (the Python for PyGhidra, 3.10 to 3.14).

**Project location.** The project is `work/ghidra/MapleThief.gpr`. Ghidra refuses project paths with a component that starts with `.`, so in an agent worktree (`.claude/worktrees/<name>/`) the project goes to the main checkout's `build/ghidra-projects/<name>/` instead. The exports still go to the worktree's `work/ghidra/`. To open the project in the GUI: `build/ghidra/ghidra_12.2_PUBLIC/ghidraRun` with `JAVA_HOME` set as printed by `tools/ghidra/setup`. The scripts appear in the Script Manager under *MapleThief* once `tools/ghidra/scripts` is added as a script directory.

## Pinned tools

| Tool | Version | Source (SHA-256 in `tools/mtport/ghidra_setup.py`) |
|---|---|---|
| Ghidra | 12.2 (2026-10-22) | `ghidra_12.2_PUBLIC_20261016.zip` from the GitHub release `Ghidra_12.2_build` |
| JDK | Temurin 25.0.4.1+1 | `OpenJDK25U-jdk_{aarch64,x64}_{mac,linux}_hotspot_25.0.4.1_1.tar.gz`, `OpenJDK25U-jdk_x64_windows_hotspot_25.0.4.1_1.zip` from `adoptium/temurin25-binaries` release `jdk-25.0.4.1+1` |
| Gradle | 9.8.0 | `gradle-9.8.0-bin.zip`, only for `buildNatives` on macOS / Linux arm64 |
| PyGhidra | 3.2.0 | PyPI, uv group `ghidra` (JPype1 1.7.1 in `uv.lock`) |

## Pipeline

1. **Import** with Ghidra's Mach-O loader, language `ARM:LE:32:v7`, compiler spec `default`.
2. **Analysis options** (`mtport.ghidra_common.ANALYSIS_OPTIONS`): Ghidra's defaults, with two exceptions:
   - The *Objective-C Message Analyzer* is off. It crashes partway through on this binary, at a different point on each run, which made the exports non-reproducible (ADR 0007).
   - There are no time limits. The decompiler timeouts of *Decompiler Switch Analysis* and *Call Convention ID* are 0 (none), and the GNU demangler's is one day. With Ghidra's 60 s default, a slower host silently dropped the switch analysis of `CDataMgr::OnServerPacket` and lost 5,222 bytes of code ([ADR 0013](../../docs/adr/0013-ghidra-analysis-without-time-limits.md), #112).

   The driver stops with an error if Ghidra doesn't know one of these options.
3. **Seed** (`scripts/seed_functions.py`): for each `LC_FUNCTION_STARTS` entry, `TMode` = the start's low bit over the code from that start to the next one. Then it disassembles from every start in its mode and creates the 2,655 functions.
4. **Auto-analysis**.
5. **Names** (`scripts/apply_names.py`): see [Naming contract](#naming-contract).
6. **Export** (`scripts/export.py`).

The pure-Python parts live in `tools/mtport/` (`ghidra_setup.py`, `ghidra_common.py`, `names.py`) and are tested in CI (`tests/test_ghidra_setup.py`, `tests/test_ghidra_common.py`, `tests/test_names.py`). `tests/test_ghidra_real.py` checks the exports in `work/ghidra/` locally and skips when they are absent. The scripts load `mtport` through `scripts/mtport_loader.py`: putting `tools/` on `sys.path` would make `tools/ghidra/` shadow Ghidra's Java `ghidra` package.

## Export schema (version 1)

Each file is one JSON object. The header keys come first, sorted, one per line. Then comes the list, with one compact record per line, sorted. Files are UTF-8 with no timestamps, so two runs on the same inputs are byte-identical and diff line by line. All addresses are armv7 VM addresses as upper-case hex. A *function pointer* (`address` of a function, `callers`, `callees`, `function`, `thunk_of`) carries the Thumb bit. Every other address doesn't.

Common header: `schema` (1), `ghidra` (`"12.2"`), `program`: `{name, slice, sha256, language, compiler, image_base}`.

**`functions.json`**: header `counts` `{functions, thumb, arm}` and `function_starts` `{function_starts, matched, missing[], wrong_mode[], extra[], extra_by_section{}}`. Then `functions[]`, sorted by entry:

| Field | Meaning |
|---|---|
| `address` | entry as a function pointer (`0x2A3C1` = Thumb at 0x2A3C0) |
| `mode` | `"thumb"` / `"arm"`: `TMode` of the entry instruction |
| `size`, `body` | number of bytes; `[[start, end), ...]` address ranges |
| `name`, `name_source` | the names-file name (`"names"`) if one applies, else Ghidra's `Namespace::name` and its source (`default`, `analysis`, `imported`, `user_defined`) |
| `section`, `block` | Mach-O `segment,section` (null outside the image), and Ghidra's memory block |
| `in_function_starts` | whether the entry is an `LC_FUNCTION_STARTS` entry |
| `thunk_of` | thunked function (pointer or `EXTERNAL:<library>::<name>`), else null |
| `no_return` | Ghidra's non-returning flag |
| `callers`, `callees` | sorted, unique function pointers (or `EXTERNAL:...`) |

**`xrefs.json`**: header `count`. Then `xrefs[]`, all memory and external references (stack and register refs left out), sorted by `(from, operand, to, type)`: `{from, to, type, operand, function}`. `to` is an address or `EXTERNAL:<library>::<label>`. `type` is Ghidra's `RefType` name (`UNCONDITIONAL_CALL`, `READ`, `DATA`, `PARAM`, `COMPUTED_JUMP`, ...). `function` is the containing function's pointer, or null.

**`strings.json`**: header `count`. Then `strings[]`, read from the Mach-O itself (not from Ghidra's string analyzer) and sorted by address: `{address, section, kind, encoding, value, refs}`, where `refs` is the sorted list of addresses that reference the string.

| `kind` | Where | Extra fields |
|---|---|---|
| `cstring` | every NUL-terminated string in a `S_CSTRING_LITERALS` section (`__cstring`, `__objc_methname`, `__objc_classname`, `__objc_methtype`) | |
| `ustring` | every NUL-terminated UTF-16LE string in `__TEXT,__ustring` | |
| `cfstring` | every 16-byte object in `__DATA,__cfstring` | `flags` (`0x7C8` 8-bit, `0x7D0` UTF-16), `target` (character data), `length` (bytes or UTF-16 units) |

`encoding` is `utf-8` or `utf-16-le`. Bytes that don't decode are given as `encoding: "hex"` with the hex string as `value`.

`analysis.log` holds the auto-analysis message log. It is empty on a clean run.

## Naming contract

Any analysis can name addresses by writing one file to `work/re/names/<source>.json`:

```json
{"source": "objc",
 "names": [
   {"address": "0x2879", "name": "-[MapleThiefAppDelegate setWindow:]", "kind": "function",
    "comment": "v12@0:4@8"},
   {"address": "0x117E44", "name": "cfstr_EAGLViewController", "kind": "data"},
   {"address": "0x2A3D1", "name": "loop_head", "kind": "label"}
 ]}
```

- `address`: armv7 VM address in hex (≤ 32 bits). For a `function` it is a function pointer, so the Thumb bit gives the mode (`0x27E0` is ARM). For a `label` (a code location) the Thumb bit is ignored. For `data` the address is used exactly as written.
- `kind`: `function` creates the function if it is missing (setting `TMode` and disassembling first), renames it with source `USER_DEFINED`, and puts `comment` in its plate comment. `label` and `data` add a `USER_DEFINED` label, make it primary and put `comment` in the EOL comment.
- Files are read in sorted file-name order. Entries for the same location (Thumb bit cleared) must agree on kind, name and mode, otherwise `apply-names` fails and names both files. The comments of agreeing entries are joined in file order. Names Ghidra can't hold are rejected up front: empty, over 2,000 characters, leading or trailing whitespace, control characters, or Ghidra's reserved defaults (`FUN_…`, `LAB_…`, `DAT_…`, `SUB_…`, `EXT_…`, `UNK_…`, `OFF_…` + hex).
- Applying is idempotent. A second run with the same files reports `0 changes` and exports byte-identical files. A clean `tools/ghidra/import` re-applies every file, so the names survive a re-import. Renaming an entry on an existing project leaves the old label of a `label`/`data` entry behind as a secondary label; `import` starts clean.

**Names → Ghidra symbols** (`mtport.names.ghidra_symbol`). Ghidra symbols can't contain spaces or other whitespace, control, format, combining or unassigned characters.

| Contract name | Ghidra namespace | Ghidra symbol |
|---|---|---|
| `-[Class sel:with:]` (ObjC, also `+[…]` and `-[Class(Category) sel]`) | global | `-[Class_sel:with:]` (the one space becomes `_`) |
| `Class::vf3` | `Class` | `vf3` |
| `ns::Tpl<a::b>::f` | `ns` → `Tpl<a::b>` | `f` (only `::` outside `<>`, `()`, `[]` splits) |
| `Class::operator<` | `Class` | `operator<` (nothing after `operator` splits) |
| anything else | global | the name, each run of invalid characters → `_` |

Because sanitizing is lossy, `apply_names.py` also stores the exact contract name per address in the program's string property map `mtport.name`, and the export's `name` is that exact name.

## Function tags

[#10](https://github.com/armaatus/maplethief-port/issues/10) tags every `LC_FUNCTION_STARTS` function as `app-objc`, `engine`, `lib:<name>` or `unknown` (rules, counts and evidence: [docs/re/libraries.md](../../docs/re/libraries.md)). The tags are computed outside Ghidra, from the exports, and then put on the functions as Ghidra *function tags*:

```sh
PYTHONPATH=tools python3 -m mtport.libtags generate   # work/re/function-tags.json, docs/re/libraries.md
tools/ghidra/run-script apply_tags.py                 # scripts/apply_tags.py [TAGS_JSON [READBACK_JSON]]
```

`apply_tags.py` gives each tagged function exactly its one tag, removes tags of this scheme that no longer apply, leaves other (hand-made) tags alone and sets each tag's comment. It is idempotent: a second run reports `0 changes`. In the GUI the tags show in the *Function Tags* window and as a column of the *Functions* table. A clean `import` starts without tags; `scripts/re-analyze` runs both commands after it.

Afterwards `apply_tags.py` reads the scheme's tags back from every function and writes `work/re/ghidra-tags.json` (`{schema: 1, count, functions: [{address, tags}]}`, in the export layout, deterministic). `tests/test_libtags_real.py::test_ghidra_carries_the_tags` compares it with the recomputed tags, so the check needs no Ghidra. The tags are not in the export schema: the export is written by `import`, before the tags exist, and the generated docs read that export.

`scripts/re-analyze` also runs `pngdat_calls.py work/re/pngdat_calls.txt`, the listing of every call to the `png/**/*.dat` pack loaders that `tests/test_pngdat_real.py` checks `mtport.pngdat.PACKS` against ([#17](https://github.com/armaatus/maplethief-port/issues/17)). It commits the four loaders' decompiled parameters to the project. Neither script changes what an export contains: a `reexport` after both is byte-identical to the one `import` wrote.

## Results on MapleThief 1.5 (armv7)

From a clean `tools/ghidra/import` with `work/re/names/objc.json` (#8) present:

- 2,876 functions (2,657 Thumb, 219 ARM). All **2,655** `LC_FUNCTION_STARTS` entries are functions with the right mode: 0 missing, 0 wrong mode.
- 221 extra functions: 217 ARM stubs in `__symbolstub1`, 1 external thunk (`_realloc`, in Ghidra's `EXTERNAL` block) and 3 false positives in `__text` (0xAF3BD, 0xBF681, 0xBF711). The 3 are tables of 32-bit words, read PC-relatively by the preceding function, that Ghidra decoded as Thumb code ([ANALYSIS.md → Ghidra cross-check](../../docs/ANALYSIS.md#ghidra-cross-check-7)). Treat `in_function_starts: false` functions in `__text` as suspect.
- 87,654 xrefs. 3,457 strings: 3,089 C strings, 1 UTF-16 string (the Korean terms of service, 11,987 units), and 367 CFStrings (366 8-bit, 1 UTF-16).
- All 1,064 ObjC names apply. Two clean runs give byte-identical `functions.json`, `xrefs.json` and `strings.json`. So does a run on the M4's efficiency cores (`taskpolicy -c background tools/ghidra/import`, ≈ 3.5 min of analysis), which stands in for a slower host (#112).
- Runtime on an Apple M4 (10 cores, Apple clang 21): first-time setup from a filled cache ≈ 25 s (unpack + 20 s `buildNatives`). `import` ≈ 60–70 s, of which auto-analysis is ≈ 50 s. `reexport` ≈ 7 s. `apply-names`, including its re-export, ≈ 7 s.

To check determinism yourself, run `tools/ghidra/import --out build/a`, then `tools/ghidra/import --out build/b`, then `cmp build/a/xrefs.json build/b/xrefs.json` (and the same for the other two files).
