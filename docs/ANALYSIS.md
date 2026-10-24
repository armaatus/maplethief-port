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

The bundle also contains `_CodeSignature/`, `CodeResources`, `ResourceRules.plist`, `PkgInfo`, an empty 0-byte `MapleThief_armv6` (a Clutch artifact, ignore it) and a stray Chartboost SDK `README.md` ("Version 3.0.4"). `iTunesArtwork` sits next to `Payload/` in the IPA.

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

Linked libraries: SystemConfiguration, Foundation, UIKit, OpenGLES, QuartzCore, CoreGraphics, AudioToolbox, AVFoundation, MediaPlayer, OpenAL, StoreKit (weak), GameKit (weak), `libstdc++.6`, `libgcc_s.1`, `libSystem.B`, CoreFoundation, `libobjc.A`.

### Functions

`LC_FUNCTION_STARTS` is present, so the function count is exact rather than estimated:

- **armv7: 2,655 functions.** 2,654 are Thumb (start address LSB set) and **1 is ARM**, the `start` entry stub at 0x27E0. The last function starts at 0xD98D0.
- **armv6: 2,655 functions**, all ARM.

Symbols are stripped: the only defined symbol is `__mh_execute_header`. Names come from ObjC metadata (≈563 app methods + ≈486 Chartboost methods, about 1,050 of the 2,655 functions), RTTI/vtables and string xrefs.

#### Ghidra cross-check ([#7](https://github.com/armaatus/maplethief-port/issues/7))

Ghidra 12.2 (`ARM:LE:32:v7`), seeded with `LC_FUNCTION_STARTS` and `TMode` before auto-analysis ([ADR 0007](adr/0007-re-tooling-ghidra.md), [tools/ghidra/README.md](../tools/ghidra/README.md)), finds **2,876 functions**:

- All **2,655** starts become functions in the right mode: 0 missing, 0 wrong mode.
- There are **221 extra functions**: the 217 ARM stubs in `__symbolstub1`, one external thunk (`_realloc`), and **3 false positives in `__text`**: 0xAF3BC (24 B), 0xBF680 (20 B) and 0xBF710 (28 B). Each is a table of 32-bit words (e.g. `0x4B586, 0x4B594, …`, likely PC-relative offsets) that the preceding function reads with a PC-relative `ldr` (from 0xAF3AC, 0xBF66E and 0xBF700). Ghidra decodes the tables as Thumb code. Nothing calls them, and no pointer to them exists in the binary. So `LC_FUNCTION_STARTS` is complete, and in-`__text` functions outside it should be treated as data.
- The export has 87,654 xrefs. Its 3,457 strings are 3,089 C strings in the four `S_CSTRING_LITERALS` sections, 367 CFStrings (366 8-bit, 1 UTF-16) and 1 UTF-16 literal in `__ustring`: the Korean Nexon terms of service, 11,987 UTF-16 units at 0xEAEE8.
- Ghidra's *Objective-C Message Analyzer* throws during parallel decompilation of this binary (`ArrayIndexOutOfBoundsException` in `findMessages`), at a different point on each run. The pipeline turns it off so the exports stay reproducible.

### Instruction mix

These counts come from a linear Thumb disassembly of the armv7 `__text`. They're approximate, because literal pools decode as junk.

