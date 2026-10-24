# `original/`: the source of truth

This folder holds exactly one input: my personally owned copy of **MapleStory Thief Edition 1.5** for iOS.

| File | What it is |
|---|---|
| `MapleThief-1.5.ipa` | Decrypted IPA, stored via Git LFS. Clutch 2.0.4 dump from my own jailbroken iPod touch. Bundle id `com.nexonmobile.maplestory.thief`, `CFBundleVersion` 1.5, minimum iOS 3.1.2. |
| `SHA256SUMS` | Expected hashes of the IPA, of the main executable inside it (`Payload/MapleThief.app/MapleThief`) and of its two thin slices (`work/slices/MapleThief.armv6`, `work/slices/MapleThief.armv7`, byte-identical to `lipo -thin` output). Paths are relative to the repo root, so `shasum -a 256 -c original/SHA256SUMS` works after `scripts/bootstrap`. |

Expected values:

- IPA: 17,872,861 bytes, SHA-256 `a6a06f050aa21af9576dbf8af3c832dcbbf38f453886ef1690447f934cb3e387`
- Main binary: universal armv6 + armv7, 2,745,584 bytes, SHA-256 `1267cf3e048113c26c8bc354087194da8f9e8ed830fcaa766da170e8e6e32066`
- armv6 slice: 1,543,296 bytes, SHA-256 `3a24f0c0592dcd8d6cdc59c4330a8b3c54f1a4d328b6479911d9b73063ed5118`
- armv7 slice: 1,197,296 bytes, SHA-256 `60d4d5e75de5eb478ed320377babca8eab6616e9a94faad805f71cf8e084e467`

## Single source of truth

Everything else is **produced from this IPA by scripts** and is **never committed**. That covers extracted files, split Mach-O slices, Ghidra projects, function lists, converted assets, generated (recompiled) C code and built binaries. Those outputs live in `work/`, `assets-out/` and `build/`, which are all git-ignored.

To recreate the working tree from scratch:

```sh
git lfs pull                     # make sure the IPA is the real file, not an LFS pointer
scripts/bootstrap                # verifies hashes, extracts to work/ipa/, splits work/slices/
                                 # (Windows: scripts\bootstrap.ps1)
```

If a script ever needs something that isn't derivable from this IPA, that's a design problem. Raise it in an issue rather than committing the derived file.

## Legal / ownership

The game code, art, audio, text and data are the property of **Nexon**. This repository exists for personal preservation and interoperability with hardware I own. Therefore:

- The repository stays **private**.
- Builds produced from it (any platform) are for personal use and must **never be shared publicly**, uploaded, or attached to releases.
- Nothing derived from the IPA is committed, apart from small reference crops in private docs where they help explain a format.
