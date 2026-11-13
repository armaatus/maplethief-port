# Function names: how many are still generated

How many guest functions still carry a generated Ghidra name (`FUN_xxxxxxxx`), and which of them the play runs enter most ([#380](https://github.com/armaatus/maplethief-port/issues/380)). Names come from the names files in `work/re/names/` ([ADR 0009](../adr/0009-re-analysis-outputs.md)); a function none of them names keeps Ghidra's default. Addresses are armv7 VM addresses; function pointers carry the Thumb bit.

## Count

From the export of a clean `scripts/re-analyze` run (`work/ghidra/functions.json`, the 2,658 functions in `__text`), by the tags of [libraries.md](libraries.md), on 2026-11-13:

| Tag | Functions | Generated name | Share |
|---|---|---|---|
| `engine` | 1,353 | 523 | 38.7 % |
| `lib:chartboost` | 675 | 163 | 24.1 % |
| `app-objc` | 620 | 0 | 0 % |
| `lib:uidevice-identifier`, `lib:apple-openal-support`, `lib:crt1` | 7 | 0 | 0 % |
| untagged (not a function start) | 3 | 3 | – |
| **total** | **2,658** | **689** | **25.9 %** |

Every Objective-C method is named from its class and selector ([objc.md](objc.md)). The unnamed ones are C and C++ functions: in the engine, mostly small accessors and helpers below the named methods; in Chartboost, C functions and blocks next to its Objective-C classes, code the port keeps offline ([ADR 0034](../adr/0034-offline-services.md)).

## The functions the play runs enter most

A build with `MAPLETHIEF_TRACE_ENTRY=ON` counts every entry of every generated function ([ADR 0030](../adr/0030-runtime-diagnostics.md)); `MAPLETHIEF_COVERAGE=PATH` writes the counts at exit, also from `play_real_test` (its driver passes the variable on). Four runs gave the counts below: `play.mechanics` (the tutorial to Henesys, then 60 kills), `play.first_map`, `play.save_quit` and `play.game_speed`.

- Together they entered **1,052** distinct functions, **127.8 million** times.
- Before #380, the 59 most-entered unnamed functions alone took **60.6 %** of those entries: the sprite and texture draw path, the `.att` and object-grid accessors, the data-table readers and the shared physics of `CMapleObject`.
- #380 named those 59 and the functions its answers needed ([`tools/mtport/flow.py`](../../tools/mtport/flow.py), the section "the functions play runs enter most"; for example `DrawImageAnchored` `0x247DD`, `CMMap::GetAttAt` `0xBC51`, `CDataMgr::ReadU8` `0x398D5`, `CMAction::Update` `0x1E585`, `CMapleObject::SweepWalls` `0x6D3BD`).
- Afterwards, **357** of the entered functions still have a generated name, and they take **4.3 %** of the entries. The most-entered of them are `0x36DF1`, `0x23719`, `0x6911D`, `0x7505` and `0x6431`, each entered 190,000 to 275,000 times.

To repeat the count: configure `mac-debug` with `-DMAPLETHIEF_TRACE_ENTRY=ON`, build `play_real_test`, run the play tests with `MAPLETHIEF_COVERAGE` set, and join the files (`0xPTR entries name`, one function a line) with the names files.
