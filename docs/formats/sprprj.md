# `.sprprj` sprite projects

A sprite project describes one sprite: the PNGs it draws from, and its **actions** (animations). Each action is a list of **frames**. A frame stays on screen for a number of game ticks and is a stack of **layers**. A layer places one PNG at a signed offset, with an alpha, a horizontal flip and a visibility flag. The engine classes are `CMSprite` → `CMAction` → `CMFrame` → `CMLayer`, plus `CMSpriteImage` for the PNGs.

Status: **fully specified**. All 138 shipped files parse with no leftover bytes, and every layer references a PNG that exists ([Validation](#validation)). Parser: [`tools/mtport/sprprj.py`](../../tools/mtport/sprprj.py).

## Files

`sprite/<folder>/<n>.sprprj`, with the folder's PNGs in `sprite/<folder>/res/`. All projects in a folder share that one `res/` folder.

| Folder | Projects | PNGs in `res/` | Notes |
|---|---|---|---|
| `mob/` | 129 (`0`–`128`) | 367 | 17 projects are the 2-byte empty file `00 00`. Some mobs reuse another mob's PNGs with their own palette (`97` and `98` both use `sell_*.png`). |
| `hero/` | 3 | 121 | 15, 6 and 6 actions. One PNG in `res/` is not listed. |
| `title/` | 2 | 12 | Two PNGs in `res/` are not listed. |
| `effect/`, `effect_kor/` | 1 each | 28 each | 17 actions each |
| `hatching/` | 1 | 16 | |
| `loading/` | 1 | 5 | the loading spinner |

## Where the game loads it

All addresses are armv7 VM addresses. Function pointers carry the Thumb bit. The code is unoptimized (`-O0`), so every field access shows up as a plain load or store at a fixed offset.

| Address | What |
|---|---|
| `0x20AC1` | `CMSprite` constructor. Each of the 18 load sites (in 13 functions) calls it right before the loader. |
| `0x20005` | **The loader**: `CMSprite::Load(const char *folder, int index)`. It builds the path with `"%s%s%s%d%s"` (`0xE3895`) from `"/sprite/"` (`0xE38A0`), the folder, `"/"` (`0xE38A9`), the index and `".sprprj"` (`0xE38AB`), reads the whole file through `0xB689`, and parses it as described below. At the end it adds the sprite to the global list at `0x11A468`. |
| `0x1FF79` | Looks in that list for a sprite with the same folder (`strcmp`) and index. If one exists, the loader skips the file and clones it (`0x20532`–`0x20990`): the images are shared (refcount `+0x3C` incremented) and the actions, frames and layers are copied. |
| `0x1BC61` `0x1BCD5` `0x1BDB1` `0x1BE99` | Read helpers: `i16` (LE, sign-extended), `u32` (LE), a string of *n* bytes (copied into `new char[n+1]`, NUL-terminated), and *n* raw bytes (into `new u8[n]`). The loader reads single bytes inline. |
| `0x21BB5` | `CMSpriteImage` get-or-create `(folder, name, palette, palette_len)`. It searches the global image list at `0x11A46C`, using `0x21155` to compare folder, name, palette length and palette bytes, and on a hit increments the refcount at `+0x3C`. Otherwise it calls `0x218F1`. |
| `0x218F1` | `CMSpriteImage` constructor. Path `"/sprite/%s/res/%s"` (`0xE38BD`), file read through `0xB5DD`. It creates the texture with `0xB9F1` when there is a palette, or `0xBABD` when there isn't. |
| `0xB9F1` | Palette swap: copies the PNG, finds the `PLTE` chunk (`0x22855`), takes `entries = PLTE length / 3` (`0x228CD`), overwrites `entries × 3` bytes of `PLTE` data with the palette (`0x22E6D`), recomputes the chunk CRC (`0x228FD`), then decodes. |
| `0x1FD9D` | `CMAction` constructor `(frame_count)` |
| `0x1F7A1` | `CMFrame` constructor `(delay, layer_count, images)` |
| `0x1E715` | `CMLayer` constructor `(image, visible, x, y, alpha, hflip)` |
| `0x1E77D` | `CMFrame` bounds, computed after the frame's layers are read |

## Encoding

Little-endian. Counts and lengths are `u8`, except `palette_len`, which is `u32`. Offsets and the frame delay are signed `i16`. Names are ASCII with no terminator. The engine does no bounds checks: it trusts the counts.

## Layout

```
u8 image_count
image_count × Image
u8 action_count
action_count × Action
<end of file>
```

**Image** (one `CMSpriteImage`; the sprite's `+0x0C` points to the array and `+0x10` holds the count)

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8 | `name_len` | |
| 1 | char[`name_len`] | `name` | PNG file name in `sprite/<folder>/res/` |
| 1+`name_len` | u32 | `palette_len` | 0 = use the PNG as is |
| 5+`name_len` | u8[`palette_len`] | `palette` | RGB triples that replace the PNG's `PLTE` chunk data (see `0xB9F1`). In every shipped file `palette_len` equals that PNG's `PLTE` length. |

**Action** (one `CMAction`; the sprite's `+0x04` points to the array and `+0x08` holds the count)

| Offset | Type | Field |
|---|---|---|
| 0 | u8 | `frame_count` |
| 1 | Frame[`frame_count`] | frames, in playback order |

**Frame** (one `CMFrame`, 3 bytes + layers)

| Offset | Type | Field | `CMFrame` field | Notes |
|---|---|---|---|---|
| 0 | i16 | `delay` | `+0x0C` | Ticks the frame stays current. Values below 2 show the frame for one tick (see [Timing](#actions-and-animation-timing)). Shipped values: 1 (513 frames), 2 (582), 3 (62), 5 (7), 10 (2), 20 (1). |
| 2 | u8 | `layer_count` | `+0x08` | 0–18 in shipped files. 22 frames have no layers. |
| 3 | Layer[`layer_count`] | layers | `+0x04` | drawn in file order, first layer at the back |

**Layer** (one `CMLayer`, 8 bytes)

| Offset | Type | Field | `CMLayer` field | Notes |
|---|---|---|---|---|
| 0 | u8 | `image` | `+0x04` | Index into the image list. The engine doesn't range-check it, but the parser does. |
| 1 | u8 | `alpha` | `+0x14` | 0–255. Shipped values: 255 (2,438 layers), 102, 178, 127, 153, 51, … |
| 2 | u8 | `hflip` | `+0x18` | Non-zero = mirror this layer horizontally. Set on 13 shipped layers. |
| 3 | u8 | `visible` | `+0x08` | Non-zero = drawn and counted in the bounds. It is 1 in every shipped layer. |
| 4 | i16 | `x` | `+0x0C` | Offset of the image's left edge from the sprite position |
| 6 | i16 | `y` | `+0x10` | Offset of the image's top edge (y grows downwards) |

There is no hitbox in the file. The only geometry the engine derives is each frame's bounding box (below).

## Derived frame bounds

`0x1E77D` walks the visible layers and stores the box around them in the frame. Width and height come from each layer's image (`0xB951` / `0xB965` on the `CMImage`). Hidden layers are skipped, and flips are ignored.

| `CMFrame` field | Value |
|---|---|
| `+0x18` / `+0x1C` | `min_x = min(layer.x)`, `max_x = max(layer.x + image width)` |
| `+0x20` / `+0x24` | `min_y = min(layer.y)`, `max_y = max(layer.y + image height)` |
| `+0x10` / `+0x14` | width `max_x − min_x`, height `max_y − min_y` |

The scan starts from `min = 10000` and `max = −10000`. A frame with no visible layer keeps those values, so the culling test below always rejects it. The `CMAction` draw culls with this box: it draws only when `x + max_x > 0`, `x + min_x < screen width`, `y + max_y > 0` and `y + min_y < screen height`. The screen size is read from the object at `0xF1440`, fields `+0x88` and `+0x8C`. The parser's `frame_bounds()` reproduces the box. Whether gameplay code also uses it for collision is not known yet.

## Actions and animation timing

A `CMAction` has no timing data of its own. Its state lives in fields that the constructor (`0x1FD9D`) and the reset functions set:

| Field | Meaning | After the constructor or a reset |
|---|---|---|
| `+0x0C` | current frame index | 0 |
| `+0x10` | ticks since reset | 0 |
| `+0x14` | flip the whole action (`0x1E619` sets it; callers pass `facing == 2`) | 0 (constructor only) |
| `+0x18` | ticks the current frame has been shown | 1 |
| `+0x1C` | use frame delays (`0x1E3A9` clears or sets it) | 1 (constructor only) |
| `+0x1D` | clamped at the first frame | 1 |
| `+0x1E` | **ended** (`0x1E3CD` reads it) | `frame_count == 1` |
| `+0x20` | action alpha (`0x1E629`; `0x1E639` sets it on every action of a sprite) | 255 (constructor only) |
| `+0x24` | alpha enabled (`0x1E68D` sets it, `0x1E69D` clears it) | 1 (constructor only) |

**Update, once per game tick** (`0x1E585`):

```c
ticks++;                                   // +0x10
if (use_delay) {                           // +0x1C
    if (frames[cur]->delay > hold) {       // frame +0x0C vs +0x18
        hold++;
        return;                            // stay on this frame
    }
    hold = 1;
}
cur++;
clamp();                                   // 0x1E509
```

**Clamp** (`0x1E509`) clears `+0x1D` and `+0x1E`. Then, if `cur < 0`, it sets `cur = 0` and `+0x1D = 1`. If `cur > frame_count − 1`, it sets `cur = frame_count − 1` and **ended** (`+0x1E`) `= 1`.

What follows from this:

- A frame with `delay = d` is current for `max(d, 1)` updates. The update that leaves it is the `d`-th one after it became current.
- **Actions never wrap.** The last frame holds. *ended* is set by the update that tries to step past the last frame, so after `Σ max(delay, 1)` updates over **all** frames. A 1-frame action is ended from the start.
- Looping is the owner's job. It polls *ended* and then either resets the action (`0x1E3F5` or `0x1E439`, which do the same) or drops it. For example, the effect list at `0x5E6C1` calls *update*, then *ended*, then either deletes the effect or restarts it, depending on the effect's loop flag.
- When `use_delay` is cleared, the action advances one frame on every update.
- Other entry points: `0x1E559` *set frame* `(i)` sets `cur = i`, `ticks = i`, `hold = 1`, then clamps. `0x1E47D` *set to end* sets `ticks = Σ delay`, `cur = last`, ended. `0x1E5F1` returns `cur` and `0x1E3B9` returns `frame_count`.

`ActionPlayer` in the parser mirrors this, and `tests/test_sprprj.py::TimingTest` pins it down.

**Tick length.** `CMTimer` (constructor `0x22165`) runs a 1 ms `CFRunLoopTimer` (`0x8E29`). Its callback `0x21F51` calls the game tick `0x2590D` whenever at least *interval* milliseconds of `gettimeofday` time (`0xA0D5`) have passed. The interval is a 64-bit value at `0x11A478`. The constructor sets it to 10, and `0x21DA1` sets it, called from:

- `CDataMgr::ApplyGameSpeed` `0x37C41`, which loads it from the table `{65, 50, 40}` at `0x108C58`, indexed by the game-speed byte `+1` of `config` (`0x11A98C`, [save.md](save.md#config-6-bytes));
- `0x980B5`, which would set 50 on a branch nothing reaches.

So one frame-delay unit is **one game tick of 40, 50 or 65 ms** (25, 20 or about 15 ticks per second; 50 ms by default). The `CMAction` update is called from `CStateGame::Paint` `0x87501` and from functions on its path: `0x5E6C1`, `0x5E7B1`, `0x61A19`, the hero and mob per-object functions `0x61895`, `0x62ADD`, `0x728E1`, `0x7458D`, and `Loading_Progress` `0x6AF09`. There is one `Paint` per tick ([re/timing-threading.md](../re/timing-threading.md#what-one-tick-runs)).

## Drawing

`0x1F2B9`, the `CMAction` draw `(x, y)`, culls by the [bounds](#derived-frame-bounds), then calls `0x1F119`, the `CMFrame` draw `(x, y, flip, alpha, alpha_enabled)`, with the action's `+0x14`, `+0x20` and `+0x24`:

- It adds 1 to `y` before drawing anything (reason unknown).
- If `alpha_enabled`, it sets the draw alpha to the action's alpha (`0x22479`). Only when that alpha is 255 does it also apply each layer's own `alpha`. At the end it restores 255.
- It skips layers that aren't `visible`.
- Unflipped, it draws the image at `(x + layer.x, y + layer.y)`. Flipped, it draws at `(x − layer.x − (image width − 1), y + layer.y)`.
- A layer is drawn mirrored when exactly one of `flip` and `hflip` is set. `0x24A65` draws mirrored and `0x24DC9` draws normally (inferred from which call each combination takes).
- Sprites are drawn **facing left** unflipped, so `facing == 2` (flipped) faces right. Checked against the game on the hero and mobs (issue #27).

`0x1F051` → `0x1E96D` is a second draw path with one more argument. One caller (`0x5FBCD`) uses it, and it isn't decoded yet.

## Worked examples

`sprite/mob/0.sprprj` (87 bytes, a portal):

```
00000000: 030c 706f 7274 616c 3031 2e70 6e67 0000  ..portal01.png..
00000010: 0000 0c70 6f72 7461 6c30 322e 706e 6700  ...portal02.png.
00000020: 0000 000c 706f 7274 616c 3033 2e70 6e67  ....portal03.png
00000030: 0000 0000 0103 0100 0100 ff00 01f2 ffd8  ................
00000040: ff01 0001 01ff 0001 f2ff d4ff 0100 0102  ................
00000050: ff00 01f2 ffd3 ff                        .......
```

- `03`: 3 images. `0c "portal01.png" 00000000`: name, no palette. The other two images follow the same pattern.
- `01`: 1 action. `03`: 3 frames.
- Frame 0: `0100` delay 1, `01` 1 layer, `00 ff 00 01 f2ff d8ff`: image 0, alpha 255, no flip, visible, x = −14, y = −40.
- Frames 1 and 2 show images 1 and 2 at (−14, −44) and (−14, −45). The action ends after 3 ticks.

`sprite/mob/96.sprprj` starts with a palette swap:

```
00000000: 080f 6669 7265 626f 6f6d 5f30 382e 706e  ..fireboom_08.pn
00000010: 6706 0000 0016 57e2 2090 200f 6669 7265  g.....W. . .fire
```

- `08`: 8 images. `0f "fireboom_08.png"`, then `palette_len` = 6, then two colours `1657e2 209020` that replace the 2-entry `PLTE` of `fireboom_08.png`.

`sprite/loading/0.sprprj` has one 12-frame action with 18 layers per frame. From frame to frame the first layer (image 0) fades 255 → 178 → 102 → 51 → 102 → 178, while the other layers stay at 255: a pulsing loading indicator.

## Unknowns

- The meaning of the options byte that selects the tick interval (`0x37C41`).
- The second draw path (`0x1E96D`).
- Whether anything besides culling uses the frame bounds.
- Why the frame draw adds 1 to `y`. In the walking and attacking frames of the hero (`hero/0` actions 3, 6, 7) and of ground mobs (`mob/60`, `mob/80`), every image's bottom edge is at `layer.y + height = 0`, so the +1 puts the lowest drawn row on the sprite's own `y` row (`tests/test_spriteview_real.py`). Floating mobs such as `mob/126` sit higher.

None of these affect parsing.

## Validation

- **Synthetic (CI)**: `tests/test_sprprj.py` covers a hand-written sample, the build/parse round trip, an error for every truncation, trailing bytes, image index out of range, unsafe names, huge `palette_len`, the timing model and the bounds.
- **Real assets (local, `real_assets`)**: `tests/test_sprprj_real.py` checks that all **138** files parse with no leftover bytes (17 empty, 860 images, 331 actions, 1,167 frames, 2,816 layers). Every layer's image exists as a PNG in `res/`, and all 163 palettes are exactly as long as their PNG's `PLTE`.
- CLI: `python3 -m mtport.sprprj check work/ipa/Payload/MapleThief.app/sprite` prints `files=138 images=860 actions=331 frames=1167 failures=0`. `dump FILE` lists one file.
- **Viewer** (issue #27): `PYTHONPATH=tools python3 -m mtport.spriteview build` writes `assets-out/viewers/sprites/`, one page per project with a player per action. Python composites each frame as [Drawing](#drawing) describes (palette swap, `y + 1`, layer alpha, both facings) onto one canvas per action, so the sprite position is the same canvas pixel in every frame. The page plays the frames for `max(delay, 1)` ticks of 65, 50 or 40 ms and toggles the anchor cross and the frame bounds. `strip PROJECT ACTION --out FILE.png` exports one action as a frame strip. `batch` renders every frame of all 138 projects in both facings and checks each frame's pixels against its layer offsets and bounds; it prints `projects=138 empty=17 actions=331 frames=1167 errors=0` (`tests/test_spriteview_real.py`).
