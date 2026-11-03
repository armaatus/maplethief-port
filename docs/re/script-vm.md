# The script VM: a C++ port of Lua 2.4

Recovered from the `armv7` slice for [#19](https://github.com/armaatus/maplethief-port/issues/19). The bytecode format and the full opcode table are in [docs/formats/script-out.md](../formats/script-out.md). Addresses are armv7 VM addresses, and function pointers carry the Thumb bit. Every function and data name below is applied in Ghidra from `work/re/names/scriptvm.json`, which `python3 -m mtport.scriptout names` writes (127 names: 70 VM functions, 53 native handlers, 4 data labels; no conflicts with the `objc`, `rtti` and `save` names files).

## Lineage: Lua 2.4

The VM is the **Lua 2.4 interpreter** (PUC-Rio, 1996) rewritten as C++ classes. The game's compiler was the stock `luac` 2.4: only the chunk header was stripped from its output. The evidence:

| Lua 2.4 | This binary |
|---|---|
| `luac` dump: `ID_CHUNK` 27, `ID_FUN` `'F'`, `ID_VAR` `'V'`, `ID_STR` `'S'`; `LoadSize` as two words `hi << 16 \| lo`; main has `lineDefined` 0 and a file name, other functions carry `marked`, the PUSHFUNCTION offset in main | the same records (`Undump::loadFunction` `0x9F359`) |
| header `"Lua"` + version `0x23` + test word + test float | **absent**: `Undump::undump` (`0x9F795`) reads `0x1B` and goes straight to the `'F'` records |
| threaded global and string operands, fixed by `Unthread` | `TFunc::unthread` `0x9ED59` |
| 67 opcodes `PUSHNIL` … `SETLINE` (`opcode.h`) | the same 67 in the same order and encodings: `Lua::execute` `0x9BC11` switches over 0x00–0x42 |
| types `LUA_T_NIL` −1 … `LUA_T_LINE` −9, 8-byte `TObject` | the same tags in the same 8-byte `{tag, value}` slots |
| builtins `assert dofile dostring error getglobal next nextvar print setfallback setglobal tonumber tostring type` | the first 13 names at `0x117950`, in the same order, with `println` in place of `nextvar` |
| `lua_CFunction` called through a pointer | a symbol of tag `LUA_T_CFUNCTION` holds its own **symbol index**, and `Lua::callNative` (`0x9B37D`) switches on it |

It isn't 2.5 or later. Lua 2.5 added vararg functions (`VARARGS`) and `STOREMAP` after `SETLINE`, and the switch here ends at `SETLINE`. Lua 3.x reorganised the opcodes around per-function constant tables. Lua 4.0 and 5.0 use the `"\033Lua"` header with versions `0x40`/`0x50`, 32-bit instruction words (5.0 register-based), upvalues and closures. None of those features appear here. Before 2.4 there was no `luac` or undump, so these files couldn't exist. The RTTI class names match Lua 2.4's C structs: `TFunc` (`TFunc` in `func.h`), `Symbol`, `TaggedString`, `Table`, `Undump` (`undump.c`), `Lua` (the interpreter state).

**Differences from Lua 2.4** (all from `Lua::execute` unless noted):

1. **Integer numbers.** `real` is `int`: `PUSHBYTE`/`PUSHWORD` push integers, arithmetic is integer (`__divsi3` for `/`), and the coercions are `atol` (`Lua::stringToNumber` `0x9A4C5`) and `"%d"` (`Lua::numberToString` `0x9A1D5`). `PUSHFLOAT` is an empty case that **doesn't skip its 4 operand bytes**, and `POWOP` does nothing.
2. **No `PUSHSELF`** (methods): it is an empty case that doesn't skip its operand.
3. **`STORELOCAL0`–`9` bug**: the case computes the slot from the last *operand* read (`aux`), not from the opcode: `stack[base + aux − 23] = *--top` (`0x9BFE2`).
4. **Arrays are flat** `TObject*[n + 1]` (`CREATEARRAY`), indexed by the key's value word. There is no hashing, and `STORERECORD` indexes by the string object's address.
5. **Errors are ignored**: `Lua::error` (`0x99161`) is empty, so a type error simply skips the operation.
6. **No fallbacks, no garbage collector.** `TFunc`s go on a list at `Lua+0xC` and are freed on reset.
7. **The built-in library is gone.** The 13 builtin names stay registered, but only `println` and `print` have a dispatcher case, and both are empty.
8. **One shared global table, reset on every load.** `Lua::reset` (`0x9CE81`) truncates the symbol table back to the 65 natives (`Table::resetToNatives` `0x9E4F5`), clears the constants and frees non-native strings (`Stringtable::sweep` `0x9DA4D`). Script globals never survive into the next script: all lasting state lives in the C++ quest manager and hero.

None of the opcodes affected by 1–4 occurs in the shipped files (24 of the 67 opcodes do; see the *Uses* column of the [opcode table](../formats/script-out.md#opcodes)). The shipped scripts run the same on this VM as on stock Lua 2.4 with integer numbers.

## Objects

| Object | Where | Notes |
|---|---|---|
| `Lua` (0x18 bytes) | `world+0x60`, made by `0x86C1D` | `+4` owner `world`, `+8` callback (`regCallBack`), `+0xC` `TFunc` list, `+0x10`/`+0x14` the C-call frame (Lua 2.4's `CBase` / `CnResults`). `Lua::Lua` `0x9D5C5`. The first instance allocates the stack and the symbol tables (`Table::Table` `0x9E98D`) |
| stack | `g_luaStack` `0x1178EC`, `g_luaTop` `0x1178F0`, `g_luaStackLimit` `0x1178F4` | 8-byte `{tag, value}` slots. `Lua::growStack` `0x9A569` doubles it (20 slots at first) |
| symbol table | vector at `0x11AA24` | `Symbol` objects `{…, tag +8, value +0xC}`. Indexes 0–64 are the natives (`Table::registerNatives` `0x9E441`), and globals follow |
| constants | vector at `0x11AA28` | `TaggedString`s of the `S` records |
| strings | 8 hash buckets (`Stringtable`, at `0x11AA2C`) | `Table::createString` `0x9E361` → `Stringtable::intern` `0x9DD65`. `TaggedString` keeps its symbol index at `+4` and its constant index at `+6` |
| function slots | `0x78`-byte table (30 × `TFunc*`) | filled by `FunctionSlots::add` `0x9ED09` in file order, read by `PUSHFUNCTION` |
| `TFunc` (0x24 bytes) | per function | `+8` marked, `+0xC` size, `+0x10` code, `+0x14` line, `+0x18` file name, `+0x1C` pc, `+0x20` `DataReader` over the code. Operands are fetched through `TFunc::fetchU8/U16/U32` (`0x9EE95`/`0x9EE71`/`0x9EE45`), and jumps re-seek the reader (`TFunc::jumpForward` `0x9EE21`, `::jumpBack` `0x9EDF1`) |

`world` is the object at `hero+0x150` that owns the map, its objects and the VM (the "world object" of [obj.md](../formats/obj.md#portals-and-arrival-points), probably `CMapleWorld`). The natives use its `+0x2C` map objects, `+0x30` hero, `+0x58` quest manager, `+0x5C` dialogue box and `+0xB0` current NPC index.

## Execution

- `Lua::loadBuffer(L, buf, size)` `0x9D0A9`: `Lua::reset`, then, if the first byte is `0x1B`, `Undump::undump`. It doesn't run anything.
- `Lua::callFunction(L, tfunc)` `0x9CD65`: pushes the function, then `Lua::doCall(L, base, 0)`.
- `Lua::doCall` `0x9BA11` (Lua 2.4's `do_call`): a `LUA_T_CFUNCTION` slot becomes a C mark and goes to `Lua::callNative`. A `LUA_T_FUNCTION` slot becomes a mark and goes to `Lua::execute`. The results are then moved down to the function's slot and adjusted to `nresults` (unless 255).
- `Lua::execute(L, tfunc, base)` `0x9BC11`: the opcode loop. It returns the first result's stack index at `RETCODE0`/`RETCODE`.
- `Lua::callNative(L, index, base)` `0x9B37D`: saves the C frame and sets `CnResults` (`L+0x14`) = number of arguments and `CBase` (`L+0x10`) = `base` + that, then switches on the symbol index. A handler reads argument *n* with `Lua::getParam(L, n)` `0x993D9` (a stack index, or −1 if *n* is outside 1…nargs) and `Lua::stackSlot` `0x9912D`. It takes the slot's value word as an integer or as a `TaggedString*` (the C string is at `+0xC`). **Nothing checks the tag**: a native given the wrong type reads garbage. Results are pushed on the stack.

## Entry points

The game runs scripts from four places. Each builds `"/script/chapter%d/%s.out"` with the quest manager's chapter (`quests+4`) and falls back to `"/script/common/%s.out"`.

| Caller | When | Runs |
|---|---|---|
| `0x63191` (hero) | the hero uses a portal or talks to an NPC (object kinds 0–59 with a script; [obj.md](../formats/obj.md)) | the object's script: **main**. Main assigns the functions and usually calls `dialog()` or a portal's `changeMap` |
| `0x839BD` (world) | NPC state refresh, for every object of kind 0–59 with a script (`world+0xB0` = the object index) | the object's script: only the function in **slot 0** (`chkQuest`), if the file has one. Main doesn't run. Queued by `updateNPCState` (`msg+0x2D`) |
| `CStateGame::Paint` `0x87501` | after loading a new map | the map script (the `.obj` map-script record): main |
| `CMob::Flush` `0x7458D` | a mob of type ≥ 0x40, except 0x41, dies | the map script: main |

`regCallBack(f)` stores `f` in `Lua+8`. The dialogue box code later calls `Lua::runCallback` (`0x9CE2D`, from `0x35E89`), which clears `Lua+8` and calls `f`. That is how a script's `dialog()` continues after the player answers (`questAfterSelect`, `mainCallBack`, `step2`, … read the answer with `getEvtIndex`). The callback runs in the environment of the last script loaded.

The shape of a typical NPC script, as a made-up example (the names of our own; the natives are real):

```lua
function chkQuest()                    -- slot 0: NPC-state pass
  hidden = 0
  if getQuestState(0, 3) >= 1 then hidden = 1 end
  setNpcHide(hidden); setNpcNotify(-1)
end
function dialog()
  initMsg(); regMsgNPC("Example.dialog.1"); showMsg(0)
end
dialog()                               -- main, on interaction
```

A portal script is a main only: it reads a quest state with `getQuestState`, then either calls `changeMap(map, portal, 0)` or `showPortalErrMsg()`.

## Natives

`Table::registerNatives` (`0x9E441`) registers the 65 names of `g_scriptNativeNames` (`0x117950`) as globals 0–64 with tag `LUA_T_CFUNCTION` and value = their own index. `Lua::callNative` has a case for 53 of them. The other 12 (11 Lua builtins and `loseRequire`) do nothing when called and return no value. *Args* are the types the handler reads (`int`: the value word as a number; `string`: as a string; `function`: the value kept as a `TFunc*`). A call with fewer arguments reads garbage; extra ones are ignored. *Calls* counts call sites in the 307 files (`python3 -m mtport.scriptout stats`). **Every native that a script calls (44 of 65) has a handler, and every call passes exactly that many arguments.** Wherever an argument is a literal, its type matches (`tests/test_scriptout_real.py`). Quest `kind` is 0 for main quests and 1 for sub-quests (`getQuestState` `0x799B1` picks between the arrays at `quests+8` and `quests+0x19C`). Inventory `kind` is 0 for equipment and 1 for other items (`hero+0xD4`/`+0xD8`, 48 slots each).

| # | Name | Handler | Args | Returns | Calls | Does |
|---|---|---|---|---|---|---|
| 0 | `assert` | – | – | – | 0 | no-op (no dispatcher case) |
| 1 | `dofile` | – | – | – | 0 | no-op (no dispatcher case) |
| 2 | `dostring` | – | – | – | 0 | no-op (no dispatcher case) |
| 3 | `error` | – | – | – | 0 | no-op (no dispatcher case) |
| 4 | `getglobal` | – | – | – | 0 | no-op (no dispatcher case) |
| 5 | `next` | – | – | – | 0 | no-op (no dispatcher case) |
| 6 | `println` | `0x9941D` | – | – | 0 | no-op (empty handler) |
| 7 | `print` | `0x99425` | – | – | 0 | no-op (empty handler) |
| 8 | `setfallback` | – | – | – | 0 | no-op (no dispatcher case) |
| 9 | `setglobal` | – | – | – | 0 | no-op (no dispatcher case) |
| 10 | `tonumber` | – | – | – | 0 | no-op (no dispatcher case) |
| 11 | `tostring` | – | – | – | 0 | no-op (no dispatcher case) |
| 12 | `type` | – | – | – | 0 | no-op (no dispatcher case) |
| 13 | `regCallBack` | `0x99449` | function | – | 449 | Lua+8 = arg; the dialogue box runs it (0x9CE2D) |
| 14 | `regMsgHero` | `0x9A0F1` | string, int | – | 66 | msg: queue a hero line, Scripts.strings[key] |
| 15 | `regMsgNPC` | `0x9A055` | string | – | 1,292 | msg: queue an NPC line, Scripts.strings[key] |
| 16 | `regSelList` | `0x99F85` | string, int | – | 253 | msg: queue a choice (key, value) |
| 17 | `initMsg` | `0x99AF1` | – | – | 576 | msg: clear the queued lines and choices |
| 18 | `showMsg` | `0x99F51` | int | – | 580 | msg: open the box in mode arg (msg+8) |
| 19 | `getLevel` | `0x9B329` | – | int | 41 | hero level (hero+0x80) |
| 20 | `getQuestState` | `0x9B269` | int, int | int | 663 | state of quest (kind, id) (0x799B1) |
| 21 | `setNpcNotify` | `0x9989D` | int | – | 100 | current NPC's quest icon; 5 = from current quest |
| 22 | `setClassUp` | `0x997E1` | int, int | – | 16 | hero job and advancement count (0x655AD) |
| 23 | `checkRequire` | `0x9B1A9` | int, int | int | 208 | 1 if quest (kind, id) requirements are met |
| 24 | `setQuest` | `0x99E15` | int, int, string, string | – | 99 | quests: prepare (kind, id, title key, msg key) |
| 25 | `startQuest` | `0x99D65` | – | – | 88 | quests: accept the prepared quest (state = 1) |
| 26 | `clearQuest` | `0x9942D` | – | – | 0 | no-op (empty handler) |
| 27 | `regRequireItem` | `0x99CDD` | int, int, int | – | 237 | prepared quest needs item (kind, id, n) |
| 28 | `regRequireMob` | `0x99C61` | int, int | – | 33 | prepared quest needs kills (mob, n) |
| 29 | `regRequireMeso` | `0x99C2D` | int | – | 53 | prepared quest needs meso (0x7CF41) |
| 30 | `regRewardItem` | `0x99B71` | int, int, int | – | 156 | prepared quest gives item (kind, id, n) |
| 31 | `regRewardMeso` | `0x99B3D` | int | – | 104 | prepared quest gives meso |
| 32 | `regRewardExp` | `0x99B09` | int | – | 125 | prepared quest gives EXP |
| 33 | `loseRequire` | – | – | – | 0 | no-op (no dispatcher case) |
| 34 | `getReward` | `0x9B099` | int, int | int | 103 | complete (kind, id): take needs, give rewards; 1/0 |
| 35 | `getEvtIndex` | `0x9B03D` | – | int | 74 | msg: answer (modes 1/3: 1 = first button; 2: value) |
| 36 | `regMsgRequire` | `0x99A75` | int, int | – | 0 | msg: show the needs of quest (kind, id) |
| 37 | `regMsgReward` | `0x999F9` | int, int | – | 75 | msg: show the rewards of quest (kind, id) |
| 38 | `changeMap` | `0x99969` | int, int, int | – | 173 | warp to (area, map, portal): world+0x98/+0x9C/+0xA0 |
| 39 | `showMarket` | `0x998D1` | int | – | 14 | open shop arg (0x4628D) |
| 40 | `getMainQuest` | `0x9A975` | – | int | 0 | current main-quest id (quests+0x198), made current |
| 41 | `setCurrent` | `0x99699` | int, int | – | 0 | quests: make (kind, id) the current quest |
| 42 | `setNpcHide` | `0x9985D` | int | – | 14 | current NPC object +6 = arg (hidden flag) |
| 43 | `regRequireQuest` | `0x99BF9` | int | – | 0 | same call as regRequireMeso (0x7CF41) |
| 44 | `getItem` | `0x9AADD` | int, int, int | int | 4 | give item (kind, id, n) if it fits: 1/0 |
| 45 | `getMeso` | `0x997A9` | int | – | 0 | hero meso += arg, with a notice |
| 46 | `getEquipState` | `0x9A9D1` | int, int | int | 0 | 1 if equipment slot arg1 holds item id arg2 |
| 47 | `showPortalErrMsg` | `0x99765` | – | – | 20 | popup 0x11 (portal scripts call it when a condition fails) |
| 48 | `setNPC` | `0x99731` | int | – | 3 | msg: speaker portrait = NPC arg |
| 49 | `startBoss` | `0x9971D` | – | – | 5 | world+0x1E4 = boss scene of the area (world+0x84) |
| 50 | `setBoss` | `0x99705` | – | – | 5 | msg: speaker sprite from the map's mobs (probably the boss) |
| 51 | `updateNPCState` | `0x99435` | – | – | 102 | msg+0x2D = 1: re-run NPC state scripts |
| 52 | `clearAll` | `0x996ED` | – | – | 3 | end the chapter: reset quest states, chapter += 1 |
| 53 | `getHeroClass` | `0x9AF29` | – | int | 10 | hero job (hero+0x88) |
| 54 | `getClassUpCount` | `0x9AED5` | – | int | 38 | hero job advancements (hero+0x8C) |
| 55 | `setNpcDir` | `0x99909` | int | – | 5 | current NPC object +8 = (arg ? 2 : 0) (facing) |
| 56 | `checkItemCount` | `0x9AF7D` | int, int | int | 3 | count of item (kind, id) in the inventory |
| 57 | `getQuestTitle` | `0x9A795` | int, int | string | 1 | title key of quest (kind, id) |
| 58 | `removeQuest` | `0x995F1` | int, int | – | 10 | drop quest (kind, id), re-run NPC states |
| 59 | `makeEquipItem` | `0x99575` | int, int | – | 3 | quests 0x7C2D9 (probably crafting, ManufactureItem_*) |
| 60 | `invenFullNotify` | `0x9966D` | – | – | 21 | popup 8 (inventory full) |
| 61 | `isInvenAvailable` | `0x9A691` | int, int | int | 32 | 1 if inventory kind has >= n free slots |
| 62 | `initMonCa` | `0x99545` | int | – | 3 | start Monster Carnival arg (0x82271) |
| 63 | `runAnimation` | `0x9950D` | int | – | 3 | world+0x1E4 = arg, play that scene (0x80935) |
| 64 | `removeItem` | `0x99485` | int, int, int | – | 3 | take item (kind, id, n) (0x63CC1) |

Notes:

- **Warps:** `changeMap(area, map, portal)` is the only script path to the warp target. Its handler calls `CMapleWorld::changeMap` (`0x81369`), which, unless a warp is already pending (`world+0xB4`), calls the sound player with 9, stores `world+0x98` = area, `+0x9C` = map, `+0xA0` = portal number, sets `+0xA4`/`+0xA8` = −1 (no explicit position), raises `+0xB4`, and reports the hero's total-ranking score to Game Center. The map change then puts the hero on that portal ([obj.md](../formats/obj.md#portals-and-arrival-points)). 158 of the 173 calls are in mains (portal scripts).
- **Dialogue:** `regMsgNPC`/`regMsgHero` localise their key immediately (`LocalizedCString(key, "Scripts")`). `regSelList` stores the key raw, and the box localises it when drawing. See [script-out.md → Dialogue keys](../formats/script-out.md#dialogue-keys).
- `regRequireQuest` calls the same quest-manager function as `regRequireMeso` (`0x7CF41`), so it would add a meso requirement. No script calls it.
- `getQuestTitle` is the only native that returns a string. Its one call feeds `regMsgNPC`.

## For the port

The VM is ordinary engine code, so it is recompiled with the rest of the binary and needs no special handling. To replace it with hand-written C++ later ([ARCHITECTURE.md → Beyond v1](../ARCHITECTURE.md)), a host VM needs only the 24 opcodes that occur, integer numbers, the per-load global reset, the four entry points above, and the 53 handlers as the native API. `mtport.scriptout.decode` gives a checked, decoded form of every shipped script to test that VM against.
