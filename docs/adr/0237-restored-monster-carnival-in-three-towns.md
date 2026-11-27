# 237. Monster Carnival restored in three towns under restored_content

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#427](https://github.com/armaatus/maplethief-port/issues/427)
- **Builds on:** [ADR 0069](0069-guest-function-hooks.md) (guest function hooks; its #379 addendum: restored content is its own setting), [ADR 0070](0070-settings-registry-and-presets.md) (`restored_content`, off in every preset, #389), [ADR 0089](0089-autosave-on-quit-and-background.md) (the no-save maps include the rooms), [ADR 0236](0236-restored-shops-from-existing-merchants.md) (the first restore)

## Context

Monster Carnival ships whole: three rooms (0.5, 4.18, 6.3), the rules (`CStateGame::StartMonsterCarnival` `0x82271`, the round's clock, `resultMonCa` `0x819E5`), the NPC's sprite and name (kind 42, NPC 38), three tickets (`item_etc` 47–49) and three scripts that start a round, `common/Spiegelman1`–`3.out`. No map places the NPC, so nothing runs the scripts, and nothing sells or drops the tickets. Restoring it under `restored_content` needs four decisions: where the NPC stands, how it is placed without changing a shipped file, how a player gets a ticket, and which of the original's rules to keep. The facts are in [re/original-bugs.md](../re/original-bugs.md#monster-carnival-restored-427): each script's texts are the `ChapterN_MonsterCarnival_Spiegelman` keys of one chapter, its level gate fits that chapter, and its room lies in that chapter's world and bears the name of the world's first town.

## Options considered

**Where.**

1. **One NPC on one map, naming one script.** Two of the three rounds would stay unreachable, and the data says the scripts are three chapters' NPCs. Rejected.
2. **One NPC whose script follows the current chapter.** Matches the keys, but a hero who went on to a later chapter could never play the band that fits his level again, and the object's name would change between loads of the same map. Rejected.
3. **Each script's NPC in its chapter's first town**, the town its room is named after: `Spiegelman1` on Kerning City (0.0), `Spiegelman2` on Orbis Village (4.0), `Spiegelman3` on Magatia Village (6.0), each on the floor beside the town's merchants. The scripts' own level gates decide who may enter. Chosen. The tiles are the port's call; the data names none.

**How it is placed.**

1. **A changed `.obj` served for those maps.** Means a second copy of the file's bytes or a file-read hook that knows about one format. Rejected.
2. **A wrapper on `CMMap::LoadObj` `0xD259`** that, after the original, rebuilds the object and name arrays one longer with the guest's `new[]` and appends the NPC. The shipped file is untouched, and everything that reads the list (sprites, draw, talk, the NPC state pass, the carnival's way back) runs later. Chosen.

**The tickets.** The scripts tell a hero without one to buy it at a shop, and no shop does.

1. **No ticket needed** (a hook skips the script's check). Changes the script's behaviour. Rejected.
2. **The NPC gives one.** Invents a mechanic. Rejected.
3. **The town's etc merchant sells it**, on one of `market.dat`'s 18 unused all-zero rows written in memory: 47 for 300 meso in shop 5 (`Etc00`), 48 for 1,500 in shop 11 (`Etc40`), 49 for 4,000 in shop 15 (`Etc60`). The prices are the port's call: the item table's value (6–7 meso) is what the Sell tab pays, so the price rises with the band instead, in the range of each town's dearest goods. Chosen.

**The texts.** The scripts pass Korean literals, which every language shows as they are. With the setting on, `-[NSBundle localizedStringForKey:value:table:]` answers such a text with the key whose Korean value it is, among the `ChapterN_MonsterCarnival_Spiegelman.*` keys (`mt_cf_set_literal_key_prefixes`). The game's own translations, no new text.

**The rules.**

- **The level scaling's sign slip is kept.** Restored content brings the game's rules back as they are; a different scaling would be the port's design.
- **The first round's score starts at 0.** The original never sets `+0x434` before a run's first round (only the result box's close sets it, after a round), so that round is paid by whatever the memory held: a run in the port read 1,679,847,283 and got the top band for no kill. The wrapper sets it to 0 when the room loads (the carnival on, its phase −1), as every later round finds it. No reachable path starts a round, so this belongs to the restore, not to `fix_original_bugs`.
- **The rooms 4.18 and 6.3 are kept as they are,** with no objects and so no arrival portal: the hero keeps the town's x and y, which in both rooms is above the floor, and the rounds play (checked by runs).

## Decision

- **`mt_hooks_install_restored_carnival(enabled)`** (`maplethief/hooks.h`, `runtime/src/hooks/restored_carnival.cpp`), with `CMMap::LoadObj` `0xD259` as a new hooked function in `recomp/hooks.py`. `mt_restored_carnival_npc_on(area, map)` is the table: the town, the tile, the script, the ticket, the merchant's shop and the price.
- **`mt_cf_set_literal_key_prefixes(prefixes)`** (`maplethief/cf.h`): the literal-to-key lookup, off by default.
- `platform/src/restored_content.cpp`'s `install_restored_carnival` sets both from `restored_content`, from `game.cpp` and the tests' boot. Off: the wrapper is registered disabled, no prefixes: every list and shop is its file's, as on the device.

## Evidence

- `play.systems2_restored_carnival_synthetic`, `_4_synthetic`, `_6_synthetic` (real assets, local; starts on 0.0, 4.0 and 6.0 with a ticket): the town's live object list is its `.obj`'s plus the NPC; the merchant lists the ticket after its listings; the NPC's first row and Accept start the script's mode in its room with the ticket taken; monsters spawn, the score starts at 0, the round runs out, the result box closes and the hero is back in the town with the band's EXP (250, 2,700, 4,500 for a score of 0); no Hangul in the NPC's pages.
- `play.systems2_restored_carnival_off_synthetic`: the setting off, 0.0's list is `00.obj`'s record for record, shop 5 is the file's, nothing written.
- `hooks_test` and `foundation_test` (CI): the table and the wrapper's switch; the literal keys on, off and by prefix.

## Consequences

- A player who turns `restored_content` on can buy a round for 300–4,000 meso and earn 250–7,700 EXP in three minutes, which steepens the level curve. Off is the original, and no preset turns it on.
- The scripts' ranking pages describe an online ranking the iOS build does not have; they are kept as the game's text.
- Where two chapters share a Korean line, the first chapter's English wording is shown (the variants differ in wording only).
- The unreached maps ([#428](https://github.com/armaatus/maplethief-port/issues/428)) list the rooms among the 21; they are now reachable with the setting on, through the carnival only.
