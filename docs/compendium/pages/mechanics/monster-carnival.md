# Monster Carnival

Monster Carnival is a timed hunting mode, run by {{npc:38}}, in which the monsters' levels follow the hero's strength and kills earn points instead of EXP. It is the game's only level-scaling mode. The code and the three tickets ship in version 1.5, but no map places the NPC whose scripts start it, so a normal game never reaches it.

## How to enter

Each of three scripts takes one ticket ({{item:etc:47}}, {{item:etc:48}} or {{item:etc:49}}), starts the mode and moves the hero to its room: {{map:0-5}} for the first, {{map:4-18}} for the second and {{map:6-3}} for the third. A round lasts 3 minutes.

In 1.5 none of this is reachable. The three scripts are never named by any object on a map, the NPC stands on no map, and nothing in the game drops, sells or gives the tickets. The rooms are on the game's list of maps where saving is refused ([saving](mechanics/saving.html)).

## Level scaling

While the mode runs, each monster's level is adjusted when it is loaded. H is the hero's (ATT + DEF) / 2, rounded down.

- A monster above H drops halfway toward it: its level minus (level minus H) / 2, rounded down. Its HP is raised to 150%.
- A monster below H is pulled further down, not up: its level plus (level minus H) / 2, rounded down, which is negative. This looks like a sign slip in the original code. Its HP is unchanged.

The adjusted level is what the [evade, miss, avoid](mechanics/accuracy.html) and monster damage rules use, so in the carnival a weak hero meets weaker monsters with tougher hides.

**Example** (made-up numbers): a hero with 50 ATT and 30 DEF has H = 40. A level 60 monster becomes 60 minus 10 = 50, with 150% HP; a level 30 monster becomes 30 minus 5 = 25, with its normal HP.

## Points and rewards

A kill gives no EXP, no drops and no quest progress. It adds the monster's carnival points to the score and counts the wave. A death costs 70, 90 or 110 points in the first, second or third mode.

When the result popup closes, the hero gets EXP by the final score:

| Mode (ticket) | Score bands | EXP |
|---|---|---|
| First ({{item:etc:47}}) | up to 1,500 / up to 2,000 / up to 2,500 / above 2,500 | 250 / 500 / 1,000 / 1,500 |
| Second ({{item:etc:48}}) | up to 800 / up to 1,400 / up to 2,000 / above 2,000 | 2,700 / 3,500 / 4,000 / 4,700 |
| Third ({{item:etc:49}}) | up to 2,000 / up to 2,500 / up to 3,000 / above 3,000 | 4,500 / 6,200 / 7,000 / 7,700 |

The best score of each mode is kept in the game's score file.

**Example:** a second-mode round that ends with 1,600 points falls in the band up to 2,000 and pays 4,000 EXP.

## Sources

The scripts `common/Spiegelman1.out` to `Spiegelman3.out` call the native `initMonCa` `0x99545`, which runs `CStateGame::StartMonsterCarnival` `0x82271`; the mode flag is `CStateGame +0x404`, the mode `+0x40C`, the timer `+0x430`, the score `+0x434` and the wave `+0x410`. Scaling is in `CMob::LoadStats` `0x73B51`, the carnival points come from the spawn table behind the pointer at `0xF13B4`, and the reward is `CStateGame::resultMonCa` `0x819E5` with its table at `0xEAD40`. The best scores are bytes `0x000` to `0x00B` of the `cupon` file. That the mode is unreachable comes from the compendium's script walk: no map object names the three scripts.
