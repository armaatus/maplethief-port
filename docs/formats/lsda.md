# LSDA: the SjLj exception tables in `__gcc_except_tab`

The language-specific data areas ("LSDAs") that the game's SjLj exception handling reads ([#37](https://github.com/armaatus/maplethief-port/issues/37), [ADR 0020](../adr/0020-sjlj-exceptions.md)). They are not a file in the app bundle but a section of the armv7 Mach-O: `__TEXT,__gcc_except_tab`, 0x1100 bytes at 0xE9490. The format is the GCC/Itanium LSDA with SjLj call-site records; this spec gives the parts this binary uses and how a throw reads them.

- **Parser:** [`recomp/exceptions.py`](../../recomp/exceptions.py) (`parse_lsda`), and at run time the personality routine in [`runtime/src/sjlj.c`](../../runtime/src/sjlj.c), which reads the same tables from guest memory.
- **Tests:** `tests/test_recomp_exceptions.py` (synthetic tables, every encoding and error), `tests/test_recomp_sjlj.py` (synthetic tables driving the real unwinder), `tests/test_recomp_exceptions_real.py` (local: every shipped LSDA).
- **Status:** **specified.** All 205 LSDAs parse; they tile the section exactly (each starts 4-byte aligned, the gaps are 0-3 zero bytes of alignment padding, nothing else).

## Who uses them

Each of the 205 functions with an SjLj context stores the address of its LSDA in the context, at `fc + 0x1C`, next to the personality routine at `fc + 0x18` (`___gxx_personality_sj0` in 187 functions, `___objc_personality_v0` in 18: 16 Chartboost functions and `+[EAGLView_Instance allocWithZone:]` / `+[EAGLView_Instance Instance]`). The function context:

| Offset | Field | Written by |
|---|---|---|
| +0x00 | `prev`: the next outer context | `_Unwind_SjLj_Register` |
| +0x04 | `call_site`: before a call, the 1-based index of its call-site record, or -1 (no landing pad); after a throw, the landing-pad index | the function; the personality |
| +0x08 | `data[0]`: the exception (`_Unwind_Exception *`) | the personality |
| +0x0C | `data[1]`: the selector (the matching filter; 0 for a cleanup) | the personality |
| +0x10 | `data[2..3]` (unused) | |
| +0x18 | personality routine | the function's prologue |
| +0x1C | LSDA | the function's prologue |
| +0x20 | `jbuf[0]`: r7 | the function's prologue |
| +0x24 | `jbuf[1]`: the resume address (Thumb bit set) | the function's prologue |
| +0x28 | `jbuf[2]`: sp | the function's prologue |

The context lives in the function's frame, 52 to 3,376 bytes above `sp`. Example, `+[EAGLView_Instance Instance]` (0x5839): context at `sp+0x60`, LSDA 0xE9498, resume address 0x58CD written by `mov r4, pc; adds r4, r4, #7; str r4, [r5, #4]` at 0x58C2..0x58C6.

## Layout

All multi-byte fixed-size values are little-endian. ULEB128/SLEB128 are the DWARF variable-length integers; LLVM pads some of them with `0x80` bytes (`82 80 00` is 2) to align the type table.

| Field | Encoding | In this binary |
|---|---|---|
| LPStart encoding | u8 | always `0xFF` (omitted: SjLj landing pads are indices, not addresses) |
| LPStart | pointer in that encoding, if not omitted | - |
| TType encoding | u8 | `0xFF` (no type table) or `0x9B` (`indirect \| pcrel \| sdata4`) |
| TType offset | ULEB128, if TType is not omitted | from the end of this field to the type-table base |
| call-site encoding | u8 | always `0x03` (udata4); **ignored**: SjLj records are ULEB128 whatever it says (GCC's and libc++abi's personalities agree) |
| call-site table length | ULEB128 | bytes of the records that follow |
| call-site records | ULEB128 landing pad, ULEB128 action | record *n* (1-based) belongs to `call_site == n` |
| action table | records of SLEB128 filter, SLEB128 next | starts right after the call-site table |
| type table | entries in the TType encoding, indexed **backwards** from the base | entry *k* is at `base - k * size` |
| exception specifications | ULEB128 type indices, 0-terminated, **after** the base | filter *-f* starts at `base + f - 1` |

