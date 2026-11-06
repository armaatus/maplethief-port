# History

MapleStory Thief Edition is a single-player, side-scrolling role-playing game for the iPhone and iPod touch, set in the world of the online game MapleStory. The player is always Robin, a Thief who later chooses between the Assassin and Bandit branches, and the story runs through a short opening and three chapters in and around Kerning City, Perion, Henesys, Orbis, El Nath, and Magatia.

## Publisher and developer

- **Bundle.** The app's identifier is `com.nexonmobile.maplestory.thief`, and its purchase products and Game Center leaderboard use the same prefix ([ANALYSIS.md](https://github.com/armaatus/maplethief-port/blob/main/docs/ANALYSIS.md#app-bundle)).
- **In the game.** The About screen sends support questions to a page of Nexon America's MapleStory site and lists the homepages of Nexon Mobile and Nexon America. The copyright line under the main menu is an image (`MainMenuCopyRight.png`) that names NEXON Korea Corp. and NEXON America Inc., with the year 2012. The launch image (`Default.png`) shows the Nexon logo. See [Credits](about/credits.html).
- **Developer.** No file names a studio. Pocket Gamer lists the developer as Nexon ([Pocket Gamer game page](https://www.pocketgamer.com/maplestory-thief-edition/)).
- **Origins.** The engine is a port of a Korean feature-phone game. Its game logic speaks phone keypad codes, a Clet lifecycle drives it, and the purchase server and the dead portal menu items are leftovers of the WIPI version ([re/program-flow.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/program-flow.md), [re/network.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/network.md#cmsocket-the-purchase-verification-server)). This is read from the code; no outside source was found for the feature-phone release.

## Releases

| Date | Event | Source |
|---|---|---|
| 2010-08-16 | TouchArcade reports the game's arrival on the App Store as a single-player MapleStory game | [TouchArcade](https://toucharcade.com/?p=47580) |
| 2010-09-15 | Pocket Gamer reports a limited-time price of $2.99 (1.79 GBP, 2.39 EUR) | [Pocket Gamer](https://www.pocketgamer.com/maplestory-thief-edition/maplestory-thief-edition-for-iphone-reduced-to-1-79-2-99) |
| 2012 or later | Version 1.5, the version the port reads, is built (probable, from the copyright year in its main menu image and the Xcode 4.2 build tools) | the game's files |

No source was found for the date the game left sale. The compendium states no end-of-sale date until one turns up.

## Versions

- **Version 1.5.** `Info.plist` gives `CFBundleVersion` 1.5 and no short version string.
- **Version badges.** The main menu shows the badge image `v1.0.5.png`. The bundle also holds `v1.0.0.png` (in `English.lproj`) and `v1.0.1.png` to `v1.0.9.png` (at the root), which nothing in the code names. They are probably the badges of earlier and later builds left in the project ([Unused content](about/unused.html#version-badges)).
- **The build.** Xcode 4.2 with the iOS 5.0 SDK on Mac OS X 10.6.8, with llvm-gcc 4.2. The binary has an armv6 and an armv7 slice.

## Platforms

- **Devices:** iPhone and iPod touch only (`UIDeviceFamily` 1), landscape only.
- **System:** iPhone OS 3.1.2 and later; it supports multitasking from iOS 4.
- **Languages:** English and Korean, chosen by the device's language ([Localization](about/localization.html)).
- **Today:** this project, the MapleThief port, rebuilds the game from a player's own copy for macOS, Windows, iOS, and Linux ([the port on GitHub](https://github.com/armaatus/maplethief-port)).
