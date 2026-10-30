# Cash shop and revive purchases

How the game sells its paid items: the cash shop (key pad panel 9), the two paid revives on the death popup, the StoreKit calls behind them, the `CMSocket` exchange that grants them, what each product gives, and what ends up in the save ([#156](https://github.com/armaatus/maplethief-port/issues/156), the input for [ADR 0072](../adr/0072-offline-cash-shop-and-revives.md)). Addresses are armv7 VM addresses; function pointers carry the Thumb bit. `CDataMgr` is the data manager ([save.md](../formats/save.md)), the hero is `CHero` (`CStateGame+0x30`), and the game state is `CStateGame`.

This is static analysis of the armv7 slice (2026-10-30): Ghidra 12.2 decompiles of the functions below (`tools/ghidra/scripts/decompile.py`, with a raised payload limit for `0x3ED19`), the English `ScriptExtra.strings`, the data tables ([data-tables.md](../formats/data-tables.md)) and the tile images, which were read in a scratch directory and are not committed. Nothing was bought: the servers are gone. The general network side (the wire format, every `CMSocket` function, the offline stubs) is in [network.md](network.md).

## Summary

- **10 products, all used.** 8 cash shop tiles and the 2 paid revive buttons each buy one product, `com.nexonmobile.maplestory.thief.<suffix>`. There is nothing else to buy.
- **No price is in the binary.** The game never sends `price`, `priceLocale` or `localizedTitle` (no such send in `work/re/imports.json`), and the tiles show no price. The App Store sheet showed it. Leftover strings of the Korean feature-phone version mention KRW charges.
- **StoreKit only collects a receipt; the server grants.** A purchase ends with an App Store receipt in a receipt list in `Documents/`. The game then connects to `50.17.236.149:20001`, sends the receipt (client op 2), and grants the product only when the server answers **op 3 with status 0 and the product id**. Without that answer nothing is granted.
- **Once granted, a purchase leaves no trace.** The receipt is removed from its list, and the product becomes ordinary meso, EXP or inventory items in `theif<n>`. Nothing in the save says an item was bought.
- **Offline today** ([ADR 0034](../adr/0034-offline-services.md)): every payment is cancelled, so the game closes its "Please Wait..." view and nothing changes.

## Products

### Cash shop tiles (panel 9)

The Cash button (−26) in the field opens the shop (game mode `0x11AA00` = `0xB`, `Shell_EnableCashShopKey` `0x66A5`), unless the map is one of the 16 where the game refuses it (popup `0x25B` "This cannot be used right now.", [ANALYSIS.md](../ANALYSIS.md)) or `crestore` still holds a receipt (popup `0x25D`, [Restores](#restores)). The tiles send −27 … −34 ([keypad.md](../controls/keypad.md#panel-9-the-cash-shop-cashshopkeypadview)); `CStateGame::OnKeyPressed` `0x8D4C9` switches on `key + 34`.

| Key | Tile image: its text | Product | `+0x538` | Refused (popup `0x259`) when | Grant (op 3) |
|---|---|---|---|---|---|
| −27 | `cash_meso_3000.png`: "Initialize SP" | `InitializeSP` | 0 | no inventory room for it: "Not enough room in your inventory." | 1 × Initialize SP (`item_etc` 129, type 7: "Initialize your SP.") |
| −28 | `cash_meso_10000.png`: "SP marble SP + 1" | `SPMarbleSP1Plus` | 1 | no inventory room | 1 × SP Marble SP+1 (`item_etc` 132, type 7: "Increase SP+1.") |
| −29 | `cash_meso_22000.png`: "Bag of Riches 22000 meso" | `mesopack3` | 2 | meso + 22,000 > 100,000,000: "You can't get anymore meso." | +22,000 meso |
| −30 | `cash_meso_50000.png` | `mesopack4` | 3 | meso + 50,000 > 100,000,000 | +50,000 meso |
| −31 | `cash_meso_120000.png` | `mesopack5` | 4 | meso + 120,000 > 100,000,000 | +120,000 meso |
| −32 | `cash_meso_300000.png` | `mesopack6` | 5 | meso + 300,000 > 100,000,000 | +300,000 meso |
| −33 | `cash_exp_40.png`: "EXP Explosion 40%" | `exppack1` | 6 | level ≥ 99: "You can't gain EXP anymore." | +40 % of the EXP the current level needs |
| −34 | `cash_exp_80.png` | `exppack2` | 7 | level ≥ 99 | +80 % of the EXP the current level needs |

The first two image names are misleading: the 3,000 and 10,000 tiles sell the SP items, not meso. Meso goes through `CHero::setMeso` `0x618E1` (it adds, and logs "Meso obtained.(+N)"); EXP through `CHero::setExp` `0x6654D`, which adds and carries the rest over into a level-up (`CHero::ExpToLevelUp` `0x65919` is the step). Items are built by `0x6A9DD` + `0x699A1` (`item_etc` id, count) and added by `CHero::addInven` `0x64309`. Close (−16) returns to the field.

### Revives (death popup `0x68`, panel 7)

When the hero dies the game opens popup `0x68` with four buttons ([keypad.md](../controls/keypad.md#panel-7-popups-popupkeypadview)). `Popup_Open` `0x3EAA1` reads the entry counts of `riprestore1` and `riprestore2` into `CDataMgr+0x694` and `+0x698` as it opens it.

| Key | Button image: its text | Product | `+0x538` | Refused when | Grant (op 3) |
|---|---|---|---|---|---|
| −25 | `popupRevivalButton.png`: "POW! Package: Immediate Revival+30 Power Elixirs" | `powpackage` | 7 | no room for 30 Power Elixirs (popup 600, "Not enough room in your inventory."); `riprestore1` not empty → popup `0x262` (restore) instead | revive in place + 30 × Power Elixir |
| −35 | `popupRevivalButtonPr.png`: "Comeback Pack: Immediate Revival+30 Power Elixirs+40% EXP" | `ComebackPack` | 8 | no room; level ≥ 99 (popup 600, "You can't gain EXP anymore."); `riprestore2` not empty → popup `0x263` | the same + 40 % of the EXP the level needs |
| −5 | Return to town | | | | free: popup `0x69` "Move to the nearest safe village.", which above level 9 announces an EXP decrease and "Meso: 10% Decrease" |
| −16 | Go to main menu | | | | free: popup `0x6D` "Do you really want to go MainMenu?" |

**Revive in place** (op 3, purchase kinds 1 and 2): `0x6140D` sets HP and MP to their maxima (hero `+0x94` ← `+0x9C`, `+0x98` ← `+0xA0`) and the hero's state to 2; hero `+0x124` = 0, `+0x128` = 1; the popup closes, the touch key pad comes back, and the game saves. The Power Elixir is `item_etc` 10 (type 1, restores 100 % HP and MP), sold in NPC shops 1, 13 and 15 for 1,300 meso, so the 30 are worth 39,000 meso.

`+0x538` 7 means `exppack2` in the shop and `powpackage` on the death popup; `CStateGame::Paint` reads it in two different branches, so they never mix.

### Cash items that no product grants

`item_etc` has two more type 7 (cash) items, Level Up Marble Lv+1 (128) and XP 20% Increase (131). No product or code path above grants them: leftovers of the feature-phone version. The item table gives the cash items a meso value (`price`: Initialize SP and Level Up Marble 10,000, SP Marble and XP 20% 5,000).

## The purchase, step by step

1. **Request** (`CStateGame::OnKeyPressed` `0x8D4C9`). After the checks above: a sound (`CMSoundPlayer::Play` 7), `CDataMgr+0x534` = 1 (a purchase is pending), `+0x538` = the kind in the tables, `-[EAGLView addLoadingView]` `0x48C9` (a full-screen "Please Wait..." view), then `-[InAppPurchaseManager loadProductsInfo:]` `0xBF695` with the product id: an `SKProductsRequest` for that one id, with the manager as its delegate. `-makePurchase:` `0xBF561`, the only method that checks `canMakePayments`, is never sent.
2. **Pay** (`-productsRequest:didReceiveResponse:` `0xBE559`). With at least one product, it sets `m_bIsBuyPayment` = 1 and adds `+[SKPayment paymentWithProduct:]` of the first one to the default queue. With invalid identifiers it only removes the loading view (and leaves `+0x534` set, [ADR 0034](../adr/0034-offline-services.md)).
3. **Transaction** (`-paymentQueue:updatedTransactions:` `0xBF0D5`). State 1 (purchased) → `-completeTransaction:` `0xBE6D9`; state 2 (failed) → `-failedTransaction:` `0xBF025` (result 2); state 3 (restored) → `-restoreTransaction:` `0xBF07D` (result 1; never happens, the game never sends `restoreCompletedTransactions`). Each finishes the transaction.
   - `-completeTransaction:` with `m_bIsBuyPayment` set keeps `[[NSData alloc] initWithData:transaction.transactionReceipt]` as `strPuchasedReceipt` (`-getPurchasedReceipt` `0xBF2FD` returns its `bytes`).
   - Without it (a transaction the game did not start in this session, which StoreKit delivers at launch), it appends the receipt to `totalrestore` instead ([Restores](#restores)).
   - **The receipt must be text.** Every reader takes its length with `strlen(bytes)`, so the bytes need a NUL within the data (or right after it).
4. **Keep the receipt** (`CStateGame::Paint` `0x87501`: the cash shop branch, and the popup `0x68` branch). While `+0x534` is set, the first frame with a receipt appends `{flag 0, i16 strlen, bytes}` to `crestore` (cash shop) or to `riprestore1` / `riprestore2` (`+0x538` 7 / 8 on the death popup). It then clears `+0x534`, sets `+0x538` = −1, sets `CStateGame+0x478` (verify pending) and restarts the frame counter `CStateGame+0x80`. If `-getIsPurchaseSuccess` is 2 instead, it clears `+0x534`, removes the loading view and resets the result: the cancel path, which is all the port does today.
5. **Connect** (`CStateGame::Paint`). On frame 2 it sets the purchase kind `CDataMgr+0x688` (0 for the shop, 1 / 2 for the revives) and calls `CDataMgr::ConnectServer` `0x37B55`. From frame 3 it polls `CMSocket::GetStatus` `0x1BC4D`; at 1 it sends op 0 (hello) with `CDataMgr::SendServerPacket` `0x371ED` and clears `+0x478`. A failed connect ends in popup `0x20` ([network.md](network.md#what-happens-offline-on-the-original)).
6. **Verify and grant** (`CDataMgr::OnServerPacket` `0x3ED19`).
   - **Op 1** (hello acknowledged), status 0 → client op 2: `u16 len` + the receipt (op 4 for kind 6). Another status → `removeLoadingView`, `Disconnect(true)`, popup `0x20`.
   - **Op 3** (purchase verified), payload `u16 op, u8 status, u8 len, char product_id[len]`, status 0 → the grant in the tables, chosen by the **product id in the answer**, not by the tile that was pressed. Then the game rewrites the receipt list with one entry fewer (a list of one becomes the two bytes `00 00`; which entry goes differs between the lists and was not pinned down), `Disconnect(false)`, `-removePurchaseReceipt`, `removeLoadingView`, and saves (`SaveSaveData` + `SaveQuestData`, except on the same 16 maps). The shop shows popup `0x26C` "Purchase complete!"; a revive closes the death popup instead.
   - Op 3 with status `0x1E`: no grant, the receipt is still removed, and the shop opens again without a popup (probably "already used"). Any other status → `removeLoadingView`, `Disconnect(true)`, popup `0x20`.

The receipt never reaches Apple from the device: only the Nexon server checked it. So the server's op 3, not StoreKit, is what the game trusts.

## Restores

A receipt stays in its list until a server confirms it, so a purchase interrupted after step 4 is granted later. The game makes the player restore before buying again:

| Trigger | Popup | Kind `+0x688` | Receipt sent | Answer |
|---|---|---|---|---|
| Cash button with `crestore` count > 0 | `0x25D` "One of the items you purchased must be restored before use.(Free)" | 3 | first `crestore` entry (copied to `CDataMgr+0x690`, length `+0x68C`) | op 3 per entry, then the next entry; popup `0x25E` "Restoration Complete!" |
| Revival with `riprestore1` count > 0 | `0x262` (same text), then `0x264` "..." | 4 | first `riprestore1` entry | op 3, revive |
| Grade revival with `riprestore2` count > 0 | `0x263`, then `0x264` | 5 | first `riprestore2` entry | op 3, revive |
| `totalrestore` count > 0, checked by `CStateGame::Paint` in the field | `0x267`, then `0x268` "..." | 6 | first `totalrestore` entry, as op 4 | op 5 per entry; popup `0x269` "Restoration Complete!" |

Offline each of these ends in popup `0x20`. A receipt left in `crestore` therefore blocks the cash shop for good, and one in `riprestore1` / `riprestore2` turns that revive button into a restore that fails. Under ADR 0034 no receipt is ever written, so this cannot happen in the port today.

## What is saved

| File | Bytes ([save.md](../formats/save.md)) | Written |
|---|---|---|
| `crestore` | receipt list: `i16 count`, then `{u8 flag, i16 len, receipt}` | step 4 (shop purchase); shortened at each op 3 |
| `riprestore1`, `riprestore2` | the same | step 4 (POW! Package, Comeback Pack); shortened at op 3 |
| `totalrestore` | the same | `-completeTransaction:` of a transaction from an earlier session; shortened at op 5 |
| `theif<n>` | `meso` `0x011` (i32), `exp` `0x003` (i32), `level` `0x002`, `hp`/`mp` `0x00D`/`0x00F`, `etc_inventory` `0x325` (48 × `{u8 id, i16 count}`) | after op 3, with `quest<n>` |

A receipt is kept as the raw bytes StoreKit gave, with `flag` always 0 where these paths write it. The grant itself is plain character data: there is no purchase flag, count or history, and no checksum.

## Implications for an offline option

From the flow above, to make a purchase work offline without changing the game's code:

- **StoreKit** must complete the payment (state 1) with a non-nil `transactionReceipt` whose bytes are NUL-terminated text, and must not deliver it again at the next launch (or it lands in `totalrestore`).
- **`CMSocket`** must connect: `CFSocketConnectToAddress` returns 0, and the socket's callback (`CMSocket_Callback` `0x1C651`) gets type 4 (connect) and type 8 (write), which makes the status 1. A local server then reads the client's frames from `CFSocketSendData` and answers, through the same callback with type 3 (data) and a `CFData`: op 0 → op 1 status 0; op 2 → op 3 status 0 with the product id; op 4 → op 5 status 0. The product id has to come from the receipt (a restore sends only the receipt), so a synthetic receipt should carry it.
- **A price in meso** fits before the payment completes: `CHero::setMeso(hero, −price, 1)` refuses with the game's own "Not enough Mesos." when the hero cannot pay, and otherwise logs "Lost Meso(s)(−N)". The hero is `CStateGame+0x30` (the current state is `SkyCanvas` `0x11A950` `+0xC`, [ADR 0051](../adr/0051-controller-focus-navigation.md); `OnServerPacket` reaches it through `CDataMgr+0x514`, which step 5 sets), and its meso is `+0xD0`.
- **Everything else stays the game's**: the checks, the receipt lists, the grant, the save and the popups.

[ADR 0072](../adr/0072-offline-cash-shop-and-revives.md) weighs the options.
