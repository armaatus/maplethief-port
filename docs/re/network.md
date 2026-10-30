# Network paths

Every code path of the armv7 slice that touches the network, what it is for, what the game does when it fails, and the offline behaviour the port should give it ([#23](https://github.com/armaatus/maplethief-port/issues/23), the input for the stubs in [#52](https://github.com/armaatus/maplethief-port/issues/52)). Addresses are armv7 VM addresses; function pointers carry the Thumb bit, instruction sites do not. *Tag* is the [#10](libraries.md) tag of the function.

This is static analysis only. None of the hosts below was contacted.

**Result: the game is fully playable offline.** There is no online play and no network step at boot. The network is used by four things, and each one already has a failure branch in the game that closes the "connecting" UI and returns control to the player:

1. **`CMSocket`**, a raw TCP client to **`50.17.236.149:20001`**, that verifies in-app purchases and restores with a Nexon server ([below](#cmsocket-the-purchase-verification-server)). It runs only after a StoreKit purchase or restore, or from the Pigmy "hatch" dialog. The device id and the APNs token travel in its first packet.
2. **Chartboost 3.0.4** interstitial ads (`https://www.chartboost.com`), started fire-and-forget at 6 game sites. The game never waits for Chartboost.
3. **`UIApplication openURL:`** for 6 promo/About buttons (Safari or the App Store), plus 3 Nexon portal URLs that are never opened and quit the app instead.
4. **Apple services** (StoreKit, Game Center, push registration), which go through their frameworks and are stubbed in #52.

Machine check: `python3 -m mtport.network check` (and `tests/test_network_real.py`) confirms that every site of a network import or a network message in `work/re/imports.json` is named in this document by its address, or sits in a Chartboost function listed in [Chartboost internals](#chartboost-internals). The lists of imports and selectors are `NETWORK_IMPORTS` and `NETWORK_SELECTORS` in `tools/mtport/network.py`. Ghidra names for the game's network functions come from `python3 -m mtport.network names` (`work/re/names/network.json`: 22 functions and 1 data label, no conflicts with the `objc`, `rtti`, `save` and `scriptvm` names files).

## Summary: paths and offline stubs

| # | Path | Owner | Trigger | Game's failure handling | Recommended offline behaviour |
|---|---|---|---|---|---|
| 1 | `CMSocket` TCP to `50.17.236.149:20001` (`CFSocket*`, `inet_addr`, `setsockopt`, `close`) | engine | after a purchase/restore, and the Pigmy hatch dialog | popup `0x20` *"Server connection has failed. Please retry connection or contact us."*, touch keypad re-enabled, pending purchase flags cleared | `CFSocketConnectToAddress` returns `kCFSocketError` (-1) at once, unless a receipt list holds a synthetic receipt of the offline cash shop: then a local fake server ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)); everything else in [CMSocket stubs](#recommended-stubs-for-cmsocket) |
| 2 | `gethostbyname` + `inet_ntoa` | engine | never (the only caller passes `byName = false`) | none (a `NULL` result leaves the address uninitialised) | return `NULL` with `h_errno = HOST_NOT_FOUND`; dead code |
| 3 | Device id: `-[UIDevice(IdentifierAddition) uniqueGlobalDeviceIdentifier]` (`if_nametoindex`, `sysctl` for the `en0` MAC) | lib:uidevice-identifier, called by engine `0x98865` | `CStateTitle` constructor, every boot | none: a nil id **crashes** in `strcpy` | must return a non-nil string: give `en0` a fixed locally administered MAC (or override the method to return a constant MD5) |
| 4 | APNs: `registerForRemoteNotificationTypes:` | app-objc | `application:didFinishLaunchingWithOptions:` | `didFailToRegister…` is empty | no-op, never call back; the token field stays empty |
| 5 | `openURL:` (6 buttons) | app-objc | user taps a promo or About link | none needed | hand the URL to the host browser (desktop) or `UIApplication` (iOS), or log and ignore; the game does not look at the result |
| 6 | Nexon "NGB" portal URLs → `exit(1)` | engine | a main-menu portal item | quits the app | as the original by default; the `OpenPortalURLAndExit` hook makes `0x22955` return without `exit` (#143, [ADR 0069](../adr/0069-guest-function-hooks.md)) |
| 7 | Chartboost: `NSURLConnection`, `SCNetworkReachability*`, `UIWebView`, `canOpenURL:`/`openURL:`, `dataWithContentsOfURL:` | lib:chartboost | `startSession` + `showInterstitial` at 6 game sites | Chartboost skips loading when unreachable; the game ignores the outcome | stub the four Chartboost entry points the game calls (`setAppId:`, `setAppSignature:`, `startSession`, `showInterstitial`) as no-ops; as a fallback, reachability "not reachable" and `NSURLConnection` failing at once |
| 8 | StoreKit, Game Center | app-objc | cash shop, boot | their own error delegates | #52 (StoreKit ADR 0034: every payment cancelled; local Game Center); `cash_shop = meso` pays in meso (ADR 0072) |

None of these paths sits on the way to gameplay: nothing at boot waits for the network (rows 3 and 4 are local calls), and the cash shop, ads and links are optional.

## CMSocket: the purchase verification server

`CMSocket` (RTTI `8CMSocket`, [cpp-classes.md](cpp-classes.md#cmsocket)) is a singleton TCP client on top of `CFSocket`. It is created at startup and talks to **one hard-coded server, `50.17.236.149` port `20001` (0x4E21)**, an Amazon EC2 address used by Nexon to verify StoreKit receipts and restore purchases. It is a leftover of the Korean feature-phone (WIPI) version: most of its UI strings talk about KRW charges, USIM cards and the "Free Hatch Coupon".

### Object and functions

`CMSocket::Instance` `0x1C935` allocates 0x84C bytes and runs the constructor `0x1C8B5`; the startup code `0x5C2D` (called from `-[EAGLView startApp]`) stores it in the global at `0x11A974`, next to the `CDataMgr` singleton at `0x11A970`. The static instance pointer is `0x11AA94`.

| Offset | Field |
|---|---|
| +0x08, +0x0C | host string, port |
| +0x10 / +0x14 | state / **status**: 1 = connected, -1 = not connected (initial value, and after any failure). `CMSocket::GetStatus` `0x1BC4D` returns +0x14. |
| +0x18, +0x1C | connect-callback and write-callback seen; both set → status 1 (`CMSocket::OnSocketEvent` `0x1BBE1`) |
| +0x20 | send queue (capacity 30) of `{u32 length, u8 *bytes}` |
| +0x24..+0x823 | 2 KiB receive buffer; +0x824 bytes expected (4 while waiting for a header), +0x828 bytes held |
| +0x830 | -2 after a send failure |
| +0x844, +0x848 | `CFSocketRef`, its `CFRunLoopSourceRef` |

| Function | Address | Does |
|---|---|---|
| `CMSocket::Connect(host, port, byName)` | `0x1C419` | resets the state, `CFSocketCreate(PF_INET, SOCK_STREAM, 0, data|connect|write callbacks, CMSocket_Callback)`, builds a `sockaddr_in` from `inet_addr(host)` (or `gethostbyname`+`inet_ntoa` when `byName`), sets `SO_NOSIGPIPE`, then **`CFSocketConnectToAddress` with a 10 s timeout, which blocks**. On -1/-2 it runs the failure path below; on 0 it clears +0x18 and starts timer 3. It always adds a run-loop source in `kCFRunLoopCommonModes` afterwards. |
| `CMSocket_Callback` | `0x1C651` | `CFSocketCallBack`: type 4 (connect) and 8 (write) → `OnSocketEvent`; type 3 (data) → `OnData` |
| `CMSocket::Send(payload, len)` | `0x1C31D` | frames the payload, queues it, flushes |
| `CMSocket::FlushSendQueue` | `0x1C1C9` | `CFSocketSendData` per queued packet; 3 consecutive errors or one timeout → `Disconnect(true)`, +0x830 = -2 |
| `CMSocket::OnData(CFDataRef)` | `0x1CA79` | appends to the receive buffer and hands each complete frame's payload to `CDataMgr::OnServerPacket` |
| `CMSocket::Disconnect(notify)` | `0x1BED5` | frees the queue; if connected, `SO_LINGER {1, 0}`, `close`, `CFSocketInvalidate`, releases; status = -1; stops timer 3; `removeLoadingView`; if `notify`, re-enables the touch keypad and opens **popup `0x20`** (*"Server connection has failed…"*, `CDataMgr.m_strPopupText.33`) |
| `~CMSocket` (D1 `0x1C7B5`, D0 `0x1C6B5`) | | frees the queue, invalidates and releases the socket if connected |
| `CDataMgr::ConnectServer` | `0x37B55` | `Connect("50.17.236.149", 20001, false)`, the only caller of `Connect` |
| `CDataMgr::SendServerPacket(op)` | `0x371ED` | builds and sends a client packet (below) |
| `CDataMgr::OnServerPacket(payload)` | `0x3ED19` | handles a server packet (below) |

The little-endian buffer helpers `CMSocket::ReadU16` `0x1BC61`, `WriteU16` `0x1BCB1`, `ReadU32` `0x1BCD5`, `WriteU32` `0x1BD09`, `CopyBytes` `0x1BD41`, `ReadString` `0x1BDB1`, `WriteFixedString` `0x1BE31` and `Swap16` `0x1BBB5` are static members: callers evaluate `CMSocket::Instance()` and drop the result before calling them, the `-O0` code of `CMSocket::Instance()->ReadU16(…)`. The save code (#20) uses them too.

### Wire format

Both directions use the same frame: **`u32 length` (little-endian, frame size including this header) + `u16` (0xFFFF from the client, ignored by the client on receipt) + payload**. A payload starts with a `u16` opcode.

Client → server (`CDataMgr::SendServerPacket`):

| Op | Payload after the opcode | When |
|---|---|---|
| 0 | 370 bytes of fixed, NUL-padded fields: **device id (80)**, **APNs device token as hex (80)**, country code (30), preferred language (30), device model (20), system name + version (20), bundle id (50), `"1.0.3"` (10), `"MapleThief"` (50) | first packet after the connection is up |
| 2 | `u16 len` + receipt bytes: the receipt `InAppPurchaseManager getPurchasedReceipt` returns (purchase kinds 0–2), or a stored one from `CDataMgr`+0x690 (kinds 3–5) | after server op 1 with status 0, kind < 6 |
| 4 | `u16 len` + stored receipt bytes | after server op 1 with status 0, kind 6 (restore) |
| 0xB4 | none | keep-alive, sent by the per-frame update `0x2590D` every 10 s (timer 3) while the status is not -1 |

Server → client (`CDataMgr::OnServerPacket`), payload `u16 op, u8 status, u8 len, char product_id[len]`:

| Op | Meaning | Effect |
|---|---|---|
| 1 | hello acknowledged | status 0 → send op 2 or 4; otherwise `Disconnect(true)` |
| 3 | purchase verified | status 0 or 0x1E: grant the product (`com.nexonmobile.maplestory.thief.*`: meso/EXP packs, SP items, `powpackage`, `ComebackPack`), update `crestore`/`riprestore1`/`riprestore2`/`totalrestore` (#20) and show *"Purchase complete!"* (0x1E: re-enable the cash shop key instead); other status → `Disconnect(true)` |
| 5 | restore result | same product dispatch, then *"Restoration Complete!"*; status other than 0/0x1E → `Disconnect(true)` |

The purchase kind lives at `CDataMgr`+0x688 (0–2 new purchases, 1/2 the in-game revival items, 3–5 re-sending a stored receipt, 6 restore all).

### When it connects

`CDataMgr::ConnectServer` has two callers, `CStateGame::Paint` `0x87501` and `CStateTitle::Paint` `0x981B9`. Both use the same pattern: open a "Connecting to network" screen, connect on frame 2, and from frame 3 poll `GetStatus` until it is 1 (send op 0) or -1 (fail).

| Flow | Opened by | Connected | Failed |
|---|---|---|---|
| **Purchase / revival**: StoreKit reports success and a receipt (`getIsPurchaseSuccess`, `getPurchasedReceipt`); the receipt is written to `crestore` and `CStateGame`+0x478 is set | `-[InAppPurchaseManager completeTransaction:]` → `CStateGame::Paint` (cash shop and death popup `0x68`) | op 0 → op 2 → op 3 | `Disconnect(true)` → popup `0x20` |
| **Restore**: popup `0x267` → `0x268`, stored receipts from `riprestore1`/`riprestore2`/`totalrestore` (kinds 3–6) | `CStateGame::OnKeyPressed` `0x8D4C9` | op 0 → op 2/4 → op 3/5 | popup `0x20` |
| **Popup `0x16`** ("Connecting to Network") → `0x17` ("Transferring Data") | `CStateGame::OnKeyPressed` `0x8D4C9`, `CStateMainMenu::OnKeyPressed` `0x9462D` | op 0 | `Disconnect(true)` → popup `0x20` |
| **Pigmy hatch** (`CStateGame::SetPigmeState` states `0xF` "Connecting to network." → `0x10`) | `CStateGame::OnKeyPressed` (Pigmy dialog) | state `0x10`, whose dialog says *"Server connection has failed…"* (no packet is sent: the online hatch was cut from the iOS build) | `Disconnect(false)`, state `0x11` |
| Title "Verifying…" (popup `0x16` while the title state paints → `0x2D`, failure `0x20`; `0x2E` = *"Cannot verify your account. Customer Center…"*) | WIPI account check; no title code opens popup `0x16` itself | popup `0x2D` | `Disconnect(true)` + popup `0x20` |

Without a working StoreKit purchase, only the Pigmy hatch and popup `0x16` reach the socket, and both end in a dismissible message.

### What happens offline on the original

`CFSocketConnectToAddress` blocks the main thread for up to 10 s and returns -1 or -2. `Connect`'s failure branch then calls `removeLoadingView`, `Disconnect(true)` (status -1, popup `0x20`, touch keypad re-enabled) and clears `CStateGame`+0x478..+0x47B, so the pending purchase is not retried on the next frame. The next `Paint` sees status -1 and runs its own failure branch once. There are no retry loops: the player is back in control after one popup. A purchase that StoreKit charged but the server never confirmed stays in `crestore` and can be retried through *Restore*.

### Recommended stubs for CMSocket

The recompiled `CMSocket` code stays as it is; the stubs are in the HLE imports. **Implemented** in #52 (`runtime/src/stubs/network.cpp`, [ADR 0034](../adr/0034-offline-services.md)) for the `CFSocket*` rows, and in #156 ([ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)) with the fake server [below](#the-fake-receipt-server-adr-0072). The table is the default: no synthetic receipt waits (with `cash_shop = off`, ADR 0034 cancels every payment, so none is ever written).

| Import | Sites (function) | Stub |
|---|---|---|
| `CFSocketCreate` | `0x1C48A` (`Connect`) | return a valid dummy `CFSocketRef` that remembers its callback |
| `inet_addr` | `0x1C4EA`, `0x1C518` (`Connect`) | real implementation (pure function) |
| `gethostbyname`, `inet_ntoa` | `0x1C4A8`, `0x1C4E6` (`Connect`, dead) | `NULL` / real `inet_ntoa` |
| `CFSocketGetNative` | `0x1C530` (`Connect`), `0x1BFA0` (`Disconnect`) | return -1 |
| `setsockopt` | `0x1C546` (`Connect`, `SO_NOSIGPIPE`), `0x1BFBE` (`Disconnect`, `SO_LINGER`) | return 0 |
| `CFSocketConnectToAddress` | `0x1C55C` (`Connect`) | **return `kCFSocketError` (-1) immediately**, no 10 s wait, never fire callbacks |
| `CFSocketCreateRunLoopSource` | `0x1C626` (`Connect`) | return a dummy source |
| `CFRunLoopAddSource` | `0x1C648` (`Connect`) | accept and ignore |
| `CFSocketSendData` | `0x1C212` (`FlushSendQueue`) | return `kCFSocketError` (unreachable once connect fails) |
| `close` | `0x1BFC6` (`Disconnect`) | return 0 (only reached when connected) |
| `CFSocketInvalidate` | `0x1BFE4`, `0x1C004` (`Disconnect`), `0x1C786` (D0), `0x1C886` (D1) | no-op |

Never let a connect succeed by default: the title flow then sits on "Verifying…" with no request sent, and the Pigmy flow shows its failure text anyway.

### The fake receipt server (ADR 0072)

With `cash_shop = meso` the StoreKit stub completes a paid purchase with a synthetic receipt, the NUL-terminated text `maplethief-offline 1 <product id>` (`MT_STUBS_RECEIPT_PREFIX`, `mt_stubs_receipt_text`), which the game keeps in a receipt list and sends here. The `CFSocket*` stubs are then a **local fake server** ([cash-shop.md](cash-shop.md#the-purchase-step-by-step)):

| Import | With a synthetic receipt waiting |
|---|---|
| `CFSocketConnectToAddress` | to `50.17.236.149:20001` (the `sockaddr_in` in the `CFData`), while `crestore`, `riprestore1`, `riprestore2` or `totalrestore` holds a synthetic receipt: returns 0, and the next stubs pump calls the socket's callout (`CMSocket_Callback` `0x1C651`) with type 4 (connect) and type 8 (write), which make `CMSocket`'s status 1. Any other address, or no synthetic receipt: -1 at once, as above |
| `CFSocketSendData` | reads the client's frames (`{u32 length incl. the header, u16 0xFFFF, payload}`, several to a send) and queues an answer per frame for the next pump: a `CFData` frame `{u32 length, u16 0, u16 op, u8 status, u8 len, char id[len]}` through the callout with type 3. Op 0 (hello) → op 1 status 0; op 2 / op 4 (`{u16 len, receipt}`) → op 3 / op 5 status 0 with the product id the synthetic receipt names; **any other receipt → status 1** (the game's popup `0x20`, as the dead server's); op `0xB4` (keep-alive) → nothing. A frame whose length runs past the data is ignored |
| `CFSocketInvalidate` | ends it: answers still queued are dropped, and later sends fail |

It answers whatever the setting, so a receipt a killed session left behind is restored for free; it never confirms a real App Store receipt. `mt_stubs_get_server_stats` counts the connects, the frames, the answers and the confirmations (tests). The Pigmy hatch and the title's "Verifying…" never have a synthetic receipt behind them, so they keep their offline behaviour.

## Device identifier and push token

The CStateTitle constructor `0x98865` fills fixed fields of the `CDataMgr` singleton that become the op 0 packet: +0x53C `[[UIDevice currentDevice] uniqueGlobalDeviceIdentifier]` (send at `0x98974`), +0x5DC `NSLocaleCountryCode`, +0x5FA `[NSLocale preferredLanguages][0]`, +0x618 `model`, +0x62C `name` (gathered, not sent), +0x640 `systemName` + `systemVersion`, +0x654 the bundle id. **The device id therefore leaves the device only inside `CMSocket` op 0** (bytes 2–81 of the payload), never through Chartboost or a URL. It is never stored on disk (#20).

`-[UIDevice(IdentifierAddition) uniqueGlobalDeviceIdentifier]` `0xC1299` is the MD5 (`NSString(MD5Addition)`) of `-macaddress` `0xC1025` (send at `0xC12C0`), which reads the `en0` MAC with `if_nametoindex` (`0xC106A`) and two `sysctl(NET_RT_IFLIST)` calls (`0xC10B2`, `0xC1134`). `-uniqueDeviceIdentifier` `0xC11D9` (MD5 of MAC + bundle id; send of `-macaddress` at `0xC1200`) is never called. If `if_nametoindex` fails, `-macaddress` returns nil, the id is nil, `UTF8String` returns `NULL` and the constructor's `strcpy` **crashes**. The HLE must give `en0` a fixed locally administered MAC (for example `02:00:00:00:00:00`) through `if_nametoindex`/`sysctl`, or replace the method's result with a constant. The other fields have the same `strcpy` hazard: every `UIDevice`/`NSLocale`/`NSBundle` string stub must return a non-nil string.

The APNs token (+0x58C) is written only by `-[MapleThiefAppDelegate application:didRegisterForRemoteNotificationsWithDeviceToken:]` (`0x29C5`, 32 bytes as hex). Registration is requested at `0x2CF6` in `application:didFinishLaunchingWithOptions:` (`0x2BFD`), which also clears the badge; `didFailToRegister…` and `didReceiveRemoteNotification:` are empty. Stub: accept the registration and never call back.

## URLs

### Opened by the game

All six go through `[[UIApplication sharedApplication] openURL:[NSURL URLWithString:…]]` in `MainMenuView` (app-objc), the result is ignored, and none is needed to play:

| Button | Function | `NSURL` load / `URLWithString:` / `openURL:` | URL |
|---|---|---|---|
| promo banner ("AD") | `-[MainMenuView clickADButton:]` `0xA8521` | `0xA854C` / `0xA8564` / `0xA8570` | `http://itunes.apple.com/us/app/maplestory-live/id479812410?l=ko&ls=1&mt=8` (App Store: *MapleStory Live*) |
| More Games | `-[MainMenuView clickMoreGame:]` `0xA8629` | `0xA8654` / `0xA866C` / `0xA8678` | `http://global.nexonmobile.com/App/?app=MapleThief` |
| About link 1 | `-[MainMenuView clickLinkURL1:]` `0xAAA15` | `0xAAA40` / `0xAAA58` / `0xAAA64` | `http://maplestory.nexon.net/FunStuff/MobileGames.aspx` |
| About link 2 | `-[MainMenuView clickLinkURL2:]` `0xAA9A5` | `0xAA9D0` / `0xAA9E8` / `0xAA9F4` | `http://global.nexonmobile.com` |
| About link 3 | `-[MainMenuView clickLinkURL3:]` `0xAA935` | `0xAA960` / `0xAA978` / `0xAA984` | `http://www.nexon.net` |
| About link 4 | `-[MainMenuView clickLinkURL4:]` `0xAA8C5` | `0xAA8F0` / `0xAA908` / `0xAA914` | `http://maplestory.nexon.net` |

`-[MainMenuView clickAbout:]` `0xA8A0D` shows the same addresses as text only. Stub for `openURL:`: return `NO` and log, or pass the URL to the host's browser behind a setting; nothing in the game depends on it. These sites are dead links today, so a port can also hide the buttons.

### Never opened: the Nexon portal URLs

`CStateMainMenu::OnKeyPressed` `0x9462D` passes `http://w.nexonmobile.com/CP/winc581.php?GV=35`, `…/CP/gift.php?GV=35` and `…/CP/gamezone.php?GV=35` to `OpenPortalURLAndExit` `0x22955` (main-menu popup 1, sub-menu 3, items 2–4). It calls `0x6015`, an empty `return 0` stub left where the Korean "NGB" (Nexon Game Box) hand-off was (`("NGB", 2, "URL", url)`), and then **`exit(1)`** through `0x8FF9`. No URL is ever opened. Whether the English build can reach these items by touch is still open. Since #143 the port can make `0x22955` return instead of quitting: the guest function hook `mt_hooks_install_portal_no_exit`, on with the setting `fix_original_bugs` of the Enhanced preset ([ADR 0069](../adr/0069-guest-function-hooks.md), [original-bugs.md](original-bugs.md)). `OnKeyPressed` then takes the switch's break after the call (`b 0x961A2` → `b 0x961EA`) and the menu stays. `runtime.hooks_portal_on` / `_off` (real assets) send OK to items 2–4 through `OnKeyPressed`: with the hook the game goes on, and without it the run ends with `exit(1)`.

### Local file URLs (not network)

`NSURL` sites that build file URLs for bundled resources: `0x1D1D4` in `0x1D119` and `0x1D730` in `0x1D67D` (`initFileURLWithPath:` for `AVAudioPlayer`, sound), and `0x9FB9C` in `-[MoviePlayerController initWithMovieFile:]` `0x9FB45` (`fileURLWithPath:` for `MPMoviePlayerController`). The movie is a local file, so the open question about a streamed intro is settled: **there is no network movie.**

## Chartboost

### The game's calls

The game starts Chartboost 3.0.4 with app id `4e950ebec46988ce5b000010` and signature `4048302a93816ccb222438a2f358bb6ffe1b8369` and immediately asks for an interstitial, six times. It never sets a delegate (no game class adopts `ChartboostDelegate`) and never checks the outcome:

| Function | `+sharedChartboost` / `setAppId:` / `setAppSignature:` / `startSession` / `showInterstitial` | When |
|---|---|---|
| `CStateMainMenu::OnKeyPressed` `0x9462D` | `0x95406` / `0x9542A` / `0x9544E` / `0x95468` / `0x95482` | starting the game from the main menu (slot loaded) |
| `CStateMainMenu::OnKeyPressed` `0x9462D` | `0x9579C` / `0x957C0` / `0x957E4` / `0x957FE` / `0x95818` | starting the game from the main menu (selected character) |
| `CStateMainMenu::OnKeyPressed` `0x9462D` | `0x9642A` / `0x9644E` / `0x96472` / `0x9648C` / `0x964A6` | starting the game from the main menu (character list) |
| `CStateGame::OnKeyPressed` `0x8D4C9` | `0x8F4D4` / `0x8F504` / `0x8F530` / `0x8F552` / `0x8F574` | in game, only if `-[EAGLView getGameCenterLoginOKorError]`; then `showMainMenu:` |
| `0x54BF9` | `0x55360` / `0x55382` / `0x553A2` / `0x553B8` / `0x553CE` | in game, only if `getGameCenterLoginOKorError`; then `showMainMenu:` |
| `0x83C71` (from `CStateGame::Paint`) | `0x84570` / `0x84594` / `0x845B6` / `0x845CE` / `0x845E6` | in game, only if `getGameCenterLoginOKorError` |

Stub (as [ARCHITECTURE.md](../ARCHITECTURE.md) plans): make `-[Chartboost setAppId:]` `0xC5189`, `-setAppSignature:` `0xC51C5`, `-startSession` `0xC3081` and `-showInterstitial` `0xC32C9` no-ops (or `+sharedChartboost` `0xC2E05` return nil, which turns all four sends into nil messages). Then none of the Chartboost code below runs. Defence in depth, if any of it is reached: `SCNetworkReachabilityGetFlags` returns flags 0 (Chartboost then reports "no internet" and skips loading), `-[NSURLConnection initWithRequest:delegate:startImmediately:]` delivers `connection:didFailWithError:` on the next run-loop turn, `+[NSData dataWithContentsOfURL:]` returns nil, `canOpenURL:`/`openURL:` return `NO`, and `UIWebView` loads nothing.

### Chartboost internals

Every Chartboost (`lib:chartboost`) function with a network import or message site, so that the coverage check can account for all of them. All are unreachable once the four entry points above are no-ops.

| Function | Network sites |
|---|---|
| `-[Chartboost loadInterstitial:shouldCache:]` `0xC36BD` | `+reachabilityForInternetConnection`, `-currentReachabilityStatus` |
| `-[Chartboost loadMoreAppsShouldCache:]` `0xC3A79` | the same |
| `-[Chartboost impressionClickTriggered:forURL:moreData:]` `0xC4729` | `NSURL` ×3, `+URLWithString:` ×3, `-canOpenURL:` |
| `-[CBAPIConnection sendRequest:]` `0xC5375` | reachability; `NSMutableURLRequest`, `NSURL`, `+URLWithString:`, `CBTaggedURLConnection -initWithRequest:delegate:startImmediately:` (base `https://www.chartboost.com`, headers `X-Chartboost-Client`/`-API`); `+sharedChartboost` ×3 |
| `-[CBAPIConnection retryDeliveries]` `0xC59C9` | reachability |
| `-[CBAPIConnection connection:canAuthenticateAgainstProtectionSpace:]` `0xC5B45` | `NSURLAuthenticationMethodServerTrust` |
| `-[CBAPIConnection connection:didReceiveAuthenticationChallenge:]` `0xC5B7D` | `NSURLAuthenticationMethodServerTrust`, `NSURLCredential` (trusts `www.chartboost.com`) |
| `-[CBAPIRequest appendDeviceInfoParams]` `0xC645D` | reachability, `+sharedChartboost` |
| `-[CBDefaultViewController orientView:]` `0xC88C9` | `+sharedChartboost` |
| `-[CBURLOpener open:]` `0xC98F1` | `-openURL:`, `NSURLRequest +requestWithURL:`, `NSURLConnection -initWithRequest:delegate:startImmediately:` (follows click redirects) |
| `-[CBURLOpener connectionDidFinishLoading:]` `0xC9AA5` | `-openURL:` |
| `-[CBReachability startNotifier]` `0xC9C15` | `SCNetworkReachabilitySetCallback`, `…ScheduleWithRunLoop` |
| `-[CBReachability stopNotifier]` `0xC9E3D` | `SCNetworkReachabilityUnscheduleFromRunLoop` |
| `+[CBReachability reachabilityWithHostName:]` `0xC9EC9` | `SCNetworkReachabilityCreateWithName` |
| `+[CBReachability reachabilityWithAddress:]` `0xC9F49` | `SCNetworkReachabilityCreateWithAddress` |
| `+[CBReachability reachabilityForInternetConnection]` `0xC9FC1` | `+reachabilityWithAddress:` |
| `+[CBReachability reachabilityForLocalWiFi]` `0xC9FF1` | `+reachabilityWithAddress:` |
| `-[CBReachability connectionRequired]` `0xCA07D` | `SCNetworkReachabilityGetFlags` |
| `-[CBReachability currentReachabilityStatus]` `0xCA12D` | `SCNetworkReachabilityGetFlags` |
| `+[CBIdentity cbma]` `0xCA23D` | `if_nametoindex`, `sysctl` ×2 (Chartboost's own MAC address) |
| `-[CBImpression initWithResponse:type:]` `0xCFD61` | `NSURL` |
| `0xD050D` (block) | `-loadHTMLString:baseURL:` |
| `0xD0645` (block) | `NSURL`, `+URLWithString:`, `-canOpenURL:` |
| `-[CBNativeInterstitialView layoutSubviews]` `0xD1935` | `+sharedChartboost` |
| `-[CBNativeInterstitialView drawRect:]` `0xD1ED1` | `+sharedChartboost` |
| `-[CBWebImageCache fetchAndCacheImageWithURL:checksum:]` `0xD2A9D` | `NSURL`, `+URLWithString:`, `+dataWithContentsOfURL:` |
| `+[CBAnimationManager transitionInWithAnimationType:layer:animationConfigurationBlock:]` `0xD3B4D` | `+sharedChartboost` ×2 |
| `+[CBAnimationManager transitionOutWithAnimationType:layer:animationConfigurationBlock:]` `0xD4E35` | `+sharedChartboost` ×2 |
| `-[CBNativeMoreAppsView layoutSubviews]` `0xD6431` | `+sharedChartboost` ×2 |
| `0xD759D` (block) | `NSURL`, `+URLWithString:`, `-canOpenURL:` |
| `-[CBNativeMoreAppsFeaturedCell prepareWithCellMeta:]` `0xD7F75` | `+sharedChartboost` ×2 |
| `-[CBNativeMoreAppsRegularCell prepareWithCellMeta:]` `0xD86A9` | `NSURL`, `+URLWithString:`, `-canOpenURL:` |
| `-[CBNativeMoreAppsWebViewCell initWithStyle:reuseIdentifier:]` `0xD8B45` | `UIWebView` |
| `-[CBNativeMoreAppsWebViewCell prepareWithCellMeta:]` `0xD8C61` | `NSURL`, `-loadHTMLString:baseURL:` |

Data uses without code sites: `CBWebView` is a `UIWebView` subclass and `CBTaggedURLConnection` an `NSURLConnection` subclass (the `UIWebView` and `NSURLConnection` class and metaclass binds in [imports.md](imports.md)). The game itself has no `UIWebView`: the Korean terms of service in `TermsView` are a `UIScrollView` of bundled text.

## Other online services

The cash shop and revive purchases end to end (products, grants, receipt lists, restores) are in [cash-shop.md](cash-shop.md) (#156, [ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)). These go through Apple frameworks rather than sockets and are specified in #52: StoreKit (`-[InAppPurchaseManager loadProductsInfo:]`/`makePurchase:`, products listed in [ANALYSIS.md](../ANALYSIS.md#networking-iap-game-center)), Game Center (`-[GameCenterManager AuthenticationUser]`, `ReportScore:forCategory:`, achievements) and push registration (above). StoreKit is the only one that leads into `CMSocket`.

## Method

1. #11's `work/re/imports.json` (code sites of every import and every `objc_msgSend` with its selector) and #10's `work/re/function-tags.json` give the candidate sites; `python3 -m mtport.network sites` prints them.
2. Every game-side function was decompiled with Ghidra 12.2 (a throwaway PyGhidra script over the project from `tools/ghidra/import`; `0x3ED19` needs a raised decompiler payload limit) and read by hand: `CMSocket::*`, `CDataMgr::{ConnectServer,SendServerPacket,OnServerPacket}`, the callers in `CStateGame`/`CStateTitle`/`CStateMainMenu`, the app delegate, `MainMenuView`, `UIDevice(IdentifierAddition)`.
3. Strings and their referencing functions come from `work/ghidra/strings.json`; popup texts are `CDataMgr.m_strPopupText.<id+1>` and the other UI strings are `ScriptExtra.strings` keys (`en.lproj`).
4. `tests/test_network_real.py` checks the coverage of this document and that the names in `work/re/names/network.json` sit on functions with the expected imports.
