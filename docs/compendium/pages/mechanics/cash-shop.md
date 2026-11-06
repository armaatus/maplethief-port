# Cash shop and revives

The original game sold ten products for real money through the App Store: eight in the cash shop, opened with the Cash button in the top right corner of the field, and two paid revives on the death popup. A purchase was granted only after a Nexon server confirmed the receipt. That server is gone, so on an original device today nothing can be bought.

## Products

Each product has an App Store identifier, `com.nexonmobile.maplestory.thief.` followed by the suffix in the table. The game holds no prices: the App Store's own payment sheet showed them, and the tiles show none.

| Where | Product | What it grants | Refused when |
|---|---|---|---|
| Cash shop, tile 1 | `InitializeSP` | one {{item:etc:129}} | the inventory is full |
| Cash shop, tile 2 | `SPMarbleSP1Plus` | one {{item:etc:132}} | the inventory is full |
| Cash shop, tile 3 | `mesopack3` | 22,000 meso | the hero would pass 100,000,000 meso |
| Cash shop, tile 4 | `mesopack4` | 50,000 meso | the same |
| Cash shop, tile 5 | `mesopack5` | 120,000 meso | the same |
| Cash shop, tile 6 | `mesopack6` | 300,000 meso | the same |
| Cash shop, tile 7 | `exppack1` | 40% of the EXP the current level needs | the hero is level 99 |
| Cash shop, tile 8 | `exppack2` | 80% of the EXP the current level needs | the hero is level 99 |
| Death popup | `powpackage` | a revive on the spot and 30 {{item:etc:10}} | no room for the elixirs |
| Death popup | `ComebackPack` | the same, plus 40% of the EXP the level needs | no room, or level 99 |

EXP from a pack carries into the next level the usual way ([EXP and levelling](mechanics/exp.html)). The 30 Power Elixirs of a revive are worth 39,000 meso at the 1,300 meso that {{npc:2}} asks for one in {{map:9-0}} ({{area:9}}, chapters 2 and 3) and in {{map:6-0}} ({{area:6}}, chapter 3).

The cash shop does not open on the 16 maps where the game also refuses to save (the tutorial, the boss rooms, three climbs and the Monster Carnival rooms; see [saving](mechanics/saving.html)).

**Example:** a level 10 hero (need 200 EXP) with 150 EXP buys the 80% pack. 150 + 160 = 310 passes 200, so the hero reaches level 11 with 110 EXP.

## Cash items no product grants

The item table has four cash items. {{item:etc:129}} and {{item:etc:132}} come from the tiles above; {{item:etc:128}} and {{item:etc:131}} have no product, no drop and no shop, and are left over from the earlier phone version. The item table gives the four a meso value (Initialize SP and Level Up Marble 10,000, the other two 5,000), which only matters if one is sold. What each item does is on [potions and scrolls](mechanics/items.html#special-items).

## How a purchase worked

1. A tap on a tile or a revive button checked the refusals above, showed a waiting screen and asked the App Store for the product.
2. After the payment, the App Store's receipt was kept in a receipt file on the device.
3. The game connected to Nexon's server, sent the receipt, and granted the product named in the server's answer. Then it removed the receipt from its file and saved.

A receipt that was never confirmed stays in its file, and the game asks for it to be restored before the next purchase. On a device today that restore fails, which blocks the cash shop for good. Once granted, a purchase leaves no trace in the save: it is ordinary meso, EXP or items.

## In the port

The port cancels every payment by default; the optional `cash_shop = meso` assist sells the six products that are not meso for the hero's own meso instead ([the port's offline cash shop, ADR 0072](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0072-offline-cash-shop-and-revives.md)).

## Sources

The cash shop's keys and the death popup's revive buttons in `CStateGame::OnKeyPressed` `0x8D4C9`; the shop panel from `Shell_EnableCashShopKey` `0x66A5`; the App Store request in `-[InAppPurchaseManager loadProductsInfo:]` `0xBF695` and `-productsRequest:didReceiveResponse:` `0xBE559`; the receipt in `-completeTransaction:` `0xBE6D9` and `CStateGame::Paint` `0x87501`; the server exchange in `CDataMgr::ConnectServer` `0x37B55` and `CDataMgr::OnServerPacket` `0x3ED19` (the server was `50.17.236.149:20001`); the grants through `CHero::setMeso` `0x618E1`, `CHero::setExp` `0x6654D`, `CHero::addInven` `0x64309` and the revive `0x6140D`. The full analysis is the project's [cash shop note](https://github.com/armaatus/maplethief-port/blob/main/docs/re/cash-shop.md).
