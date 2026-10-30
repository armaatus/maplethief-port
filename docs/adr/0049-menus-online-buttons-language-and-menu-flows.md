# 49. The menus before the game: online buttons, the Korean locale and the menu flow tests

- **Status:** Accepted
- **Date:** 2026-10-30
- **Issue:** #75
- **Builds on:** [ADR 0034](0034-offline-services.md) (StoreKit and GameKit offline), [ADR 0035](0035-foundation-subset.md) (`NSLocale` from the configured language), [ADR 0037](0037-uikit-host-classes-nib-and-compositor.md) (`openURL:` through the host's `open_url` hook), [ADR 0044](0044-virtual-clock-and-device-golden-screens.md) (`--fixed-step`, `--shot`), [ADR 0047](0047-touch-input-mouse-and-fingers.md) (`--tap` through `Input`)

## Context

Before the game come the shell's own UIKit screens: `MainMenuView` (Play, Help, About, Game Center, More Games, a promo banner), the Korean `TermsView`, and `ChooseCharacterView` (three slots, New, Delete, Start, Back and its hand-built alerts) ([docs/re/objc-shell.md](../re/objc-shell.md)). They run recompiled on the host UIKit classes. #75 asked for each screen to work, for the online features to do nothing harmful, and for a character to be created, selected and deleted, each with a scripted test.

The first runs on the epic tip showed that the screens already worked. Three questions were still open:

- **The online buttons.** The banner, More Games and About's four links call `-[UIApplication openURL:]` with Nexon and App Store URLs ([network.md → URLs](../re/network.md#urls)). network.md left open whether the port opens them in the host's browser or refuses them. Game Center's button presents a `GKLeaderboardViewController`, and ADR 0034 dismisses it at once.
- **The terms screen.** `-[MainMenuView drawRect:]` shows `TermsView` only when `[NSLocale currentLocale]`'s country code is `KR` and there is no `CTerms` save file. The runtime answers `KR` when its configured language is `ko` (ADR 0035), but the frontend configured CF itself and never read `MAPLETHIEF_LANGUAGE`. The terms screen could not be reached.
- **Testing a swipe.** The help pager is a paging `UIScrollView` (Korean build: two pages). `--tap` can only click.

## Options considered

For `openURL:`:

1. **Open the URL in the host's browser** (`SDL_OpenURL` behind the `open_url` hook). This is the device's behaviour, but every one of the six addresses is a dead or redirected Nexon page or a delisted App Store app. On a desktop, a stray click on the banner would leave the game for a browser window.
2. **Refuse and log** (no hook): `openURL:` answers `NO`, and the game ignores the answer. **Chosen.**
3. **Hide the buttons.** This changes the screen against the device's, and would need patches to the recompiled `drawRect:`.

For the language: a `settings.ini` key, or the environment variable the runtime already defines. The environment variable is chosen for now, because a language setting belongs to the options work, and the tests need only a switch.

## Decision

- **`openURL:` is refused.** The platform sets no `open_url` hook. The banner, More Games and the About links log `uikit: -[UIApplication openURL:] <url>: not opened (no handler)` and leave the screen as it was. A later setting can add a hook if anyone wants the links.
- **Game Center stays emulated** (ADR 0034). The button presents the leaderboard, which is dismissed on the next pump. The menu looks as it did before the click.
- **The frontend applies `MAPLETHIEF_LANGUAGE`** (`en` or `ko`) through `mt_cf_config_from_env` before it sets the bundle (`platform/src/game.cpp`). It logs the language when it is not English, and a bad value falls back to `en`. `ko` plays the Korean build: the `KR` locale, the terms screen until `CTerms` exists, the two-page help, and the Korean tables and strings.
- **`--swipe T,X1,Y1,X2,Y2`**, a test aid next to `--tap`, presses at (X1, Y1) at T seconds and drags in 8 even steps to (X2, Y2). It releases there 0.3 s after the press. Like a tap, it is a left button and mouse motion handed to the frontend's event handler (ADR 0047), so it takes the mouse's path. Taps and swipes are delivered in time order. The uikit trace logs where a paging scroll view settles when a drag ends (`UIScrollView 0x…: drag ended, pages to page N`).
- **The menu flows are tested by `mtport.menuflows`**, one CTest per flow (`menus.help_about`, `menus.terms`, `menus.online`, `menus.new_game`, `menus.create`, `menus.select`, `menus.delete`; label `real_assets`, skipped without the game). Each flow runs one or more launches of `MapleThief --fixed-step 1` on one data directory. It checks the uikit trace's actions, `openURL:` lines and pages, the save files and their sizes, and the shots by hash. A closed screen must leave the same picture as before it opened. A deleted slot must look as a relaunch with one character does. The loading screen is the one shot that is nearly all dark. The shots stay in the test output directory and are never committed. Every launch must also end cleanly with an empty unimplemented report and no GL misuse. Clicks wait until the menu has slid in (3 s), clear of #122.

## Consequences

- No click in the menus leaves the game or waits on the network. A player who wants Nexon's pages has to open them by hand. That is no loss, because they are dead.
- The Korean build, and its terms screen, can be played and tested with `MAPLETHIEF_LANGUAGE=ko`. #68 and #69 can reach `TermsView`'s agreement button and the two-page help through it.
- Character creation asks for nothing but Yes or No: the game has no name entry, so no text input is needed (docs/ANALYSIS.md).
- The flows run about 5 minutes in a Debug build (the compositor costs about 0.1 s a window frame), and less in Release. They run serially, like the other real-asset game tests.
- The flows check that screens open, close, write and delete what they should. They do not check that a screen looks like the device's. That is #60's job, through the device golden screens.
