# Bugs and glitches

These are the bugs of MapleStory Thief Edition 1.5 itself, as the port found them while rebuilding the game, told as a player would meet them. Each entry says what triggers it, what happens, and what the port does about it. The technical account, with addresses and tests, is [re/original-bugs.md](https://github.com/armaatus/maplethief-port/blob/main/docs/re/original-bugs.md).

The port has three answers. **Fixed in Enhanced:** the Enhanced preset's setting `fix_original_bugs` removes a bug that ends the game or leaves the player stuck; the Original presets keep it. **Neutralised:** the port never lets its own clock, files, or keys trigger the bug, in any mode. **Kept:** the port does what the device did.

## Bug 1: tapping play too early

Right after launch, the main menu's Play button already accepts a tap before the game has loaded the character slots. A tap released in the first quarter of a second, before anything is drawn there, makes the character select screen read a slot list that is still empty, and the app closes. Only a blind tap reaches it. **Fixed in Enhanced:** the port ignores Play until the menu is ready.

## Bug 2: the portal items quit the game

Three items of the engine's own main-menu list were meant to open Nexon Game Box pages of the Korean version. The hand-off to the Game Box is an empty stub in this version, so choosing one of them opens nothing and quits the game. Whether the English version can reach these items by touch is still open. **Fixed in Enhanced:** the port stays on the menu.

## Bug 3: the 49-day freeze

The game's millisecond clock is a 32-bit number taken from the wall clock, and it jumps from its highest to its lowest value every 49.7 days of real time. A copy that runs, or sits suspended, across that moment stops getting ticks for the next 49.7 days: no input, no drawing, until it is relaunched. **Neutralised:** the port starts its clock far from the jump.

## Bug 4: a damaged options file

The options file stores the game speed and a second setting as small numbers, and the game uses them as table positions without checking them. A hand-edited or damaged file with a value above 2 makes the game tick on every poll of its timer, or read whatever lies beyond the table. The options menu itself only writes valid values. **Neutralised:** the port reads any larger value as 2.

## Bug 5: saves written in place

The game writes a save over the old file, byte by byte. If the app is killed or the storage is full in the middle, the save is left half old and half new, and nothing checks it on loading. **Neutralised:** the port writes a new file and swaps it in, so a save is either the old one or the new one.

## Bug 6: the save loaders trust the save

When it loads a save, the game takes item numbers, skill levels, the map, and the quest records as they are, without checking that they make sense. Only a hand-edited or damaged save has wrong values, and what such values do further on was not traced. **Kept:** the game never writes such values itself.

## Bug 7: data files without bounds

The loaders of the map, collision, tile set, and sprite files trust the sizes and counts in them. A malformed file would write past the end of the game's buffers or draw wrong tiles. Every file the game ships is well formed, and the port's tools check each one. **Kept.**

## Bug 8: save file quirks

The save files have small flaws that change nothing a player sees: a few bytes are never written and hold leftovers from memory, one byte of each quest record overlaps the next record, a check for an existing save looks for a misspelled file name the game never writes, and a shortened list keeps its old bytes after its end. **Kept,** so that a device's saves load in the port byte for byte.

## Bug 9: a clock set back

If the device's clock is set back while the game runs, the game gets no ticks until the clock has caught up again. A clock set forward gives one tick at once and moves every running timer forward by the jump. Only a clock set by hand moves that far. **Kept.**

## Bug 10: the quit item of the main menu

An item of the engine's own main-menu list stops the music and quits the app after the player confirms. iOS apps are not meant to quit themselves, but the player asked for it. As with the portal items, whether the English version can reach it is open. **Kept.**

## Bug 11: confirming a quest without touching it

The quest tab of the in-game menu shows a quest's page when the player double-taps its row; a tap remembers which quest it was. Confirming a row with a key instead opens the page without remembering the quest, so the page is drawn from whatever was left in memory: the game crashes, or it shows the title of the quest touched last. The device has no key that reaches this tab, so only the port's controller and keyboard can trigger it. **Neutralised:** the port remembers the quest as a touch would.

## Bug 12: the shop list stops at the third row

In an NPC shop, moving the cursor with keys stops at the third row, and the list never scrolls, because the shop resets its scroll position from the touch drag on every frame. The device's shop has no up or down keys; a touch picks a row. **Neutralised:** with the port's keys the cursor walks the whole list and the list scrolls with it.

## Bug 13: the item grids assume six columns

The item and equipment grids of the in-game menu are drawn four cells to a row, but their key handlers step six cells up and down. Down from the first cell lands two cells too far, on a cell that is not drawn, and the drawn grid never follows. No key reaches these grids on the device. **Neutralised:** the port's keys move one cell left or right and one drawn row up or down.
