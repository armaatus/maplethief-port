# `NIBArchive` (compiled nib)

Apple's binary format for compiled Interface Builder files on iOS. The app ships exactly one, `MainWindow.nib` (1,235 bytes, `NSMainNibFile = MainWindow` in `Info.plist`). `UIApplicationMain` loads it at startup, before the app delegate gets `application:didFinishLaunchingWithOptions:`; what that does is in [program-flow.md → Boot](../re/program-flow.md#boot).

The format is not documented by Apple. This spec is what the parser needs for this file, checked against it byte for byte. Parsers:

- [`tools/mtport/nib.py`](../../tools/mtport/nib.py) (`python3 -m mtport.nib dump|outlets FILE`). Tests: `tests/test_nib.py` (synthetic archives) and `tests/test_nib_real.py` (the shipped nib, local only).
- The runtime's reader, [`runtime/src/uikit/nib.cpp`](../../runtime/src/uikit/nib.cpp) (#54, [ADR 0037](../adr/0037-uikit-host-classes-nib-and-compositor.md)), with the same checks. `UIApplicationMain` decodes the file with it at launch. Tests: `tests/cpp/uikit_test.cpp` (a synthetic `MainWindow.nib`, every truncation and malformed tables) and `runtime.boot_real` (the shipped nib, local only: 23 archive objects decode to 5 guest objects).

## Layout

All integers are little-endian. The file is a header followed by four tables, back to back in this order.

| Offset | Type | Field | MainWindow.nib |
|---|---|---|---|
| 0 | `char[10]` | magic `NIBArchive` | |
| 10 | u32 | format version | 1 |
| 14 | u32 | coder version | 9 |
| 18 | u32, u32 | object count, objects offset | 23, 50 |
| 26 | u32, u32 | key count, keys offset | 29, 119 |
| 34 | u32, u32 | value count, values offset | 66, 593 |
| 42 | u32, u32 | class count, classes offset | 9, 1035 |

A **varint** stores 7 bits per byte, least significant group first. The byte with the high bit **set** is the last one, which is the opposite of LEB128. For example, 300 is `2C 82`.

| Table | Record |
|---|---|
| objects | `varint class_index, varint first_value, varint value_count`. Object *i*'s values are the next `value_count` records of the values table. |
| keys | `varint length, char name[length]` (UTF-8, no terminator) |
| values | `varint key_index, u8 type, payload` |
| classes | `varint length, varint fallback_count, i32 fallback[fallback_count], char name[length]`. The name includes its NUL. No class in MainWindow.nib has fallbacks. |

| Type | Payload |
|---|---|
| 0, 1, 2, 3 | `i8`, `i16`, `i32`, `i64` |
| 4, 5 | none: true, false |
| 6, 7 | `f32`, `f64` |
| 8 | `varint length` + bytes (data) |
| 9 | none: nil |
| 10 | `u32` object index (a reference) |

Object 0 is the root. Its values point at arrays: `UINibObjectsKey` (every object IB knows about), `UINibTopLevelObjectsKey`, `UINibConnectionsKey` and `UINibVisibleWindowsKey`. Array elements are values with the key `UINibEncoderEmptyKey`. An `NSString` keeps its text in `NS.bytes` (data, UTF-8). Geometry such as `UIBounds` and `UICenter` is a data value whose first byte is `0x06`, followed by `f32`s (observed, not documented): `06 | 0 0 320 480` for bounds, `06 | 160 240` for a center. A `UIClassSwapper` stands in for an object of a custom class (`UIClassName`) built from `UIOriginalClassName`. A `UIProxyObject` is a placeholder such as `IBFilesOwner` (the `UIApplication`) or `IBFirstResponder`.

The parsers check every index against its table, refuse gaps, overlaps and trailing bytes, and require the objects' value runs to cover the values table in order. A varint has at most 5 bytes and must fit 32 bits. With those rules, `encode(parse(file)) == file` holds for MainWindow.nib.

## Worked example

The start of each part of `MainWindow.nib` (`xxd`), decoded by hand:

```
00000000: 4e49 4241 7263 6869 7665 0100 0000 0900  NIBArchive......
00000010: 0000 1700 0000 3200 0000 1d00 0000 7700  ......2.......w.
00000020: 0000 4200 0000 5102 0000 0900 0000 0b04  ..B...Q.........
00000030: 0000 8280 8685 8689 ...
```

