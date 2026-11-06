# Options

The game's options are sound, game speed and vibration. They live in one small options file shared by all three character slots, which the game creates with its defaults on the first start and rewrites when an option changes.

## The options

| Option | Choices | Default |
|---|---|---|
| Sound | effects and music each on or off | both on |
| Game speed | three speeds: a game tick of 65, 50 or 40 milliseconds | 50 milliseconds |
| Vibration | on or off | on |

- **Sound.** Effects and music switch independently, so there are four combinations. Turning music back on resumes the current track.
- **Game speed** sets the length of the game's tick, and with it everything that counts in ticks: walking, attacks, buffs and timers. At the default speed the game runs 20 ticks a second; the slower setting gives about 15.4 and the faster 25. The tick counts on the [mechanics pages](mechanics/index.html) are the same at every speed; only the seconds change.
- **Vibration** makes the phone vibrate on a critical hit, when the hero is hit and at some moments of the story's scenes. Switching it on gives a short test vibration.

The options are reached from the popup menu's last tab, which also holds Save and the exit to the main menu; the game's help pages open from there too.

**Example:** a buff of 600 ticks lasts 30 seconds at the default speed, 39 seconds at the slower and 24 seconds at the faster.

## Unknown settings

The options file has three more bytes. One is never read by the game's loader, one is cleared when the death popup opens, and one picks a value from a short table that two game functions read. What they mean is unknown.

## In the port

The port keeps the game's own options and adds its own settings beside them; it also guards against a damaged options file, whose out-of-range game speed the original would read unchecked ([the project's notes on the options file](https://github.com/armaatus/maplethief-port/blob/main/docs/formats/save.md)).

## Sources

The options file is `config`, loaded and created by `CDataMgr::LoadConfig` `0x38611` and written by the options menu (`0x50131`, `0x46A29`). Sound is applied by `0x37C79`, the game speed by `CDataMgr::ApplyGameSpeed` `0x37C41` from the table at `0x108C58`, vibration by `0x37BD1` through `CMSoundPlayer::SetVibrate` `0x1CC61`, with the test vibration from `VibratePreview` `0x1D309`. The vibrations themselves are `CMSoundPlayer::Vibrate` `0x1D2E9`, called from `CHero::TakeDamage` `0x65B55`, `CMob::TakeHit` `0x73715` and the scenes. The help pages are opened by `Shell_EnalbeHelpScrollKey` `0x6655` (sic).
