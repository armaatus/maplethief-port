# Known issues

What is open today, each with where it's tracked. An entry holds for as long as its issue is open; if the issue linked next to it is closed, the entry is out of date. Something missing? [Open an issue](https://github.com/armaatus/maplethief-port/issues/new).

## While playing

| What | Workaround | Source |
|---|---|---|
| Text uses an open font instead of Apple's fonts, so it looks plainer and widths differ a little. It hasn't been compared with a real device, so a line may wrap or clip differently | None | [#118](https://github.com/armaatus/maplethief-port/issues/118), [ADR 0036](adr/0036-coregraphics-and-uikit-drawing.md) |
| The cash shop can't take real money: every payment is cancelled, as Apple's store is not there | `cash_shop = meso` lets it pay in the game's meso ([settings](play/settings.md)) | [ADR 0034](adr/0034-offline-services.md), [ADR 0072](adr/0072-offline-cash-shop-and-revives.md) |
| More Games and the banner do nothing: they opened Nexon's pages, which are gone | `hide_online_ui = on` hides them | [Troubleshooting](play/troubleshooting.md#while-playing) |
| On Windows and Linux, the window size keys Ctrl+1 … Ctrl+5 also attack once in the field, because Ctrl is an attack key | Press them in a menu, or give the sizes other keys in [`[window]`](controls/remapping.md#the-windows-size-keys) | [Keyboard](controls/keyboard.md#the-ports-own-keys) |

## Getting it running

| What | Workaround | Source |
|---|---|---|
| The download imports only the exact file. A repacked copy of the game is refused, and a re-signed copy is refused by the build too | Build from source: the build accepts a repacked copy | [ADR 0135](adr/0135-accept-an-ipa-by-its-main-binary.md), [Your game file](play/your-ipa.md#if-it-doesnt-match) |
| macOS blocks the download the first time, because it isn't notarised (by choice: no legal name on the binaries) | macOS 15 and later: open it once, then System Settings › Privacy & Security › **Open Anyway**. macOS 13 and 14: Control-click › **Open**. All steps in [Troubleshooting](play/troubleshooting.md#mac-apple-could-not-verify-maplethief-is-free-of-malware) | [ADR 0148](adr/0148-downloads-stay-unsigned.md) |
| Windows SmartScreen warns about the download, again with every release, because it isn't signed | **More info**, then **Run anyway** ([Troubleshooting](play/troubleshooting.md#windows-windows-protected-your-pc)) | [ADR 0148](adr/0148-downloads-stay-unsigned.md) |
| A freshly downloaded Mac app, or one that can't write next to itself, keeps its data in your user folder | [Troubleshooting](play/troubleshooting.md#the-game-keeps-its-data-in-the-user-folder) says how to fix each cause | [ADR 0127](adr/0127-data-folder-next-to-the-game.md) |
| Linux needs glibc 2.39 or later (Ubuntu 24.04, Fedora 40, current SteamOS). Only x86_64 is built and checked, and there's no AppImage or Flatpak | Update your distribution | [ADR 0065](adr/0065-linux-and-steam-deck-build.md), [Linux and Steam Deck](play/linux-and-steam-deck.md#not-done-yet) |

## Not checked yet

Nobody has confirmed these on the real thing yet. They may well be fine.

| What | Source |
|---|---|
| The whole game: all chapters, mini-games and endings, on each system | [#79](https://github.com/armaatus/maplethief-port/issues/79), [#92](https://github.com/armaatus/maplethief-port/issues/92), and the automated Chapter 1 runs [#160](https://github.com/armaatus/maplethief-port/issues/160), [#161](https://github.com/armaatus/maplethief-port/issues/161), [#162](https://github.com/armaatus/maplethief-port/issues/162) |
| A clean Windows 11 machine with only the zip | [#82](https://github.com/armaatus/maplethief-port/issues/82) |
| A real Steam Deck: Game Mode's window, the controller layout, the touch screen | [#84](https://github.com/armaatus/maplethief-port/issues/84) |
| Steady speed on Windows, the Steam Deck and Intel Macs; it's only been measured on an Apple M4 | [#142](https://github.com/armaatus/maplethief-port/issues/142) |
| Every `game_speed` length on a real 144 Hz screen; it's only been measured on a 60 Hz screen and with no display lock | [#272](https://github.com/armaatus/maplethief-port/issues/272) |
| The GPU renderer (`gfx_backend = gpu`) on Windows and Linux; it's only been run on macOS. The default is `software` | [#131](https://github.com/armaatus/maplethief-port/issues/131) |
| The GPU renderer (`gfx_backend = gpu`) and the filters on a Nintendo Switch; they're built but haven't run on a console. The default is `software` | [#242](https://github.com/armaatus/maplethief-port/issues/242) |
| The first-start fullscreen fix on macOS 27 | [#179](https://github.com/armaatus/maplethief-port/issues/179) |
| Playing with only a real gamepad or only a keyboard | [#132](https://github.com/armaatus/maplethief-port/issues/132) |
| A forced crash showing the crash dialog on macOS and Windows | [#198](https://github.com/armaatus/maplethief-port/issues/198) |
| The data folder on all three systems | [#264](https://github.com/armaatus/maplethief-port/issues/264) |
| Real touchpads (DualSense, Steam Deck) as the gamepad pointer | [#185](https://github.com/armaatus/maplethief-port/issues/185) |
| Window size keys and fullscreen on a second display | [#196](https://github.com/armaatus/maplethief-port/issues/196) |
| The Saves tab's file dialogs and Open data folder on real desktops | [#192](https://github.com/armaatus/maplethief-port/issues/192) |
| All 44 sounds and the music by ear, over a long session | [#124](https://github.com/armaatus/maplethief-port/issues/124) |
| The picture against screenshots from a real iPhone or iPod | [#60](https://github.com/armaatus/maplethief-port/issues/60) |

## Not available yet

| What | Source |
|---|---|
| The iOS app beyond the first map: it installs and reaches the first map by touch on an iPhone 12, the only device checked so far. Controllers, the safe area, the app lifecycle and the audio session on iOS are not checked on a device yet. Steps: [Playing on your iPhone](play/iphone.md) | [#73](https://github.com/armaatus/maplethief-port/issues/73), [#128](https://github.com/armaatus/maplethief-port/issues/128), [#197](https://github.com/armaatus/maplethief-port/issues/197) |
| A Nintendo Switch homebrew build | Epic [#233](https://github.com/armaatus/maplethief-port/issues/233) |

## Building from source

| What | Workaround | Source |
|---|---|---|
| The [build guide](BUILDING.md) hasn't been followed on a clean machine yet, and the one-command build hasn't been run on a real Windows PC | Tell us where it goes wrong | [#93](https://github.com/armaatus/maplethief-port/issues/93), [ADR 0133](adr/0133-one-command-builds.md) |
| `just` splits its arguments on spaces, so a path with a space needs quotes inside the quotes | Use `scripts/build` (Windows `scripts\build.ps1`) directly | [ADR 0135](adr/0135-accept-an-ipa-by-its-main-binary.md) |
| A Linux game built with uncommitted changes doesn't include them, because the container exports `HEAD` | Commit first (the build warns), or use `scripts/linux-game build --worktree` | [ADR 0133](adr/0133-one-command-builds.md) |