- Header: magic, format version 1, coder version 9, then (count, offset) pairs: 23 objects at 0x32, 29 keys at 0x77, 66 values at 0x251, 9 classes at 0x40B.
- Objects at 0x32: `82 80 86` is object 0 (class 2, `NSObject`; values 0 to 5) and `85 86 89` is object 1 (class 5, `UIClassSwapper` for the `EAGLView`; values 6 to 14). Each byte is a one-byte varint with the stop bit set.
- Keys at 0x77: `93` then 19 bytes `UIOriginalClassName`, then `86 UIBlue`, ...
- Classes at 0x40B: `8f 80` then `NSMutableArray\0`: name length 15 (with the NUL), no fallbacks.

Values at 0x251:

```
00000251: 8c0a 0800 0000 830a 0400 0000 930a 1400  ................
...
00000271: 1000 0000 9508 9106 0000 0000 0000 0000  ................
00000281: 0000 a043 0000 f043 8a08 8906 0000 2043  ...C...C...... C
00000291: 0000 7043                                ..pC
```

- Value 0 at 0x251: `8c 0a 08000000` is key 12 (`UINibTopLevelObjectsKey`), type 10, object 8 (an `NSArray`).
- Value 6 at 0x277, object 1's first: `95 08 91 06 00000000 00000000 0000a043 0000f043` is key 21 (`UIBounds`), type 8, length 17, then the `0x06` geometry prefix and four `f32`s 0, 0, 320, 480.
- Value 7 at 0x289: `8a 08 89 06 00002043 00007043` is key 10 (`UICenter`), 9 bytes: `0x06`, then 160, 240.

## MainWindow.nib

`python3 -m mtport.nib dump work/ipa/Payload/MapleThief.app/MainWindow.nib`, condensed:

| Object | Class | What it is |
|---|---|---|
| #14 | `UIProxyObject` `IBFilesOwner` | the `UIApplication` |
| #15 | `UIProxyObject` `IBFirstResponder` | |
| #3 | `UIClassSwapper` → `MapleThiefAppDelegate` (from `UICustomObject`) | the app delegate, created by the nib |
| #10 | `UIWindow` | bounds 0, 0, 320, 480, white, `UIAutoresizingMask` 36; the only visible window |
| #1 | `UIClassSwapper` → `EAGLView` (from `UIView`) | the window's only subview, bounds 0, 0, 320, 480, `UIAutoresizingMask` 18; decoded with `-[EAGLView initWithCoder:]` |

Outlet connections (`python3 -m mtport.nib outlets`):

| Source | Outlet | Destination |
|---|---|---|
| `IBFilesOwner` | `delegate` | `MapleThiefAppDelegate` |
| `MapleThiefAppDelegate` | `window` | `UIWindow` |

The delegate's `glView` and `m_ViewController` outlets are not connected. The delegate looks the view up through `+[EAGLView_Instance Instance]` instead, and creates the view controller itself. `application:didFinishLaunchingWithOptions:` asks for a nib named `EAGLViewController`, which the bundle doesn't have (`tests/test_nib_real.py` checks that MainWindow.nib is the only nib). The window gets no `rootViewController`.

## Unknowns

These are questions about Apple's format, not about this game: the binary only hands the file to `UIApplicationMain`, so its code can't answer them. The game ships one nib, and the parser and the runtime's reader need none of the answers to round-trip it byte-exact and decode it, so each is closed with what the shipped file shows.

- ~~**The `0x06` geometry prefix**~~: not needed. All 4 geometry values (`UIBounds`, `UICenter` of the window and the view) start with `0x06` followed by `f32`s, and no other data value in the file starts that way with a whole number of `f32`s after it. The parser keeps the data as bytes; `Value.floats()` and `dump` read it as floats only in that case.
- ~~**Format version 1 and coder version 9**~~: not needed. They are the only values in the one shipped file; the parser accepts and records other versions.
- ~~**Class fallbacks**~~: not needed. No class in the file has any, so the `fallback` entries are covered only by the synthetic tests, where the parser checks each against the class table.
- ~~**Value types not in this file**~~: not needed. The file uses types 0, 4, 5, 6, 8 and 10; types 1, 2, 3, 7 and 9 (`i16`, `i32`, `i64`, `f64`, nil) follow the public descriptions of the format and are covered by the synthetic tests.
- ~~**Root keys** `UINibAccessibilityConfigurationsKey` and `UINibKeyValuePairsKey`~~: not needed. Both point at the same empty array (#16), so there is nothing for the runtime to apply.

## Validation

1 of 1 shipped nibs parses and re-encodes byte-exact (`tests/test_nib_real.py`, run locally against `work/ipa/`), and the runtime's reader parses and decodes it (`runtime.boot_real`).
