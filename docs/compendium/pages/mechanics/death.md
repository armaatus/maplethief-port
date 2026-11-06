# Death and revives

The hero dies when HP reaches 0. A popup then offers the way back: a free return to the nearest town at a price in EXP and meso, a revive on the spot with {{item:etc:130}}, or one of two paid revives.

## Death

At death the hero's HP is set to 0 and the hero is marked dead. From level 10, after the first job advancement, the game also sets an EXP penalty: a quarter of the current level's need, but never more than the EXP the hero has.

The death popup offers:

- **Return to town:** EXP minus the penalty (none below level 10 or before the advancement) and meso minus a tenth. The hero goes to the nearest town, the same choice as {{item:etc:24}}, and comes back with full HP and MP.
- **Revive here** with {{item:etc:130}}, or with one of the two paid revives of the [cash shop](mechanics/cash-shop.html). The hero stands up where it fell, with full HP and MP. No monster, shop or quest gives Reverse.
- **Go to the main menu**, which asks first.

In [Monster Carnival](mechanics/monster-carnival.html) a death costs carnival points instead.

**Example** (made-up numbers): a level 20 hero with a need of 1,000, 180 EXP and 5,000 meso dies. The penalty is 250, capped at 180, so the hero returns to town with 0 EXP and 4,500 meso.

## Sources

`CHero::Die` `0x65979` (the dead flag at `+0x0C`, the penalty at `+0x158`), the death popup's keys in `CStateGame::OnKeyPressed` `0x8D4C9`, `CHero::Revive` `0x6140D`. The popup is popup `0x68`, opened by `Popup_Open` `0x3EAA1`.
