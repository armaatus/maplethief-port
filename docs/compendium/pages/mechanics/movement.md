# Movement

The hero walks, jumps and climbs at fixed speeds; monsters walk at the speed of their table row and move in one of three ways. Some of the physics is not worked out yet.

## The hero

- The hero walks 5 units a tick and moves sideways at 7 units a tick in a jump. A third speed of 4 is probably the ladder speed.
- The jump's push is a shared constant of 256; next to it lie a value of 74 (probably gravity) and a fixed-point one (65,536). How many pixels a jump rises is unknown.
- In {{area:9}} the ground is ice and the hero slides, unless wearing {{item:equip:52}}, which turn the slide off.

**Example:** at the default 20 ticks a second the hero walks 100 units a second, and a running jump carries the hero sideways at 140.

## Monsters

Monsters walk at the speed of their table row and move by its movement kind: kind 1 walks and turns at the edges of its platform, kind 2 floats and never lands (the bosses), and kind 3 is the {{monster:65}}, which explodes on contact. The movement shows on each [monster's page](monsters/index.html).

**Example:** {{monster:2}} is a walker: it paces its platform and turns round at each edge.

## Not known yet

The units of the jump's push and of gravity, and so the height of a jump in pixels, need a trace of the hero's height over one jump.

## Sources

The `CHero` constructor `0x68A59` (speeds at `+0x20`, `+0x24` and `+0x28`), the shared physics constants set in the `CMapleObject` constructor `0x6DBF5` (the jump at `0x11A9C8` through the pointer at `0xF13C4`, `0x11A9D0` and `0x11A9D4`), the movement keys `KeyWalk` `0x60DD9`, `KeyJump` `0x61651` and `KeyUpDown` `0x63665`, monster walking in `CNpc::Walk` `0x6FCBD`, and the ice slide in `0x64CA1` (world `+0x94`).
