# Saving

The game keeps three character slots. Each slot is two small files on the device, one for the character and one for its quests, and the game writes both together whenever it saves. It has no autosave: the player saves from the menu, and a few events save on their own.

## Slots and what a save holds

A new game in an empty slot writes the slot at once: a level 1 hero with 100 meso, five {{item:etc:3}}, five {{item:etc:6}} and a {{item:equip:72}} in hand, standing in the tutorial on {{map:2-8}}. The slot screen shows each saved slot's job, level, worn items and the title of its current quest section.

The character file holds:

- the job branch and the number of job advancements;
- the level, EXP, unspent skill points, HP, MP and meso;
- 48 equipment slots and 48 item slots of inventory, and the five worn items;
- the level of each skill;
- the five quick-slot shortcuts;
- the map and the position on it.

The quest file holds the state of every quest. Two more files are shared by all slots: the options ([options](mechanics/options.html)) and the best [Monster Carnival](mechanics/monster-carnival.html) scores. No file has a checksum or a version number.

Deleting a slot on the slot screen removes both of its files.

**Example:** a hero saved on {{map:2-3}} after a hunt reloads on that map, at the saved position, with the same inventory and quest progress.

## When the game saves

- **The menu's Save.** The popup menu's last tab has Save; it asks first, then saves and confirms, or says that it cannot save here.
- **After a cash shop grant**, once the server had confirmed a purchase ([cash shop](mechanics/cash-shop.html)).
- **After a scroll's result** ([potions and scrolls](mechanics/items.html#upgrade-scrolls)), so a failed scroll cannot be undone by quitting.
- **After a quest reward item** is given.

Leaving to the main menu through the menu's exit does not save.

The menu opens only in the field with no popup open, and the game ignores the keys while a map loads, during a warp, while the hero is knocked back and while a scroll is applied.

**Example:** a hero reads an upgrade scroll and it fails. The game saves at once, so closing the app and reloading shows the slot used up.

## Where the game refuses to save

On 16 maps the menu's Save says it cannot save, and the cash shop does not open. Boss fights and climbs are not saved halfway.

| Maps | Why |
|---|---|
| {{map:2-8}}, {{map:2-9}} | the tutorial |
| {{map:3-8}}, {{map:4-17}}, {{map:7-4}}, {{map:9-5}}, {{map:10-5}} | boss rooms |
| {{map:2-6}}, {{map:2-7}} | the first climb |
| {{map:6-1}}, {{map:6-2}} | the second climb |
| {{map:9-9}}, {{map:9-10}} | the third climb |
| {{map:0-5}}, {{map:4-18}}, {{map:6-3}} | the Monster Carnival rooms |

Each map's page shows the same fact in its infobox. One of the game's loading tips says that boss stages cannot be saved.

**Example:** a hero on {{map:2-7}}, halfway up the climb, opens the menu and picks Save: the game refuses, and the last save stays as it was.

## In the port

The port can also save when the player quits or leaves the app, wherever the menu's Save would work ([autosave on quit, ADR 0089](https://github.com/armaatus/maplethief-port/blob/main/docs/adr/0089-autosave-on-quit-and-background.md)).

## Sources

The files and their layout are in the project's [save format](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/save.md): the character file is written by `CDataMgr::SaveSaveData` `0x38711` and read by `CHero::LoadSave` `0x677E9`, the quest file by `CDataMgr::SaveQuestData` `0x38F55`; a new slot comes from `CDataMgr::LoadSaveData` `0x39989`; deleting a slot is `0x9462D`. The menu's Save and its map check are in `Game_ExitToMainMenu` `0x54BF9`, the key guards in `CStateGame::OnKeyPressed` `0x8D4C9`, and the other saves in `CDataMgr::OnServerPacket` `0x3ED19`, `CHero::Flush` `0x62ADD` and `0x7C2D9` ([autosave note](https://github.com/armaatus/maplethief-port/blob/main/docs/re/autosave.md)).
