# Online features

MapleStory Thief Edition plays fully offline: nothing at start-up waits for the network, and there is no online play. Its online parts were ads, paid items, a leaderboard, and links to Nexon's websites. None of them works as it did, and each one already fails in a way the game handles ([re/network.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/network.md)).

## Ads

The game carries Chartboost 3.0.4, an ad network's library. Six places in the game start a Chartboost session and ask for a full-screen ad, without waiting for the answer. With no answer the game simply goes on. The port answers no to Chartboost at start-up, so none of its code runs ([ADR 0026](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0026-objective-c-strategy.md), [ADR 0034](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0034-offline-services.md)).

## Paid items

The game sold ten products through the App Store ([re/cash-shop.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/cash-shop.md)):

- **The cash shop** (the Cash button in the field): {{item:etc:129}}, {{item:etc:132}}, four bags of meso (22,000, 50,000, 120,000, and 300,000 meso), and two EXP packs (40% and 80% of the EXP the current level needs).
- **The death popup:** two paid revives that bring Robin back on the spot with 30 {{item:etc:10}}, one of them with 40% of a level's EXP as well.

The game shows no prices: the App Store sheet did. Paying alone granted nothing. The game sent the App Store receipt to a Nexon server (a fixed address, port 20001), and only that server's answer added the item, the meso, or the EXP. The server no longer answers, so on a device today a purchase is never granted and the game reports that the connection failed ([re/network.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/network.md#cmsocket-the-purchase-verification-server)). Once granted, a purchase leaves no trace in the save.

In the port every payment is cancelled by default, and the game closes its waiting screen with nothing changed. The Assist `cash_shop = meso` sells the six products that are not meso for the hero's own meso and runs the game's own grant ([ADR 0072](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0072-offline-cash-shop-and-revives.md)).

## Game Center

The game reports one score to a Game Center leaderboard (`com.nexonmobile.maplestory.thief.totalranking`) when the hero changes maps and at one other point, and the main menu's Game Center button shows that leaderboard. It submits no achievements. The port keeps a local Game Center: the button shows the leaderboard and the port closes it at once ([ADR 0084](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0084-offline-polish-hidden-online-ui-local-game-center.md)).

## Push notifications

At start-up the game registers for push notifications and clears the app badge. It keeps the device token for the purchase server and does nothing when a notification arrives. The port registers nothing.

## Links

The main menu and its About screen open six addresses in Safari or the App Store: a More Games page, the mobile games page of Nexon America's MapleStory site, the homepages of Nexon Mobile, Nexon America, and the online MapleStory, and an App Store link to another Nexon game. All are dead links today. Three more items of the engine's own menu would open Nexon Game Box pages of the Korean version, but they quit the game instead (see [bug 2](about/bugs.html#bug-2-the-portal-items-quit-the-game)). The port opens none of these addresses, and its setting `hide_online_ui` hides the buttons.
