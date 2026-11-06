## Downloads

**To play**, download the file for your system:

| System | File |
|---|---|
| Windows 10/11 (64-bit) | `MapleThief-<version>-windows-x64.zip` |
| macOS 13+ (Apple silicon and Intel) | `MapleThief-<version>-macos-universal.zip` |
| Linux x86_64, Steam Deck (glibc 2.39+) | `MapleThief-<version>-linux-x86_64.tar.gz` |

These contain the program only: none of the game's art, music, maps or text. **You need your own copy of the game**, `MapleThief-1.5.ipa` (version 1.5, decrypted). On first launch the game asks for it, checks it and copies the game's files into its data folder. We can't provide the file. [Playing MapleThief](https://github.com/armaatus/maplethief-port/blob/main/docs/play/README.md) has the steps for each system and explains how to check your copy.

**To build or change the port**, the `MapleThief-kit-<version>-<os>-<arch>` files hold our own code only: the platform shell without the game, the runtime and SDL3 libraries, the recompiler, the tools and the docs ([Build from source](https://github.com/armaatus/maplethief-port/blob/main/docs/build/README.md)).

`SHA256SUMS` lists every file's SHA-256: `sha256sum -c SHA256SUMS` (Linux) or `shasum -a 256 -c SHA256SUMS` (Mac).

MapleThief is an unofficial fan project, not affiliated with or endorsed by Nexon. MapleStory is a trademark of Nexon. [What's included, and what isn't](https://github.com/armaatus/maplethief-port/blob/main/docs/legal.md).
