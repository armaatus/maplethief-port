# Movement

The hero walks, jumps and climbs at fixed speeds; monsters walk at the speed of their table row and move in one of three ways.

## The hero

- The hero walks 5 pixels a tick, moves sideways at 7 pixels a tick in a jump, and climbs ladders and ropes at 4 pixels a tick.
- A jump rises 50 pixels and lasts 7 ticks on flat ground, so a running jump covers 49 pixels sideways. The arc is a parabola: the jump starts with a push of 256 and gravity adds 74 to it each tick. A side jump, and the knock-back of a hit, rise a quarter as high, 12 pixels. The bot of the chapter runs clears about 50 pixels with each jump (`play.chapter_1_*`).
- In {{area:9}} the ground is ice and the hero slides, unless wearing {{item:equip:52}}, which turn the slide off.

**Example:** at the default 20 ticks a second the hero walks 100 pixels a second, and a running jump carries the hero sideways at 140.

## Monsters

Monsters walk at the speed of their table row and move by its movement kind: kind 1 walks and turns at the edges of its platform, kind 2 floats and never lands (the bosses), and kind 3 is the {{monster:65}}, which explodes on contact. The movement shows on each [monster's page](monsters/index.html).

**Example:** {{monster:2}} is a walker: it paces its platform and turns round at each edge.

## Sources

The `CHero` constructor `0x68A59` (speeds at `+0x20`, `+0x24` and `+0x28`), the shared physics constants set in the `CMapleObject` constructor `0x6DBF5` (the jump at `0x11A9C8` through the pointer at `0xF13C4`, `0x11A9D0` and `0x11A9D4`), the fall step `CMapleObject::FallStep` `0x6CF69`, the movement keys `KeyWalk` `0x60DD9`, `KeyJump` `0x61651` and `KeyUpDown` `0x63665`, monster walking in `CNpc::Walk` `0x6FCBD`, and the ice slide in `0x64CA1` (world `+0x94`).
