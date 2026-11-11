# MapleThief for the Nintendo Switch

MapleThief runs MapleStory Thief Edition, the 2010 iPhone game, natively on a Nintendo Switch as a homebrew app. It is an unofficial fan project, not affiliated with or endorsed by Nexon or Nintendo. MapleStory is a trademark of Nexon.

This download contains the program only: none of the game's art, music, maps or text. You need **your own copy** of the game, the file `MapleThief-1.5.ipa` (version 1.5, decrypted, 17,872,861 bytes, SHA-256 `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`). We can't provide it.

You also need a Switch that already runs homebrew apps from the Homebrew Menu. This project doesn't explain how to set that up.

## Copy

1. Copy the `switch` folder of this download to the root of your SD card. It holds `switch/maplethief/MapleThief.nro`.
2. Copy your `MapleThief-1.5.ipa` into the same folder, next to it: `sdmc:/switch/maplethief/MapleThief-1.5.ipa`.

## Start

Start **MapleThief** from the Homebrew Menu. Open the Homebrew Menu through a game (hold R while starting one) rather than from the Album: MapleThief then has all the memory it needs.

The first start imports your game file: MapleThief finds `MapleThief-1.5.ipa` (or any other `.ipa` in that folder), checks it, and copies the game's files into `sdmc:/switch/maplethief/game/`. This takes a minute. If the file is wrong, it says why. Afterwards it offers to delete the `.ipa` to free space; the game no longer needs it. Every later start goes straight to the game.

Your saves and settings are in `sdmc:/switch/maplethief/data/`. Copy that folder to back them up.

## More

- Playing on the Switch (controls, saves, troubleshooting): https://github.com/armaatus/maplethief-port/blob/main/docs/play/switch.md
- Licences: `LICENSE` (MIT, the port's own code), `THIRD_PARTY.md` and `licenses/` (the libraries and fonts it includes).
