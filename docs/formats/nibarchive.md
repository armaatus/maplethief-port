# `NIBArchive` (compiled nib)

Apple's binary format for compiled Interface Builder files on iOS. The app ships exactly one, `MainWindow.nib` (1,235 bytes, `NSMainNibFile = MainWindow` in `Info.plist`). `UIApplicationMain` loads it at startup, before the app delegate gets `application:didFinishLaunchingWithOptions:`; what that does is in [program-flow.md → Boot](../re/program-flow.md#boot).

The format is not documented by Apple. This spec is what the parser needs for this file, checked against it byte for byte. Parser: [`tools/mtport/nib.py`](../../tools/mtport/nib.py) (`python3 -m mtport.nib dump|outlets FILE`). Tests: `tests/test_nib.py` (synthetic archives) and `tests/test_nib_real.py` (the shipped nib, local only).

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

The parser checks every index against its table, refuses gaps, overlaps and trailing bytes, and requires the objects' value runs to cover the values table in order. With those rules, `encode(parse(file)) == file` holds for MainWindow.nib.

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

## Validation

1 of 1 shipped nibs parses and re-encodes byte-exact (`tests/test_nib_real.py`, run locally against `work/ipa/`).
