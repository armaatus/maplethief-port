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
- **Same size, different fingerprint**: the file may have been repacked or re-signed. An IPA is a zip file, so repacking changes its fingerprint even when the game inside is the same. [Open an issue](https://github.com/armaatus/maplethief-port/issues) with the fingerprint you got, and we'll see whether we can support it.

## Where to put it

- **Download (option A)**: the game asks for it on the first start. Pick the file wherever it is.
- **Build it yourself (option B)**: copy it into the project's `original/` folder, named exactly `MapleThief-1.5.ipa` ([original/README.md](../../original/README.md)).