**Call-site records.** The landing-pad field is the value the personality writes back into `call_site` when it installs the landing pad: the index the function's resume code dispatches on. In this binary record *n* always has landing pad *n - 1*. The action field is 0 for a cleanup only, otherwise 1 + the offset of the first action record.

**Action records.** `filter > 0` catches type-table entry `filter` (an entry 0 means `catch (...)` / `@catch (...)`); `filter == 0` is a cleanup; `filter < 0` is an exception specification. `next` is the displacement from the `next` field itself to the next record, 0 at the end of the chain.

**Type-table entries.** With `0x9B`: the entry's address plus its signed 32-bit value is the address of a pointer to the `type_info` (GCC's `read_encoded_value`, where a raw 0 stays 0, catch-all).

## What a throw finds

The personality, for a context with `call_site = n`: -1 means no landing pad for this call (skip the frame), 0 means `std::terminate`, otherwise read record *n*. Then, in chain order, a matching catch or a violated exception specification is a handler (selector = its filter); a cleanup filter or an action of 0 makes the frame a cleanup (selector 0). Phase 1 looks for a handler without changing anything; phase 2 runs every cleanup on the way and installs the handler.

What the 1,238 call sites of the binary contain:

| Kind | Call sites | Where |
|---|---|---|
| cleanup only (action 0) | 928 | destructors and `@synchronized` exits; the landing pad ends in `_Unwind_SjLj_Resume` |
| empty exception specification (filter -1, spec list `00`) | 258 | functions declared `throw()`; the landing pad calls `std::terminate` (all 258 `std::terminate` call sites of the binary) |
| catch-all (type entry 0) | 52 | `-[CBJsonStreamParser parse:]` and `-[CBJsonTokeniser getStringToken:]` (Chartboost's two `@catch` blocks) |
| typed catch | 0 | |

There is no typed `catch` anywhere, and no exception specification lists a type.

## Examples

Cleanup only, two call sites, no type table (0xE9498, `+[EAGLView_Instance Instance]`):

```
ff          LPStart omitted
ff          TType omitted
03          call-site encoding (ignored)
04          call-site table: 4 bytes
00 00       call site 1: landing pad 0, cleanup
01 00       call site 2: landing pad 1, cleanup
```

A `throw()` function (0xE9E98, 0x74045):

```
ff 9b 88 00 TType 0x9B, base = 0xE9E9C + 8 = 0xE9EA4
03 04       call-site encoding, 4 bytes of records
00 00       call site 1: landing pad 0, cleanup
01 01       call site 2: landing pad 1, action 1
7f 00       action 1: filter -1, end of chain
00          base: the spec list of filter -1, empty (any exception violates it)
            (3 bytes of padding follow)
```

Chartboost's catch-all (0xEA420, `-[CBJsonTokeniser getStringToken:]`): TType 0x9B with offset `a0 00` (0x20), 12 call sites: 10 lead to action 1, `(1, end)`, a `catch (...)`, and 2 are cleanups. The one type entry, at `base - 4`, is 0.

## Validation

`tests/test_recomp_exceptions_real.py` (local, needs the IPA) parses every LSDA referenced by the 205 contexts, checks that they tile `__gcc_except_tab`, that the call-site encoding is 3, LPStart is omitted, TType is `0xFF` or `0x9B`, every type entry is 0 and every spec list empty, and the counts above. It also runs each function's own resume dispatch for every landing-pad index of its LSDA (`recomp.exceptions.dispatch`) and checks it reaches a landing pad, not the dispatch's trap.
