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

Release downloads contain the program, never the game's art, sound or data. The program includes the game's code, recompiled for your system ([ADR 0100](adr/0100-public-repository-and-player-releases.md) records that decision and its risk). On first launch the game asks for your own copy, checks it and copies the game's files from it into its data folder on your computer. A check over every download (the release guard, [ADR 0125](adr/0125-player-builds-icon-guard-modes-and-release-flow.md)) makes sure that none of the game's files are in it. The icon is our own drawing, not Nexon's art.

**Don't share a build that contains the game's files.** The ones you make yourself with `just build`, `scripts/linux-game build` or the macOS app preset include the game's bundle, and so do texture dumps and packs made from it, and the `game/` folder that the import writes into the data folder.

## itch.io

The same player builds are also on itch.io, at [armaatus.itch.io/maplethief](https://armaatus.itch.io/maplethief): the files of the published GitHub Release, checked against its `SHA256SUMS` and the release guard again before they are uploaded, never a build made only for itch.io and never a release candidate ([ADR 0146](adr/0146-distribution-on-itch-io.md)). The page's text is our own; its cover and banner are our own art, and its screenshots of the game are taken from a local build and never stored in this repository.

The download is free. The page offers an optional donation ("$0 or donate") and a Buy Me a Coffee link. A donation is a tip for the work on the port. It buys nothing: not the game, which belongs to Nexon, and not any right to it. If Nexon objects, donations are switched off first, then the screenshots and the downloads are withdrawn, on itch.io and on GitHub; the source code stays.

## The compendium site

The [MapleThief Compendium](https://armaatus.github.io/maplethief-port/) is an unofficial fan reference for the game, built with this repository's tools. It is the one place this project publishes the game's content, and only what a compendium needs:

- facts from the game's data, maps and scripts;
- item, skill and UI icons, monster, NPC and hero sprites, and map renders;
- the game's dialogue in English and Korean, and the item and skill descriptions, quoted as they are.

It never publishes the game's music or sounds (they are named, not published), the program or code derived from it, game files to download, or anything that helps get or decrypt the game.

The site is built on a computer with its owner's own copy of the game. It is never stored in this repository: the tools and our own text are here, the built pages are not. Every page says it is an unofficial fan project, not affiliated with or endorsed by Nexon, and that the game content belongs to Nexon.

If Nexon asks, the site comes down within a day. The source stays, because it contains no game content.

## Rights holders

If you represent Nexon and have a concern about this project, please [open an issue](https://github.com/armaatus/maplethief-port/issues) or contact the maintainer through GitHub. We will respond quickly.

The reasoning behind these rules is in [ADR 0100](adr/0100-public-repository-and-player-releases.md), for itch.io in [ADR 0146](adr/0146-distribution-on-itch-io.md), and for the compendium site in [ADR 0121](adr/0121-public-compendium-site.md).
