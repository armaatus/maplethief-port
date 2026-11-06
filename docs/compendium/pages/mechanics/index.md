# Mechanics and guides

This section explains how MapleStory Thief Edition 1.5 decides what happens in play: how hard the hero hits, what a kill drops, how EXP turns into levels, and the systems around the main game. Every rule comes from the decompiled game code and is written here in plain words, with a worked example and the addresses of the functions behind it. The guides at the end turn the rules and the game's own tables into advice on where to train and how to earn meso.

All arithmetic in the game is integer arithmetic. A division drops the fraction (it rounds toward zero), so 7 / 2 is 3. The worked examples follow the same rule.

The research behind these pages is in the project's notes on [the game's rules](https://github.com/armaatus/maplethief-port/blob/main/docs/re/mechanics.md), [the cash shop](https://github.com/armaatus/maplethief-port/blob/main/docs/re/cash-shop.md), [saving](https://github.com/armaatus/maplethief-port/blob/main/docs/re/autosave.md) and [the key pad](https://github.com/armaatus/maplethief-port/blob/main/docs/controls/keypad.md). Addresses are armv7 addresses in the 1.5 binary.
