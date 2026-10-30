# 72. Offline cash shop and revives: an opt-in Assist

- **Status:** Proposed
- **Date:** 2026-10-30
- **Issue:** #156
- **Builds on:** [ADR 0034](0034-offline-services.md) (StoreKit sells nothing, sockets fail fast). If accepted, this replaces ADR 0034's option 4 ("offline unlock", out of scope there) with the option chosen below; ADR 0034's decision stays the default.

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
   - Starting prices, to be tuned:

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

**Proposed, not decided: Joris picks.** Recommendation: **option 3, earned with meso**, with the meso packs not sold. It keeps the original's items reachable offline without removing the game's economy, it uses the game's own meso check and messages, and its prices are one table to tune in a playthrough. If Joris also wants option 2, it costs almost nothing on top, as a `free` value of the same setting. Option 4 is the choice if only the revives should come back. Whatever is chosen is an Assist, off in every preset, and with it off the game behaves as under ADR 0034.

## Consequences

- If accepted, the implementation is #156's third task: the mechanism above, the setting, the tests, the acceptance criterion's real-asset test, an update of [ARCHITECTURE.md](../ARCHITECTURE.md)'s stubs row, and this ADR's status set to Accepted with the chosen option. ADR 0034 keeps its decision as the default; its status line gets a note that ADR 0072 decides its option 4.
- The playthrough that tunes option 3's prices is `needs-human` (game feel).
- **How we'd know this was wrong:**
  - **A purchase hangs or a receipt stays behind.** For example, the game waits on a status the fake server never raises. Then the fallback is a direct grant (the third rejected alternative).
  - **The prices break the game's pacing.** Then the price table changes; nothing in the save depends on it.
  - **Players never turn it on.** Then option 1's dead ends could instead be hidden, as a presentation choice.
