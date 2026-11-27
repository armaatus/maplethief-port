# SquashFS and the type-2 AppImage

**Status: specified** for the parts we read. This is not a game format. It is the container of the Linux player AppImage our own build writes ([ADR 0193](../adr/0193-linux-glibc-2-35-appimage-and-flatpak-bundle.md), [#349](https://github.com/armaatus/maplethief-port/issues/349)). `mtport.linux_package appimage` packs it with `mksquashfs` in the Linux build image. We read it back so that the release guard sees every file in it. The layout follows the kernel's `fs/squashfs/squashfs_fs.h` and the community description at <https://dr-emann.github.io/squashfs/>.

- **Parser**: [`tools/mtport/squashfs.py`](../../tools/mtport/squashfs.py): `superblock` and `entries` (every entry with its bytes), `appimage_offset` and `appimage_parts` (the runtime and the entries). CLI: `python3 -m mtport.squashfs list IMAGE|APPIMAGE`.
- **Tests**: [`tests/test_linux_packages.py`](../../tests/test_linux_packages.py), on `tests/fixtures/linux/fixture.squashfs`. That is a 12 KB image of our own synthetic files, made by the build image's `mksquashfs -b 4096 -comp gzip -all-root`, so one file spans blocks, ends in a fragment and has an all-zero (sparse) block. The tests also cover a symlink, modes, damaged and foreign images (another compressor, version 3, cut short) and an AppImage made from a fake runtime. Locally, `mtport.release guard` reads the real player AppImage (`scripts/release build`).
- **Where it is used**: `mtport.release` splits an `*.AppImage` into its runtime and its files.

All integers are little-endian.

## The AppImage

A type-2 AppImage is an ELF executable, the *runtime*, with the SquashFS image appended. The ELF identification carries `41 49 02` ("AI", type 2) at offset 8. The image starts where the ELF section header table ends, `e_shoff + e_shentsize × e_shnum`, as the runtime itself computes it. Our runtime is `AppImage/type2-runtime` release 20251108 (944,632 bytes on x86_64). The guard accepts only its SHA-256s (`APPIMAGE_RUNTIMES`). After the image's `bytes_used`, `mksquashfs` pads to 4 KiB with zeros. The reader refuses anything else there.

## Superblock (96 bytes at the image's start)

| Offset | Type | Field |
|---|---|---|
| 0 | `char[4]` | magic `hsqs` |
| 4 | u32 | inode count |
| 8 | u32 | modification time |
| 12 | u32 | block size (4 KiB to 1 MiB, a power of two) |
| 16 | u32 | fragment entry count |
| 20 | u16 | compression: 1 zlib (read), 2 lzma, 3 lzo, 4 xz, 5 lz4, 6 zstd (refused) |
| 22 | u16 | log2 of the block size |
| 24 | u16 | flags (not needed to read) |
| 26 | u16 | id count |
| 28 | u16, u16 | version 4.0 |
| 32 | u64 | root inode reference |
| 40 | u64 | bytes used |
| 48 | u64 × 6 | id, xattr, inode, directory, fragment and export table starts |

## Metadata blocks

Inodes, directories and the fragment table are stored in *metadata blocks*: a u16 header, then that many bytes (low 15 bits). The block is compressed unless bit 15 is set, and inflates to at most 8 KiB. A stream continues into the next block. An *inode reference* is `block << 16 | offset`: the block's position relative to the inode table's start, and the offset within the inflated block.

## Inodes

Each inode starts with u16 type, u16 permissions, u16 uid index, u16 gid index, u32 mtime and u32 inode number. The types read are:

| Type | Body |
|---|---|
| 1 directory | u32 block, u32 links, u16 size, u16 offset, u32 parent |
| 8 extended directory | u32 links, u32 size, u32 block, u32 parent, u16 index count, u16 offset, u32 xattr |
| 2 file | u32 blocks start, u32 fragment, u32 fragment offset, u32 size, then the block sizes |
| 9 extended file | u64 blocks start, u64 size, u64 sparse, u32 links, u32 fragment, u32 fragment offset, u32 xattr, then the block sizes |
| 3, 10 symlink | u32 links, u32 target size, the target |

Devices, FIFOs and sockets are listed as `other`, which the guard rejects. A file has `size / block size` blocks, rounded down when its tail is in a fragment (fragment ≠ `0xFFFFFFFF`) and up otherwise. Each block size word has bit 24 set when the block is stored uncompressed. Its low 24 bits are the stored length, and 0 means a sparse block of zeros.

## Directories

A directory's listing is `size − 3` bytes in the directory table, from the inode's (block, offset). It is a run of headers: u32 count − 1, u32 inode block, u32 base inode number. Each header is followed by `count` entries of u16 offset, i16 inode number delta, u16 type and u16 name size − 1, then the name. The reader refuses `.`, `..` and names with `/` or NUL, and a directory that loops.

## Fragments

The fragment table is an array of u64 pointers, one per 512 entries, to metadata blocks of 16-byte entries: u64 start, u32 size word (bit 24: uncompressed), u32 unused. A file's tail is the fragment block's bytes from its fragment offset.
