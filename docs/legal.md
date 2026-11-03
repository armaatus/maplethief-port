# What's included, and what isn't

maplethief-port is an unofficial fan project. It is **not affiliated with, endorsed by or connected to Nexon** in any way. MapleStory and MapleStory Thief Edition are trademarks of Nexon. The game's program, art, music, sounds, maps and text belong to Nexon.

## This repository contains

- our own code: the recompiler, the runtime that stands in for iOS, the desktop and phone frontends, and the tools;
- our own documentation, including notes on how the original game works (addresses, names, file formats);
- tests with made-up data.

All of it is under the [MIT licence](../LICENSE). Third-party libraries keep their own licences ([THIRD_PARTY.md](../THIRD_PARTY.md)).

## This repository does not contain

- the game (`MapleThief-1.5.ipa`) or any file from it;
- converted art, sound or data;
- the C code the recompiler generates from the game, or anything built from it.

You need **your own copy** of the game to play. We can't give you one, and we don't explain how to decrypt one. See [Your game file](play/your-ipa.md).

## Downloads

Release downloads contain the program, never the game's art, sound or data. On first launch the game asks for your own copy and reads everything from it. (Until that lands, [#182](https://github.com/armaatus/maplethief-port/issues/182), releases contain only the tools, and you [build the game yourself](build/README.md).)

**Don't share a build that contains the game's files.** The ones you make yourself with `just build`, `scripts/linux-game` or the macOS app preset include the game's bundle, and so do texture dumps and packs made from it.

## Rights holders

If you represent Nexon and have a concern about this project, please [open an issue](https://github.com/armaatus/maplethief-port/issues) or contact the maintainer through GitHub. We will respond quickly.

The reasoning behind these rules is in [ADR 0100](adr/0100-public-repository-and-player-releases.md).
