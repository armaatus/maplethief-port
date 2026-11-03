# `original/`: put your own copy of the game here

This folder is where the build looks for the game. The repository **never** contains it ([ADR 0100](../docs/adr/0100-public-repository-and-player-releases.md)): `*.ipa` is git-ignored, and CI fails if one ever shows up.

Put your copy here, named exactly:

```
original/MapleThief-1.5.ipa
```

It must be **MapleStory Thief Edition 1.5** for iOS (bundle id `com.nexonmobile.maplestory.thief`), **decrypted**. Other versions and encrypted App Store copies won't work. We can't provide the file, and we don't explain how to decrypt one ([docs/legal.md](../docs/legal.md)).

## Check that it's the right file

`scripts/bootstrap` checks it for you and stops with a clear message if anything's wrong. To check by hand:

| File | Size | SHA-256 |
|---|---|---|
| `MapleThief-1.5.ipa` | 17,872,861 bytes | `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387` |
| `Payload/MapleThief.app/MapleThief` (inside it) | 2,745,584 bytes | `1267cf3e048113c26c8bc354087194da8f9e8ed830fcaa766da170e8e6e32066` |

```sh
shasum -a 256 original/MapleThief-1.5.ipa          # macOS
sha256sum original/MapleThief-1.5.ipa              # Linux
Get-FileHash original\MapleThief-1.5.ipa           # Windows PowerShell
```

An IPA is a zip file, so a copy that was repacked or re-signed has a different hash even when the game inside is the same. If only the IPA's hash differs, open an issue with both hashes and we'll look at it.

[`SHA256SUMS`](SHA256SUMS) lists the same hashes, plus those of the two thin slices `scripts/bootstrap` cuts from the program. After a bootstrap, `shasum -a 256 -c original/SHA256SUMS` checks them all.

## What happens to it

Nothing in this folder is ever changed. `scripts/bootstrap` extracts the IPA into the git-ignored `work/`, and everything else (converted assets, recompiled code, the built game) is generated from there into `work/`, `assets-out/` and `build/`, which are all git-ignored too. The game's files are treated as untrusted data: they are parsed, never executed.
