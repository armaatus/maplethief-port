# Speech: where the game's text can be heard by the host, and the system voices per platform

Research for [#378](https://github.com/armaatus/maplethief-port/issues/378): can the port read the game's text aloud, as an accessibility option? This note lists where each kind of text can be read **without changing what the guest does**, and the speech interfaces each target offers. Addresses are armv7 VM addresses; function pointers carry the Thumb bit. The speech layer itself is built ([ADR 0174](../adr/0174-text-to-speech-layer-system-voices-and-queue.md), #390), and so are the dialogue and the scenes' speech boxes with their speakers ([ADR 0187](../adr/0187-text-events-dialogue-and-scene-boxes-for-speech.md), #391, [below](#speakers)) and the notify lines and popups ([ADR 0191](../adr/0191-text-events-notify-lines-and-popups-for-speech.md), #392, [below](#notify-lines-and-popups)); the other text sources are the follow-ups listed at the end.

## Verdict

Yes, on every target but the Switch. The text the player reads is in a few known places, all readable from host code at the moment it appears, and every desktop and mobile target has a system voice a native program can call without a screen reader running. The Switch has none open to homebrew, so the option is hidden there (ADR 0142's capability table), until a bundled voice is worth its size.

## Where the text can be read

| Text | When it appears | Where the host can read it | How |
|---|---|---|---|
| NPC dialogue pages (a script's `showMsg`) | `CChatBox::FirstPage` `0x34CD1` (page 0), `CChatBox::NextPage` `0x34A05` (page + 1) | after either returns: the wrapped lines at `CChatBox +0x18`, their count at `+0x1C` ([qol.md](qol.md#fast-text)); the page's `CMsg {kind, text, face}` in the list `+0x14`; the speaker [below](#speakers) | a wrapper (`mt_hook_wrap`, ADR 0069) that calls the original and then reads; it writes nothing. **Built** (#391) |
| The box's answer buttons and quest rows | `CChatBox::Show` `0x3573D` (mode `+0x08`: 0 OK, 1 / 3 Accept / Refuse, 2 a list) | the rows `+0x20` (`CSelItem`, added by `AddRow` `0x357B5`); the selected row `+0x30` | the same wrapper, after the last page: **built** (#391). The row under the controller focus is not read yet |
| The scenes' speech box | the runner's command 50 sets it; each OK shows the next two lines | `CStateGame +0x1E9` (a box waits), speaker `+0x4B4`, lines `+0x4C8` (first shown) of `+0x4D0` | a wrapper around `CStateGame::Paint` reads it after each frame; speak when `+0x1E9` turns on or `+0x4C8` moves. **Built** (#391) |
| The notify list (pick-ups, meso, EXP) | `CNotifyTextList::Add` `0x78339` `(list, text, colour)` on `CStateGame +0x4C`; the lists `+0x50` / `+0x54` (top centre, the map's name) go through the same `Add` | its `text` argument, a NUL-ended string ([below](#notify-lines-and-popups)) | a wrapper that reads the text and calls the original. **Built** (#392) |
| Popups (death, confirmations, notices) | `Popup_Open` `0x3EAA1` `(mgr, id, text, kind)` (also observed for id `0x68`, ADR 0072) | after it, the `CMPopup` at `*0x11A96C`: the message's wrapped lines `+0x08` (count `+0x0C`), the kind `+0x10` (which says the buttons, images); the id `0x108C64` ([below](#notify-lines-and-popups)) | a wrapper that calls the original and then reads. **Built** (#392) |
| Item, equip and skill names and descriptions | when a list row or a tooltip shows in the popup menu (panel 1) | the data tables' fields ([text.md](text.md#the-data-tables)), by the row's item or skill id | by the controller focus's row (ADR 0051); the widget's row-to-id mapping is to be found |
| The shell's UIKit screens (main menu, character select, About, terms, alerts) | a control gains the focus, or a screen opens | `UILabel` / `UITextView` text in the runtime's own host classes (ADR 0037); the shell's buttons are custom buttons with images per state and **no title**, so a button needs a name from a port table keyed by its image | host code only: the runtime's view tree and the focus |
| The port's own menus | a row gains the focus, or a value changes | `platform/src/port_text.def` through `tr()` | host code only |

**Encodings.** Script and `ScriptExtra` text reaches the game as UTF-8 ([text.md](text.md#localizedcstring-0x6dd1)). The data tables' strings are ASCII (English) or CP949 (Korean, never valid UTF-8); the speech layer converts them with the runtime's Korean encodings (`runtime/src/cf/encoding.cpp`) before they reach a voice. A text pack's strings are UTF-8 throughout.

**Rejected: every string the game draws.** `TextCache_DrawText` `0x9031` sees each text texture as it is drawn. The chat box draws every line every frame and the HUD draws its numbers too, so this would need a frame-to-frame diff, has no idea what a string is (a damage number, a name, a page) and cannot tell a speaker from a line. The places above say what each text is, so the voice can say "who: what" and can drop, queue or interrupt by kind.

**Not a guess.** Unlike ADR 0053's rejected HP polling, every row above reads what the function or the state just set; none infers an event from a value changing for another reason.

## Speakers

Built in #391 ([ADR 0187](../adr/0187-text-events-dialogue-and-scene-boxes-for-speech.md), `maplethief/text_events.h`). No field holds the speaker's name as text: the box's paint looks it up every frame. The decompiles of `CChatBox::paint` `0x32BBD`, `CChatBox::drawChatBox` `0x35189` and their helpers give where each comes from. The text events read the same places without calling the game's lookups, which allocate on the guest heap.

**A dialogue page** (`CChatBox::paint`). The page's `CMsg` is `{vtable, text +0x04, kind +0x08, face +0x0C}` (`CMsg::CMsg` `0x31D95`); `FirstPage` and `NextPage` copy its kind to the box's `+0x0C`. Kind 0 (`AddHeroPage`, the script native `regMsgHero`) is the hero: the box draws the hero's face and the caption `ScriptExtra` `CChatBox.paint.1`. Kind 1 (`AddNpcPage`, `regMsgNPC`) is the NPC the box talks for, `CStateGame +0xAC` (the box's state is `+0x48`). `CChatBox::Show` sets it through `CChatBox::SetNpc` `0x34FA1`, which also puts the NPC's sprite in the box's `+0x40`.

- `+0xAC` 0 or more is an NPC id. Its name is in the NPC table (`*0xF1480`, the table's bytes at `0x11A988`): `CDataMgr::FindRow` `0x3990D` (mgr, table, column 0, id) walks the rows (count `+0x02`, stride `+0x04`, rows from `+0x06`) for the one whose byte at column 0 is the id, and `CDataMgr::ReadString` `0x37999` copies the name, column 1, 32 bytes ([text.md](text.md#the-data-tables)). English names are ASCII; Korean ones CP949.
- `+0xAC` −1 is the map's boss (the script native `setBoss`, `CChatBox::SetBossSpeaker` `0x32B4D`): `CMobs_FindBoss` `0x75711` takes the first mob in the mob list (`*0x11A9EC`: count `+0x04`, items `+0x08`) whose id (`+0xA0`) is 0x40 or more and not 0x41, and the paint draws its name `+0x9C` through `LocalizedCString` (`Scripts`), which returns a name that is no key unchanged.

**The answer** (`CChatBox::paint`, the last page). Mode `+0x08` 1 and 3 show the NPC key pad's Accept / Refuse buttons, images with no text. Mode 2 draws the rows `+0x20` (`CSelItem {vtable, key +0x04, value +0x08}`), each key localized in `Scripts`. Mode 0 shows OK. The required and reward items (`+0x24`, `+0x28`) are drawn on the last page too; they are not read yet.

**The scenes' speech box** (`CStateGame::Paint` `0x87501` → `drawChatBox(box, +0x4B4, lines, 2)`). Paint passes the lines `+0x4C8` and `+0x4C8 + 1` of `+0x4D0` that are below the count `+0x4D4`. The speaker `+0x4B4`:

| `+0x4B4` | Who | The name drawn |
|---|---|---|
| −1 | the hero | `ScriptExtra` `CChatBox.drawChatBox.1` |
| −2 | the map's boss | `CChatBox::SetBossSpeaker`, then the boss's `+0x9C` as it is |
| −0x40, −0x42 to −0x45 | the bosses of those mob ids | `ScriptExtra` `CChatBox.drawChatBox.2` to `.6` |
| 0 or more | an NPC id | the NPC table, as above (`SetNpc` first) |

Any other negative value would reach `strlen(NULL)`; no scene uses one.

**The text.** `Text_Wrap` `0x22EC5` breaks a line after its last space, keeping the space at the end of the line, or at a newline or `$`, which it drops. Joined back, a space goes only between two lines that have none. The scenes' strings end their first line in `\r`, which the wrap keeps. The text events make runs of white space one space.

**Measured** (`play.speech_dialogue`, Debug arm64, a new game, 2026-11-27): the intro's 5 speech boxes and MacArthur's 4 pages are 9 events and 9 utterances in the order the game shows them, each with a speaker and whole UTF-8; with speech on, the `rand()` state, the tick, the hero, the quests and the mobs at the field afterwards equal the run with speech off.

## Notify lines and popups

Built in #392 ([ADR 0191](../adr/0191-text-events-notify-lines-and-popups-for-speech.md), `maplethief/text_events.h`), from the decompiles of `CNotifyTextList::Add`, `CNotifyText::CNotifyText`, `Popup_Open`, `Popup_Close` `0x3E945`, the `CMPopup` setters and `Popup_Paint` `0x791F5`.

**A notify line.** `CNotifyTextList::Add` `0x78339` takes `(list, text, colour)`:

- `text` is a NUL-ended string the caller made: `CHero::setMeso` `0x618E1`, `CHero::addInven` `0x64309` and `CHero::setExp` `0x6654D` format a `ScriptExtra` string (`CHero.setMeso.3`, `CHero.addInven.1` / `.2` after the item's name, `CHero.setExp.2`) and the amount into a stack buffer; `CStateGame::Paint` passes the map's name (`0x87E28`, [map-names.md](map-names.md)). The text is UTF-8, but an item's name in a Korean game is CP949 (the tables), inside a UTF-8 sentence.
- `colour` is `0xRRGGBB` (white `0xFFFFFF` for the hero's lines).
- With the list's most entries `+0x0C` below 1 (the save's information lines off, [save.md](../formats/save.md)) it adds nothing. Otherwise, at the maximum, it deletes the oldest entry and takes its lines off the total `+0x08`; then a new `CNotifyText` (`0x7798D`, 0x18 bytes: `Text_Wrap` at 240 points into `+0x04`, count `+0x08`, colour `+0x0C`, the time `+0x10`) is appended to the entries `+0x04` and its line count added to `+0x08`.
- Eleven functions call it: the hero's meso, items and EXP on `CStateGame +0x4C`; the map's name on `+0x54`; quests and the scripts' notices (`0x5FF3D`, `0x6003D`, `0x601A5`, `0x605FD`, `0x7B305`, `0x7B68D`, `0x7EFF1`).

**A popup.** There is one popup at a time, the `CMPopup` at `*0x11A96C` (its constructor `0xE4E9`; `*0xF144C` points to `0x11A96C`). `Popup_Open` `0x3EAA1` `(mgr, id, text, kind)`:

1. calls `Popup_Close` `0x3E945` (frees the last message's lines, sets `0x108C64` to −1) and `Shell_EnableAlertKey` (the key pad's panel 7);
2. stores the id in `0x108C64`; for the death popup (`0x68`) reads the revive restore counts into `mgr +0x694` / `+0x698` ([cash-shop.md](cash-shop.md));
3. copies the message into `+0x04` (`CMPopup::SetText` `0xE861`, a `new[]` copy): the preset at `0x108C70 + id × 0x200` (100 strings of 512 bytes) when the id is below 100 and the preset is set, else `text`, else "". What fills the presets is not established: no function in the export writes the table by address, and `CStateGame::SetPigmeState` `0x823A1` only reads two of them (`+0x4600`, `+0x4800`) as NPC pages;
4. sets the kind `+0x10` and its two labels `+0x70` / `+0x74` (`CMPopup::SetKind` `0xE799`, `CMPopup::SetButtons` `0xE781`), and `+0x78` (`0xE851`); then `+0x14` = 0 and `+0x18` = 1 (popup 5's amount);
5. lays it out (`CMPopup::Layout` `0xE8B5`: 302 × 170 at most 240 high, 18-point lines) and wraps the message with `Text_Wrap` into `+0x08`, count `+0x0C`. `$` breaks a line.

| Kind `+0x10` | Buttons (`Popup_PlaceShellButtons` `0x78E91`) | Label pointers `+0x70` / `+0x74` (the `CMPopup` fields they copy, `ScriptExtra` keys; freed, [below](#notify-lines-and-popups)) |
|---|---|---|
| 0 | none: the Empty button over the text, a tap anywhere is OK | not set (the last popup's stay) |
| 1 | Yes / No | `+0x20` `CMPopup.CMPopup.1`, `+0x24` `.2` |
| 2, 5 | Yes / No | `+0x2C` `.4`, `+0x24` `.2` |
| 3, 6 | OK | `+0x28` `.3`, "" |
| 4 | OK2 / Cancel, with − / + and an amount (`+0x18`, × the price `+0x1C`) | `+0x28` `.3`, "" |

A popup has no title, and its buttons are images. The labels are no text to read: `LocalizedCString` returns the `UTF8String` of an autoreleased `NSString`, so the pointers the constructor keeps in `+0x20`–`+0x2C` (and `SetKind` copies to `+0x70` / `+0x74`) point at freed buffers once the pool drains (#392's run read garbage there). The key pad's images draw the buttons; without the key pad, `0x78A65` draws images from `*(*0xF143C) +0xFC` too. The speech says the buttons by the kind. The **death popup** is `Popup_Open(mgr, 0x68, "", 0)` (`CHero::Flush` `0x62ADD`, and `CStateGame::OnKeyPressed` `0x8D4C9` reopening it after each of its sub-popups): no message, and `CStateGame::Paint` places its four image buttons (Grade revival, Revival, Return to town, Go to main menu, [keypad.md](../controls/keypad.md#panel-7-popups-popupkeypadview)). Its Return to town opens `0x69`, kind 1, "`CStateGame.OnKeyPressed.1`$`.2`: N `.3`$`.4`: 10% `.5`" (or `.6` alone below level 10).

The hero's lines are formatted into a buffer in the caller's stack frame, so the text events read `Add`'s text there too (from `sp` up), not only in the image and the heap.

**Measured** (`play.speech_notices_synthetic`, Debug arm64, a built level-10 Beginner on 2.3, 2026-11-27): a kill, a Red Snail Shell and meso picked up, a fall and Return to town gave 7 notice events (the map's name twice on `+0x54`, five lines on `+0x4C`: the EXP, the pick-up, the meso, the death's "EXP obtained.(+0)" and "Lost Meso(s)") and the popups `0x68` (kind 0, no message) then `0x69` (kind 1); 6 notice utterances and 2 dialogue ones. A line the list drops within the turn it came (its most entries reached) is still an event. The `rand()` state, the tick, the hero, the quests and the mobs equal the run with speech off.

## System voices per platform

| Target | Interface | Notes |
|---|---|---|
| macOS 13+ | `AVSpeechSynthesizer` (AVFoundation) | Objective-C++ in the platform layer. Voices per language (`AVSpeechSynthesisVoice voiceWithLanguage:`), rate, pitch and volume per utterance; `stopSpeakingAtBoundary:` to interrupt. `NSSpeechSynthesizer` is deprecated. With VoiceOver on, `NSAccessibilityPostNotification` with an announcement goes to the user's screen reader instead. |
| iOS 16+ | `AVSpeechSynthesizer` | Same API. It speaks through its own audio session unless `usesApplicationAudioSession` is set; with it set, the speech mixes with the game's session (the port's audio session, `platform/src/audio_session.cpp`, ADR 0045), and the game can duck its music while speaking. `UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, …)` goes to VoiceOver. |
| Windows 10/11 | SAPI 5 (`ISpVoice`, `sapi.h`, part of the Windows SDK and of every Windows) | COM, `Speak` with `SPF_ASYNC` and `SPF_PURGEBEFORESPEAK` to interrupt; plays through its own audio output. The WinRT `Windows.Media.SpeechSynthesis.SpeechSynthesizer` has the newer voices but returns a stream to play, which would go through our mixer. A screen reader (Narrator, NVDA, JAWS) is reached through UI Automation's `UiaRaiseNotificationEvent`. |
| Linux, Steam Deck | Speech Dispatcher (`libspeechd`, C) | Loaded with `dlopen` (`libspeechd.so.2`) so the game runs without it; `spd_say` with priority `SPD_TEXT` / `SPD_MESSAGE`, `spd_cancel`. Orca, the Linux screen reader, speaks through it. SteamOS 3.7.13 added Orca and espeak-ng for its own screen reader; whether `speech-dispatcher` is installed and running in Game Mode is to be checked on a Deck. Without it the option says it is unavailable. |
| Nintendo Switch (homebrew) | none | libnx exposes no speech service. Nintendo's own voice features are system applets, not a service a program can call. A bundled synthesizer (flite, BSD-style licence; espeak-ng is GPL-3 and would bind the NRO) is possible but costs size and voices per language; not planned. |

SDL3 has no speech API.

## Follow-ups

Epic [#398](https://github.com/armaatus/maplethief-port/issues/398) (Backlog): [#390](https://github.com/armaatus/maplethief-port/issues/390) the speech layer (backends, queue, settings, the capability), [#391](https://github.com/armaatus/maplethief-port/issues/391) dialogue and the scenes' speech boxes, [#392](https://github.com/armaatus/maplethief-port/issues/392) notify lines and popups, [#393](https://github.com/armaatus/maplethief-port/issues/393) the menus. The design decisions are in #390's [ADR 0174](../adr/0174-text-to-speech-layer-system-voices-and-queue.md).
