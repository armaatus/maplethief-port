# Your game file

The port is only the "engine". The game itself (its program, art, music and text) comes from your own copy of MapleStory Thief Edition. That copy is an **IPA** file, the package format of iPhone apps.

## Which file

| | |
|---|---|
| Game | MapleStory Thief Edition for iPhone and iPod touch, by Nexon Mobile (2010) |
| Version | **1.5** (the last one) |
| Name to give it | `MapleThief-1.5.ipa` |
| Size | 17,872,861 bytes (about 17 MB) |
| SHA-256 | `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387` |

It has to be a **decrypted** copy. Apps bought on the App Store are encrypted for the device they were installed on, and the port can't read such a file.

**We can't provide the file, link to one, or explain how to decrypt one.** The game belongs to Nexon. See [What's included, and what isn't](../legal.md).

## Check your file

The game checks the file itself and tells you when something's wrong. To check it before you start, compute its SHA-256 "fingerprint" and compare it with the one above:

- **Windows**: open PowerShell in the folder with the file and run `Get-FileHash MapleThief-1.5.ipa`.
- **Mac**: open Terminal in that folder and run `shasum -a 256 MapleThief-1.5.ipa`.
- **Linux**: `sha256sum MapleThief-1.5.ipa`.

The long string it prints must match exactly.

## If it doesn't match

- **Different size and fingerprint**: probably another version of the game, or an encrypted copy. The port only supports 1.5.
- **Right game, different fingerprint**: the file may have been repacked. An IPA is a zip file, so repacking changes its fingerprint, and usually its size, even when the game inside is the same.
  - If you **build it yourself**, a repacked copy is fine: the build accepts any copy whose game program matches the known one, and says so if it doesn't. A re-signed copy doesn't pass, because signing changes the program.
  - The **download** imports only the exact file above. [Open an issue](https://github.com/armaatus/maplethief-port/issues) with the fingerprint you got, and we'll see whether we can support it.

## Where to put it

- **Download and play**: the game asks for it on the first start. Choose the file wherever it is, or drag it onto the window. The game copies the game's files into the `game/` folder of its data folder ([Saves](saves.md) lists where that is), so you can move or delete the IPA afterwards. `--import-ipa FILE` imports another copy.
- **Build it yourself**: copy it into the project's `original/` folder, named exactly `MapleThief-1.5.ipa` ([original/README.md](../../original/README.md)), or name it when you build: `scripts/build-from-ipa PATH` ([Build from your own copy](../build/README.md#build-from-your-own-copy-wherever-it-is)).
