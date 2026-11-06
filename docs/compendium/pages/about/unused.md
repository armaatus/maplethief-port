# Unused content

What version 1.5 ships but never uses: leftovers of earlier versions, features that were cut, and data that nothing reads. Each entry gives what it is, where it sits in the app bundle, the evidence that it is unused, and what it holds. The unused scripts, with their text, follow at the end of the page.

## Monster Carnival

**What:** a timed contest in which the hero defeats summoned monsters for points. **Where:** the scripts `common/Spiegelman1`, `Spiegelman2`, and `Spiegelman3`, which take {{item:etc:47}}, {{item:etc:48}}, or {{item:etc:49}} and start a three-minute round on {{map:0-5}}, {{map:4-18}}, or {{map:6-3}}. The engine's code for the round is complete ([re/mechanics.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/mechanics.md)). **Evidence:** the native function that starts a round is called only from these three scripts, and no map object names them, so no NPC or portal runs them. Their host, {{npc:38}}, has a name and a sprite in the tables but stands on no map. **Content:** their dialogue, in Korean only, is under [the unused scripts](#unused-scripts); it explains the rules, the Carnival points, and a ranking that players could submit from the main menu for a fee.

## Orphan collision grid

**What:** `map/106.att`, the collision grid of a map 10.6 that does not exist. **Where:** beside the 90 maps' files; there is no `106.map` and no `106.obj`. **Evidence:** the game loads an `.att` only together with its map, and no warp of any script leads to map 10.6. The file is also in an older format: 2,112 raw bytes, one per cell, with no header, which the game's loader would misread. **Content:** laid out 44 cells wide it is a 22 by 24 tile map with unbroken ladders capped like those of areas 4, 7, and 10, and a full-width floor near the bottom ([formats/att.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/att.md#106att-a-leftover-raw-grid)). Area 10 is part of El Nath, so it is probably an early export of an El Nath map.

## Empty shop rows

**What:** 18 rows of the shop stock table. **Where:** `market.dat`, rows 202 to 219, in all three languages. **Evidence:** every byte of these rows is zero, and the shop screen keeps only rows whose shop number matches the open shop. **Content:** nothing; they are reserved space ([formats/data-tables.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md)). A related slip: row 146, in the middle of a shop's stock, has shop number 0.

## Shops no NPC opens

**What:** three shops with stock but no keeper: {{shop:0}}, {{shop:1}}, and {{shop:16}}. **Where:** their rows in `market.dat`. **Evidence:** the only way to the shop screen is the script function that opens a shop, and no script passes these numbers ([kb.md](https://github.com/armaatus/maplethief-port/blob/main/docs/compendium/kb.md)). **Content:** their stock is listed on each shop's page.

## Cash items no product grants

**What:** two paid items of the feature-phone version, {{item:etc:128}} and {{item:etc:131}}. **Where:** the other-items table, as cash items (type 7) beside the two that are sold. **Evidence:** none of the ten App Store products, and no other code path, grants them ([re/cash-shop.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/cash-shop.md#cash-items-that-no-product-grants)). **Content:** their names, descriptions, and a meso value, on their item pages.

## The build-up value

**What:** `buildup.dat`, a one-byte file in each language's data folder. **Where:** `English/data`, `Korean/data`, and `Japanese/data`. **Evidence:** the title screen loads it and stores its number in the data manager, but no code reads that field afterwards ([formats/data-tables.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/data-tables.md#buildupdat)). **Content:** the digit 1. The name suggests a build or content revision; what it controlled is unknown.

## Intro movie and logo timer

**What:** code to play a movie before the game and to show a logo on a timer. **Where:** the classes `MoviePlayerController` and `MovieOverlayView`, and the shell's `drawMapleLogo__`, `startAppEnv`, and `movieFinishedCallback:`. **Evidence:** nothing sends those messages and nothing creates the movie player; at start-up the shell goes straight from the end of the logo step to the game ([re/program-flow.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/program-flow.md)). **Content:** no movie file ships in the bundle.

## Special map object kinds

**What:** three kinds of map object that the engine handles but no map uses: kinds 40 and 41, special NPCs that set the current NPC without running a script, and kind 42, a marker whose position the Monster Carnival code looks up. **Where:** the engine's object code; the `.obj` files would place them. **Evidence:** no shipped `.obj` file holds an object of these kinds ([formats/obj.md](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/obj.md)). Whether a script could create one at run time is not known.

## Unused root images

**What:** two pictures of the shell. **Where:** the root of the app bundle. **Evidence:** no string in the binary names them ([re/objc-shell.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/objc-shell.md#images-and-the-bundle)). **Content:**

- `MainMenuCompanyLogo.png`, a Nexon Mobile logo; the main menu has a `companyLogo` view that is never given an image.
- `AlertButton.png`, a plain button of the alert boxes, which use Yes, No, and OK buttons of their own instead.

The icons and the launch image are not named by the code either, but iOS itself uses them.

## Version badges

**What:** ten small images reading a version number, V 1.0.0 to V 1.0.9. **Where:** `v1.0.0.png` in `English.lproj`, the others at the root. **Evidence:** the main menu names only `v1.0.5.png`; no string in the binary names the other nine ([re/objc-shell.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/objc-shell.md#images-and-the-bundle)). **Content:** the version text alone. They are probably the badges of the game's other builds, kept in the project from version to version. The badge on screen, 1.0.5, does not match the bundle's version, 1.5.
