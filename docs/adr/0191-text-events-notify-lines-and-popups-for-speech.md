# 191. Text events: notify lines and popups for the speech, merged per turn and spoken like a page

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#392](https://github.com/armaatus/maplethief-port/issues/392) (epic [#398](https://github.com/armaatus/maplethief-port/issues/398))
- **Builds on:** [ADR 0187](0187-text-events-dialogue-and-scene-boxes-for-speech.md) (the text events, `SpeechText`), [ADR 0174](0174-text-to-speech-layer-system-voices-and-queue.md) (the speech layer and its queue rules), [ADR 0069](0069-guest-function-hooks.md) (wrappers), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) and [ADR 0072](0072-offline-cash-shop-and-revives.md) (`Popup_Open` already observed for the death popup)

## Context

#392 adds the two other places [docs/re/speech.md](../re/speech.md) found the game's text whole: the notify lines (a pick-up, meso, EXP, the map's name) and the engine's popups (the death popup, confirmations, notices). The decompiler settled what was open in #378 ([speech.md](../re/speech.md#notify-lines-and-popups)):

- `CNotifyTextList::Add` `0x78339` is `(list, text, colour)`: the text a NUL-ended string the caller built, the colour `0xRRGGBB`. It adds nothing while the list's most entries (`+0x0C`, the save's "information lines") is below 1.
- `Popup_Open` `0x3EAA1` is `(mgr, id, text, kind)`. The one `CMPopup` (`*0x11A96C`) gets the message (`+0x04`, a copy of a preset or of `text`), wrapped at `$` and the width (`+0x08`, count `+0x0C`), the kind (`+0x10`) and the two button labels the kind uses (`+0x70`, `+0x74`, the localized "Yes", "No", "OK" or ""). A popup has no title. The death popup (`0x68`) has no message at all: its four choices are images the field draws.

A fight adds several notify lines a second, and dialogue must not be drowned out.

## Options considered

1. **Wrappers, as the pages** (chosen). Both functions become hookable; a wrapper reads the text (Add's argument, before the original runs) or the popup (after it), once, as it is set.
2. **Observed entries** (ADR 0053). `Popup_Open` is already one for the cash shop, but an entry sees the arguments before the preset is picked and the message wrapped, and the observer's listeners are the game events', not the text events'.
3. **Polling the list's entries or the popup id every turn**. The list drops its oldest line when full and a popup can be closed and reopened (the death popup after each sub-popup) within one turn: a diff would miss or double lines.
4. **One utterance per notify line.** A fight's burst would fill the speech's queue (4) at once and drop most of them, each said as a separate short sentence.

## Decision

**Runtime.** Two more text event kinds (`maplethief/text_events.h`):

- **notice** (`MT_TEXT_EVENT_NOTICE`): `CNotifyTextList::Add`'s text (runs of white space as one, CP949 converted), when the list keeps lines; `value` says which of the current `CStateGame`'s lists it is (0 `+0x4C` under the compass, 1 `+0x50`, 2 `+0x54` the map's name; −1 another), `index` the colour.
- **popup** (`MT_TEXT_EVENT_POPUP`): after `Popup_Open`, the message's wrapped lines joined, `index` the id (`0x108C64`), `value` the kind, `speaker` the button labels of kinds 1 to 6, one per line (kind 0 has none: a tap anywhere is OK, and `+0x70` still holds the last popup's).

As in ADR 0187, nothing is written and no guest function is called; `runtime_test` runs a synthetic list and popup with the wrappers on and off and compares the guest byte for byte.

**Platform** (`SpeechText`):

- A **popup** is `SpeechKind::kDialogue`, like a page: it interrupts. Its message, then its buttons ("Yes or No.", "OK.", port text around the game's labels). The death popup says the port's words for its image buttons (`speech_death_popup`).
- The **notify lines** of one run-loop turn are **merged** into one `kNotice` utterance ("EXP obtained.(+7). Meso obtained.(+70)."), handed to the speech after that turn's dialogue: a notice queues behind what is being said (ADR 0174's rule), so a page or a popup is never cut off by one, and a long fight keeps at most the queue's 4 waiting, the oldest dropped.
- **`speech_notices = on | off`** (Accessibility, `--speech-notices`, default on, live, needs `kCapSpeech`): off says no notify line; popups and dialogue are still said. ADR 0174 had no such setting. The default is on because the lines are what the player would otherwise only see (a pick-up, a level's EXP).

## Consequences

- Two more hookable functions (`MT_HOOK_FN_NOTIFY_TEXT_ADD`, `MT_HOOK_FN_POPUP_OPEN`); the generated code changes in two places. `Popup_Open` stays observed for the cash shop: the observer runs in the body, the wrapper around it.
- The wrappers run only while speech is on; off, the game runs as before (ADR 0187's rule).
- A Korean item name in a pick-up line comes from the tables in CP949 inside a UTF-8 sentence; a line that is not valid UTF-8 as a whole is read as CP949, so such a mixed line can come out with a wrong word. English, and every line made only of `.strings` text, are exact.
- The popup menu's tabs and lists, the shop and the shell's screens are #393's, not here.
