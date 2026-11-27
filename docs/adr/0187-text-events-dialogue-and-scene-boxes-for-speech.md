# 187. Text events: the dialogue and the scenes' speech boxes read where the game sets them, for the speech

- **Status:** Accepted (delegated by Joris, 2026-11-27)
- **Date:** 2026-11-27
- **Issue:** [#391](https://github.com/armaatus/maplethief-port/issues/391) (epic [#398](https://github.com/armaatus/maplethief-port/issues/398))
- **Builds on:** [ADR 0174](0174-text-to-speech-layer-system-voices-and-queue.md) (the speech layer and its queue rules), [ADR 0069](0069-guest-function-hooks.md) and its wrapper addendum (hooks and wrappers), [ADR 0053](0053-observed-functions-game-events-and-rumble.md) (game events), [ADR 0096](0096-scene-skip-runs-every-frame-faster.md) (the scene skip)

## Context

The speech layer (#390) speaks what it is handed. #391 hands it the NPC dialogue (a script's `showMsg`: pages, then an answer) and the scenes' speech boxes (the intro, the chapter cards, the bosses' entrances). [docs/re/speech.md](../re/speech.md) found where each is whole: a page right after `CChatBox::FirstPage` `0x34CD1` or `CChatBox::NextPage` `0x34A05` returns, a speech box in `CStateGame +0x1E9` / `+0x4C8` / `+0x4D0` once the scene runner or OK has set it. The speech must also say who speaks, the name the box draws, which no field holds as text: the box's paint looks it up each frame.

Whatever reads must leave the game as it was: the same `rand()` draws, the same heap, the same state, with speech on or off.

## Options considered

1. **Wrappers that call the original and then read** (chosen for the pages). `FirstPage` and `NextPage` become hookable (`recomp/hooks.py`); the wrapper sees the page the moment it is set, once, with nothing to diff.
2. **Observed entries** (ADR 0053). They see a function's arguments at its entry, before the page exists; the page would have to be read at the next turn, which is the polling option below.
3. **Polling the box every turn from the platform** (as `scene_skip.h` reads the scene box). No new hook, but a page is "new" only by comparing pointers and text with the last turn's, which can miss a page a fast double OK replaces within one turn and can't tell a repeated page from the same one.
4. **Calling the game's own name lookups** (`CDataMgr::FindRow` `0x3990D`, `CDataMgr::ReadString` `0x37999`, `LocalizedCString` `0x6DD1`). They allocate on the guest heap (`new[]`, autoreleased strings), so speech on would change the heap the game sees.
5. **Every string the game draws** (`TextCache_DrawText`): rejected by #378 (no idea what a string is, every line every frame).

## Decision

**Runtime: text events** (`maplethief/text_events.h`, `runtime/src/events/text_events.cpp`), with listeners like the game events' (`mt_text_events_add_listener`), and no SDL. An event carries a kind, a speaker (kind, id, the UTF-8 name the game draws) and UTF-8 text:

- **page**: a wrapper around `FirstPage` and `NextPage` (`MT_HOOK_FN_CHAT_BOX_FIRST_PAGE` / `_NEXT_PAGE`) calls the original and, when it returned true, reads the wrapped lines (`+0x18`, `+0x1C`) and joins them into one text. `Text_Wrap` `0x22EC5` keeps the space it breaks at and drops a newline or `$` it breaks at, so a space goes between two lines only where neither has one; runs of white space (the scene lines end in `\r`) become one space.
- **answer**: after the last page's event, when the box's mode (`+0x08`) asks for one: 1 and 3 Accept / Refuse (the buttons are images; the port says the words), 2 the rows (`+0x20`), each row's Scripts key localized.
- **scene box**: a wrapper around `CStateGame::Paint` `0x87501` reads the box after the frame, when it opens or its first line moves (OK shows the next two), the two lines Paint hands `CChatBox::drawChatBox` `0x35189`. Paint can delete its own state (a scene's exit to the main menu), so the wrapper reads only a state that is still the engine's, as the screen shake's does.
- **The speaker** is the one the game draws, found as its paint finds it ([speech.md](../re/speech.md#speakers)): the hero's caption from `ScriptExtra.strings`, an NPC's name from the NPC table by its id, a boss's from the mob list. Every read is checked against the image's segments and the committed heap; a bad pointer means no event (counted). The `.strings` are read on the host side (`mt_ns::localized_utf8`, a text pack's first); a table name that is not UTF-8 (Korean CP949) is converted; every string an event carries is valid UTF-8.

No guest function is called and no guest byte is written: `runtime_test` compares a synthetic guest run with the wrappers on against one with them off, byte for byte and register for register, and `play.speech_dialogue` compares the game's `rand()` state, the hero, the quests and the mobs with a run with speech off.

**Platform: `SpeechText`** (`platform/src/speech_text.{h,cpp}`) listens, keeps the events (they arrive inside the guest's call) and says them at the next turn as `SpeechKind::kDialogue`: "speaker: text", the answer in the same utterance as its page (an utterance of its own would interrupt the page). It turns the wrappers on only while speech is on, so speech off runs the game exactly as without #391.

**`skip_intros = on` says no scene box.** The skip taps every box away (ADR 0096); saying each would be a burst of cut-off voices for a scene the player chose not to watch. Dialogue pages are still said: the skip never answers a dialogue.

## Consequences

- Two more hookable functions; the generated code changes in two places (each a hook wrapper around the unchanged body). `tests/test_recomp_hooks.py` checks the lists agree.
- `CStateGame::Paint` carries one more wrapper (of four), on only with speech on.
- The speaker names follow a text pack (`Scripts`, `ScriptExtra`, the tables through the pack's patched `.dat`).
- The answer words and the "speaker: text" form are port text (`port_text.def`), so a pack translates them.
- #392 (notify lines and popups) and #393 (menus) can add kinds to the same events.
