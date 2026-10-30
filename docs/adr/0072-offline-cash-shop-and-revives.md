# 72. Offline cash shop and revives: an opt-in Assist

- **Status:** Accepted (2026-10-30: option 3, earned with meso, with level-scaled and escalating prices instead of the flat table; Joris on [#156](https://github.com/armaatus/maplethief-port/issues/156#issuecomment-6529085418))
- **Date:** 2026-10-30
- **Issue:** #156
- **Builds on:** [ADR 0034](0034-offline-services.md) (StoreKit sells nothing, sockets fail fast). This decides ADR 0034's option 4 ("offline unlock", out of scope there): the option chosen below. ADR 0034's decision stays the default.

## Context

The original sold 10 products through In-App Purchase: 8 cash shop tiles (Initialize SP, SP Marble SP+1, four meso packs of 22,000 to 300,000 meso, EXP +40 % and +80 %) and two paid revives on the death popup (POW! Package: revive in place + 30 Power Elixirs; Comeback Pack: the same + 40 % EXP). App Store sales and Nexon's verification server are gone, so the port cancels every payment ([ADR 0034](0034-offline-services.md)): the shop and the revive buttons are visible but sell nothing, as on a phone with purchases switched off.

Facts ([re/cash-shop.md](../re/cash-shop.md), 2026-10-30):

- **The server grants, not StoreKit.** StoreKit only produces a receipt. The game stores it in a receipt list (`crestore`, `riprestore1`, `riprestore2`), sends it through `CMSocket` (client op 2), and grants the product only on **op 3 status 0 with the product id**, by that id. The grant is the game's own code in `CDataMgr::OnServerPacket` `0x3ED19`: meso and EXP through `CHero::setMeso`/`setExp`, items through `CHero::addInven`, a revive through `0x6140D`, then a save and "Purchase complete!".
- **The game checks before buying**: inventory room, the 100,000,000 meso cap, level < 99 for EXP, and the 16 maps without a cash shop. A fake store never has to.
- **No price is in the binary.** The game never reads `SKProduct.price`; the App Store sheet showed it.
- **A receipt that is never confirmed blocks.** The shop, and each revive button, first restores a pending receipt, and offline that restore fails with popup `0x20`. So an option must never leave a receipt it cannot also confirm.
- **A grant leaves no trace.** After op 3 the receipt is gone and the product is plain meso, EXP or items in `theif<n>`. Saves stay compatible with the original game either way.
- **Balance anchors.** The median NPC shop listing costs 1,300 meso; the dearest costs 99,000. A Power Elixir is 1,300, so a POW! Package's 30 are worth 39,000 meso. The item table gives Initialize SP a meso value of 10,000 and SP Marble 5,000. Without a revive, the death popup offers *Return to town*, which above level 9 announces an EXP decrease and "Meso: 10% Decrease", and *Go to main menu*.
- **Presets** ([#144](https://github.com/armaatus/maplethief-port/issues/144)): Original, Original + controller, Enhanced and Custom. Gameplay-changing options sit in a visibly separate Assists group and never turn on through a preset.

Whatever is chosen, this is a gameplay change. It is **opt-in under Assists, off in every preset** (only Custom can have it on), and with it off the shop behaves exactly as under ADR 0034.

## Options considered

All the "on" options share one mechanism, described once under [Mechanism](#mechanism-shared-by-options-2-to-4): a fake store and a local fake server for `CMSocket`, so that the game's own checks, receipt lists, grant, save and popups run unchanged.

1. **Off, as today (ADR 0034).** Every payment is cancelled; the loading view closes and nothing changes.
   - Touches: nothing.
   - Gameplay: the original's offline behaviour. The shop and the two revive buttons are dead ends that do nothing visible.
   - Effort: none. Risk: none.
2. **Free.** Every tile and both revives succeed at no cost.
   - Touches: the StoreKit stub (complete instead of cancel, with a synthetic receipt), the socket stub (the fake server), one `settings.ini` key in the Assists group, the preset table (#144), tests.
   - Gameplay: removes the game's economy. One 300,000 meso tap buys the dearest NPC item three times over and can be repeated up to the 100,000,000 cap. SP Marbles bypass skill-point progression, EXP packs bypass levelling (two taps of +80 % are more than a level), and death costs nothing. It is a cheat, not an assist.
   - Effort: about 2 to 3 days. Risk: the mechanism's (below).
3. **Earned with meso.** Each product has a meso price; a purchase succeeds only when the hero can pay, and the price is paid through the game's own `CHero::setMeso(hero, −price, 1)` (which refuses with "Not enough Mesos." or logs "Lost Meso(s)(−N)"). The meso packs are not sold: buying meso with meso means nothing, so those four tiles cancel as today.
   - Touches: option 2's, plus a price table and the payment step in the StoreKit stub (read the hero from guest memory, call `setMeso` before completing; cancel when it refuses).
   - Gameplay: the paid items become a meso sink. The revive becomes a choice between paying meso and Return to town's losses. SP Marbles and EXP packs still shortcut progression, but at a price the player earns by playing. Prices need a playthrough to tune, and fixed prices weigh less as the hero gets richer.
   - Effort: about 3 to 4 days, plus tuning (`needs-human`: game feel). Risk: the mechanism's, plus wrong prices (a setting can be retuned in a later release without breaking saves, since nothing about a purchase is saved).
   - Starting prices as proposed (flat; the decision replaced them with the [pricing model](#pricing), which lands on about these at level 59):

     | Product | Price (meso) | Why |
     |---|---|---|
     | POW! Package (revive + 30 Power Elixirs) | 39,000 | 30 × the NPC shops' 1,300: the revive itself is the bonus |
     | Comeback Pack (+ 40 % EXP) | 79,000 | POW! Package + EXP 40 % |
     | EXP Explosion 40 % | 40,000 | the dearer NPC items' range |
     | EXP Explosion 80 % | 80,000 | twice the 40 % |
     | Initialize SP | 10,000 | the item table's own value |
     | SP Marble SP+1 | 50,000 | ten times the item table's 5,000: SP is the scarcest progression |
     | meso packs | not sold | |

4. **Revives only** (found while mapping). The two death-popup buttons work, free or for meso (option 3's two prices); the cash shop stays as today.
   - Touches: option 2 or 3, limited to `powpackage` and `ComebackPack`.
   - Gameplay: the smallest change. A revive spares the Return-to-town penalty and adds Power Elixirs (and, for the Comeback Pack, EXP); levelling, meso and SP stay the original's.
   - Effort: the same mechanism, so barely less than option 2 or 3. Risk: the same.

Considered and not proposed:

- **Hide the shop and the revive buttons.** It changes the original's UI, not what it sells; the Cash button and the revive images are part of the touch key pad and the death popup. It is a presentation question for a later Enhancements issue, and it can be combined with option 1.
- **Sell the cash items in an NPC shop** (a runtime edit of `market.dat`'s table). It works for the four items but not for the revives, and adds a second path the original never had.
- **Grant directly** (a host override of the tile handlers, or of `CDataMgr::SendServerPacket` feeding `OnServerPacket` by hand). It skips the receipt lists, which is simpler, but it overrides the engine's own functions (since ADR 0034 the only host override is Chartboost's), and a direct `OnServerPacket` call from inside `SendServerPacket` nests the whole grant inside the send. Kept as the fallback if the fake server proves fragile.

### Mechanism (shared by options 2 to 4)

- **Fake store** (`runtime/src/stubs/storekit.cpp`): when the setting allows the product, `-[SKPaymentQueue addPayment:]` completes the transaction (state 1) on the next pump with a synthetic, NUL-terminated text receipt that names the product (for example `maplethief-offline 1 com.nexonmobile.maplestory.thief.powpackage`). A refused product is cancelled as today. Option 3 calls `CHero::setMeso(hero, −price, 1)` first and cancels when it returns 0. Transactions are never redelivered, so `totalrestore` is never written.
- **Fake server** (`runtime/src/stubs/network.cpp`): for `50.17.236.149:20001` only, and only while the setting is on, `CFSocketConnectToAddress` returns 0 and the stubs pump calls the socket's callout (`CMSocket_Callback` `0x1C651`) with type 4 and type 8. `CFSocketSendData` parses the client's frames, and the pump answers through the callout with type 3 and a `CFData`: op 0 → op 1 status 0; op 2 → op 3 status 0 with the product id read from the receipt; op 4 → op 5 status 0; op 0xB4 (keep-alive) → nothing. `close` and `CFSocketInvalidate` end it. Every other connect still fails at once, so the Pigmy hatch and the title's "Verifying…" keep their offline behaviour.
- **Pending receipts.** The grant follows the purchase within a few frames, so a receipt is left behind only if the app is killed in between. To keep the "never leave a receipt it cannot confirm" rule, the fake server also answers restores of its **own** synthetic receipts while the setting is off; it never answers a real App Store receipt. With the setting off and no synthetic receipt pending, the game behaves exactly as under ADR 0034.
- **Setting**: one key in the Assists group of `settings.ini` (for example `cash_shop = off | meso | free`, with only the values of the chosen option), off in every preset (#144). The log names the purchase and the grant.
- **Tests**: in CI, the fake server's protocol against synthetic frames (op 0/2/4 answers, framing, the product id taken from the receipt, no answer when off) and the store's complete/cancel choice. Locally, with real assets: a run that buys one item with the setting on (meso or inventory changed in guest memory, the receipt list back to zero entries, the `theif` bytes saved) and the same tap with it off, which still ends in the cancelled path (ADR 0034's `stubs_real_test`). That is the issue's acceptance criterion.
- **Risks**: the callbacks' timing (the game polls the status from `CStateGame::Paint` and expects the answers on later frames, which the pump gives); a synthetic receipt pending when the setting changes (handled above); and the save files holding synthetic receipts, which the original game on a device would send to a dead server (the same dead end it has today).

## Decision

**Option 3, earned with meso** (Joris, 2026-10-30, [#156](https://github.com/armaatus/maplethief-port/issues/156#issuecomment-6529085418)), with prices that are **not flat**: they follow the hero's level, rise with each fall and purchase, and cool down at the next level-up. The meso packs are not sold. The setting is `cash_shop = off | meso` in the **Assists** group, `off` in every preset (Original, Original + controller, Enhanced); with it off the game behaves as under ADR 0034, except that the fake server still answers restores of the port's own synthetic receipts (never a real App Store receipt).

The mechanism is the one above, with one simplification: the fake server accepts a connection to the receipt server whenever one of the receipt lists holds a synthetic receipt, whatever the setting, which is the pending-receipt rule in one test (with nothing synthetic pending every connect fails at once, as before). The fake store asks a payment policy (`mt_storekit_set_policy`), the policy pays with the game's `CHero::setMeso(hero, −price, 1)`, the transaction completes with the synthetic receipt `maplethief-offline 1 <product id>`, and the fake server confirms it, so the game's own receipt lists, grant, save and popups run. The implementation is `runtime/src/cashshop/` (`maplethief/cashshop.h`), `runtime/src/stubs/storekit.cpp` and `network.cpp`, and the frontend's price line (`platform/src/price_line.cpp`).

### Pricing

One pure function, `mt_cash_price` (and its mirror `tools/mtport/cashprice.py`, which also derives the inputs from the shipped tables):

```
income = exp_to_level × meso_per_exp            the meso a hero earns over its current level
base   = income × share[product]
price  = base × step^min(steps, max_steps)       escalation, capped
price  = max(price, resale[product],              never resold at a profit
             wealth_floor × the hero's meso,      never trivial for a rich hero
             min_price)
price  = rounded up to 10 meso
```

- **The level curve** is the game's own: `exp_to_level` is `CHero::ExpToLevelUp` (hero.dat's EXP column), and `meso_per_exp` is what the shipped monsters drop per EXP they give: the median over the 66 normal monsters of `mob.dat` (2.05 meso per EXP) times the 80 % drop chance, **1.64**. So a product costs the same share of a level's progress at level 10 as at level 70.
- **The shares** put the proposal's anchors at **level 59**, whose income (39,458 meso) is nearest a POW! Package's 39,000 (30 Power Elixirs at the NPC shops' 1,300): the POW! Package and EXP Explosion 40 % cost one level's income, the Comeback Pack and EXP Explosion 80 % two, SP Marble SP+1 1.25, Initialize SP 0.25.
- **Escalation.** A revive's steps are the falls since the last level-up, less one, plus the revives bought since it: the first fall at a level pays the base price, each paid revive and each further fall a step more. Any other product's steps are its purchases at the current level. Each step is ×1.5, at most 4 steps (×5.06).
- **Cool-down**: a level-up clears every count. A new character in a slot starts from zero; loading a slot reads its counts back.
- **Floors.** The resale floor is what the product's items sell back for (`item_etc`'s price, the item's +0x9C): Initialize SP 10,000, SP Marble 5,000, and 30 × 300 = 9,000 for the revives' Power Elixirs, so a purchase never sells back at a profit. The wealth floor is 10 % of the hero's meso, the share Return to town takes, so a rich hero's revive is never cheaper than walking. And at least 100 meso.

The knobs (`mt_cash_knobs`, `mt_cash_default_knobs()`), to tune in a playthrough:

| Knob | Default | What it does |
|---|---|---|
| `meso_per_exp_milli` | 1640 | a level's meso income per EXP, × 1000 |
| `share_pct` | Initialize SP 25, SP Marble 125, EXP 40 % 100, EXP 80 % 200, POW! 100, Comeback 200, meso packs 0 | a product's base price, in percent of a level's income; 0 = not sold |
| `resale` | Initialize SP 10,000, SP Marble 5,000, POW! and Comeback 9,000, EXP packs 0 | the resale floor, in meso |
| `step_pct` | 150 | each escalation step |
| `max_steps` | 4 | the escalation's cap |
| `wealth_floor_pct` | 10 | the floor in percent of the hero's meso |
| `min_price` | 100 | the floor in meso |

What that gives at no step and no meso (`python3 -m mtport.cashprice <English/data>` prints every level):

| Level (EXP to level) | Initialize SP | SP Marble SP+1 | EXP 40 % | EXP 80 % | POW! Package | Comeback Pack |
|---|---|---|---|---|---|---|
| 1 (10) | 10,000 | 5,000 | 100 | 100 | 9,000 | 9,000 |
| 10 (200) | 10,000 | 5,000 | 330 | 660 | 9,000 | 9,000 |
| 30 (5,350) | 10,000 | 10,970 | 8,780 | 17,550 | 9,000 | 17,550 |
| 59 (24,060) | 10,000 | 49,330 | 39,460 | 78,920 | 39,460 | 78,920 |
| 70 (34,650) | 14,210 | 71,040 | 56,830 | 113,660 | 56,830 | 113,660 |
| 99 (71,740) | 29,420 | 147,070 | 117,660 | 235,310 | 117,660 | 235,310 |

A POW! Package at level 59 after one paid revive and a second fall (2 steps) costs 88,790; capped, 199,760.

### The counters

Per save slot, in the port's own file `Documents/maplethief-cashshop<slot>` (`key = value` lines: the level they count at, the falls and paid revives since the last level-up, each product's purchases at this level), written with `mt_saves_replace_file` (atomic, [ADR 0074](0074-save-files-atomic-writes-sanitising-and-ios-documents.md)) at every change. The game's `theif<n>` is never touched, so saves stay the original's. The cash shop learns of the game's events through the observed functions ([ADR 0053](0053-observed-functions-game-events-and-rumble.md), `maplethief/events.h`), with three more observed entries:

| Event | Observed function | Counts |
|---|---|---|
| death popup | `Popup_Open` `0x3EAA1` with id `0x68` | a fall: the first such open since the hero was alive (the game reopens it after each of its sub-popups) |
| revived | the revive `0x6140D` | the hero is up again |
| load / new character | `CDataMgr::LoadSaveData` `0x39989` | the slot in play; a new character (no cached `theif<slot>` buffer) clears the slot's counts |
| level-up | `CHero::LevelUp` `0x65089` (already observed) | clears the counts |

If the hero's level differs from the counters' (a level gained while the setting was off), the counts start afresh at that level.

### The price is shown before the purchase

The game has nothing to hook for it: the tiles and the revive buttons are images, and its own price display was the App Store's sheet. So the **frontend draws a price line**: while the cash shop (game mode `0xB`) or the death popup (`0x68`) is open, a box in the window's bottom-left corner lists what that screen sells and its current price, and the hero's meso ("Cash shop prices in meso (you have 590)", "EXP Explosion 40% 100 ...", "meso packs: not sold"). It is drawn like the frame-time overlay ([ADR 0071](0071-frame-telemetry-and-frame-time-overlay.md)): into the window only, after every read-back, so never in a shot, a golden or a capture. Its quotes come from the run-loop turn, from the same function the store charges with.

### Edge cases

- **Not enough meso**: the game's `setMeso` refuses and shows its own "Not enough Mesos."; the payment is cancelled (the game's cancel path).
- **The game's own checks** (inventory room, the meso cap, level 99, the 16 maps) run before the payment, as in the original.
- **A process killed between the payment and the grant.** The meso is paid in memory and the receipt is in a receipt list; the game saves only after the grant. After a relaunch the meso are back (the save is from before) and the receipt is restored for free by the fake server. Rare (the grant follows within frames), and in the player's favour; accepted.

## Consequences

- `cash_shop = meso` sells six products for meso at a price that rises with the level and with how often the player relied on them at that level; the four meso packs cancel as before. `off` (the default) is ADR 0034's behaviour.
- [ADR 0034](0034-offline-services.md) keeps its decision as the default; its status line notes that this ADR decides its option 4. [ADR 0053](0053-observed-functions-game-events-and-rumble.md)'s observed functions grow by three, and its events by four kinds, with listeners beside the host hook.
- The playthrough that tunes the knobs is `needs-human` (game feel): every knob above can change in a later release without touching a save, since only the port's counters file depends on the model.
- **How we'd know this was wrong:**
  - **A purchase hangs or a receipt stays behind.** For example, the game waits on a status the fake server never raises. Then the fallback is a direct grant (the third rejected alternative).
  - **The prices break the game's pacing.** Then the knobs change; nothing in the game's save depends on them.
  - **The price line is missed or in the way.** Then it moves, or the game's own popup text gets the price through a text hook.
  - **Players never turn it on.** Then option 1's dead ends could instead be hidden, as a presentation choice.
