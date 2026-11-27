# Speech: where the game's text can be heard by the host, and the system voices per platform

Research for [#378](https://github.com/armaatus/maplethief-port/issues/378): can the port read the game's text aloud, as an accessibility option? This note lists where each kind of text can be read **without changing what the guest does**, and the speech interfaces each target offers. Addresses are armv7 VM addresses; function pointers carry the Thumb bit. The speech layer itself is built ([ADR 0174](../adr/0174-text-to-speech-layer-system-voices-and-queue.md), #390), and so are the dialogue and the scenes' speech boxes with their speakers ([ADR 0187](../adr/0187-text-events-dialogue-and-scene-boxes-for-speech.md), #391, [below](#speakers)); the other text sources are the follow-ups listed at the end.

## Verdict

Yes, on every target but the Switch. The text the player reads is in a few known places, all readable from host code at the moment it appears, and every desktop and mobile target has a system voice a native program can call without a screen reader running. The Switch has none open to homebrew, so the option is hidden there (ADR 0142's capability table), until a bundled voice is worth its size.

## Where the text can be read

| Text | When it appears | Where the host can read it | How |
|---|---|---|---|
| NPC dialogue pages (a script's `showMsg`) | `CChatBox::FirstPage` `0x34CD1` (page 0), `CChatBox::NextPage` `0x34A05` (page + 1) | after either returns: the wrapped lines at `CChatBox +0x18`, their count at `+0x1C` ([qol.md](qol.md#fast-text)); the page's `CMsg {kind, text, face}` in the list `+0x14`; the speaker [below](#speakers) | a wrapper (`mt_hook_wrap`, ADR 0069) that calls the original and then reads; it writes nothing. **Built** (#391) |
| The box's answer buttons and quest rows | `CChatBox::Show` `0x3573D` (mode `+0x08`: 0 OK, 1 / 3 Accept / Refuse, 2 a list) | the rows `+0x20` (`CSelItem`, added by `AddRow` `0x357B5`); the selected row `+0x30` | the same wrapper, after the last page: **built** (#391). The row under the controller focus is not read yet |
| The scenes' speech box | the runner's command 50 sets it; each OK shows the next two lines | `CStateGame +0x1E9` (a box waits), speaker `+0x4B4`, lines `+0x4C8` (first shown) of `+0x4D0` | a wrapper around `CStateGame::Paint` reads it after each frame; speak when `+0x1E9` turns on or `+0x4C8` moves. **Built** (#391) |
| The notify list (pick-ups, meso, EXP) | `CNotifyTextList::Add` `0x78339` on `CStateGame +0x4C` | the new `CNotifyText` (`0x7798D`): lines `+0x04`, count `+0x08` ([hud-extras.md](hud-extras.md)) | an observed entry (ADR 0053) or a wrapper; the argument layout of `Add` is to be confirmed. The lists `+0x50` / `+0x54` (top centre) go through the same `Add` |
| Popups (death, confirmations, notices) | `Popup_Open` `0x3EAA1`, id in its arguments (already observed for id `0x68`, ADR 0072) | not yet known: the popup's strings come from `ScriptExtra` through `LocalizedCString` `0x6DD1`, but which member holds them is to be found | a decompile of `Popup_Open` and the popup's paint |
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
