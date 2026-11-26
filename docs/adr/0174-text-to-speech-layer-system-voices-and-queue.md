# 174. Text-to-speech: a speech layer over the system voices, with the queue rules in the port

- **Status:** Accepted (delegated by Joris, 2026-11-26)
- **Date:** 2026-11-26
- **Issue:** [#390](https://github.com/armaatus/maplethief-port/issues/390) (epic [#398](https://github.com/armaatus/maplethief-port/issues/398))
- **Builds on:** [ADR 0093](0093-accessibility-colour-filters-screen-shake-and-text-height.md) (the Accessibility settings, not in the presets), [ADR 0142](0142-platform-capabilities-and-the-switch-frontend.md) (the capability table), [ADR 0045](0045-audio-mixer-and-output.md) and its addendum (the iOS audio session), [ADR 0076](0076-settings-menu.md) (live settings), [ADR 0108](0108-text-packs.md) (text packs)

## Context

#378's research ([docs/re/speech.md](../re/speech.md)) found that the game's text can be read by host code where it appears (dialogue pages, the scenes' speech box, the notify list, popups, the port's menus) and that every target but the Switch has a system voice a native program can call without a screen reader running: `AVSpeechSynthesizer` on macOS and iOS, SAPI 5's `ISpVoice` on Windows, Speech Dispatcher's `libspeechd` on Linux and the Steam Deck. SDL3 has no speech API. libnx exposes none either.

Three later children feed the voice: dialogue and the scenes' speech boxes (#391), notify lines and popups (#392), the menus (#393). They differ in how urgent they are: a new dialogue page makes the last one stale, a pick-up notice is news the player still wants after the page, and a run of twenty pick-ups must not keep the voice busy for a minute.

## Options considered

1. **The system voice per platform, called directly** (chosen). Free, installed, in the player's language and with the voice they picked in the system's settings; no size cost. Each platform needs its own small backend, and Linux needs `libspeechd`, which a desktop distribution usually has (Orca speaks through it) but SteamOS's Game Mode may not.
2. **A bundled synthesizer** (flite, espeak-ng). The same voice everywhere, the Switch included. It costs megabytes per language, sounds worse than the system voices, and espeak-ng's GPL-3 would bind the binary. Kept for the Switch as a later option if a player asks.
3. **The screen reader** (VoiceOver, Narrator, NVDA, Orca through announcements). Right for a player who runs one: their voice, their rate, their interruption keys. But it reaches no one without a screen reader, and each platform's announcement API has its own queueing that the port can't steer. A later setting (`speech = screen_reader`) can add it beside the system voice; not now.
4. **Queue rules in each backend** (each API's own queue: SAPI's, speechd's priorities). The platforms would behave differently, and the rules could only be tested on each OS with a voice.

## Decision

**The speech layer** is `platform/src/speech.{h,cpp}`: `Speech`, a front over one `SpeechBackend`. Its API, which #391, #392 and #393 use:

- `Speech::say(SpeechKind kind, std::string utf8, double now)`, with `SpeechKind::kDialogue`, `kNotice` or `kMenu`. The caller formats the text ("who: what"); the speech layer adds nothing.
- `Speech::turn(double now)` once per run-loop turn (game.cpp's turn hook does it), `stop()`, `set_enabled(bool)`, `set_voice(SpeechVoice)` (language, rate, volume), `available()`, `stats()`.
- `now` is `speech_clock()` in the game: the host's clock, because a voice speaks in real time whatever the game's speed or a pause.
- `Game::speech` holds the game's one `Speech`.

**The queue rules live in `Speech`**, so every backend behaves alike and `platform_test` checks them on the log backend:

- A **dialogue** or **menu** utterance interrupts: what is being spoken stops, and it is spoken at once. Notices already waiting stay queued behind it.
- A **notice** queues: spoken at once if the voice is idle, else after the current one. At most `Speech::kQueueLimit` (4) wait; past it the oldest waiting notice is dropped.
- With `speech = off` nothing reaches a backend; turning it off stops the voice and empties the queue.
- Each backend says whether it is still speaking, or that it can't tell (the log backend, Speech Dispatcher), and then `Speech` estimates the end from the text: Hangul and CJK at about 6.5 characters a second, anything else at 14, plus a quarter second, divided by the rate.

**Backends.** Each starts an utterance (cutting off the last), stops, and reports whether it speaks:

| Platform | Backend | Rate (50..200 %) | Volume (0..100 %) | Language |
|---|---|---|---|---|
| macOS, iOS | `AVSpeechSynthesizer` (`speech_apple.mm`, ARC) | `AVSpeechUtteranceDefaultSpeechRate` × rate, within the API's range | the utterance's volume | `voiceWithLanguage:`, else the default voice |
| Windows | SAPI 5 `ISpVoice` (`speech_sapi.cpp`, `sapi.lib`): `SPF_ASYNC \| SPF_PURGEBEFORESPEAK \| SPF_IS_NOT_XML` | `SetRate`, logarithmic: 10·log₃(rate) | `SetVolume` | the first voice token whose `Language` is the tag's LANGID, else the default |
| Linux, Steam Deck | Speech Dispatcher, `libspeechd.so.2` loaded with `SDL_LoadObject` (no headers, no link) | `spd_set_voice_rate` −100..100 | `spd_set_volume` −100..100 | `spd_set_language` |
| Switch, Android | none | | | |
| any (`MAPLETHIEF_SPEECH=log`) | the log backend: a `speech: <kind> [<lang> <rate>% <volume>%] <text>` line per utterance, nothing heard | | | |

`MAPLETHIEF_SPEECH = system | log | none` picks the backend (default `system`). AVSpeechSynthesizer starts on its own thread, so just after `speakUtterance:` it may still say it is not speaking: until it has been seen speaking, an utterance counts as speaking for up to a second. Speech Dispatcher's connection opens at the first utterance, since `spd_open` may start its daemon. On iOS the synthesizer uses the app's audio session (`usesApplicationAudioSession = YES`), so the voice mixes with the game's sound in the category SDL set (ADR 0045's addendum) rather than interrupting it.

**The voice's language** follows the game's: `ko` gives `ko-KR`, anything else `en-US`. An open text pack's manifest `language` (ADR 0108) wins when it is a tag (letters, digits, `-` or `_`, a letter first, at most 35 characters; `_` becomes `-`), since the text the voice reads is then the pack's.

**Settings** (Accessibility, off, in no preset, live, ADR 0093's rule for the player's own needs): `speech = off | on` (`--speech`), `speech_rate = 50..200` in steps of 10 (`--speech-rate`), `speech_volume = 0..100` in steps of 10 (`--speech-volume`). A change in the settings menu speaks a sample sentence in the new voice, so the player hears what they chose.

**The capability.** `kCapSpeech` in `platform_caps.h`: the desktop and iOS have it, the Switch does not, so its menu has no speech rows. Where the platform has the capability but no voice loads at run time (Linux without `libspeechd`; Android, which is a desktop-capability build without a backend for now) the rows stay and say *not available*, with a help line on installing speech-dispatcher, through the settings menu's new `set_setting_unavailable(key, why)`; the game starts and runs as before.

## Consequences

- #391, #392 and #393 only produce text and a kind; none of them touches a platform API or a queue.
- A platform's voice is tested only where it runs: macOS by `platform_test`'s "the system voice speaks a test string" with `MAPLETHIEF_SPEECH_SYSTEM_TEST=1` (a local run), Windows and Linux by CI building their backends, plus a local run on a machine with a voice. CI tests the queue on the log backend.
- The estimate for Speech Dispatcher can be off for a slow voice: a queued notice may cut off the end of the one before it. If players notice, speechd's end-of-speech callback (`SPDConnection`'s `callback_end`, which needs the struct's layout) is the fix.
- The voices play through their own audio outputs, not the game's mixer: they are not in an `--audio-capture` and `master_volume` does not change them; `speech_volume` does. Ducking the game's music while the voice speaks is a later option.
- Android has no voice yet: its `TextToSpeech` needs JNI through SDL's activity. A follow-up when Android players ask.
