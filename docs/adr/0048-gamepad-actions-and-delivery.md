# 48. Gamepads as key pad actions: one action layer, delivered once per game tick

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #67
- **Builds on:** [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`Input`, `Input::touch` for virtual touches, the activity listener), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`mt_uikit_turn`), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (the virtual clock of the scripted runs); the facts are [docs/controls/keypad.md](../controls/keypad.md) (#66)

## Context

The game reads its controls in two ways ([docs/controls/keypad.md](../controls/keypad.md)):

- **The pad's BOOLs.** In the field (key pad panel 0), `TouchKeyPadView`'s touch handlers set ten BOOL properties (`onLeftKey` … `onShort4Key`, ivars +104 to +113), and `SkyCanvas::Tick` `0x2590D` polls them once per tick. It turns the held set into **one key per tick** by a fixed priority (attack first; d-pad + jump chords `'1'`/`'3'` before plain directions), releasing the old key a tick before pressing the new one. The pad is live while `touchKeyPadEnable` (+228) is set.
- **Key events.** Every other button calls `Clet_HandleEvent(2 press | 3 release, code, 0)` `0x5A75`. `SkyCanvas` keeps **one** pending press (+0xD8) and **one** pending release (+0xE8), −1000 meaning none, and a second event before its tick overwrites the first.

`EAGLView.statusOfTouchKeyPad` (ivar +92) says which panel is up: 0 the game pad, 1 the popup menu, 3 a cut scene, 5 NPC chat, 6 the shop, 7 a popup, 8 the map, 9 the cash shop, 10 help.

A gamepad must reach both: movement, jump, attack and the four quick slots in the field, and OK / back / tabs in menus. It must play the same as the on-screen keys (#67's acceptance criteria: holding directions or attack on the pad moves the hero exactly as holding the keys does).

What had to be decided:

- **Where device input becomes game input**: synthetic touches on the key pad, or the game's own key entry points.
- **How events reach the guest** without being lost to `SkyCanvas`'s single slots, and without breaking the tick's chords and priority.
- **The context**: which bindings apply when.
- **The stick, the triggers, several pads, hot-plug, focus loss.**
- **What has no key**: actions only a touch can do.

## Options considered

For the delivery:

1. **Synthetic touches on the key pad** (a virtual finger on the d-pad's left arm for "left"). The game would see exactly what a finger does. But each action needs a point that depends on the panel and the mask, a chord needs two fingers and the pad's own 50-point finger matching, and on panels without that key (menus) a touch hits whatever button is there. Fragile, and not what the keys of a menu mean.
2. **Call the engine's key handlers directly** (`CStateGame::OnKeyPressed`). It skips `SkyCanvas`'s tick, so chords, the priority and the two-tick switch would have to be re-made, and the engine would see input outside its frame.
3. **The key pad's own entry points: the BOOLs for the pad keys, `Clet_HandleEvent` for the rest. Chosen.** A press sets the same BOOL a finger sets (through its property setter, `objc_msgSend`), so `Tick` stays the one place that turns held keys into key codes; every other action goes in through the function every key pad button calls.

For the timing:

1. **Deliver at once, as the event arrives.** Two events within one tick (a quick tap: press and release in one poll) overwrite each other in `SkyCanvas`'s slots, or a BOOL set and cleared before the tick polls it is never seen.
2. **A FIFO, one step per game tick, only while both of `SkyCanvas`'s slots are empty. Chosen.** Nothing is lost; a tap lasts at least one tick.

## Decision

**The runtime's action layer (`maplethief/input.h`, `runtime/src/input/input.cpp`)** knows the guest and nothing of SDL:

- **The actions** are a fixed table, `mt_input_action` with a stable name (`mt_input_action_named`): `up`, `down`, `left`, `right`, `jump`, `attack`, `slot1`–`slot4` (each with its pad BOOL and its key code: −1, −2, −3, −4, `'2'`, `'5'`, `'7'`, `'9'`, `'0'`, `'#'`), and the key-only `ok` (−5), `back` (−16), `menu` (−16), `map` (`'*'`), `cash_shop` (−26), `minus` (`'4'`), `plus` (`'6'`), `revive` (−25), `grade_revive` (−35). Bindings files (#70) name actions by these names.
- **`mt_input_press` / `mt_input_release`** queue an event. Presses are counted per action: the guest sees the first press and the last release (an X button and the right trigger both bound to attack).
- **`mt_input_turn`** runs first in every `mt_uikit_turn`, before the queued touches. It reads the panel and whether the pad is live (the game's one `EAGLView`, found in the windows), and the tick from CMTimer's `lastFrame` (`0x11A4D8`, an int64 the game sets before every `SkyCanvas::Tick`). It applies **at most one step per tick, and only while both of `SkyCanvas`'s slots (`0x11A950` → +0xD8, +0xE8) are empty**. A step is either:
  - **a batch of BOOL changes for different pad keys**, while the pad is live: each through its setter (`setOnLeftKey:`), then `checkCrossKeyPad` if a direction changed, as the touch handlers do. Keys pressed together reach the tick together, like two fingers, so left + jump is the tick's own `'1'`;
  - or **one `Clet_HandleEvent(2|3, code, 0)`** (`mt_call_guest`): the key-only actions, and the pad actions while the pad is off.
- **A release goes the way its press went.** A BOOL set while the pad was live is cleared through the setter even if the pad is off by then (a popup opened mid-walk), and a key press is released as a key.
- The guest addresses are `mt_input_config` (defaults the game's); reads check that the address is mapped (an image segment or the committed heap), so a synthetic guest without the image is never read out of its arena. Trace channel `input`; counters in `mt_input_get_stats`.

**The frontend's bindings (`platform/src/actions.{h,cpp}`, `Actions`)** map a device control to an action:

- A **control** is a direction (the d-pad and the stick merged; the arrow keys of #68), a gamepad button, a gamepad trigger or a key (#68). The table is data (`default_gamepad_bindings()`, keypad.md's proposed mapping): each row has a field action and a menu action.
- **The context** is the key pad's panel (`mt_input_panel`): panel 0 is the field, every other panel (and none) a menu. A press resolves its action by the context at that moment; its release releases that same action, whatever the context is by then, so a menu that opens during a walk does not leave the walk held.
- **The default gamepad bindings** (SDL3's positional names; South is A on an Xbox pad and Cross on a PlayStation pad):

  | Control | Field | Menus |
  |---|---|---|
  | d-pad / left stick | up, down, left, right | up, down, left, right |
  | South (A, Cross) | jump | OK |
  | West (X, Square) | attack | revive |
  | East (B, Circle) | quick slot 2 | back |
  | North (Y, Triangle) | quick slot 1 | grade revive |
  | left shoulder | quick slot 3 | minus |
  | right shoulder | quick slot 4 | plus |
  | right trigger | attack | — |
  | left trigger | cash shop | — |
  | Start | menu (the popup menu) | back |
  | Back (View) | map | map |

- **Touch fallbacks**: an action that has no key code in a panel becomes a synthetic touch through `Input::touch` (virtual fingers `0x1010`+), driven one step per turn by `Actions::turn()`. Implemented: the help pages (panel 10, `HelpScrollKeyPadView`, a paging scroll view its keys do not turn): right is a 300-point swipe to the left over 8 turns (next page), left the reverse. Left to #69: the popup menu's shortcut-panel toggle (GL widget 11, item 1, no key code), item double tap and drag in its submenus, the main menu's UIKit buttons other than Play and the terms' agreement button, `ChooseCharacterView`'s slot selection (#75 touches the character screens). NPC chat's quest rows are reachable by the keys already.

**Gamepads (`platform/src/gamepad.{h,cpp}`, `Gamepads`)** turn SDL3 gamepad events into controls:

- **The left stick** has a radial dead zone (engages at 0.30 of its range, lets go below 0.22) and **8-way d-pad emulation**: each direction covers 135° around its axis (its own 45° sector and the two diagonals beside it). A sector edge moves 8° outwards while its direction is held and 8° inwards while it is not, so a stick on an edge does not flicker. A diagonal holds two directions; the tick chooses (left and right before up and down). The d-pad and the stick are merged: a direction is held while either holds it.
- **The triggers** press past 0.5 and release below 0.4.
- **Hot-plug**: `SDL_EVENT_GAMEPAD_ADDED` opens a pad (and `open_connected()` at start-up), `_REMOVED` closes it and releases what it held. Events of a pad SDL reports as no longer connected are ignored: SDL sends recentring axis events for a pad that is going away, which would otherwise press a direction.
- **Several pads: the first active one wins.** A pad becomes the active one by a press while no other pad holds anything; until then its input is ignored (a control held during that time is not pressed later). The active pad (`Gamepads::active()`) is the one #72 rumbles.
- **Focus lost, window hidden or minimised** release everything held; a control counts again only once it is let go and pressed (the stick: once it moves).
- Button presses reach `Input`'s activity listener through `Input::handle` (ADR 0047); stick and trigger presses through `Input::note` (#71).
- `main.cpp` starts `SDL_INIT_GAMEPAD` for the game; `game.cpp` pumps every event through `Input` and `Gamepads` and calls `Actions::turn()` once per turn.

## Consequences

- A gamepad plays the game through the game's own input path: chords, priority, the two-tick switch, "a held walk key blocks new presses" and "attack fires once per press" are the engine's, not re-made. Holding the d-pad and attack on a pad moves the hero tick for tick as holding the on-screen keys does: `input.gamepad_hold` (real assets) holds them once as key pad touches (the #65 path) and once as a virtual Xbox 360 pad on the same turns, and the hero's per-tick x, y, state, flags, HP and MP are byte-identical (118 ticks: walk left, attack, walk right with attack).
- `input.gamepad_play` (real assets) plays from a fresh install to the first map with a virtual Xbox 360 pad's A, then with an Xbox 360, a PS4 and a mapped PS5 pad plugged in in turn: walk by stick and d-pad, jump, attack by X and by the right trigger, Double Step by LB, a Blue Potion by B and a Red Potion by Y, each checked in the hero's guest state (CHero x +0x4, y +0x8, state +0x10, HP +0x94, MP +0x98, the item counts).
- `tests/cpp/gamepad_test.cpp` (CI, every OS) checks the stick's zones and sectors, every default binding in the field and in a menu on SDL3 virtual joysticks of the Xbox 360, PS4 and PS5 (with a mapping) kinds by `SDL_GetGamepadTypeForID`, the merge, the trigger thresholds, a release across a context change, hot-plug, two pads and focus loss, the help swipe, and the runtime's delivery into a synthetic `EAGLView`, `TouchKeyPadView` and `SkyCanvas`: nothing lost, one step per tick, batches, a busy slot waits, a release by its press's route.
- An action takes at least one tick to reach the guest and a tap lasts at least one tick: the scripted runs see a tick every 50 ms (118 ticks in `input.gamepad_hold`'s 6 s), so that is at most one tick of added latency, and a press–release pair never vanishes.
- The pad BOOLs only work while the pad is live. With the pad off in the field (a cut scene sets panel 3), the pad actions go in as key codes, which the engine treats as single presses (no held walk). That is what the device does with its keys there.
- What only a touch can do stays open for #69, listed above. A bindings UI (#70) edits `Actions::bindings()` rows and stores action names.
- Real pads are not in CI: an Xbox and a PlayStation pad on Joris's machines are a needs-human check on #67.