- **IT blocks:** about 1,000. **`tbb`/`tbh` jump tables:** about 180. **`blx` to immediate:** about 9,100, mostly Thumb→ARM interworking calls into the 4-byte ARM `__symbolstub1` stubs. **`blx reg`:** about 370 (ObjC/C++ virtual and function-pointer calls; `objc_msgSend` itself goes through a stub).
- **No hardware divide.** Integer division goes through `__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`/`__divdi3`/`__udivdi3` imports.
- **VFP is used moderately** (about 4,000 instructions). In descending order: `vmov`, `vldr`, `vstr`, `vpush/vpop`, `vcvt`, `vcmpe` + `vmrs APSR_nzcv`, `vadd`, `vmul`, `vsub`, `vdiv`, a few `vsqrt`. It has both `.f32` and `.f64` arithmetic, and it **uses d16–d31 (VFPv3-D32)**, so the CPU state needs 32 D registers.
- **NEON is effectively unused.** The ~100 `vorr dN, dM, dM` are register moves, and the handful of `vld1/vst1/vhadd/vaddl/vld4` hits sit in literal-pool noise. This needs confirming via a Ghidra-based count ([#34](https://github.com/armaatus/maplethief-port/issues/34)).
- About 60 `dmb ish` barriers and a few `ldrex/strex`, likely inlined atomics (dispatch_once fast path, refcounts).
- The armv6 slice also uses hardware VFPv2 (`vldr/vmov/vstr/vcvt/vcmpe`).

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
| CommonCrypto | 2 | `CC_MD5`, `CC_SHA1` (Chartboost device ID / signing) |
| AVFoundation (C) | 1 | `AVAudioSessionCategoryAmbient` |

**Not imported:** no `fopen/fread`, no `pthread_*`, no `sin/cos`, no `glClear`. All file reads go through CFBundle/CFURL/NSData. All threading is NSThread + GCD. Rotation uses `glRotatex`, and trig is probably done with lookup tables.

Framework classes referenced: `AVAudioPlayer AVAudioSession CABasicAnimation CAEAGLLayer CAKeyframeAnimation EAGLContext GKAchievement GKLeaderboard GKLeaderboardViewController GKLocalPlayer GKPlayer GKScore MPMoviePlayerController NSArray NSAssertionHandler NSAutoreleasePool NSBundle NSCharacterSet NSData NSDate NSDecimalNumber NSDictionary NSError NSFileHandle NSFileManager NSKeyedArchiver NSKeyedUnarchiver NSLocale NSMutableArray NSMutableData NSMutableDictionary NSMutableString NSMutableURLRequest NSNotificationCenter NSNull NSNumber NSObject NSSet NSString NSThread NSTimer NSURL NSURLConnection NSURLCredential NSURLRequest NSUserDefaults NSValue SKPayment SKPaymentQueue SKProductsRequest UIActivityIndicatorView UIApplication UIBezierPath UIButton UIColor UIDevice UIFont UIImage UIImageView UILabel UIPageControl UIScreen UIScrollView UITableView UITableViewCell UITextView UIView UIViewController UIWebView`.

Many of these (NSURLConnection, UITableView(Cell), CA animations, NSDecimalNumber, CommonCrypto, Reachability) are only reachable from Chartboost, which shrinks the real HLE surface once ads are stubbed. [#11](https://github.com/armaatus/maplethief-port/issues/11) should tag each import as *game*, *Chartboost-only* or *both*.

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

Categories (`__objc_catlist`, 4): `UIDevice (IdentifierAddition)` (`uniqueDeviceIdentifier`, `macaddress`, …), `NSString (MD5Addition)`, and `NSArray`/`NSDictionary (ATFunctionalCollectionExtensions)` (`each:`/`map:`/`reduce:`/`filter:`). The first two are the common "UIDevice-with-UniqueIdentifier" snippet. They're probably used by Chartboost or analytics and are safe to stub.

`__objc_protolist` holds 19 protocols: `NSObject`, `NSCoding`, UIKit's `UIApplicationDelegate`, `UIScrollViewDelegate`, `UIWebViewDelegate`, `UITableViewDataSource`/`UITableViewDelegate`, StoreKit's `SKProductsRequestDelegate`, `SKRequestDelegate`, `SKPaymentTransactionObserver`, and 9 Chartboost-internal ones (`CBAPIConnectionDelegate`, `CBJsonStreamParserDelegate`, …). The first pass guessed `GKLeaderboardViewControllerDelegate` and `ChartboostDelegate` here; neither is in the metadata (no class adopts them).

The full recovered metadata is in [docs/re/objc.md](re/objc.md) (generated by `python3 -m mtport.objc generate`, cross-checked against `otool -oV`): 563 app + 486 Chartboost + 15 category methods = **1,064 IMPs**, all distinct and each one an `LC_FUNCTION_STARTS` entry. The armv7 slice has no root classes of its own: every superclass chain ends in an imported UIKit/Foundation class, bound through dyld bind opcodes. `dyld_info -fixups` on macOS 26 lists the right bind addresses but pairs many of them with the wrong symbol for this armv7 binary; use `mtport.macho.MachO.binds` instead.

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

C++ exceptions use **SjLj** (`__gxx_personality_sj0`, `_Unwind_SjLj_*`) with a 4.3 KB `__gcc_except_tab`. libstdc++ is linked dynamically, so STL templates are inlined/instantiated in the binary, but the runtime support (`new`/`delete`, `type_info` vtables) is imported.

`ScriptExtra.strings` keys (see [Localization](#localization)) leak C++ member names such as `CDataMgr.m_strPopupText`, `CDataMgr.drawControlInfo`, `CDataMgr.LoadSaveData`, `CDataMgr.SaveSaveData` and `CDataMgr.m_strMiddleSpotName`. These help with naming.

### RTTI and vtables (#9)

[docs/re/cpp-classes.md](re/cpp-classes.md) (generated by `python3 -m mtport.rtti generate`, naming rules in [ADR 0008](adr/0008-cpp-rtti-and-virtual-function-names.md)) has the hierarchy and every vtable. The facts:

- Every `type_info` vptr is a dyld bind (addend 8) to an imported `__cxxabiv1` vtable: 21 `__class_type_info`, 40 `__si_class_type_info` and 2 `__vmi_class_type_info` (`IStateAdaptor : IState, UtilAdaptor` at +4; `CFloatingBG : CFloatingEff, UtilAdaptor` at +36). All bases are public and non-virtual, so there are no VTTs or construction vtables.
- `Build` is the root of 41 of the 63 classes (`Build` → `CMUtil` → `UtilAdaptor` → …), including `Lua`, `TFunc`, `Undump`, `CMapleObject` and the states (through `IStateAdaptor`). `Build` is a base class, not only a script-VM class.
- 68 vtables (63 primary, 5 secondary at `offset_to_top` -4 or -36), with 266 slots, all in `__DATA,__const` except those of `IState`, `UtilAdaptor` and `CPopup` (`__DATA,__data`). Every non-pure slot is an `LC_FUNCTION_STARTS` function. The 31 pure slots (in `IState`, `IStateAdaptor`, `CMapleObject` and `CFloatingBGAlgorithm`) are bound to `___cxa_pure_virtual`.
- Every class has a virtual destructor in slots 0/1 (D1, D0). At -O0 both come from one body that calls `operator delete` behind a constant flag. 50 of the 63 classes have no other virtual function: that includes `Lua`, `TFunc`, `Undump`, `Table`, `Symbol` and `Build`, so the script VM has no virtual interface. The only interfaces are `IState` (slots 2–22, plus 23 in `IStateAdaptor` and 24 in `CStateMainMenu`), `CMapleObject` (2–4), `CFloatingEff` (2) and `CFloatingBGAlgorithm` (2–4).
- `IState` slot 9 is `Paint` and slot 6 is `OnKeyPressed`, and `CMapleObject` slot 4 is `Flush`. The state and object overrides look up `CStateGame.Paint.N`, `CStateTitle.Paint.N`, `CStateGame.OnKeyPressed.N` and `CHero.Flush.N` keys. `IStateAdaptor` gives default bodies for slots 4, 5, 7, 8 and 11–23, many of them empty (8–12 bytes). The three states override 2, 3, 6, 9, 10 and 22, and `CStateGame` also overrides 4, 5 and 7.
- 45 non-virtual functions look up `Class.method.N` keys of an RTTI class too (`CChatBox.drawChatBox`, `CDataMgr.LoadSaveData`, `CAniManager.makeAni*`, `CMPopup.CMPopup` = the constructor, …). `rtti.py` only names vtable slots from them. Naming the rest is left to program-flow work ([#12](https://github.com/armaatus/maplethief-port/issues/12)).
- In Ghidra, `tools/ghidra/apply-names` with `work/re/names/rtti.json` gives 189 functions a `Class::` name (126 destructors, 10 thunks, 53 other virtuals), plus data labels for the 63 `type_info` objects, their names and the 68 vtables.

## Script VM

- `script/` has **307 `.out` files**: `chapter1/` 27, `chapter2/` 26, `chapter3/` 29, `common/` 225.
- Header (`script/chapter1/Manzie.out`, 336 bytes): `1B 46 00 00 | 17 00 00 00 | 09 00 "Temp.dat\0" | ...`. That is ESC + `'F'` rather than Lua's `ESC "Lua"`, followed by a u32, a u16 length-prefixed source name `Temp.dat`, then what looks like numeric constants and length-prefixed identifiers (`chkQuest`, `dialog`, `tquest`, `mainQuest10`, …).
- RTTI `Lua`/`TFunc`/`Table`/`Symbol`/`Undump`/`Build` suggest a **custom Lua-derived VM**, perhaps with Lua 4-era naming. It is not stock bytecode.
- **Dialogue text is not in the bytecode.** `en.lproj/Scripts.strings` (binary plist, 2,219 entries) maps keys of the form `<Chapter>_<Kind>_<NPC>.<function>.<n>` (e.g. `Chapter1_Quest_Darklord.mainQuest21.3`) to English text. `ko.lproj` has the Korean equivalent. This means the scripts reference strings by (script, function, index), and the disassembler ([#19](https://github.com/armaatus/maplethief-port/issues/19)) can resolve text through this table.

## Rendering

**OpenGL ES 1.1 fixed-function only**, with **31 GL entry points**:

- Core (24): `glBindTexture glBlendFunc glColor4x glDeleteTextures glDisable glDisableClientState glDrawArrays glEnable glEnableClientState glGenTextures glIsTexture glLoadIdentity glMatrixMode glOrthox glPopMatrix glPushMatrix glRotatex glScalef glTexCoordPointer glTexImage2D glTexParameteri glTranslatex glVertexPointer glViewport`
- OES framebuffer (7): `glGenFramebuffersOES glBindFramebufferOES glGenRenderbuffersOES glBindRenderbufferOES glFramebufferRenderbufferOES glGetRenderbufferParameterivOES glCheckFramebufferStatusOES`

Notable absences: **no `glClear`** (every frame presumably overdraws the full screen), no `glColorPointer`, no `glTexSubImage2D`, no `glDrawElements`, no `glReadPixels`, no FBO deletion. Fixed-point `x` variants are used for color, ortho, translate and rotate, while `glScalef` is float. The EAGL drawable is RGBA8 (`kEAGLColorFormatRGBA8`, `kEAGLDrawablePropertyRetainedBacking` is referenced) via `CAEAGLLayer`/`EAGLContext`.

CoreGraphics + UIKit graphics are used to render **text and gradients into bitmaps**, which are then uploaded as textures: `CGBitmapContextCreate`, `CGContextDrawLinear/RadialGradient`, `CGContextSetTextDrawingMode`, `UIGraphicsBeginImageContext(WithOptions)`, `UIFont`, `UIColor`, `UIBezierPath`. PNG decoding goes through `UIImage` → `CGImage` → `CGDataProviderCopyData`, given the `CGImageGet*` introspection imports. The binary contains **no PNG decoder of its own**: there are no zlib/libpng strings (`inflate`, `IHDR`, zlib error messages). The runtime therefore has to decode PNGs on the host. stb_image ([ADR 0003](adr/0003-dependencies.md)) decodes all 1,362 shipped PNGs, and its CgBI output is bit-identical to macOS ImageIO for all 113 CgBI files (checked 2026-10-24).

## Audio

- **OpenAL** (`alGen/DeleteBuffers`, `alGen/DeleteSources`, `alBufferData`, `alSourcei`, `alSourcePlay`, `alListenerf`, `alGetError`, 10 `alc*` calls including `alcGetProcAddress`) with **`ExtAudioFile*`** decoding, which points to the classic Apple "MyOpenALSupport" pattern for SFX. The only extension name in the binary is `alBufferDataStatic`. Nothing stops, pauses, queues or positions a source and nothing queries one: there is no `alSourceStop/Pause`, `alSourcef`, `alGetSource*`, `alSourceQueueBuffers` or `al*3f`. Sounds are fire-and-forget buffers, optionally looped, under a master listener gain.
- **`AVAudioPlayer`** for BGM, plus `AVAudioSession` (category Ambient), `AudioSession*` (C API) and `AudioServicesPlaySystemSound`.
- `sound/` holds **44 files: 18 `.mp3` and 26 `.wav`**, named by number (`0.mp3` … `43.wav`). The numbering is a single sound-ID space, and BGM vs. SFX probably correlates with mp3 vs. wav ([#29](https://github.com/armaatus/maplethief-port/issues/29)).
- **WAV:** all 26 are 16-bit PCM: 13 mono at 8 kHz, 11 mono at 22.05 kHz, and 2 stereo at 44.1 kHz (`10.wav`, `20.wav`).
- **MP3:** `0`–`6`, `30`, `31`, `34` and `42.mp3` (11 files) are MPEG-1 Layer III, 44.1 kHz joint stereo, 128 kbps. `35`–`41.mp3` (7 files) are MPEG-2 Layer III, 22.05 kHz joint stereo, 80 kbps. They are 30.6–61.9 s long and carry ID3v2.2/2.3 tags but **no Xing/LAME info header**, so there is no encoder-delay/padding data for gapless looping.

## Networking, IAP, Game Center

- `CFSocket*`, `gethostbyname`, `inet_*`, `setsockopt`, `if_nametoindex`, `SCNetworkReachability*` and the C++ class `CMSocket` are present, and so are `NSURLConnection` (Chartboost). The URLs in the binary are Nexon/Chartboost marketing links (`global.nexonmobile.com`, `w.nexonmobile.com/CP/*.php`, `maplestory.nexon.net`, an App Store link). **The game has no online play.** The port will run fully offline.
- **StoreKit (cash shop)** product ids: `com.nexonmobile.maplestory.thief.` + `ComebackPack`, `exppack1`, `exppack2`, `InitializeSP`, `mesopack3`–`mesopack6`, `powpackage`, `SPMarbleSP1Plus`.
- **Game Center:** leaderboard `com.nexonmobile.maplestory.thief.totalranking` plus achievements (`reportAchievementWithCompletionHandler:`, `MyAttainedAchievement`).

## Save data

- Strings include `CDataMgr.LoadSaveData`, `CDataMgr.SaveSaveData`, `Documents`, `NSSearchPathForDirectoriesInDomains`, `NSHomeDirectory`, `archivedDataWithRootObject:`, `unarchiveObjectWithData:`, `standardUserDefaults` and `NSFileHandle`.
- Working hypothesis: a C++ save blob written to a file in `Documents/` and/or wrapped in `NSKeyedArchiver` → `NSUserDefaults`. [#20](https://github.com/armaatus/maplethief-port/issues/20) will confirm.

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
| root | about 113 UI PNGs for the ObjC shell (`MainMenu*`, `ChooseCharacter*`, `KeyPad*`, `Alert*`, `popup*`, `npc*Button`, `market*`, `cash_*`, `Terms*`, `v1.0.x.png`, icons, `Default.png`), `MainWindow.nib` |

First-look format notes (to be specified properly in `docs/formats/`):

- **`.map`** (specified: [formats/map.md](formats/map.md)): `i16 width, i16 height, u8 layers` (`28 00 1C 00 03` = 40×28, 3 layers for `00.map`), then each layer as byte RLE: `n ≤ 0xF0` + one value = `n` copies, `n > 0xF0` = `n − 0xF0` literal bytes. Cell `0xFF` is empty, and any other value indexes the area's 16×16 tile sheet. Files are named `"%d%d.map"` of (area, map), and the area selects `<area>.mapprj`. Loader `0xD6D1` (`CMMap`) bakes all layers into one RGBA bitmap at load time. All 90 files decode exactly.
- **`.att`** (specified: [formats/att.md](formats/att.md)): magic `03 "ATT"` and a u32 cell count, both ignored by the loader `0xD5B1`, then `(value, count)` byte runs over a 2w × 2h grid (one cell per 8 × 8 px). Values are flags: `0x01` air, `0x02` one-way foothold, `0x04` ladder/rope, `0x08` ladder top, `0x10` wall. All collision is in this grid; the tiles of the `.map` have none. A lookup past the end of the grid returns foothold (`0xBBFD`). All 90 packed files decode exactly. The orphan `106.att` is a headerless raw 44 × 48 grid with no `.map`: a leftover.
- **`.obj`** (specified: [formats/obj.md](formats/obj.md)): `u8 count`, then records `i16 cell, u8 kind, u8 len, name`. `cell` is a tile index; `−1` marks the map-script record (`PerionMap`, …). Kind 0–3 is a portal number (3 hidden), 4–59 an NPC (`npc<kind−4>`, sprite `sprite/mob/<kind>`), 60+ a mob spawn. `name` is the script run on interaction (`/script/chapter%d/` then `/script/common/<name>.out`). Portal destinations are set by those scripts, not by the file. The `|` in the first notes was the low byte of the next record's cell. Loader `0xD259`. All 90 files decode exactly.
- **`.mapprj`** (specified: [formats/mapprj.md](formats/mapprj.md)): `i16 tile_w, i16 tile_h, u8 count`, then `count` Pascal-string names of sheets in `map/tile/` (`0.mapprj` = `10 00 10 00 01 0F "Kerningcity.png"`). One project per area; every shipped one is 16×16 with a single 320 px wide sheet, so tile `v` is the rect `(16·(v mod 20), 16·(v div 20))`. Loader `0xE13D` builds one `CMMapTileSet` (`0xD019`; `+0x34` = `cols × rows`) per name. `map/back/<n>.png` is a fixed 480×320 backdrop per area, drawn at screen (0, 0) with no parallax (`0x86109`; areas 5→4, 7→11, 8→12; `7.png` is never loaded). **`SkyCanvas` is not a sky layer**: it is the engine's screen-canvas singleton (screen size at `+0x88`/`+0x8C`, font name from `ScriptExtra.strings`).
- **`.sprprj`** (specified in [formats/sprprj.md](formats/sprprj.md)): a `u8`-counted image list (u8-length name plus a `u32`-length palette that replaces the PNG's `PLTE`; 163 images use one), then `u8`-counted actions → frames (`i16` delay in game ticks, `u8` layer count) → 8-byte layers (`u8` image, alpha, hflip, visible; `i16` x, y). The loader is `CMSprite` at `0x20005`. Frame delays count game ticks of 40/50/65 ms (`CMTimer`), and actions clamp at their last frame instead of wrapping. All 138 files parse.
- **`png/**/*.dat`**: **all 40 are ordinary PNG files** (signature `89 50 4E 47`, several palettized) with a different extension. They're presumably sprite-sheet atlases whose slicing rectangles live in code or in other tables.
- **Data tables** ([spec](formats/data-tables.md)): header `u16 columns, u16 rows, u16 stride`, then `rows` fixed-size records of `u8`/`i16`/`i32`/NUL-padded `char[n]` columns. The game keeps the raw file and reads columns by byte offset through four data-manager accessors (`0x398D5` u8, `0x3793D` i16, `0x385D1` i32, `0x37999` string). Row counts: hero 100, mob 69, npc 39, item_equip 74, item_etc 138, skill 24, market 220. `buildup.dat` is the ASCII integer `"1"`, read with `atoi` into a data-manager field that no code was found to read.

### PNGs & CgBI

- **1,322 PNG files** in total (plus the 40 PNG-in-`.dat`).
- **113 are Apple CgBI "crushed" PNGs**: 112 at the bundle root and 1 in `English.lproj`, i.e. the images Xcode processed for the UIKit shell. **None of the PNGs under `png/`, `sprite/` or `map/` are CgBI**, because the engine's own assets were copied as folder references and are standard PNGs.
- CgBI decoding: drop the `CgBI` chunk, inflate IDAT as raw deflate (no zlib header/Adler-32), swap BGRA→RGBA and un-premultiply alpha.

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
| "`png/` … 40 `.dat` files" (implied custom format) | All 40 are **plain PNGs** with a `.dat` extension. |
| "90 maps, each `.map` + `.att` + `.obj`" | 90 `.map`/`.obj`, but **91 `.att`** (orphan `106.att`). |
| "`English/data`, `Korean/data`, `Japanese/`" | `Japanese/data` exists but is **identical to English**. |
| Script text location not mentioned | Dialogue lives in **`*.lproj/Scripts.strings`** (bplist), not in the `.out` files. |
| "CFSocket/gethostbyname (networking, probably ads/analytics)" | Still plausible. The C++ `CMSocket` class also exists, so M1 needs to check what it connects to. |
| Not mentioned | Both slices use VFP; armv7 uses **d16–d31**; no hardware divide; ~1,000 IT blocks, ~180 TBB/TBH; 2 static constructors; entry via `LC_UNIXTHREAD`. |

## Open questions

1. What does `CMSocket` connect to, and can it simply be failed?
2. ~~Exact compression scheme of `.att`~~: solved, see [formats/att.md](formats/att.md) (and [formats/map.md](formats/map.md), [formats/obj.md](formats/obj.md)).
3. Which `png/*.dat` atlases are sliced where (code tables vs. data)?
4. What is the script VM's lineage (Lua 4.0 vs. 5.0 vs. custom)? Does `1B 46` mean "Lua-like, format F"?
5. Retina: the only `@2x` files are `Icon@2x.png` and `Icon-Small@2x.png`, so the game renders at 480×320 only. Does the engine still scale its GL viewport on Retina devices?
6. What is the intro movie? There are **no video files** in the bundle, so `MPMoviePlayerController` either streams a URL or is dead code.
7. What are the 2 static constructors (`__mod_init_func`)?
