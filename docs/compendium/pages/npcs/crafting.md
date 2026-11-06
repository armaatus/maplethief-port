# Crafting

{{npc:12}} in {{map:0-0}} ({{area:0}}) is the only crafter. He makes weapons, hats, overalls, shoes, and earrings from minerals and jewels and a fee in meso: 12 recipes in chapter 1 and 18 in each of chapters 2 and 3. Many recipes come back in the next chapter with the same materials and fee; each chapter's recipe has its own row below. The materials drop from monsters or come from the [trades](npcs/trades.html) of {{npc:11}}, {{npc:18}} and {{npc:31}}.

Crafted equipment is the only equipment with random stats. The game rolls each of its six stats between the item's minimum and maximum, adds the item's bonus stat and gives it 7 upgrade slots. Bought and dropped equipment has every stat at its minimum, no bonus and 5 upgrade slots ([mechanics](https://github.com/armaatus/maplethief-port/blob/main/docs/re/mechanics.md#drops), `Quest_MakeEquipItem` `0x7C2D9`).

To craft, talk to {{npc:12}}, choose to craft, pick the slot and then the item. The recipe needs every material and the fee in the inventory; without them the conversation ends and nothing is taken.
