# `.out`: compiled scripts (Lua 2.4 bytecode)

**Status: fully specified.** All 307 shipped files load with zero errors and no trailing bytes. Every byte of code decodes to a known opcode, every jump lands on an instruction, and every threaded operand chain is consumed exactly. Issue [#19](https://github.com/armaatus/maplethief-port/issues/19). The VM that runs these files, the native functions they call and the entry points are in [docs/re/script-vm.md](../re/script-vm.md).

A `.out` file is **Lua 2.4 bytecode as `luac` 2.4 writes it, with the chunk header removed**. Stock luac 2.4 writes `ESC "Lua" 0x23 <test word> <test float>` before the first function. These files have only the `ESC`, and `'F'` follows it directly. The game's loader skips the header check to match. The code inside is unmodified Lua 2.4: the same 67 opcodes in the same order, the same operand encodings and the same threaded global and string operands. Numbers, however, are integers in this VM (see [Numbers](#numbers)). Dialogue text is not in the files. Scripts pass `Scripts.strings` keys to the message natives (see [Dialogue keys](#dialogue-keys)).

## Where the game loads it

Addresses are armv7 VM addresses, and function pointers carry the Thumb bit. The names come from `work/re/names/scriptvm.json` (`python3 -m mtport.scriptout names`).

| Address | What |
|---|---|
| `0x63191`, `0x839BD`, `0x7458D`, `0x87501` | the four places that run a script: `"/script/chapter%d/%s.out"` with the current chapter, then `"/script/common/%s.out"` if that file is missing ([script-vm.md](../re/script-vm.md#entry-points)) |
| `0x9D0A9` `Lua::loadBuffer` | resets the VM, checks the first byte is `0x1B`, and undumps the file |
| `0x9F795` `Undump::undump` | `ESC`, then `loadChunk`. Returns main |
| `0x9F735` `Undump::loadChunk` | reads `'F'` records while bytes remain and the next byte is `'F'` |
| `0x9F359` `Undump::loadFunction` | one `F` record and the `V`/`S` records after it |
| `0x9F2C9` `Undump::loadString` | `u16 size` + `size` bytes. Keeps `size − 1` bytes (drops the NUL) |
| `0x9ED59` `TFunc::unthread` | walks one operand chain and writes the symbol index into each link |
| `0x9ED09` `FunctionSlots::add` | gives a non-main function the next slot number (0, 1, …) |
| `0x98F95`, `0x98EE1` | `DataReader::u16` / `::u8`: little-endian, no bounds checks |

The loader never checks a length against the buffer, and it doesn't validate opcodes or chains. A malformed file corrupts memory. The parser below rejects every such case.

## Files

- 307 files in `script/`: `chapter1/` 27, `chapter2/` 26, `chapter3/` 29, `common/` 225. A chapter folder overrides `common/` for the same name.
- Sizes run from 20 bytes (`common/Darklord.out` and others: a main that only returns) to 15,693 bytes (`chapter3/ZenumistChief.out`). The total is 297,511 bytes.
- 1,086 functions: 307 mains and 779 others. 193 files have only a main (portals, map scripts, shops). The most functions in one file is 25.
- 221 of the 307 base names are named by a `.obj` record. The other 10 (`Barun_old`, `Darklord_0827`, `Darklord_new`, `Darklord_ori`, `Manzie_error`, `Spiegelman1`–`3`, `YetiMap`, `pig_old`) are never run.
- Endianness: little-endian. Strings are UTF-8, except Korean text left in a few unused scripts, which is EUC-KR. The game's C-string→`NSString` helper `0x6DD1` makes the same fallback.

## Layout

```
file      = 0x1B  function+                       // main first, then the others
function  = 'F'  u16 size_hi  u16 size_lo  u16 line
            ( line == 0 ? string source : u16 marked )
            u8 code[size_hi << 16 | size_lo]
            ref*
ref       = ('V' | 'S')  u16 head  string name     // a global / a string constant
string    = u16 len  u8 bytes[len]                // bytes[len - 1] == 0
```

| Field | Type | Notes |
|---|---|---|
| `0x1B` | u8 | `ID_CHUNK`. The game refuses a buffer whose first byte isn't this. |
| `'F'` (`0x46`) | u8 | `ID_FUN`. The loader stops at the first byte that isn't `'F'`. All files end exactly after the last record. |
| `size_hi`, `size_lo` | u16, u16 | code size, as Lua 2.4's `LoadSize`. `size_hi` is 0 in every file. |
| `line` | u16 | `lineDefined`. **0 marks main.** The others hold the source line of `function` (1–600). |
| `source` | string | main only. Always `"Temp.dat"`: the compiler's input file name, unused. |
| `marked` | u16 | others only: the offset, **in main's code**, of the operand of the `PUSHFUNCTION` that creates this function |
| `code` | bytes | the function's bytecode (see [Opcodes](#opcodes)). There is no inline data. |
| `head` | u16 | offset in this function's code of the first operand that names the symbol |
| `name` | string | the global's name (`V`) or the constant's text (`S`) |

`V`/`S` records follow each function until the next byte is neither (`'F'` or the end of the file). The files contain 6,933 `V` and 1,968 `S` records.

### Threaded operands

Every global operand (`PUSHGLOBAL`, `STOREGLOBAL`) and every string-constant operand (`PUSHSTRING`, `PUSHSELF`, each word of `STORERECORD`) holds a 16-bit placeholder. The uses of one symbol within one function form a chain. The `V`/`S` record's `head` is the offset of the first use. The u16 stored there is the offset of the next use, and so on until a link of 0 ends the chain (offset 0 is always an opcode, so it can't be an operand). The loader looks up or creates the symbol (`Table::findSymbolByName` `0x9E401`, `Table::findConstantByName` `0x9E3DD`), then `TFunc::unthread` overwrites every link with its index. This is Lua 2.4's `Unthread`.

Each chain in the shipped files lands only on operands of its own kind, and every global or string operand is on exactly one chain (`mtport.scriptout.decode` checks both). The disassembler therefore resolves operands to names straight from the chains. Symbol indexes depend on what is already loaded, so they aren't part of the file.

### `PUSHFUNCTION` and function slots

`PUSHFUNCTION` has a 4-byte operand. In the file it is a stale pointer from the compiler's memory (`f0 89 94 04` = `0x049489F0` in `Manzie.out`). For each non-main function, in file order, the loader takes the next slot number `n` (0, 1, …) from `FunctionSlots::add` and writes it into `main.code[marked]`, then zeroes the 3 bytes after it. At run time `PUSHFUNCTION` reads the u32 and pushes `FunctionSlots::get(n)`. The slot table has 30 entries (`0x78` bytes, allocated in `Lua::reset` `0x9CE81`) and nothing checks the count. The shipped maximum is 25. `marked` always points into **main**: Lua 2.4 only allows `function` statements at top level. In every file, every `PUSHFUNCTION` is in main, is claimed by exactly one function, and is followed by the `STOREGLOBAL` of the function's name. The disassembler names functions that way.

Slot 0 matters: the NPC-state pass runs the function in slot 0 without running main (see [script-vm.md](../re/script-vm.md#entry-points)). In all 114 files that define functions, slot 0 is `chkQuest`.

## Opcodes

One opcode byte, then its operands with no padding: `b` = u8, `w` = u16, `f` = 4-byte float, `p` = 4-byte function slot, `g` = u16 global (threaded `V`), `s` = u16 string constant (threaded `S`). Jumps are relative to the end of the instruction: forward `pc += w` (`ONTJMP`, `ONFJMP`, `JMP`, `IFFJMP`), backward `pc -= w` (`UPJMP`, `IFFUPJMP`). *Stack* uses Lua 2.4's notation: `before → after`, with the top on the right. *Uses* counts instructions in all 307 files. *Game* gives where the interpreter `Lua::execute` (`0x9BC11`, a `tbh` over 0x00–0x42) departs from Lua 2.4's `lua_execute`. A byte above `0x42` falls through the switch and is skipped as a 1-byte no-op.

| Code | Name | Operands | Stack | Uses | Game |
|---|---|---|---|---|---|
| `00` | PUSHNIL | | `→ nil` | 0 | |
| `01` | PUSH0 | | `→ 0` | 1,752 | |
| `02` | PUSH1 | | `→ 1` | 3,147 | |
| `03` | PUSH2 | | `→ 2` | 950 | |
| `04` | PUSHBYTE | `b` | `→ b` | 2,676 | |
| `05` | PUSHWORD | `w` | `→ w` | 253 | |
| `06` | PUSHFLOAT | `f` | `→ f` | 0 | **no-op that doesn't skip its 4 operand bytes** (no floats) |
| `07` | PUSHSTRING | `s` | `→ STR[s]` | 1,974 | |
| `08` | PUSHFUNCTION | `p` | `→ FUN[p]` | 779 | slot number, not a pointer (above) |
| `09`–`12` | PUSHLOCAL0–9 | | `→ LOC[n]` | 0 | |
| `13` | PUSHLOCAL | `b` | `→ LOC[b]` | 0 | |
| `14` | PUSHGLOBAL | `g` | `→ VAR[g]` | 9,001 | |
| `15` | PUSHINDEXED | | `t i → t[i]` | 0 | arrays only, `i` indexes a C array of slot pointers |
| `16` | PUSHSELF | `s` | `t → t t[STR[s]]` | 0 | **no-op that doesn't skip its 2 operand bytes** |
| `17`–`20` | STORELOCAL0–9 | | `x →` (LOC[n] = x) | 0 | **stores to local (last operand − 23), not local n** (`0x9BFE2` reads the operand variable instead of the opcode) |
| `21` | STORELOCAL | `b` | `x →` (LOC[b] = x) | 0 | |
| `22` | STOREGLOBAL | `g` | `x →` (VAR[g] = x) | 2,877 | |
| `23` | STOREINDEXED0 | | `t i v →` (t[i] = v) | 0 | arrays only |
| `24` | STOREINDEXED | `b` | `t i a_b…a_1 v → t i a_b…a_1` | 0 | arrays only |
| `25` | STORELIST0 | `b` | `t v_1…v_b → t` (t[i] = v_i) | 0 | |
| `26` | STORELIST | `b m, b n` | `t v_1…v_n → t` (t[i + 40·m] = v_i) | 0 | |
| `27` | STORERECORD | `b n`, `n × s` | `t v_1…v_n → t` (t[STR[s_i]] = v_i) | 0 | uses the string object's address as an array index |
| `28` | ADJUST0 | | top = base | 779 | |
| `29` | ADJUST | `b` | top = base + b | 0 | |
| `2A` | CREATEARRAY | `w` | `→ {}` | 0 | `new TObject*[w + 1]`, uninitialised, not a hash table |
| `2B` | EQOP | | `x y → x == y` | 2,039 | 1 or nil, by `Lua::equalObjects` `0x9916D` |
| `2C` | LTOP | | `x y → x < y` | 82 | numbers, or strings by `strcmp` |
| `2D` | LEOP | | `x y → x <= y` | 19 | |
| `2E` | GTOP | | `x y → x > y` | 1 | |
| `2F` | GEOP | | `x y → x >= y` | 89 | |
| `30` | ADDOP | | `x y → x + y` | 122 | integer |
| `31` | SUBOP | | `x y → x − y` | 0 | integer |
| `32` | MULTOP | | `x y → x · y` | 0 | integer |
| `33` | DIVOP | | `x y → x / y` | 0 | integer (`__divsi3`) |
| `34` | POWOP | | `x y → x ^ y` | 0 | no-op: both operands stay |
| `35` | CONCOP | | `x y → x .. y` | 0 | `sprintf("%s%s")` into a 100-byte stack buffer |
| `36` | MINUSOP | | `x → −x` | 290 | |
| `37` | NOTOP | | `x → not x` | 0 | |
| `38` | ONTJMP | `w` | `x → x` | 260 | jump if x ≠ nil (the compiler emits a `POP` after it) |
| `39` | ONFJMP | `w` | `x → x` | 229 | jump if x = nil |
| `3A` | JMP | `w` | | 1,099 | |
| `3B` | UPJMP | `w` | | 0 | |
| `3C` | IFFJMP | `w` | `x →` | 1,741 | jump if x = nil |
| `3D` | IFFUPJMP | `w` | `x →` | 0 | |
| `3E` | POP | | `x →` | 489 | |
| `3F` | CALLFUNC | `b nargs, b nresults` | `f a_1…a_n → r_1…r_m` | 6,368 | `nresults` 255 = all. Shipped: 0 (5,281) or 1 (1,087) |
| `40` | RETCODE0 | | returns base…top | 1,086 | |
| `41` | RETCODE | `b` | returns base + b…top | 0 | |
| `42` | SETLINE | `w` | | 0 | debug line. The files were compiled without debug info. |

Only the 24 opcodes with a non-zero *Uses* occur. The scripts have **no local variables, tables, loops, arithmetic other than `+` and unary `−`, or string concatenation**. Every value is a global, and every block is `if`/`elseif`/`else` (`IFFJMP` + `JMP`) or `and`/`or` (`ONTJMP`/`ONFJMP` + `POP`). None of the opcodes the game gets wrong (`PUSHFLOAT`, `PUSHSELF`, `STORELOCALn`, `POWOP`) appears. A runtime only needs the 24 that occur to run the shipped content (it should still reject the rest).

### Numbers

The VM keeps numbers as 32-bit integers in the value word of an 8-byte `{tag, value}` slot. Lua 2.4 uses a `float`. `PUSH0/1/2`, `PUSHBYTE` and `PUSHWORD` push integers, `ADDOP`–`DIVOP` are integer operations, and the string↔number coercions use `atol` and `"%d"`. Tags are Lua 2.4's: nil −1, number −2, string −3, array −4, function −5, C function −6, mark −7, C mark −8, line −9.

## Dialogue keys

Scripts never hold dialogue. They push a key and call `regMsgNPC(key)`, `regMsgHero(key, n)` or `regSelList(key, value)`, or pass keys to `setQuest`. Each message native looks the key up in `<lang>.lproj/Scripts.strings` with `[NSBundle localizedStringForKey:value:table:@"Scripts"]` (`LocalizedCString` `0x6DD1`).

- `Scripts.strings` is an Apple **binary property list** (`bplist00`), a dictionary of 2,219 string → string entries (`en.lproj`). `ko.lproj` has 2,219 too: it lacks `Chapter3_Quest_Zenumist_Chief.dialog.1` and has one key `en` lacks. `ScriptExtra.strings` (527 entries) holds text for the C++ code (`CHero.setMeso.1`, …), keyed `<Class>.<Method>.<n>`. Scripts never use it.
- Keys are `<Prefix>.<function>.<n>`. The prefix is `<Chapter>_<Kind>_<Name>` (`Chapter1_Quest_Manji`, `BackChapter1_SubQuest_Alex`, `Chapter2_MonsterCarnival_Spiegelman`) or a few others (`Common_Dialog_Pig`, `Copy_…`, `ManufactureItem_Chapter2`, `Tutorial…`). `<function>` is usually the script function that uses the key, and `<n>` counts from 1. The prefix names the NPC, not the file: `Manzie.out` uses `Chapter1_Quest_Manji.*`.
- The 307 files contain **1,761 distinct key-shaped string constants, and all of them are in `en.lproj/Scripts.strings`**. 458 table entries are never referenced (for example most `BackChapter1_*` and several `ManufactureItem_*` lists).
- **Exceptions:** 127 `PUSHSTRING`s in 8 files are literal text rather than keys. `NSLocalizedString` returns an unknown key unchanged, so the game would show these as written. They are all Korean in the 7 unused files (`chapter3/Darklord_new.out` 38, `common/Spiegelman1`–`3.out` 25 each, `common/pig_old.out` 11, `common/Barun_old.out` 1, `common/Manzie_error.out` 1). The one in a shipped script is `chapter1/Sabi2.out`'s `regMsgNPC("...")`, a deliberate ellipsis line.

## Worked example

`chapter1/Manzie.out` (336 bytes, 3 functions). The file starts with main's record:

```
00000000: 1b46 0000 1700 0000 0900 5465 6d70 2e64  .F........Temp.d
00000010: 6174 0008 f089 9404 2200 0008 5881 9404  at......"...X...
00000020: 2200 0014 0e00 3f00 0040 5606 0009 0063  ".....?..@V....c
00000030: 686b 5175 6573 7400 5611 0007 0064 6961  hkQuest.V....dia
00000040: 6c6f 6700 4600 0035 0005 0001 0028 0236  log.F..5.....(.6
```

`1B` · `46` `F` · size `0000:0017` = 23 · line `0000` = main · source `0900` `"Temp.dat\0"` · 23 code bytes `08 f0899404 | 22 0000 | 08 58819404 | 22 0000 | 14 0e00 | 3f 00 00 | 40` · `V` head `0x0006` `"chkQuest"` · `V` head `0x0011` `"dialog"` (its chain: `0x11` → `0x0E` → 0, which is the `PUSHGLOBAL` at `0x10` and the `STOREGLOBAL` at `0x0D`). Next comes `F` with size 0x35, line 5 and `marked` `0x0001` (slot 0, `chkQuest`). The file ends with `S` head `0x000B` `"Chapter1_Quest_Manji.dialog.1"` after the third function. `python3 -m mtport.scriptout dis` prints:

```
function main  ; source "Temp.dat", 23 bytes
    0000  08 f0 89 94 04 PUSHFUNCTION  f0 (chkQuest)
    0005  22 00 00       STOREGLOBAL   chkQuest
    0008  08 58 81 94 04 PUSHFUNCTION  f1 (dialog)
    000D  22 00 00       STOREGLOBAL   dialog
    0010  14 0e 00       PUSHGLOBAL    dialog
    0013  3f 00 00       CALLFUNC      0 0  ; dialog()
    0016  40             RETCODE0

function f0 chkQuest  ; line 5, 53 bytes
    0000  28             ADJUST0
    0001  02             PUSH1
    0002  36             MINUSOP
    0003  22 00 00       STOREGLOBAL   notify
    ...
    0010  3f 02 01       CALLFUNC      2 1  ; getQuestState(0, 12) [native]
    ...
function f1 dialog  ; line 20, 24 bytes
    0007  14 00 00       PUSHGLOBAL    regMsgNPC
    000A  07 00 00       PUSHSTRING    "Chapter1_Quest_Manji.dialog.1"  ; "I feel uneasy. Where could Master be?"
    000D  3f 01 00       CALLFUNC      1 0  ; regMsgNPC("Chapter1_Quest_Manji.dialog.1") [native]
```

The raw bytes column shows the file's bytes, so threaded operands show their chain links (`14 0e 00`).

## Parser

[`tools/mtport/scriptout.py`](../../tools/mtport/scriptout.py): `decode` / `load` (bounds-checked, strict), `decode_code` (linear sweep), `calls` (call sites by symbolic stack), `listing`, `load_strings`, the opcode table `OPCODES` and the native catalogue `NATIVES`.

```sh
python3 -m mtport.scriptout dis work/ipa/Payload/MapleThief.app/script/chapter1/Manzie.out \
    --strings work/ipa/Payload/MapleThief.app/en.lproj/Scripts.strings
python3 -m mtport.scriptout stats work/ipa/Payload/MapleThief.app/script --strings …/Scripts.strings
```

Tests: `tests/test_scriptout.py` (synthetic chunks built by a small assembler in the test, covering every opcode, the operand chains, jumps, slots and malformed input) and `tests/test_scriptout_real.py` (all 307 shipped files: zero unknown opcodes, every byte accounted for, every native call catalogued with its argument count, all 1,761 keys resolved, the inline-text exceptions above). The real-asset tests skip when `work/ipa/` is absent.

## Validation

307 / 307 files load. Per function, the decoded instructions cover the code exactly, and so do the chains. 38,102 instructions, 0 unknown opcodes. Checked locally against `MapleThief-1.5.ipa` on 2026-10-24.
