# Binary & bundle analysis

What is known about `MapleThief.app` (version 1.5) **before** any deep reverse engineering. This is a first-pass inventory, not an RE log. The detailed findings go into `docs/re/` and `docs/formats/` as the M1 issues land.

All numbers below were re-verified on macOS from `original/MapleThief-1.5.ipa` (2026-10-23) with `file`, `lipo`, `otool -l/-L/-oV`, `nm -u`, `strings`, `llvm-objdump` and small Python scripts. Corrections to the first notes are listed in [Corrections](#corrections-to-the-first-inspection-notes).

To reproduce: `scripts/bootstrap` extracts to `work/ipa/` and splits the slices into `work/slices/MapleThief.armv6` / `work/slices/MapleThief.armv7` (byte-identical to `lipo -thin`, no Xcode needed).

---

## App bundle

From `Info.plist`:

| Key | Value |
|---|---|
| `CFBundleIdentifier` | `com.nexonmobile.maplestory.thief` |
| `CFBundleVersion` | `1.5` (no `CFBundleShortVersionString`) |
| `CFBundleExecutable` | `MapleThief` |
| `MinimumOSVersion` | `3.1.2` (`LC_VERSION_MIN_IPHONEOS` says 3.1) |
| `DTXcode` / `DTXcodeBuild` | `0420` / `4C199` (Xcode 4.2) |
| `DTSDKName` | `iphoneos5.0` (9A334) |
| `DTCompiler` | `com.apple.compilers.llvmgcc42` (llvm-gcc 4.2) |
| `BuildMachineOSBuild` | `10K549` (Mac OS X 10.6.8) |
| `UIDeviceFamily` | `[1]`, iPhone/iPod only |
| `UIInterfaceOrientation` | `UIInterfaceOrientationLandscapeLeft` |
| `UIStatusBarHidden` | `true` |
| `UIApplicationExitsOnSuspend` | `false` (it supports backgrounding, iOS 4+) |
| `NSMainNibFile` | `MainWindow` (compiled `NIBArchive` format) |
| `CFBundleIconFiles` | `Icon.png`, `Icon@2x.png`, `Icon-72.png`, `Icon-Small(-50,@2x).png` |

The bundle also contains `_CodeSignature/`, `CodeResources` (byte-identical to `_CodeSignature/CodeResources`; the IPA stores it as a regular file, not as the usual symlink), `ResourceRules.plist`, `PkgInfo`, an empty 0-byte `MapleThief_armv6` (a Clutch artifact, ignore it) and a stray Chartboost SDK `README.md` ("Version 3.0.4"). `iTunesArtwork` sits next to `Payload/` in the IPA.

## Binary

`Payload/MapleThief.app/MapleThief`: universal (fat) Mach-O, 2,745,584 bytes.

| Slice | Fat offset | Size | `__text` | Encryption |
|---|---|---|---|---|
| armv6 | 4,096 | 1,543,296 | 0x12AE5C (1,224,284 B) at 0x2AC0, **ARM mode only** | `cryptid 0`, cryptsize 1,323,008 |
| armv7 | 1,548,288 | 1,197,296 | 0xD713C (880,956 B) at 0x27E0, **Thumb-2** | `cryptid 0`, cryptoff 4,096, cryptsize 978,944 |

Both fat entries use alignment 2^12, cputype 12 (ARM) and cpusubtype 6 (armv6) / 9 (armv7). Slice SHA-256 (as split by `scripts/bootstrap`, identical to `lipo -thin`): armv6 `3a24f0c0592dcd8d6cdc59c4330a8b3c54f1a4d328b6479911d9b73063ed5118`, armv7 `60d4d5e75de5eb478ed320377babca8eab6616e9a94faad805f71cf8e084e467`.

Both slices are decrypted. **The armv7 slice is the primary target.** The armv6 slice is a full ARM-mode build of the same program with the same function count (see below). That makes it a cross-check oracle, and possibly an easier lifting target, since it has no Thumb-2 and no IT blocks. [#30](https://github.com/armaatus/maplethief-port/issues/30) has to weigh this.

armv7 segment layout (vmaddr):

| Segment | vmaddr | vmsize | Notable sections |
|---|---|---|---|
| `__PAGEZERO` | 0x0 | 0x1000 | |
| `__TEXT` | 0x1000 | 0xF0000 | `__text`, `__stub_helper` (0xA50), `__symbolstub1` (0x364 = 217 × 4-byte ARM stubs), `__objc_methname/classname/methtype`, `__cstring` (0x7219), `__ustring` (0x5DA8, UTF-16 literals), `__gcc_except_tab` (0x1100), `__const` |
| `__DATA` | 0xF1000 | 0x2A000 | `__lazy_symbol` (217 ptrs), `__nl_symbol_ptr` (82 ptrs), `__mod_init_func` (**2 static constructors**), `__objc_*` (classlist 75 entries, catlist 4, protolist 19), `__data` (0x1B7F4), `__cfstring` (0x16F0 ≈ 367 CFStrings), `__const`, `__common`, `__bss` |
| `__LINKEDIT` | 0x11B000 | 0xB000 | |

Load commands of note: `LC_DYLD_INFO_ONLY` (compressed binds/rebases), `LC_FUNCTION_STARTS`, `LC_UNIXTHREAD` (entry `pc = 0x27E0`, classic crt1 `start` in ARM mode, not `LC_MAIN`), `LC_ENCRYPTION_INFO`, `LC_CODE_SIGNATURE`, 15 `LC_LOAD_DYLIB` + 2 `LC_LOAD_WEAK_DYLIB` (StoreKit, GameKit).

**Start-up ([#43](https://github.com/armaatus/maplethief-port/issues/43), [ADR 0027](adr/0027-process-start-up.md)).** `start` is crt1's 11-instruction ARM stub: it reads `argc` at `[sp]`, computes `argv`, `envp` and `apple`, aligns `sp` to 8, does `blx 0x280C` (`main`, Thumb) and `b exit`. It never writes `NXArgc`/`NXArgv`/`environ`/`__progname`; dyld does, through `__DATA,__program_vars` (`0xF1370`) = `{0x1000 (Mach header), 0x11A400, 0x11A404, 0x11A408, 0x11A40C}` (all in `__common`). Before `UIApplicationMain` the guest calls only `operator new` (51 times), `operator new[]`, `strcpy` (50), `__cxa_atexit` (2: `0x6639`, `0x9DBB1`) and `objc_msgSend` (`+[NSAutoreleasePool alloc]`, `-init`); `UIApplicationMain(1, argv, nil, nil)` is called from `0x2844` (return address `0x2849`) in `main`. The runtime boots the recompiled program to that point (`runtime.boot_real`).

**Fixups ([#31](https://github.com/armaatus/maplethief-port/issues/31)).** The armv7 executable is **not position-independent**: its header flags are 0x10085 (`MH_NOUNDEFS | MH_DYLDLINK | MH_TWOLEVEL | MH_BINDS_TO_WEAK`, no `MH_PIE`) and its rebase stream is empty (`rebase_size 0`), so dyld always loads it at its link-time addresses, the slide 0 that the recompiler assumes ([ADR 0014](adr/0014-recompiler-design.md)). It has **1,272 binds**:

- **1,055 plain binds.** By section: 487 in `__objc_data` (150 classes' `__objc_empty_cache` and `__objc_empty_vtable`, framework superclasses and metaclass isa words), 367 `__cfstring` isa words, 72 in `__const` and 27 in `__data` (the libstdc++ typeinfo vtables with addend 8, `___cxa_pure_virtual` vtable slots, two `__NSConcreteGlobalBlock` isa words, a `malloc`/`realloc`/`free` table), 66 `__objc_classrefs`, 28 `__nl_symbol_ptr`, 4 `__objc_const` and 4 `__lazy_symbol`.
- **213 lazy binds**, one per `__lazy_symbol` slot except those 4.
- **4 weak binds**, the C++ weak definitions `operator new`/`delete` and their `[]` forms (`__Znwm`, `__ZdlPv`, `__Znam`, `__ZdaPv`), whose lazy slots are bound eagerly instead.

`python -m recomp load` writes them, the 333 imports with their stub and pointer addresses, the 2 init functions and the 15 ObjC section ranges to `work/recomp/loader.json`. `objdump --macho --bind/--lazy-bind/--weak-bind/--rebase` and `dyld_info -opcodes` agree with it on every entry. `dyld_info -fixups` (macOS 26.6) agrees on every address and kind, but **names the wrong target for 933 of the 1,268 bind and lazy-bind entries**, often the next target of its table (`_AudioServicesPlaySystemSound` for `__objc_empty_vtable`, `__si_class_type_info` for `__class_type_info`). The data structures confirm the loader: for example, the typeinfo at 0x1195F0 has the two words of a `__class_type_info`. Don't trust that column for this binary.

Linked libraries: SystemConfiguration, Foundation, UIKit, OpenGLES, QuartzCore, CoreGraphics, AudioToolbox, AVFoundation, MediaPlayer, OpenAL, StoreKit (weak), GameKit (weak), `libstdc++.6`, `libgcc_s.1`, `libSystem.B`, CoreFoundation, `libobjc.A`.

### Functions

`LC_FUNCTION_STARTS` is present, so the function count is exact rather than estimated:

- **armv7: 2,655 functions.** 2,654 are Thumb (start address LSB set) and **1 is ARM**, the `start` entry stub at 0x27E0. The last function starts at 0xD98D0.
- **armv6: 2,655 functions**, all ARM.

The symbol table is stripped: its only defined symbol is `__mh_execute_header`. The `LC_DYLD_INFO` **export trie is not** ([#10](https://github.com/armaatus/maplethief-port/issues/10), `MachO.exports`): it lists 143 symbols, namely 117 `_OBJC_CLASS_$_`/`_OBJC_METACLASS_$_`/`_OBJC_IVAR_$_` symbols, crt1's `start`, `_NXArgc`, `_NXArgv`, `_environ` and `___progname`, `__mh_execute_header`, 16 `_bson_*` functions and variables of the MongoDB C driver's `bson.c` that Chartboost bundles, and the Chartboost helpers `_cb_bson_buffer_append_objc_object_recursively`, `_CBNativeMoreAppsCellDrawGradient`/`Borders` and `_initialBufferSize`. Names come from ObjC metadata (≈563 app methods + ≈486 Chartboost methods, about 1,050 of the 2,655 functions), RTTI/vtables and string xrefs.

#### Library code ([#10](https://github.com/armaatus/maplethief-port/issues/10))

`python3 -m mtport.libtags generate` tags every function as `app-objc`, `engine` or `lib:<name>` from ObjC metadata, the export trie, signature scans, vtables and the call/reference graph; counts, bytes and the evidence are in [docs/re/libraries.md](re/libraries.md). In short: **1,353 engine functions (626 KiB, 73% of `__text`)**, 620 `app-objc` (142 KiB), 675 Chartboost (91 KiB, one block from 0xC2D60 to the end), and three small pieces of other third-party code: crt1's `start`, `UIDevice(IdentifierAddition)`/`NSString(MD5Addition)` (4 functions, used by the game, not by Chartboost) and Apple's `MyOpenALSupport.h` (2 functions). **There is no zlib, libpng, stock Lua or STL code**: the script VM is the game's own C++ rewrite of the Lua 2.4 interpreter ([Script VM](#script-vm)). Every function has a tag (0 `unknown`).

The game's code is **compiled without optimization** (`-O0` style: every value goes through the stack, constants are built as `movs` + `movt #0`), while Chartboost is optimized. A recompiler sees a lot of redundant loads and stores but very regular code.

#### Ghidra cross-check ([#7](https://github.com/armaatus/maplethief-port/issues/7))

Ghidra 12.2 (`ARM:LE:32:v7`), seeded with `LC_FUNCTION_STARTS` and `TMode` before auto-analysis ([ADR 0007](adr/0007-re-tooling-ghidra.md), [tools/ghidra/README.md](../tools/ghidra/README.md)), finds **2,876 functions**:

- All **2,655** starts become functions in the right mode: 0 missing, 0 wrong mode.
- There are **221 extra functions**: the 217 ARM stubs in `__symbolstub1`, one external thunk (`_realloc`), and **3 false positives in `__text`**: 0xAF3BC (24 B), 0xBF680 (20 B) and 0xBF710 (28 B). Each is a table of 32-bit words (e.g. `0x4B586, 0x4B594, …`, likely PC-relative offsets) that the preceding function reads with a PC-relative `ldr` (from 0xAF3AC, 0xBF66E and 0xBF700). Ghidra decodes the tables as Thumb code. Nothing calls them, and no pointer to them exists in the binary. So `LC_FUNCTION_STARTS` is complete, and in-`__text` functions outside it should be treated as data. Function discovery ([#32](https://github.com/armaatus/maplethief-port/issues/32)) confirms it: all three lie in the literal pools of the function before them (0xAF359, 0xBF561, 0xBF695).
- The export has 87,654 xrefs. Its 3,457 strings are 3,089 C strings in the four `S_CSTRING_LITERALS` sections, 367 CFStrings (366 8-bit, 1 UTF-16) and 1 UTF-16 literal in `__ustring`: the Korean Nexon terms of service, 11,987 UTF-16 units at 0xEAEE8.
- Ghidra's ARM P-code can drop IT conditions. In the block checked for ADR 0014, `itttt gt` at 0xC95EE (`-[CBUnderfinedProgressBar step:]`, four VFP/NEON instructions), only the first instruction is conditional: `vmulgt.f32`, `vaddgt.f32` and `vstrgt s0, [r0]` at 0xC95F4–0xC95FC come out unconditional. NEON arithmetic is an opaque `CALLOTHER`. Read Ghidra's decompilation of such blocks with care; it is one reason the recompiler does not lift from P-code ([ADR 0014](adr/0014-recompiler-design.md)).
- Ghidra's *Objective-C Message Analyzer* throws during parallel decompilation of this binary (`ArrayIndexOutOfBoundsException` in `findMessages`), at a different point on each run. The pipeline turns it off so the exports stay reproducible.
- Ghidra's default analysis results **depend on the host's speed** ([#112](https://github.com/armaatus/maplethief-port/issues/112)). *Decompiler Switch Analysis* re-decompiles a function after each round of newly recovered switch targets, and it drops any decompile that goes over its 60 s timeout without a message. The packet dispatcher `CDataMgr::OnServerPacket` (0x3ED18, 29,427 bytes, nested `tbb`/`tbh`) takes about 17 s per decompile on an Apple M4. On a slower Windows host, the last round went over the limit, and 5,222 bytes of switch cases (0x3F102–0x40EE0) were never disassembled. On the M4's efficiency cores (`taskpolicy -c background`), the defaults also cut `CStateGame::Paint` (0x87500) and `CStateGame::OnKeyPressed` (0x8D4C8). The pipeline sets every analysis timeout to "none", and the export is then byte-identical on both core types.

#### Function discovery ([#32](https://github.com/armaatus/maplethief-port/issues/32))

`python -m recomp discover` ([ADR 0016](adr/0016-function-discovery.md)) does recursive descent with the recompiler's own decoder from every seed and writes `work/recomp/functions.json` plus `work/recomp/discovery-report.md`. Measured 2026-10-26 on an Apple M4:

- **Coverage.** All 2,655 seeds are analysed in 1.5 s, the ARM entry stub 0x27E0 included (its A32 decoder came with [#33](https://github.com/armaatus/maplethief-port/issues/33)). Discovery reaches **327,142 instructions** (859,464 bytes) in 30,810 basic blocks. Inside the function bounds, every other byte is one of these:
  - alignment padding (`nop`, `mov r8, r8`), 2,604 bytes;
  - data;
  - after a noreturn call, 18 bytes;
  - a float table that `-[CBNativeInterstitialView layoutSubviews]` reads through `adr`, 32 bytes;
  - three dead `b` instructions (0x297E6, 0x30376, 0xACE62), 6 bytes.

  The linear census differs because it decodes jump tables and padding as code and never reaches the landing pads.
- **Jump tables.** There are **184 `tbb`/`tbh` (132 + 52)**, at the same sites as the linear census, plus **16 branch tables** in Chartboost's SjLj landing pads (`addw r1, pc, #k; add.w r1, r1, r0, lsl #2; mov pc, r1` into a run of `b.w`, 2 to 61 cases). All are bounded by a `cmp` and a `bhi`. In the -O0 game code the index usually goes through stack slots between the `cmp` and the table, and sometimes another register holding the same value is compared. The 200 tables have 2,103 distinct targets and 3,224 bytes of `tbb`/`tbh` data, and the targets match Ghidra's `COMPUTED_JUMP` xrefs for all 200. There is no `ldr pc` or `add pc` table and no other indirect jump: every `bx`/`mov pc`/`ldr pc`/`pop {pc}` that is not a table is a return.
- **Indirect calls and returns** ([#35](https://github.com/armaatus/maplethief-port/issues/35)). There are **371 `blx rN`** in 154 functions (vtable calls, ObjC blocks and callbacks), 2,052 `pop {..., pc}` and 635 `bx lr` returns, and no other indirect control flow. The dispatch table has the 2,655 functions; its 16-byte bucket index has 55,056 buckets (110 KB) with at most 4 functions per bucket, and every seed above except the 18 data words is in it. The 222 function imports are reached through `mt_import_thunks`: 217 stubs (4-byte ARM stubs at 0xF0C90–0xF0FF0, after `__text`) and 5 stub-less ones at `0xFFFE0000 + 4 * id` (`___cxa_pure_virtual`, the two personality routines, `_realloc`, `dyld_stub_binder`).
- **Calls.** There are 18,821 direct calls and **one tail call**: the ARM entry stub ends in `b` to the `_exit` stub, after its `blx` into the Thumb code at 0x280D. No Thumb `b` leaves its function.
- **Noreturn.** 481 calls go to noreturn imports (`_abort`, `_exit`, `___assert_rtn`, `std::terminate`, `_Unwind_SjLj_Resume`, `_objc_exception_rethrow`) or to the **5 functions that never return**: 0x8FF9 (`exit` wrapper), `OpenPortalURLAndExit` 0x22955, 0x25771 (both call 0x8FF9), `-[CBActionWrapper init]` 0xD79B5 (`NSLog` + `abort`) and the entry stub 0x27E0 (`b _exit`). The first three are the three "tail-call fall-throughs" of the ADR 0014 trial.
- **Data.** Literal pools hold 15,608 bytes.
- **SjLj.** Each of the **205 SjLj functions has one resume point**. In 189, the game's, it is `mov r4, pc; adds r4, #7`, one halfword that no branch reaches. In the 16 Chartboost ones it is `ldr r1, =k; orr r1, #1; add r1, pc`, leading to the landing pads and their branch table. Discovery reaches 2,102 bytes of this code that no branch leads to. Ghidra never disassembles them.
- **Seeds.** 1,064 ObjC IMPs, 189 vtable slot functions, 2 init functions, 1,221 call targets and 79 code addresses computed in code (callbacks, block invoke functions) are all `LC_FUNCTION_STARTS` entries. Of the 1,384 odd words in data sections that point into `__text`, 18 are not starts: Korean UTF-8 string bytes and one CFString length, not pointers.
- **Ghidra cross-check.** 207 functions differ from Ghidra, all of them explained (the ARM stub agrees since #33):
  - 205 by the SjLj resume code;
  - 4 by bytes after a noreturn call (Ghidra does not know 0x8FF9 never returns);
  - 2 by literal words that Ghidra decoded as code.

  `CDataMgr::OnServerPacket`, `CStateGame::Paint` and `CStateGame::OnKeyPressed` have all of Ghidra's code.
- **Lifting.** With these results, **all 2,655 functions lift cleanly** (#30's prototype: 2,375 of the Thumb ones; #32: 2,524; #33: 2,525 with the ARM stub; #34 adds the 130 with NEON or VFP↔integer conversions). They also **all compile**: `python -m recomp coverage` ([#40](https://github.com/armaatus/maplethief-port/issues/40)) compiles each of the 62 generated units with clang `-std=c17 -Wall -Wextra -Wpedantic -Werror -O0` and charges any error to its function. Its `work/recomp/coverage.md` reports 2,655 of 2,655 discovered, lifted cleanly and compiled, in every #10 tag, with no SjLj context or import table problem.

#### Instruction census ([#33](https://github.com/armaatus/maplethief-port/issues/33), [#34](https://github.com/armaatus/maplethief-port/issues/34))

`python -m recomp census` discovers and lifts every function and writes `work/recomp/census.json`. It counts each instruction *form*, meaning each way of using an instruction that the lifter handles differently (keys and schema in `recomp/census.py`). Measured 2026-10-26:

- **Classes.** The 327,142 discovered instructions have **244 forms**:
  - 189 integer forms with 323,198 instructions. These are 167 Thumb forms, 10 A32 forms in the entry stub, and the 12 condition codes in use (`vs`/`vc` never occur);
  - 43 VFP forms with 3,563 instructions;
  - 12 NEON forms with 381 instructions (below).
- **Traps.** **No instruction traps** (#34; until then the 381 NEON instructions and 84 float↔integer `vcvt` did: `.s32.f32` 41, `.f32.s32` 24, `.f64.s32` 11, `.s32.f64` 7, `.u32.f64` 1).
- **NEON forms** (instructions, functions): `vorr d,d,d` 103 in 67 (every one is `vorr d0, d0, d0`, a no-op), `vmov.i32 q,#0` 67 in 29 (`q8` 64, `q4` 3), `vadd.f32 d,d,d` 62 in 21 (7 in IT blocks), `vcvt.f32.s32 d,d` 55 in 26, `vmul.f32 d,d,d` 38 in 11 (1 in an IT block), `vsub.f32 d,d,d` 27 in 14, `vcvt.s32.f32 d,d` 16 in 2, `vcvt.f32.u32 d,d` 10 in 3, `vneg.f32 d,d` 2, `vcvt.u32.f32 d,d` 1. No Q-register arithmetic, no NEON loads or stores, no other NEON instruction. `recomp/decode/neon.py` decodes these classes (D and Q), and Capstone agrees on every one of them in the binary.
- **VFP/NEON snippets.** `tests/recomp/vfp_forms.txt` lists the 55 VFP and NEON forms, and every one is in a passing snippet, plus edge cases for each conversion and for flush-to-zero / default NaN ([ADR 0022](adr/0022-guest-floating-point-mode.md)). A CI test enforces that. A local test checks that the list is the binary's census.
- **Integer mnemonics.** The binary uses `adc add addw adr and asr b bic bl blx bx cbnz cbz cmn cmp dmb eor it* ldm ldr ldrb ldrd ldrh ldrsb ldrsh lsl lsr mla mls mov movt movw mul mvn orr pop push rrx rsb sbc smmla smmul stm str strb strd strh sub sxtb sxth tbb tbh trap tst ubfx umull uxtb uxth`. None of `clz`, `rev`, `rbit`, `sbfx`, `bfi`, `bfc`, `smull`, `umlal`, `smlal`, `ssat`, `usat`, `smulbb`, `ldrex`/`strex`, `sdiv`/`udiv`, `teq` or `orn` appears in discovered code. The decoder and lifter still handle most of them. **8,540 integer instructions are conditional**: 7,531 conditional branches and about 1,000 inside IT blocks.
- **A32.** The entry stub uses 10 forms: `ldr` at `[rn]` and post-indexed, `add` with an immediate and with `lsl`, `bic`, `mov`, `cmp`, `bne` back, `blx #imm` into Thumb, and `b` to the `_exit` stub.
- **Snippets.** `tests/recomp/integer_forms.txt` lists the 189 integer forms, and every one is in a passing snippet. A CI test enforces that. A local test checks that the list is the binary's census.
- **Generated C.** The C of all 2,655 functions is 42 MB. As one translation unit it compiles with clang `-std=c17 -Wall -Wextra -Werror -Wpedantic` in 14 s at `-O0`, 30 s at `-O2` and 32 s at `-O3` (one core, Apple M4), and passes the MSVC-target syntax check.
- **Function sizes in C.** They are very uneven. The four largest functions are 1.26 MB, 1.05 MB, 0.96 MB and 0.82 MB of C (`f_0003ED19`, `f_00087501`, `f_0008D4C9`, `f_0003A899`), and each one alone takes several seconds at `-O3`. Split into 64 address cells of `__text` (60 non-empty, [ADR 0021](adr/0021-generated-code-partition-and-build.md)), the program builds in Release in about 7 s with 10 cores (58 s of CPU time), and in 23 s as a universal binary.
- **Leaf functions** ([#41](https://github.com/armaatus/maplethief-port/issues/41), `python -m recomp difftest`, [ADR 0017](adr/0017-unicorn-differential-testing.md)). **786 functions are leaves**: they have no SjLj entry, indirect jump, trap, coprocessor access or import, they lift cleanly, and they call only other leaves (138 do). Most of the other functions call imports: 1,141 of the 1,185 that pass every other criterion but fail the clean-lift criterion (which also excludes indirect calls) have an `MT_CALL_IMPORT`. The leaves are small -O0 code, with a median of 15 instructions and a maximum of 612. 43 use VFP. Run in Unicorn and as recompiled C on 1,000 seeded random inputs each, **775 agree on every input** (r0–r14, NZCV, d0–d31, every changed byte). That is 780,051 returning and 458,655 faulting runs with zero mismatches, in about 85 s on 8 cores. (Before #34–#37 were merged: 741 leaves, 730 passing, 1 with VFP.) The other 11 dereference random heap words or loop on a random count too often to reach 1,000 returning runs in 10,000 attempts.

### Instruction mix

These counts come from the `mtport.armscan` sweep (Capstone, each `LC_FUNCTION_STARTS` function decoded linearly with its literal pools and `tbb`/`tbh` tables skipped), re-counted for [ADR 0014](adr/0014-recompiler-design.md): **330,063 instructions** in the 2,655 functions. The recompiler's own decoder gives identical text for all of them, the NEON ones included (#34).

- **IT blocks:** 986 `it` instructions covering 1,018 instructions, in 306 engine, 50 app-objc and 65 Chartboost functions. **`tbb`/`tbh` jump tables:** 132 `tbb`, 52 `tbh`. **`blx` to immediate:** about 9,100, mostly Thumb→ARM interworking calls into the 4-byte ARM `__symbolstub1` stubs. **`blx reg`:** about 370 (ObjC/C++ virtual and function-pointer calls; `objc_msgSend` itself goes through a stub).
- **No hardware divide.** Integer division goes through `__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`/`__divdi3`/`__udivdi3` imports.
- **VFP is used moderately** (about 4,000 instructions). In descending order: `vmov`, `vldr`, `vstr`, `vpush/vpop`, `vcvt`, `vcmpe` + `vmrs APSR_nzcv`, `vadd`, `vmul`, `vsub`, `vdiv`, a few `vsqrt`. It has both `.f32` and `.f64` arithmetic, and it **uses d16–d31 (VFPv3-D32)**, so the CPU state needs 32 D registers.
- **FPSCR is never written.** All 121 `vmrs` are `vmrs APSR_nzcv, fpscr` after a `vcmpe`, and neither slice has a `vmsr`. So FPSCR keeps the value iOS gives every new thread, **0x03000000: default NaN (DN) and flush-to-zero (FZ) on, round to nearest** ("RunFast mode": XNU `vfp_state_initialize` sets `FPSCR_DEFAULT = FPSCR_DN | FPSCR_FZ` in `osfmk/arm/status.c`, and the arm64 kernel runs 32-bit processes with `FPCR_DEFAULT_32 = FPCR_DN | FPCR_FZ`; [ADR 0022](adr/0022-guest-floating-point-mode.md)). VFP therefore flushes denormal operands and tiny results to signed zero and returns the default NaN `0x7FC00000` for every NaN result, exactly like NEON. (#33 and earlier assumed FPSCR = 0.)
- **NEON is used for f32 arithmetic** ([ADR 0014](adr/0014-recompiler-design.md), [#34](https://github.com/armaatus/maplethief-port/issues/34)): **381 Advanced SIMD instructions in 121 functions** (Chartboost 192, app-objc 112, engine 77). They are 103 `vorr dN, dM, dM` register moves, 67 `vmov.i32 qN, #0`, 129 f32 lane operations on D registers (`vadd/vsub/vmul/vneg.f32 dN, ...`: scalar float math done in NEON lanes, likely the compiler's choice for the Cortex-A8; 8 of them inside IT blocks) and 82 `vcvt` between f32 and s32/u32 on D registers. NEON arithmetic always flushes denormals to zero and returns the default NaN; on iOS VFP does too (above). The `vld1/vst1/vhadd/vaddl/vld4` hits of the first linear sweep were literal-pool noise.
- 66 `dmb ish` barriers and no `ldrex/strex`. All 66 are in Chartboost's synthesized `atomic` property accessors ([re/timing-threading.md](re/timing-threading.md#atomics-and-barriers), from a function-by-function sweep). The "few `ldrex/strex`" an earlier linear sweep reported were literal pools decoded as code.
- The armv6 slice also uses hardware VFPv2 (`vldr/vmov/vstr/vcvt/vcmpe`), with d0–d4 and d8–d15 only and no NEON. It has 286,599 instructions in 1,224,284 bytes and as much conditional execution as armv7's IT blocks: 2,382 conditional non-branch instructions, about 1,084 of them `andeq r0, r0, r0` (zero words of literal pools), so about 1,300 real ones.

## Imports (333)

`nm -u` lists **333 undefined symbols** in each slice, and the lists are identical. This list drives the HLE workload.

| Group | Count | Highlights |
|---|---|---|
| ObjC class refs (`_OBJC_CLASS_$_*` / `METACLASS`) | 79 | 61 distinct framework classes (below) |
| CoreGraphics | 40 | bitmap contexts, `CGImage*` introspection, linear + radial gradients, clip paths, blend mode, text drawing mode |
| CoreFoundation | 34 | `CFBundleCopyResourceURLForLocalization`, `CFURLCreateDataAndPropertiesFromResource` (this is how files are read), `CFRunLoopTimer*`, `CFSocket*`, `CFString*`, `CFData*` |
| OpenGL ES 1.1 (+OES FBO) | 31 | see [Rendering](#rendering) |
| OpenAL | 19 | `al*` / `alc*` incl. `alcGetProcAddress` (probably `alBufferDataStatic` extension) |
| libc / compiler-rt | 43 | `malloc/calloc/realloc/free`, `mem*/str*`, `printf/puts/vfprintf/vsprintf/__snprintf_chk`, `floor/pow/trunc`, `gettimeofday`, `sysctl(byname)`, `gethostbyname/inet_*/setsockopt/close/if_nametoindex`, `atoi/atol`, `abort/exit/__assert_rtn`, integer-division helpers, `__floatdisf` |
| C++ ABI | 14 | `operator new/delete(/[])`, `__cxa_atexit`, `__cxa_pure_virtual`, `std::terminate`, `__class_type_info`/`__si_`/`__vmi_class_type_info` vtables, `__gxx_personality_sj0`, `_Unwind_SjLj_Register/Unregister/Resume` |
| ObjC runtime | 14 | `objc_msgSend`, `objc_msgSend_stret`, `objc_msgSendSuper2`, `objc_get/setProperty`, `objc_sync_enter/exit`, `objc_enumerationMutation`, `objc_begin/end_catch`, `objc_exception_rethrow`, `__objc_personality_v0`, empty cache/vtable |
| GCD | 11 | `dispatch_async/sync/after/once/time`, `dispatch_get_global_queue`, `_dispatch_main_q`, `dispatch_group_create/async/wait`, `dispatch_release` |
| UIKit (C functions) | 10 | `UIApplicationMain`, `UIGraphics*` image contexts, `UIImagePNGRepresentation`, `UIRectFill` |
| AudioToolbox | 9 | `ExtAudioFileOpenURL/Read/Get/SetProperty/Dispose`, `AudioSessionInitialize/SetActive/SetProperty`, `AudioServicesPlaySystemSound` |
| Foundation (C) | 9 | `NSLog`, `NSHomeDirectory`, `NSSearchPathForDirectoriesInDomains`, `NSClassFromString`, `NSSelectorFromString`, locale keys |
| SystemConfiguration | 6 | `SCNetworkReachability*` |
| QuartzCore (C) | 4 | `CATransform3DRotate/Scale/Translate`, `kCAFillModeForwards` |
| Blocks | 4 | `_Block_object_assign/dispose`, `_NSConcreteStackBlock/GlobalBlock` |
| EAGL constants | 3 | `kEAGLColorFormatRGBA8`, `kEAGLDrawablePropertyColorFormat/RetainedBacking` |
| CommonCrypto | 2 | `CC_MD5` (game: `-[NSString(MD5Addition) stringFromMD5]` for the `UIDevice(IdentifierAddition)` device ID that `FUN_00098864` asks for), `CC_SHA1` (Chartboost request signing) |
| AVFoundation (C) | 1 | `AVAudioSessionCategoryAmbient` |

**File reads.** `FUN_0000a2c0` reads every game file: CFStrings for the name and folder in `kCFStringEncodingASCII`, `CFBundleCopyResourceURLForLocalization(main, name, NULL, folder, CFSTR("English"))` (the literal at `0x117F44`), then `CFURLCreateDataAndPropertiesFromResource`. `FUN_00008f70` reads a given URL the same way, asking for (and releasing) the properties dictionary. All 2,172 files of the shipped `.app` resolve through these calls ([ADR 0032](adr/0032-corefoundation-object-model.md), `cf.real_bundle`). Of the 51 CoreFoundation imports, 14 are Foundation class objects, 24 functions, 12 data objects (`NSLocaleCountryCode/LanguageCode` among them) and `___CFConstantStringClassReference`.

**Not imported:** no `fopen/fread`, no `pthread_*`, no `sin/cos`, no `glClear`. All file reads go through CFBundle/CFURL/NSData. No thread is ever created: `NSThread` is used only for `+isMainThread`, and GCD only by Chartboost's background image loads and one main-queue `dispatch_async` in `GameCenterManager` ([re/timing-threading.md](re/timing-threading.md)). Rotation uses `glRotatex`, and trig is probably done with lookup tables.

**Functions and data ([#36](https://github.com/armaatus/maplethief-port/issues/36), [ADR 0019](adr/0019-import-thunks-and-hle-abi.md)).**
- **222 functions.** 217 are called through their `__symbolstub1` stub. 5 are reached only through a pointer: `___cxa_pure_virtual` (vtable slots), `___gxx_personality_sj0` and `___objc_personality_v0` (SjLj function contexts), `_realloc` (a bson function-pointer table) and `dyld_stub_binder`.
- **111 data objects.** 79 are ObjC class and metaclass objects. The other 32 are variables and constants, with these sizes: the pointer variables `_kCF*`, `_NS*Key`, `_kEAGL*` and `___stderrp` (4 bytes each); `_CGRectZero`/`_CGRectNull` (16); `_CGAffineTransformIdentity` (24); the block isas `__NSConcreteStackBlock`/`GlobalBlock` (128); `___CFConstantStringClassReference`; `__dispatch_main_q`; the objc empty cache and vtable; the three `__cxxabiv1` type-info vtables.
- **13 weak imports.** These are the StoreKit and GameKit classes, `_CC_MD5`, `_CC_SHA1`, `__NSConcreteStackBlock` and `__dispatch_main_q`.

**The C library ([#44](https://github.com/armaatus/maplethief-port/issues/44), [ADR 0028](adr/0028-libc-hle.md)).** There are 34 libSystem C-library imports plus the 7 libgcc helpers. `exit` among them is implemented with process start-up ([#43](https://github.com/armaatus/maplethief-port/issues/43)). Blocks, GCD, CommonCrypto and `__cxa_atexit` belong to other runtime modules.
- **Device id.** `-[UIDevice(IdentifierAddition) macaddress]` (`0xC1025`) builds the MIB `{4 CTL_NET, 17 AF_ROUTE, 0, 18 AF_LINK, 3 NET_RT_IFLIST, if_nametoindex("en0")}` at `[sp+0x68]`. It calls `sysctl` twice, once for the size and once for the data. It then reads the MAC at `buf + 0x70 + 8 + sdl_nlen` (`0xC1162`–`0xC116E`): the armv7 `if_msghdr` is 112 bytes, and the address follows the interface name in the `sockaddr_dl`.
- **Printf formats.** The format strings in the binary use `%s %d %@ %x %02X %ld %lu %lld %llu %c %p %e %.17g %.1s %%`. None uses positional arguments.

**Calling convention ([#36](https://github.com/armaatus/maplethief-port/issues/36)).** The game follows the iOS armv7 ABI, not base AAPCS:
- **Arguments.** They are passed as a sequence of words, in r0-r3 and then on the stack.
- **Soft-float.** float and double travel in core registers.
- **64-bit values.** They are only 4-byte aligned, so they are never moved to an even register and can be split between r3 and `[sp]`. At 0x8ED8, `CFRunLoopTimerCreate` gets `fireDate` in r1:r2 and `interval` in r3 and `[sp]`.
- **Structure results.** A structure larger than 4 bytes is returned through a pointer in r0. At 0xD7EC0, `CGRectInset` gets the result pointer in r0, the CGRect in r1-r3 and `[sp]`, and `dx`/`dy` at `[sp+4]` and `[sp+8]`.
- **Struct-returning imports.** Six imports use the r0 result pointer: `_objc_msgSend_stret`, `_CGRectInset`, `_CGAffineTransformMakeRotation` and `_CATransform3DRotate/Scale/Translate`.

Framework classes referenced: `AVAudioPlayer AVAudioSession CABasicAnimation CAEAGLLayer CAKeyframeAnimation EAGLContext GKAchievement GKLeaderboard GKLeaderboardViewController GKLocalPlayer GKPlayer GKScore MPMoviePlayerController NSArray NSAssertionHandler NSAutoreleasePool NSBundle NSCharacterSet NSData NSDate NSDecimalNumber NSDictionary NSError NSFileHandle NSFileManager NSKeyedArchiver NSKeyedUnarchiver NSLocale NSMutableArray NSMutableData NSMutableDictionary NSMutableString NSMutableURLRequest NSNotificationCenter NSNull NSNumber NSObject NSSet NSString NSThread NSTimer NSURL NSURLConnection NSURLCredential NSURLRequest NSUserDefaults NSValue SKPayment SKPaymentQueue SKProductsRequest UIActivityIndicatorView UIApplication UIBezierPath UIButton UIColor UIDevice UIFont UIImage UIImageView UILabel UIPageControl UIScreen UIScrollView UITableView UITableViewCell UITextView UIView UIViewController UIWebView`.

### Who uses which import ([#11](https://github.com/armaatus/maplethief-port/issues/11))

[docs/re/imports.md](re/imports.md) (generated by `python3 -m mtport.imports generate`) lists every import with its code sites, the functions that use it and a class. **The HLE layer must implement 245 of the 333 imports**: 178 are used only by game code, 67 by both the game and Chartboost. 87 are Chartboost-only and can be stubbed once ads are; 1 (`dyld_stub_binder`) is used only by the lazy-binding `__stub_helper`.

- **Chartboost-only:** all of SystemConfiguration (Reachability), QuartzCore except `CAEAGLLayer` (the `CATransform3D*` calls, CA animations, `kCAFillModeForwards`), `NSURLConnection`/`NSURLRequest`/`NSMutableURLRequest`/`NSURLCredential`, `UITableView(Cell)`, `UIWebView`, `UIBezierPath`, `CABasicAnimation`/`CAKeyframeAnimation`, `NSDecimalNumber`, `NSKeyedArchiver`/`Unarchiver`, `NSUserDefaults`, `NSTimer`, `NSDate`, `NSNotificationCenter`, `CC_SHA1`, `NSLog`, the CG gradient and clip-path calls, most of GCD (`dispatch_once/sync/after/group_*`, only `dispatch_async` + `_dispatch_main_q` are shared) and the ObjC `@try` runtime (`objc_begin_catch` etc.).
- **Game-only:** all of OpenGL ES, OpenAL, AudioToolbox, AVFoundation, MediaPlayer, StoreKit, GameKit, the C++ runtime (`operator new/delete`, `std::terminate`, RTTI, `__gxx_personality_sj0`) and the compiler-rt division helpers. The SjLj unwinder (`_Unwind_SjLj_*`) is used by both.
- Functions: 1,979 game, 666 Chartboost (the 57 Chartboost classes' IMPs, the ATFunctionalCollectionExtensions categories and everything reachable only from them), none shared, 10 unreferenced (category methods whose selector nothing sends, and their block helpers). The 666 match #10's `lib:chartboost` tags exactly; #10 also puts 9 of the 10 unreferenced ones there, by where they are linked.
- Capstone (`tools/mtport/armscan.py`) and Ghidra find identical call and load sites for all 333 imports (10,230 sites). Of 5,530 `objc_msgSend`-family calls, 5,413 have a known selector and 1,848 a known receiving class.

## Objective-C shell

`__objc_classlist` holds **75 classes: 18 app classes and 57 Chartboost classes**.

| App class | Superclass | Methods |
|---|---|---|
| `MapleThiefAppDelegate` | NSObject | 12 |
| `EAGLView` | UIView | 80 |
| `EAGLView_Instance` | NSObject | 9 |
| `EAGLViewController` | UIViewController | 7 |
| `MainMenuView` | UIView | 57 |
| `ChooseCharacterView` | UIView | 29 |
| `TermsView` | UIView | 5 |
| `TouchKeyPadView` | UIView | **149** |
| `MapKeyPadView` | UIView | 15 |
| `NpcKeyPadView` | UIView | 47 |
| `PopupKeyPadView` | UIView | 42 |
| `MarketKeyPadView` | UIView | 40 |
| `CashShopKeyPadView` | UIView | 21 |
| `HelpScrollKeyPadView` | UIView | 5 |
| `MovieOverlayView` | UIView | 2 |
| `MoviePlayerController` | UIViewController | 6 |
| `GameCenterManager` | NSObject | 18 |
| `InAppPurchaseManager` | NSObject | 19 |

That's about **563 methods** (instance + class), most of them in the input/keypad views. `TouchKeyPadView` alone has 149, so it's the main bridge between touch input and the engine. The app delegate also implements the remote-notification callbacks.

What each class does is in [docs/re/objc-shell.md](re/objc-shell.md) ([#22](https://github.com/armaatus/maplethief-port/issues/22)), with its framework selectors, call edges and images, and a native-rewrite vs UIKit-emulation estimate per class. Every (framework class, selector) that live game code sends, per framework area, is in [docs/re/host-surface.md](re/host-surface.md) (generated by `python3 -m mtport.hostsurface generate`, [#49](https://github.com/armaatus/maplethief-port/issues/49)): 268 selectors, 78 of them Foundation's. In short:

- The shell is an `EAGLView` (GL ES 1.1 surface and boot), the two pre-game menus (`MainMenuView`, `ChooseCharacterView`), a Korean terms screen, and `TouchKeyPadView` with 10 button panels the engine shows and hides (alpha 1/0 by index).
- Every button ends in `Clet_HandleEvent(2|3, phone key code, 0)` from one of 52 `click…`/`release…Button:` methods. `SkyCanvas::Tick` also polls the pad's `on…Key` BOOLs.
- `engine` code sends 108 distinct selectors that reach shell classes, from 39 functions, mostly through `+[EAGLView_Instance Instance].objView`. That includes twelve 61-byte C bridges `Shell_Enable…`/`Shell_Disable…` at `0x6655`–`0x69C5`.
- The movie player classes are dead, and `InAppPurchaseManager`/`GameCenterManager` exist only if StoreKit payments and Game Center are available at boot.

Categories (`__objc_catlist`, 4): `UIDevice (IdentifierAddition)` (`uniqueDeviceIdentifier`, `macaddress`, …), `NSString (MD5Addition)`, and `NSArray`/`NSDictionary (ATFunctionalCollectionExtensions)` (`each:`/`map:`/`reduce:`/`filter:`). The first two are the common "UIDevice-with-UniqueIdentifier" snippet. They're probably used by Chartboost or analytics and are safe to stub.

`__objc_protolist` holds 19 protocols: `NSObject`, `NSCoding`, UIKit's `UIApplicationDelegate`, `UIScrollViewDelegate`, `UIWebViewDelegate`, `UITableViewDataSource`/`UITableViewDelegate`, StoreKit's `SKProductsRequestDelegate`, `SKRequestDelegate`, `SKPaymentTransactionObserver`, and 9 Chartboost-internal ones (`CBAPIConnectionDelegate`, `CBJsonStreamParserDelegate`, …). The first pass guessed `GKLeaderboardViewControllerDelegate` and `ChartboostDelegate` here; neither is in the metadata (no class adopts them).

The full recovered metadata is in [docs/re/objc.md](re/objc.md) (generated by `python3 -m mtport.objc generate`, cross-checked against `otool -oV`): 563 app + 486 Chartboost + 15 category methods = **1,064 IMPs**, all distinct and each one an `LC_FUNCTION_STARTS` entry. The armv7 slice has no root classes of its own: every superclass chain ends in an imported UIKit/Foundation class, bound through dyld bind opcodes. `dyld_info -fixups` on macOS 26 lists the right bind addresses but pairs many of them with the wrong symbol for this armv7 binary; use `mtport.macho.MachO.binds` instead.

The runtime prototype of [ADR 0026](adr/0026-objective-c-strategy.md) (#46) loads this metadata into guest memory and resolves every method from it (`tests/test_objc_runtime_real.py`), which confirms three layout facts:

- **Selectors.** The 951 selrefs already point at the one copy of each name: the linker coalesced `__objc_methname`, which holds 1,143 distinct selector names.
- **Instance sizes.** The SDK the app was built against gives `NSObject` 4 bytes, `UIView` 46 (`MovieOverlayView` has no ivars and `instanceStart = instanceSize = 46`; `UIView` subclasses with ivars start at 48) and `UIViewController` 152. Host classes use these sizes, so no ivar offset has to slide.
- **Superclasses.** Every metaclass's superclass word is bound to the superclass's metaclass.

### Chartboost 3.0.4

The 57 `CB*` classes (Chartboost, CBAPIConnection, CBWebView, CBNativeMoreApps*, CBReachability, CBCrypto, a full SBJson-derived `CBJson*` stream parser/writer, CBBSON, …) are confirmed by the strings `Chartboost-iOS-SDK` / `3.0.4` and build paths like `/Users/alfredo/Documents/gits/client/Chartboost/...`. Hard-coded Chartboost app id/signature strings are present. **All of this gets stubbed or removed.** The game only needs `[Chartboost sharedChartboost]` and friends to be no-ops.

## C++ engine

**63 polymorphic classes** have surviving RTTI names (`typeinfo` name strings in `__cstring`/`__const`). The first pass counted 64, but its 64th, `CPW`, is the bytes `43 50 57` inside Thumb code (`str r3, [r0, r1]` + `ldr.w`, six times in `0xBCED8`–`0xBCFE6`), not a type name ([#9](https://github.com/armaatus/maplethief-port/issues/9)):

- **World/objects:** `CMapleWorld`, `CMapleObject`, `CHero`, `CMob`, `CMobs`, `CItem`, `CDropItems`, `CSelItem`, `CMapArms`, `CMobArms`
- **Quests/skills:** `CQuest`, `CQuestList`, `CReward`, `CRequire`, `CSkill`
- **Map/sprites:** `CMMap`, `CMMapTileSet`, `CMLayer`, `CMSprite`, `CMSpriteImage`, `CMImage`, `CMFrame`, `CMAction`, `CAniManager`, `SkyCanvas`
- **Resources/data:** `CMResMgr`, `CDataMgr`, `DataReader`, `CLoaderThread`, `Stringtable`, `TaggedString`, `SVectorClass`
- **System:** `CMSoundPlayer`, `CMSocket`, `CMTimer`, `CMUtil`, `UtilAdaptor`
- **States:** `IState`, `IStateAdaptor`, `CStateTitle`, `CStateMainMenu`, `CStateGame`
- **UI/effects:** `CPopup`, `CMPopup`, `CChatBox`, `CMScrollBar`, `CMsg`, `CNotifyText`, `CNotifyTextList`, `CFloating`, `CFloatingBG`, `CFloatingEff`, `CFloatingBGImage`, `CFloatingBGAlgorithm`, `CFloatingBGFlowerAlgorithm`, `CDamageFloatings`, `CEffectFloatings`
- **Script VM:** `Lua`, `TFunc`, `Table`, `Symbol`, `Undump`, `Build`

C++ exceptions use **SjLj** (`__gxx_personality_sj0`, `_Unwind_SjLj_*`) with a 4.3 KB `__gcc_except_tab`. libstdc++ is linked dynamically and only its ABI support is imported (`new`/`delete`, `type_info` vtables, `__cxa_*`, `std::terminate`). No container or string members are imported and there is no `std::` RTTI and no STL error string, so the engine does not use STL containers or `std::string` ([#10](https://github.com/armaatus/maplethief-port/issues/10)).

`ScriptExtra.strings` keys (see [Localization](#localization)) leak C++ member names such as `CDataMgr.m_strPopupText`, `CDataMgr.drawControlInfo`, `CDataMgr.LoadSaveData`, `CDataMgr.SaveSaveData` and `CDataMgr.m_strMiddleSpotName`. These help with naming.

### Exceptions ([#37](https://github.com/armaatus/maplethief-port/issues/37))

`python -m recomp exceptions` writes `work/recomp/exceptions.json`: every SjLj function context, its LSDA and where its resume dispatch leads ([ADR 0020](adr/0020-sjlj-exceptions.md), LSDA format in [formats/lsda.md](formats/lsda.md)). Measured 2026-10-26:

- **Contexts.** **205 functions** register an SjLj context (`_Unwind_SjLj_Register` once each), exactly the 205 with a discovered resume entry. 189 are game code (LLVM-GCC: the resume address from `mov r4, pc; adds r4, r4, #7`, a compare chain as the dispatch), 16 are Chartboost (clang: `ldr; orr #1; add pc`, a `mov pc` table of `b.w` as the dispatch). The context sits 52 to 3,376 bytes above `sp` and has the layout prev, call_site, data[4], personality, LSDA, then the `__builtin_setjmp` buffer r7, resume address, sp. The personality is `___gxx_personality_sj0` in 187 functions and `___objc_personality_v0` in 18 (the 16 Chartboost ones and `+[EAGLView_Instance allocWithZone:]`/`Instance`).
- **Tables.** The 205 LSDAs tile `__gcc_except_tab` (4,055 bytes of tables plus alignment padding). Of the 1,238 call sites, **928 are cleanups**, **258 are empty `throw()` specifications** whose landing pads call `std::terminate` (these are all 258 `std::terminate` calls of the binary), and **52 are `catch (...)`**, all in Chartboost's `-[CBJsonStreamParser parse:]` and `-[CBJsonTokeniser getStringToken:]`. There is **no typed catch** and no specification with a type.
- **Dispatch.** For every function, each landing-pad index of its LSDA, run through its own resume code, reaches a landing pad. Clang's dispatch bounds the index with `cmp #n; bhi` over an *n*-entry table, so it admits *n*, one past the table (an LLVM off-by-one; the personality never produces *n*).
- **Nothing throws in normal play.** No import starts an exception: `__cxa_throw`, `__cxa_allocate_exception`, `__cxa_rethrow`, `__cxa_begin_catch`, `objc_exception_throw` and `_Unwind_SjLj_RaiseException` are not imported. The only raising calls are `_Unwind_SjLj_Resume` (205 functions) and `objc_exception_rethrow` (the 2 Chartboost `@catch` blocks), and they, `objc_begin_catch`/`objc_end_catch` and every `std::terminate` call are only in landing-pad code, which runs only after an exception was raised. An exception could therefore only come from a system library: Foundation raising an `NSException` on misuse, or `operator new` failing. The HLE layer does neither in normal play, so the landing pads are dead code there; the runtime still implements the full unwinder (`runtime/src/sjlj.c`), and an exception nothing catches, a violated `throw()` or `std::terminate` traps loudly.

### RTTI and vtables (#9)

[docs/re/cpp-classes.md](re/cpp-classes.md) (generated by `python3 -m mtport.rtti generate`, naming rules in [ADR 0008](adr/0008-cpp-rtti-and-virtual-function-names.md)) has the hierarchy and every vtable. The facts:

- Every `type_info` vptr is a dyld bind (addend 8) to an imported `__cxxabiv1` vtable: 21 `__class_type_info`, 40 `__si_class_type_info` and 2 `__vmi_class_type_info` (`IStateAdaptor : IState, UtilAdaptor` at +4; `CFloatingBG : CFloatingEff, UtilAdaptor` at +36). All bases are public and non-virtual, so there are no VTTs or construction vtables.
- `Build` is the root of 41 of the 63 classes (`Build` → `CMUtil` → `UtilAdaptor` → …), including `Lua`, `TFunc`, `Undump`, `CMapleObject` and the states (through `IStateAdaptor`). `Build` is a base class, not only a script-VM class.
- 68 vtables (63 primary, 5 secondary at `offset_to_top` -4 or -36), with 266 slots, all in `__DATA,__const` except those of `IState`, `UtilAdaptor` and `CPopup` (`__DATA,__data`). Every non-pure slot is an `LC_FUNCTION_STARTS` function. The 31 pure slots (in `IState`, `IStateAdaptor`, `CMapleObject` and `CFloatingBGAlgorithm`) are bound to `___cxa_pure_virtual`.
- Every class has a virtual destructor in slots 0/1 (D1, D0). At -O0 both come from one body that calls `operator delete` behind a constant flag. 50 of the 63 classes have no other virtual function: that includes `Lua`, `TFunc`, `Undump`, `Table`, `Symbol` and `Build`, so the script VM has no virtual interface. The only interfaces are `IState` (slots 2–22, plus 23 in `IStateAdaptor` and 24 in `CStateMainMenu`), `CMapleObject` (2–4), `CFloatingEff` (2) and `CFloatingBGAlgorithm` (2–4).
- `IState` slot 9 is `Paint` and slot 6 is `OnKeyPressed`, and `CMapleObject` slot 4 is `Flush`. The state and object overrides look up `CStateGame.Paint.N`, `CStateTitle.Paint.N`, `CStateGame.OnKeyPressed.N` and `CHero.Flush.N` keys. `IStateAdaptor` gives default bodies for slots 4, 5, 7, 8 and 11–23, many of them empty (8–12 bytes). The three states override 2, 3, 6, 9, 10 and 22, and `CStateGame` also overrides 4, 5 and 7.
- 49 non-virtual functions look up `Class.method.N` keys of an RTTI class too (`CChatBox.drawChatBox`, `CDataMgr.LoadSaveData`, `CAniManager.makeAni*`, `CMPopup.CMPopup` = the constructor, …). `rtti.py` only names vtable slots from them. `mtport.flow` ([#12](https://github.com/armaatus/maplethief-port/issues/12)) names the 42 that no other names file names (`CAniManager::makeAniChapter1`, `CDataMgr::keyPopupItemInven` and `_2`, …).
- The other `IState` slots are named from their call sites (`KNOWN_SLOTS` in `rtti.py`, [re/program-flow.md](re/program-flow.md#the-istate-state-machine)): 2 `Enter`, 3 `Exit`, 4 `Resume`, 5 `Pause`, 7 `OnKeyReleased`, 10 `Update`, 11 `SetParam`, 17 `SetCurrentState`, 18–20 parent/current accessors. Slots 8, 12, 13–16 (push/pop), 21–23 have no call through the vtable in the game code. Every transition goes through the non-virtual `IStateAdaptor::ChangeState` `0xAF65`.
- **No code uses RTTI, and there is no inlined `dynamic_cast`** ([#45](https://github.com/armaatus/maplethief-port/issues/45), measured 2026-10-27 with Capstone over all 2,654 discovered Thumb functions and a per-block constant tracker for `movw`/`movt`, literal loads and `add rX, pc`, 13,082 resolved PC-relative addresses). `__dynamic_cast` is not imported, so a cast to a derived class would be a call to it. The two forms GCC inlines would show up in this scan, and neither does. `dynamic_cast<void *>` loads offset-to-top at vptr − 8: the only loads at displacement −8 are 8 `r7`-relative frame locals in `0x22EC4` and 4 stack-array indexings in `0x49480`/`0x4AB00`, none from a vptr. `typeid` loads the type_info at vptr − 4, and there is no load at displacement −4. No code site references any of the 63 `type_info` objects or their names either. The type_info objects are reached only from their vtables' type_info slots, so RTTI matters only to exception matching in the runtime (ADR 0031), and the game has no typed catch.
- In Ghidra, `tools/ghidra/apply-names` with `work/re/names/rtti.json` gives 189 functions a `Class::` name (126 destructors, 10 thunks, 53 other virtuals), plus data labels for the 63 `type_info` objects, their names and the 68 vtables.

## Program flow

Traced in [re/program-flow.md](re/program-flow.md) ([#12](https://github.com/armaatus/maplethief-port/issues/12)), with every function on the path named in Ghidra (`work/re/names/flow.json` from `python3 -m mtport.flow names`).

- **Boot:** `start` `0x27E0` → `main` `0x280D` → `UIApplicationMain(argc, argv, nil, nil)`. `MainWindow.nib` ([formats/nibarchive.md](formats/nibarchive.md)) holds a `UIWindow` with one `EAGLView` subview plus the app delegate. Decoding the view (`-[EAGLView initWithCoder:]`) creates the ES 1.1 context and framebuffer and calls `startApp`, which calls the engine entry **`Clet_Start` `0x5C2D`** (prints `startClet`). All of this happens before `application:didFinishLaunchingWithOptions:`. The runtime runs this launch through its own `UIApplicationMain` and nib decoder ([ADR 0037](adr/0037-uikit-host-classes-nib-and-compositor.md)) up to the main menu: after 2 s the window holds `TouchKeyPadView` and `MainMenuView` (480 × 320, its background image and the Play button at (46, 148, 120, 48)), and a tap on Play sends `-[MainMenuView clickPlay:]` (`runtime.launch_real`). With no saved character, Play starts a new game at once: `CStateMainMenu::OnKeyPressed` (`0x9462D`) finds every slot empty, loads slot 0's defaults (`LoadSaveData(0)`, `LoadQuestData(0)`), closes the main menu and changes to `CStateGame`, which plays the intro; slot 0 is saved as `Documents/theif0` (1,124 bytes) and `quest0` (4,781 bytes); `ChooseCharacterView` appears only when a slot is in use. **First GL frame** ([#117](https://github.com/armaatus/maplethief-port/issues/117), [ADR 0038](adr/0038-gl-recording-eagl-silent-audio-boot-to-first-frame.md), `runtime.boot_real`): the first `presentRenderbuffer:` already draws (frame 1: 1,229 GL calls, 33 `glDrawArrays`, 132 `glTexImage2D`, 133 textures of 1,489,604 bytes alive) into a 480 × 320 RGBA8 renderbuffer with no retained backing; by then `CMSoundPlayer` has made 44 OpenAL buffers and 44 sources and decoded the 26 WAV effects. The boot to that frame needs no import or selector beyond the runtime's. The engine is a feature-phone port: a Clet lifecycle (`Clet_Pause`/`Clet_Resume` from the app delegate) and phone keypad key codes.
- `Clet_Start` creates `SkyCanvas` (the canvas, current state at `+0xC`), the singletons (`CMResMgr`, `CMSoundPlayer`, `CMTimer`, `CPopup`, `CDataMgr`, `CMSocket`) and `CStateTitle`. `CMapleWorld` is created later by `CStateGame::LoadGame`.
- **Frame loop:** one `CFRunLoopTimer` on the main run loop, re-armed every **1 ms** (`RunLoopTimer_Arm` `0x8E29`). `CMTimer::OnRunLoopTimer` `0x21F51` runs `SkyCanvas::Tick` `0x2590D` when the frame interval has passed: 10 ms at first, then 65/50/40 ms from the speed setting. `Tick` polls the `TouchKeyPadView` key ivars and calls `OnKeyPressed`/`OnKeyReleased`, then `Update` and `Paint`, then `FlushLCD` (`presentRenderbuffer:`). No `CADisplayLink`, no `NSTimer`, no threads (`CLoaderThread` loads synchronously inside `CStateGame::Paint`).
- **Timing** ([re/timing-threading.md](re/timing-threading.md), [#21](https://github.com/armaatus/maplethief-port/issues/21)):
  - The game ticks at a fixed step of **50 ms by default** (20 ticks/s; 65/40 ms by the speed option). Logic is not delta-time: each tick runs `Update` and `Paint` once, and most of the simulation and the sprite animation run in `CStateGame::Paint`.
  - A slow tick delays the next one (drift): no catch-up, no frame skipping.
  - Real-time ms timers sit beside the ticks: 4 `CMTimer` sub-timers (keep-alive, touch hold, a seconds counter), plus popup, touch-repeat and double-tap timers.
  - The MSVC-style `rand()` LCG is seeded once from the clock.
  - `GetTimeMs` wraps to 32 bits. At each 2³¹ ms crossing (every 49.7 days of wall-clock time) a running copy stops ticking.
- **States:** `CStateTitle` → `CStateMainMenu` → `CStateGame` → `CStateMainMenu`, each by `new` + `IStateAdaptor::ChangeState`, which deletes the old state.
- The logo timer and intro movie code (`drawMapleLogo__`, `movieFinishedCallback:`, `MoviePlayerController`) is dead: nothing sends those selectors.
- The 2 static constructors build a 50-entry text texture cache (`0x11A428`) and a static script-VM `TaggedString` (`0x11AA10`).

## Script VM

Specified in [formats/script-out.md](formats/script-out.md) (file format, opcode table) and [re/script-vm.md](re/script-vm.md) (VM internals, native API, entry points) ([#19](https://github.com/armaatus/maplethief-port/issues/19)).

- `script/` has **307 `.out` files**: `chapter1/` 27, `chapter2/` 26, `chapter3/` 29, `common/` 225 (a chapter file overrides `common/`). 10 are never named by a `.obj` (`*_old`, `Darklord_new`, `Spiegelman1`–`3`, …).
- They are **Lua 2.4 `luac` bytecode with the chunk header stripped**: `1B` then `'F'` function records (`u16 size_hi, u16 size_lo, u16 line`, the source name `"Temp.dat"` for main, the code), each followed by `'V'`/`'S'` records that name the globals and string constants its threaded operands refer to. The `Manzie.out` bytes once read as "a u32 and constants" are the code size `0000:0017`, line 0 and main's code.
- The VM is a **C++ port of the Lua 2.4 interpreter** (`Lua`, `TFunc`, `Undump`, `Table`, `Symbol`, `TaggedString`; `Lua::execute` `0x9BC11`): the same 67 opcodes, tags and undump records, but with **integer numbers**, no `PUSHSELF`, flat arrays, ignored errors, a `STORELOCALn` bug, and **every global reset to the 65 natives on each script load**. The shipped scripts use only 24 opcodes (no locals, tables, loops or concatenation), none of the broken ones. All 307 files disassemble with zero unknown opcodes (`python3 -m mtport.scriptout`).
- **Natives:** 65 names at `0x117950` are globals 0–64 of tag C-function, dispatched by symbol index in `Lua::callNative` `0x9B37D`. 13 are Lua 2.4's builtins (all no-ops here), 52 are game functions: dialogue (`initMsg`, `regMsgNPC`, `regSelList`, `showMsg`, `regCallBack`, …), quests (`setQuest`, `regRequire*`, `regReward*`, `getReward`, `getQuestState`, …), hero/inventory, NPC state and `changeMap(area, map, portal)`, the only script path to the warp target at world `+0x98`/`+0x9C`/`+0xA0`. Scripts call 44 of them, always with the handler's argument count.
- **Entry points:** talking to an NPC or entering a portal runs the object's script's main. The NPC-state pass (`0x839BD`) runs only its slot-0 function (`chkQuest`). The map script runs on entering a map and when certain mobs die, and the dialogue box later runs the function given to `regCallBack`.
- **Dialogue text is not in the bytecode.** Scripts pass keys `<Prefix>.<function>.<n>` (`Chapter1_Quest_Manji.dialog.1`) that the message natives look up in `<lang>.lproj/Scripts.strings` (binary plist, 2,219 entries in `en`). All 1,761 key-shaped constants in the scripts resolve. 127 literal (non-key) strings remain, all Korean in the unused files except one `"..."` in `chapter1/Sabi2.out`.

## Rendering

**OpenGL ES 1.1 fixed-function only**, with **31 GL entry points**:

- Core (24): `glBindTexture glBlendFunc glColor4x glDeleteTextures glDisable glDisableClientState glDrawArrays glEnable glEnableClientState glGenTextures glIsTexture glLoadIdentity glMatrixMode glOrthox glPopMatrix glPushMatrix glRotatex glScalef glTexCoordPointer glTexImage2D glTexParameteri glTranslatex glVertexPointer glViewport`
- OES framebuffer (7): `glGenFramebuffersOES glBindFramebufferOES glGenRenderbuffersOES glBindRenderbufferOES glFramebufferRenderbufferOES glGetRenderbufferParameterivOES glCheckFramebufferStatusOES`

Notable absences: **no `glClear`** (every frame overdraws the full screen), no `glColorPointer`, no `glTexSubImage2D`, no `glDrawElements`, no `glReadPixels`, no FBO deletion. Fixed-point `x` variants are used for color, ortho, translate and rotate, while `glScalef` is float. The EAGL drawable is RGBA8 (`kEAGLColorFormatRGBA8`, `kEAGLDrawablePropertyRetainedBacking` is referenced) via `CAEAGLLayer`/`EAGLContext`.

**What the game calls on the boot and launch path** (the GL trace file of `runtime.launch_real`, 6 s: boot, main menu, Play, loading screen, first map; 139 presents, 2,244 draws; [ADR 0040](adr/0040-gles-fixed-function-pipeline.md), 2026-10-29):

- **Matrices:** one `glMatrixMode(GL_PROJECTION)` + `glOrthox(0, 480, 0, 320, -1, 1)` (y up, origin bottom left) and `glViewport(0, 0, 480, 320)` at start-up. Around every draw the modelview gets `glLoadIdentity`, `glPushMatrix`, `glScalef(1, 1, 1)`, `glPopMatrix`. No `glRotatex` or `glTranslatex` on this path: sprites are placed in the vertex arrays.
- **Draws:** only `glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)`. Vertices `2 × GL_SHORT`, stride 0, often on the guest stack; texture coordinates `2 × GL_FIXED`, stride 0. `GL_VERTEX_ARRAY` is enabled once; `GL_TEXTURE_COORD_ARRAY` is enabled and disabled around each textured draw.
- **Caps:** `GL_BLEND` is enabled and disabled around every draw (2,256), `GL_TEXTURE_2D` and `GL_ALPHA_TEST` around every textured one (2,104): 6,464 `glEnable` and 6,464 `glDisable`. Without `glAlphaFunc` (not imported) the alpha test is `GL_ALWAYS` and passes everything. No other cap is touched.
- **Blending:** only `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`.
- **Colour:** `glColor4x` with the 8-bit colour shifted left by 8. White is `(0xFF00, 0xFF00, 0xFF00, 0xFF00)` = 0.99609375, i.e. 254 by GL's rounding. Fades are black quads with decreasing alpha (0.82, 0.66, 0.49, 0.33, 0.16).
- **Textures:** 565 uploads, every one `GL_RGBA`/`GL_UNSIGNED_BYTE` at level 0 with pixels, all powers of two from 1 × 1 to 512 × 512 (most 32 × 32 and smaller: text and sprite pieces). Every texture gets `GL_LINEAR` for both filters and keeps `GL_REPEAT`. 27 `glDeleteTextures`, 109 `glIsTexture`.
- **Frames:**
  - Frame 1 has 33 draws and 132 uploads, and ends with the fade-in: a black untextured quad at alpha 254 over the whole screen.
  - The main-menu frames draw only that quad; the menu itself is UIKit views over the GL view.
  - The loading screen after Play has about 58 draws a frame; the first map (Henesys, with its "Peaceful Henesys" title) has about 6 large ones (366,000 pixels, 2.4× overdraw).

CoreGraphics + UIKit graphics are used to render **text and gradients into bitmaps**, which are then uploaded as textures: `CGBitmapContextCreate`, `CGContextDrawLinear/RadialGradient`, `CGContextSetTextDrawingMode`, `UIGraphicsBeginImageContext(WithOptions)`, `UIFont`, `UIColor`, `UIBezierPath`. PNG decoding goes through `UIImage` → `CGImage` → `CGDataProviderCopyData`, given the `CGImageGet*` introspection imports. The binary contains **no PNG decoder of its own**: there are no zlib/libpng strings (`inflate`, `IHDR`, zlib error messages). The runtime therefore has to decode PNGs on the host. stb_image ([ADR 0003](adr/0003-dependencies.md)) decodes all 1,362 shipped PNGs, and its CgBI output is bit-identical to macOS ImageIO for all 113 CgBI files (checked 2026-10-24).

**Text and bitmaps through CoreGraphics** (decompiled 2026-10-28, [ADR 0036](adr/0036-coregraphics-and-uikit-drawing.md)):

- `TextCache_DrawText` (`0x9031`) draws every engine text texture with UIKit: a 512 × 32 (32 × 32 for one character) `kCGImageAlphaPremultipliedLast` bitmap context over a global buffer, `CGContextSetGrayFillColor(1, 1)`, `UIGraphicsPushContext`, `[UIFont fontWithName:@"MarkerFelt-Thin" size:]` (literal at `0x117E74`), `-drawAtPoint:(0, 0) withFont:`. The context is not flipped, so the text lies upside down at the bottom of the bitmap. `0x72F1` measures with `-sizeWithFont:` and the same font.
- The fonts named in the binary are `MarkerFelt-Thin`, `Helvetica` and `Helvetica-Bold`; Hangul came from iOS's fallback (AppleGothic). `ScriptExtra.strings` names `Gulim` / `굴림` for `SkyCanvas`, a font no iOS 4 device had. The port draws them all with NanumGothic, which has a glyph for every character of the game's `.strings` files and data tables (`tests/test_hle_cg_real.py`).
- The texture loader `FUN_00009534` decodes with `-[UIImage initWithData:]`, maps alpha info 1–4 to premultiplied-last and the rest to no-alpha-skip-last (`| kCGBitmapByteOrder32Big`), and draws into a flipped power-of-two context. The map tile cutter `FUN_0000d6d0` reads the decoded sheet with `CGDataProviderCopyData` as RGBA, alpha in byte 3. `colorAtPixel:onImage:` (`0xB4DB5`) draws a whole image into a 1 × 1 context over 4 stack bytes with the image's bytes per row.
- Gradients, clip paths, `UIBezierPath`, `UIGraphicsBeginImageContextWithOptions` and `UIGraphicsGetCurrentContext` are used only by Chartboost.

## Audio

Full inventory and call map: [re/audio.md](re/audio.md) ([#29](https://github.com/armaatus/maplethief-port/issues/29)).

- **OpenAL** (`alGen/DeleteBuffers`, `alGen/DeleteSources`, `alBufferData`, `alSourcei`, `alSourcePlay`, `alListenerf`, `alGetError`, 10 `alc*` calls including `alcGetProcAddress`) with **`ExtAudioFile*`** decoding (Apple's oalTouch `MyGetOpenALAudioData` `0xA0045`) for **SFX**. The only extension name in the binary is `alBufferDataStatic`, and its only use is dead. Nothing stops, pauses, queues, loops or positions a source and nothing queries one: there is no `alSourceStop/Pause`, `alSourcef`, `alGetSource*`, `alSourceQueueBuffers` or `al*3f`, and the one `alSourcei` sets `AL_BUFFER`. Effects are fire-and-forget, one buffer and one source per id, all decoded at start-up, under a master listener gain (0 or 1 for the effects setting).
- **`AVAudioPlayer`** for **BGM**, one per id, created on first play, `setNumberOfLoops:-1`, volume 0.7 (0 while music is off); plus `AVAudioSession` (category Ambient), `AudioSession*` (C API, interruption listener) and `AudioServicesPlaySystemSound` (only `kSystemSoundID_Vibrate`, the phone-era vibration setting).
- `sound/` holds **44 files: 18 `.mp3` and 26 `.wav`**, named by sound id (`0.mp3` … `43.wav`). The engine class `CMSoundPlayer` resolves an id as `sound/<id>.mp3` or `sound/<id>.wav` (`pathForResource:@"%d" ofType:@"mp3"/@"wav" inDirectory:@"sound"`), and **which one is decided by a hard-coded list** (`Sound_IsBGM` `0x1CC79`: 0–6, 30, 31, 34–42 are BGM), which matches the files exactly. Every id except **42** is played somewhere; `42.mp3` is unused.
- Music: the area and map pick the track (`CStateGame::PlayMapBGM` `0x81031`: town track restarted on map 0, field track resumed elsewhere, track 34 on the five boss maps); the main menu plays 31; the `CAniManager` scenes play their own (mostly 34). Effects go through `Game_PlaySound` `0x80F91` or `CMSoundPlayer::Play` `0x1D3B9` with constant ids.
- **WAV:** all 26 are 16-bit PCM: 13 mono at 8 kHz, 11 mono at 22.05 kHz, and 2 stereo at 44.1 kHz (`10.wav`, `20.wav`); 13 carry editor chunks (`LIST`, `DISP`, `ID3x`, `_PMX`) after the data.
- **MP3:** `0`–`6`, `30`, `31`, `34` and `42.mp3` (11 files) are MPEG-1 Layer III, 44.1 kHz joint stereo, 128 kbit/s CBR. `35`–`41.mp3` (7 files) are MPEG-2 Layer III, 22.05 kHz joint stereo, 80 kbit/s CBR, with ID3 titles of the original MapleStory tracks ("To Kerning City", "East of Henesys", ...). They are 30.6–61.9 s long and carry ID3v2.2/2.3 tags but **no Xing/LAME info header**, so there is no encoder-delay/padding data for gapless looping.

## Networking, IAP, Game Center

- Every network path is in [re/network.md](re/network.md) ([#23](https://github.com/armaatus/maplethief-port/issues/23)), with a recommended offline stub for each. **The game has no online play and needs no network at boot.** The port will run fully offline.
- **`CMSocket`** (engine, `0x1C419` `CMSocket::Connect`) is a raw TCP client to one hard-coded server, **`50.17.236.149:20001`**, that verifies StoreKit receipts and restores purchases (frames `u32 length` + `u16` + payload; client ops 0 hello, 2 receipt, 4 restore, 0xB4 keep-alive; server ops 1, 3, 5). It is only reached after a purchase/restore or from the Pigmy hatch dialog. On failure the game shows popup `0x20` (*"Server connection has failed…"*) and returns control; there are no retries. Its hello packet carries the **device id** (MD5 of the `en0` MAC, from `-[UIDevice(IdentifierAddition) uniqueGlobalDeviceIdentifier]`, collected by `0x98865`) and the APNs token: that is the only place the device id is sent. `gethostbyname`/`inet_ntoa` are dead code (the host is a literal IP).
- **Chartboost 3.0.4** (`https://www.chartboost.com`) owns every `NSURLConnection`, `SCNetworkReachability*`, `UIWebView`, `canOpenURL:` and `dataWithContentsOfURL:` use. The game calls `setAppId:`/`setAppSignature:`/`startSession`/`showInterstitial` at 6 sites, fire-and-forget, with no delegate.
- **`openURL:`**: 6 `MainMenuView` buttons open Nexon/App Store pages (`global.nexonmobile.com`, `maplestory.nexon.net`, `www.nexon.net`, an App Store link). The three `w.nexonmobile.com/CP/*.php` portal URLs are never opened: their menu items call `exit(1)` (`0x22955`).
- **StoreKit (cash shop)** product ids: `com.nexonmobile.maplestory.thief.` + `ComebackPack`, `exppack1`, `exppack2`, `InitializeSP`, `mesopack3`–`mesopack6`, `powpackage`, `SPMarbleSP1Plus`. **A purchase does not check `canMakePayments`** ([#52](https://github.com/armaatus/maplethief-port/issues/52)): `CStateGame::OnKeyPressed` (`0x8D4C9`) sends `-[EAGLView addLoadingView]` (`0x48C9`, a full-screen "Please Wait..." view), sets `CDataMgr+0x534 = 1` / `+0x538 = 7`, then `-loadProductsInfo:` to `InAppPurchaseManager`. Only `CStateGame::Paint` (`0x87501`, cash shop case 10 and popup `0x68`) removes the view, when `-getIsPurchaseSuccess` is 1 or 2. With no manager (`canMakePayments` NO) the view stays for ever. A failed (cancelled) transaction is the clean path: `-failedTransaction:` (`0xBF025`) sets 2, and `Paint` clears `+0x534`, removes the view and resets the result. Invalid products remove the view but leave `+0x534` set. The port cancels every payment ([ADR 0034](adr/0034-offline-services.md)).
- **Game Center:** leaderboard `com.nexonmobile.maplestory.thief.totalranking`, reported with `-[GameCenterManager ReportScore:forCategory:]` (`0xBFE41`, a 64-bit score) from `CMapleWorld::changeMap` (`0x81369`) and `0x65089`. **The shipped game reports no achievement:** the selref of `SubmitAchievement:PercentComplete:` (`0xFABB0`) is read only at `0xC0782`, inside that method's own load block (`0xC05E5`). `-FindPlayerByid:`, `-ResetAchievements` and `-ReloadHighScoreByCategory:` have no sender. The authentication block sets `-[EAGLView setGameCenterLoginOKorError:YES]` whatever the error, and that flag only gates Chartboost calls. Every delegate call goes through `dispatch_async` on the main queue to `-callDelegate:withArg:error:`, which asserts `[NSThread isMainThread]`; `EAGLView`, the delegate, implements none of the callbacks. The leaderboard screen is a `GKLeaderboardViewController` that `-[EAGLView viewGameCenterRank:]` (`0x4E09`) presents modally. The port emulates GameKit with `gamecenter.json` ([ADR 0034](adr/0034-offline-services.md)).

## Save data

Specified in [formats/save.md](formats/save.md) ([#20](https://github.com/armaatus/maplethief-port/issues/20)).

- **Raw binary files in `Documents/`**, written through `NSFileHandle` (path `NSHomeDirectory()/Documents/<name>` at `0x6EF1`; write `0xB785`, read `0xB7C9`). There is no checksum and no version field. The port keeps them at `<data dir>/Documents/<name>` (the guest home directory maps to the data directory, [ADR 0035](adr/0035-foundation-subset.md)), so real saves copied there load unchanged. The game never uses `NSUserDefaults` or `NSKeyedArchiver`; every `standardUserDefaults`, `archivedDataWithRootObject:`, `unarchiveObjectWithData:` and `CC_SHA1` use is inside Chartboost. `CC_MD5` only hashes the MAC address for a device id that is sent over the network, never stored.
- Three slots, each two files: `theif<n>` (sic, 1,124 bytes: job, level, EXP, SP, HP/MP, meso, 48 + 48 inventory slots, 13 skill levels, 5 worn items, 5 shortcuts, map and position, caption) and `quest<n>` (4,781 bytes: chapter, 2 × 100 flag bytes, 10 active-quest records). Written as a pair by `CDataMgr::SaveSaveData` `0x38711` and `SaveQuestData` `0x38F55`. Choosing an empty slot creates both at once (`LoadSaveData` `0x39989`): level 1, 100 meso, a Wooden Club, 5 Red and 5 Blue Potions, map `28`.
- Shared files: `config` (6 option bytes, including a game-speed index into 65/50/40), `cupon` (3 best scores + 50 string records), `CTerms` (terms accepted), and the purchase receipt lists `crestore`, `riprestore1`, `riprestore2`, `totalrestore`.
- Quirks: 20 bytes of `theif` and 9 of `quest` are never written (uninitialised heap). Each quest record overlaps the next by one byte. A slot-exists check probes `thief%d`, which never exists.

## Data layout

All paths are relative to `MapleThief.app/`.

| Path | Contents |
|---|---|
| `script/` | 307 `.out` scripts (see [Script VM](#script-vm)) |
| `map/` | **90 `.map` + 90 `.obj` + 91 `.att`** (an orphan `106.att` has no `.map`), **11 `.mapprj`** (`0`–`10`), subfolders `back/` and `tile/`, 22 PNGs in total |
| `sprite/` | **138 `.sprprj` + 577 PNGs** in `hero/` (3), `mob/` (129), `effect/`, `effect_kor/`, `hatching/`, `loading/`, `title/` (2); each project folder has a `res/` folder of frame PNGs |
| `png/` | 19 folders (`IGM_ui alert arm cab costume face game item logo mainmenu map market minimap npc popmenu scroll skill static ui`), **607 PNGs + 40 `.dat`** |
| `English/data`, `Korean/data`, `Japanese/data` | 8 tables each: `buildup hero item_equip item_etc market mob npc skill` `.dat` |
| `sound/` | 18 mp3 + 26 wav |
| `en.lproj`, `ko.lproj` | `InfoPlist.strings`, `Scripts.strings`, `ScriptExtra.strings` (binary plists) |
| `English.lproj` | only `v1.0.0.png` |
| root | 115 PNGs, 98 of them named by the ObjC shell ([objc-shell.md](re/objc-shell.md#images-and-the-bundle); the rest are icons, `Default.png`, unused version badges, `MainMenuCompanyLogo.png` and `AlertButton.png`) (`MainMenu*`, `ChooseCharacter*`, `KeyPad*`, `Alert*`, `popup*`, `npc*Button`, `market*`, `cash_*`, `Terms*`, `v1.0.x.png`, icons, `Default.png`), `MainWindow.nib` |

First-look format notes (to be specified properly in `docs/formats/`):

- **`.map`** (specified: [formats/map.md](formats/map.md)): `i16 width, i16 height, u8 layers` (`28 00 1C 00 03` = 40×28, 3 layers for `00.map`), then each layer as byte RLE: `n ≤ 0xF0` + one value = `n` copies, `n > 0xF0` = `n − 0xF0` literal bytes. Cell `0xFF` is empty, and any other value indexes the area's 16×16 tile sheet. Files are named `"%d%d.map"` of (area, map), and the area selects `<area>.mapprj`. Loader `0xD6D1` (`CMMap`) bakes all layers into one RGBA bitmap at load time. All 90 files decode exactly.
- **`.att`** (specified: [formats/att.md](formats/att.md)): magic `03 "ATT"` and a u32 cell count, both ignored by the loader `0xD5B1`, then `(value, count)` byte runs over a 2w × 2h grid (one cell per 8 × 8 px). Values are flags: `0x01` air, `0x02` one-way foothold, `0x04` ladder/rope, `0x08` ladder top, `0x10` wall. All collision is in this grid; the tiles of the `.map` have none. A lookup past the end of the grid returns foothold (`0xBBFD`). All 90 packed files decode exactly. The orphan `106.att` is a headerless raw 44 × 48 grid with no `.map`: a leftover.
- **`.obj`** (specified: [formats/obj.md](formats/obj.md)): `u8 count`, then records `i16 cell, u8 kind, u8 len, name`. `cell` is a tile index; `−1` marks the map-script record (`PerionMap`, …). Kind 0–3 is a portal number (3 hidden), 4–59 an NPC (`npc<kind−4>`, sprite `sprite/mob/<kind>`), 60+ a mob spawn. `name` is the script run on interaction (`/script/chapter%d/` then `/script/common/<name>.out`). Portal destinations are set by those scripts, not by the file. The `|` in the first notes was the low byte of the next record's cell. Loader `0xD259`. All 90 files decode exactly.
- **`.mapprj`** (specified: [formats/mapprj.md](formats/mapprj.md)): `i16 tile_w, i16 tile_h, u8 count`, then `count` Pascal-string names of sheets in `map/tile/` (`0.mapprj` = `10 00 10 00 01 0F "Kerningcity.png"`). One project per area; every shipped one is 16×16 with a single 320 px wide sheet, so tile `v` is the rect `(16·(v mod 20), 16·(v div 20))`. Loader `0xE13D` builds one `CMMapTileSet` (`0xD019`; `+0x34` = `cols × rows`) per name. `map/back/<n>.png` is a fixed 480×320 backdrop per area, drawn at screen (0, 0) with no parallax (`0x86109`; areas 5→4, 7→11, 8→12; `7.png` is never loaded). **`SkyCanvas` is not a sky layer**: it is the engine's screen-canvas singleton (screen size at `+0x88`/`+0x8C`, font name from `ScriptExtra.strings`).
- **`.sprprj`** (specified in [formats/sprprj.md](formats/sprprj.md)): a `u8`-counted image list (u8-length name plus a `u32`-length palette that replaces the PNG's `PLTE`; 163 images use one), then `u8`-counted actions → frames (`i16` delay in game ticks, `u8` layer count) → 8-byte layers (`u8` image, alpha, hflip, visible; `i16` x, y). The loader is `CMSprite` at `0x20005`. Frame delays count game ticks of 40/50/65 ms (`CMTimer`), and actions clamp at their last frame instead of wrapping. All 138 files parse.
- **`png/**/*.dat`** (specified: [formats/png-dat.md](formats/png-dat.md)): **not atlases but packs**: each file is 2–109 complete palette PNGs stored back to back (473 in all), which is why each file starts like a PNG. The engine cuts a pack with a length table compiled into the binary, `u32 lengths[40][255]` at `0xFC740` (`__DATA,__data`, one row per pack), not by rects. Each piece becomes a normal `CMImage` that is drawn whole (loaders `0xF5A9` flat, `0xF6A5` 2-D, `0xF445` 3-D, `0xF34D` one image, all on the resource manager `*(0xF143C)`). The only sub-rects are computed in drawing code: `num.dat` digit strips (width / 10, `0x3A07D`) and `box.dat` corner quadrants (`0x3A899`). The 5 `mainmenu/*.dat` packs are never loaded. All 40 split cleanly and match the table.
- **Data tables** ([spec](formats/data-tables.md)): header `u16 columns, u16 rows, u16 stride`, then `rows` fixed-size records of `u8`/`i16`/`i32`/NUL-padded `char[n]` columns. The game keeps the raw file and reads columns by byte offset through four data-manager accessors (`0x398D5` u8, `0x3793D` i16, `0x385D1` i32, `0x37999` string). Row counts: hero 100, mob 69, npc 39, item_equip 74, item_etc 138, skill 24, market 220. `buildup.dat` is the ASCII integer `"1"`, read with `atoi` into a data-manager field that no code was found to read.

### PNGs & CgBI

- **1,322 PNG files** in total (plus the 40 PNG-in-`.dat`).
- **113 are Apple CgBI "crushed" PNGs**: 112 at the bundle root and 1 in `English.lproj`, i.e. the images Xcode processed for the UIKit shell. **None of the PNGs under `png/`, `sprite/` or `map/` are CgBI**, because the engine's own assets were copied as folder references and are standard PNGs.
- CgBI decoding: drop the `CgBI` chunk, inflate IDAT as raw deflate (no zlib header/Adler-32), swap BGRA→RGBA and un-premultiply alpha. Spec and un-crusher: [formats/cgbi.md](formats/cgbi.md) (`python3 -m mtport.cgbi`). All 113 are 8-bit BGRA with filter Sub on every row; the `CgBI` payload is `30 00 20 02`, or `30 00 20 06` in exactly the 8 fully opaque files. The decoded pixels match `sips` exactly.
- Standard PNGs (1,209 files + 473 in the `.dat` packs) use only palette (8-bit, or 4-bit in 8 pack images, mostly with `tRNS`), RGB 8 and RGBA 8: no grey, no 16-bit, no interlacing. The stdlib codec [`tools/mtport/png.py`](../tools/mtport/png.py) decodes all of them (ADR 0010).

### Localization

- `English/` and `Korean/` tables differ in `item_equip`, `item_etc`, `mob`, `npc` and `skill`. Korean strings are **CP949/EUC-KR** (e.g. `bc d2 c8 af …` decodes to "소환의 비석"). The game converts all table text with `kCFStringEncodingEUC_KR`, and every Korean string is plain EUC-KR. Korean rows match English in every numeric column. `buildup`, `hero` and `market` are byte-identical across languages.
- **`Japanese/data` is a byte-identical copy of `English/data`**, and there is no `ja.lproj`. Japanese isn't actually localized in this build.
- Script/UI text lives in `en.lproj`/`ko.lproj` `Scripts.strings` (2,219 entries) and `ScriptExtra.strings` (527 entries, keyed by C++ `Class.member.index`).
- Korean-specific images exist (`*_kor.png`, `sprite/effect_kor/`).

## Port feasibility notes

- This is a small, **offline**, 2D game with **2,655 functions**, a few hundred KB of real game code once Chartboost is excluded, simple GLES1 drawing, OpenAL + AVAudioPlayer sound and a thin ObjC shell. That makes it a good **static-recompilation** candidate.
- Having `LC_FUNCTION_STARTS` and a parallel ARM-only armv6 slice greatly de-risks function discovery and mode detection.
- **touchHLE** (open-source 32-bit iOS emulator) mainly targets iPhone OS 2–3 era apps. This build relies on iOS 4/5 features (GCD, blocks, `UIGraphicsBeginImageContextWithOptions`, `UIScrollView`/`UITableView`/`UIWebView`, `MPMoviePlayerController`, GameKit), so it likely won't run there unmodified. Its framework implementations (GLES1-on-GL, OpenAL, CoreGraphics subsets, `objc_msgSend` over guest metadata, NIB loading) are a valuable reference.
- The real HLE surface is smaller than 333 imports suggests, because a large chunk only serves Chartboost.

## Corrections to the first inspection notes

| First notes said | Verified |
|---|---|
| "armv7 text is about 979 KB" | 978,944 B is the **encrypted range** (`cryptsize`). `__text` itself is **880,956 B (0xD713C)**. |
| "Roughly 2,000 Thumb prologues; an estimated 2,000–4,000 functions" | `LC_FUNCTION_STARTS` gives **exactly 2,655** (2,654 Thumb + 1 ARM entry stub). |
| "About 25 GL entry points" | **31** (24 core + 7 OES framebuffer). There is no `glClear`. |
| "About 15 app classes" | **18** app classes (as listed) plus **57** Chartboost classes; 4 categories. |
| "1,323 PNGs" | **1,322** `.png` files (plus 40 `.dat` files that are PNGs). |
| "113 CgBI PNGs that need un-crushing" | Correct, but they're **only the root-level/shell UI images** (112 + 1 in `English.lproj`); all engine assets are normal PNGs. |
| "`png/` … 40 `.dat` files" (implied custom format) | All 40 are **packs of plain PNGs** (2–109 each, back to back), cut by a length table in the binary ([formats/png-dat.md](formats/png-dat.md)). |
| "90 maps, each `.map` + `.att` + `.obj`" | 90 `.map`/`.obj`, but **91 `.att`** (orphan `106.att`). |
| "`English/data`, `Korean/data`, `Japanese/`" | `Japanese/data` exists but is **identical to English**. |
| Script text location not mentioned | Dialogue lives in **`*.lproj/Scripts.strings`** (bplist), not in the `.out` files. |
| "NEON is effectively unused" (~100 `vorr` moves) | **381 NEON instructions in 121 functions**, mostly f32 lane arithmetic on D registers and `vcvt` (#30, [ADR 0014](adr/0014-recompiler-design.md)). |
| "CFSocket/gethostbyname (networking, probably ads/analytics)" | Still plausible. The C++ `CMSocket` class also exists, so M1 needs to check what it connects to. |
| Not mentioned | Both slices use VFP; armv7 uses **d16–d31**; no hardware divide; ~1,000 IT blocks, 184 TBB/TBH (132 + 52); 2 static constructors; entry via `LC_UNIXTHREAD`. |

## Open questions

1. ~~What does `CMSocket` connect to, and can it simply be failed?~~: solved, it is the purchase verification server `50.17.236.149:20001`, and yes: a connect that fails at once ends in one dismissible popup, see [re/network.md](re/network.md#cmsocket-the-purchase-verification-server).
2. ~~Exact compression scheme of `.att`~~: solved, see [formats/att.md](formats/att.md) (and [formats/map.md](formats/map.md), [formats/obj.md](formats/obj.md)).
3. ~~Which `png/*.dat` atlases are sliced where (code tables vs. data)?~~: solved, they are PNG packs cut by the length table at `0xFC740`, see [formats/png-dat.md](formats/png-dat.md).
4. ~~What is the script VM's lineage (Lua 4.0 vs. 5.0 vs. custom)?~~: solved, it is Lua 2.4 (`1B` = `ID_CHUNK`, `46` = `ID_FUN`), see [re/script-vm.md](re/script-vm.md#lineage-lua-24).
5. Retina: the only `@2x` files are `Icon@2x.png` and `Icon-Small@2x.png`, so the game renders at 480×320 only. Does the engine still scale its GL viewport on Retina devices?
6. ~~What is the intro movie?~~: dead code. Nothing sends `initWithMovieFile:` or `movieFinishedCallback:`, see [re/program-flow.md](re/program-flow.md#boot).
7. ~~What are the 2 static constructors (`__mod_init_func`)?~~: a text texture cache and a static `TaggedString`, see [re/program-flow.md](re/program-flow.md#static-constructors).
