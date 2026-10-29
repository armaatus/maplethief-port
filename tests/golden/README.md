# Golden images

Two kinds live here ([ADR 0005](../../docs/adr/0005-test-harness.md), [ADR 0044](../../docs/adr/0044-virtual-clock-and-device-golden-screens.md)):

- **Synthetic goldens** (`data/`, committed): images our tests draw from code, checked with `golden.h` (`check_golden`). Nothing in them comes from the game.
- **Game screens** (`work/golden/`, never committed): screenshots from a real device and our own renders of the same screens. Both are game art. The list of screens, how to reach them and how to compare them is [`device_screens.json`](device_screens.json), our own data.

```
work/golden/device/<name>.png   screenshots from the device (Joris), never written by a tool
work/golden/ours/<name>.png     our own recorded renders (MAPLETHIEF_UPDATE_GOLDEN=1)
```

## The screens

| Name | Run | Shot | What it shows |
|---|---|---|---|
| `title` | fresh | 2.5 s | the main menu after its slide-in, with the promo banner: the first still screen after launch (the game has no title screen of its own; see below) |
| `main_menu` | fresh | 4 s | the main menu with the banner closed |
| `intro_dialog` | fresh | 28.5 s | the intro scene's first speech box (Roy) with its orange border, the game's `GL_LINES` ([#121](https://github.com/armaatus/maplethief-port/issues/121)) |
| `npc_dialog` | fresh | 50 s | MacArthur's first dialog on the first map |
| `first_map` | fresh | 56 s | the first playable map with the HUD and the key pad |
| `popup` | fresh | 59 s | "This cannot be used right now." after the cash shop button |
| `character_select` | saved | 5 s | the character select on the second launch |

**"Title".** Before the game draws, iOS shows the launch image (`Default.png`, the Nexon logo), which is SpringBoard's, not the game's. The game's title state (`CStateTitle`) draws only a black fade, and its first visible frame is already the main menu sliding in. So the title in these tests is the main menu as it stands after the slide-in, banner and all ([ANALYSIS.md → Program flow](../../docs/ANALYSIS.md#program-flow)).

Times are virtual seconds after the first run-loop turn (`MapleThief --fixed-step 1`); the taps of each run are in `device_screens.json`. The `saved` run starts from the data directory the `fresh` run left (slot 0 is saved when Play starts a new game).

## Capturing on the device (needs a human)

**Device.** Any device that runs the 32-bit game 1.5: iOS 3.1.2 to 10.3.3 (iOS 11 dropped 32-bit apps), e.g. iPod touch 3rd/4th/5th generation, iPhone 3GS to 5c. Every such device has a Home button. Note the model and the iOS version. The system language must be **English** (the port runs the `en` strings).

**State.** The `fresh` screens need a fresh install of version 1.5: no saved character. Deleting the app deletes its saves; back them up first (a device backup) if you want to keep them. If you cannot reinstall, delete every character in the character select (Delete) first, and leave the game's options at their defaults; then Play starts a new game as on a fresh install.

**Taking a screenshot.** Press Home and Sleep/Wake (the top or side button) together briefly. The screen flashes white; the picture goes to the Camera Roll. Take each screenshot when nothing moves any more.

**Steps.** In this order, in one session, then the second launch:

1. `title`: launch the game. The Nexon logo shows, then the main menu slides in. Wait until nothing moves (about 3 s after the menu appears). Screenshot.
2. `main_menu`: tap the close button (x) at the right end of the promo banner at the bottom. Wait 1 s. Screenshot.
3. `intro_dialog`: tap Play. The loading screen, then the intro scene ("Peaceful Henesys", "I'm Robin") play by themselves for about 20 s. When the first speech box (Roy: "Robin! Are you insane? ...") shows at the bottom, wait 2 s. Screenshot. Do not tap yet.
4. `npc_dialog`: tap the speech box; tap again for each next line, 5 taps in all, waiting for each line. After the 5th ("By the way, MacArthur wanted to talk to you. Go see him.") the screen fades and the map loads. When MacArthur's dialog box ("Robin! Where've you been? ...") shows in the middle, wait 2 s. Screenshot.
5. `first_map`: tap OK 4 times, waiting for each new text. After the 4th the dialog closes and the HUD and the key pad appear. Wait 2 s. Screenshot. Do not touch the key pad.
6. `popup`: tap the cash shop button ($, the orange coin at the top right). The popup "This cannot be used right now." appears. Wait 1 s. Screenshot. Then tap Ok.
7. `character_select`: press Home, then double-press Home and close MapleThief in the app switcher (iOS 4 to 6: hold its icon, tap the red minus; iOS 7 and later: swipe it up). Launch it again. When the main menu has slid in, tap Play (the banner may stay). Wait 2 s. Screenshot.

**Files.** Copy the seven screenshots to the Mac as PNG, unchanged: Image Capture (or Photos' "Export Unmodified Original") over USB, or AirDrop on iOS 7 and later. Not JPEG, not HEIC, not a re-encoded or resized copy, not a photo of the screen. Name them exactly:

```
work/golden/device/title.png
work/golden/device/main_menu.png
work/golden/device/intro_dialog.png
work/golden/device/npc_dialog.png
work/golden/device/first_map.png
work/golden/device/popup.png
work/golden/device/character_select.png
```

Any of the forms the device saves is fine; the harness turns and scales them (below).

## Running the comparison

```sh
# all screens, after building with work/gen (scripts/bootstrap, python -m recomp all):
ctest --preset mac-debug -R platform.device_golden --output-on-failure
# or directly (any build; Release is about 4x faster):
PYTHONPATH=tools python3 -m mtport.devicegolden \
    --binary build/mac-debug/platform/MapleThief.app/Contents/MacOS/MapleThief \
    --out build/test-output/mac-debug/device_golden [--only title,main_menu] [--repeat 2]
# record our own renders (after a deliberate rendering change; look at them first):
MAPLETHIEF_UPDATE_GOLDEN=1 ctest --preset mac-debug -R platform.device_golden
# compare one pair by hand:
PYTHONPATH=tools python3 -m mtport.goldencmp ours.png shot.png --channel 16 --ratio 0.01 \
    --mask 180,84,200,120 --diff diff.png --normalised shot-480x320.png
```

Each screen gives two results: **ours** (against `work/golden/ours/`, exact) and **device** (against `work/golden/device/`). A missing golden is skipped with a message ("not captured yet"), not failed; the test reports skipped (77) when the game cannot start or every case was skipped. The output directory gets `<name>.actual.png` (our frame), `<name>.device.png` (the normalised screenshot), `<name>.device.diff.png`, `<name>.ours.diff.png` (on a mismatch) and `runs/<run>/` (the game's log, shots and data directory). Diff colours as in `golden.h`: grey matches, yellow within the channel tolerance, red differs, magenta outside the other image, dark blue masked.

## Normalisation

`mtport.goldencmp.normalise` makes every screenshot the 480 × 320 landscape screen of [ADR 0037](../../docs/adr/0037-uikit-host-classes-nib-and-compositor.md):

| Saved as | Done |
|---|---|
| 480 × 320 | nothing |
| 320 × 480 | portrait (the window's orientation): turned with ADR 0037's mapping, screen pixel (x, y) = window pixel (y, 479 − x), a quarter turn clockwise |
| 960 × 640 | Retina: the game is not Retina-aware, so iOS shows it at 2×; each 2 × 2 block averaged |
| 640 × 960 | turned, then averaged |
| 1136 × 640, 640 × 1136 | a 4-inch device shows the game letterboxed: the 88-pixel bars must be black and are cropped, then as above |

Other sizes are rejected with a message. Alpha becomes 255. Colour profiles are ignored (the stored values are compared).

**Retina and the tolerance.** On a Retina device iOS scales the 480 × 320 GL layer up with filtering, and draws UIKit's text and vector content at 2×. Averaging the 2 × 2 blocks gives back an image close to the 1× screen, but every edge is softer, so a 2× screenshot gets the looser `device_2x` tolerance.

## Tolerance and masks

A pixel differs when any channel is off by more than `channel`; a screen matches when at most `ratio` of its compared (unmasked) pixels differ (golden.h's rule).

| Tolerance | channel | ratio | For |
|---|---|---|---|
| `ours` | 0 | 0 | our recorded renders: the runs are deterministic |
| `device_1x` | 16 | 1 % | 480 × 320 / 320 × 480 screenshots |
| `device_2x` | 32 | 5 % | Retina and 4-inch screenshots |

The device numbers are **starting values**: they are set once the first real screenshots are compared. Known differences that stay inside them: texture filtering and blending round differently on the device's GPU; edge fringes are darker by design (premultiplied texels under `SRC_ALPHA` blending, as on the device, [ADR 0041](../../docs/adr/0041-texture-formats-and-premultiplied-alpha.md)); white is 254 (`glColor4x(0xFF00)`).

Masks (per screen in `device_screens.json`) leave out what differs for a known reason:

- **text**: every engine text texture and UIKit label is drawn with NanumGothic instead of MarkerFelt-Thin / Helvetica ([#118](https://github.com/armaatus/maplethief-port/issues/118)): the speech and dialog text and names, name tags, popup text, button labels;
- **animations**: the first map's two portals change every few frames, so the screenshot catches another phase.

The main menu, title and character select need no masks: their text is images and nothing on them moves once they are still. The intro speech box's orange border is drawn with `GL_LINES` (#121); its masks leave the text out and keep the border compared. MacArthur's dialog (`npc_dialog`) is a UIKit view over the GL frame: our render of it is the same before and after #121 (0 pixels differ).

## Determinism

The fixed-step mode puts one virtual clock behind every clock the guest reads, so a scripted run reaches the same frames bit for bit: `--repeat 2` checks it, and Debug, Release and the universal binary under `arch -x86_64` give identical shots ([ADR 0044](../../docs/adr/0044-virtual-clock-and-device-golden-screens.md)).
